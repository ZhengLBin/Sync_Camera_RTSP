#include "../includes/tcp_streamer.h"
#include <iostream>
#include <cstring>
#include <sstream>

// ShmFrameBuffer实现
ShmFrameBuffer::ShmFrameBuffer(AVFrame* frame) : pts(frame->pts) {
    size = frame->width * frame->height * 3 / 2;
    data = new uint8_t[size];

    uint8_t* dst = data;

    // 复制Y平面
    uint8_t* src_y = frame->data[0];
    for (int i = 0; i < frame->height; i++) {
        memcpy(dst, src_y, frame->width);
        dst += frame->width;
        src_y += frame->linesize[0];
    }

    // 复制U平面
    uint8_t* src_u = frame->data[1];
    for (int i = 0; i < frame->height / 2; i++) {
        memcpy(dst, src_u, frame->width / 2);
        dst += frame->width / 2;
        src_u += frame->linesize[1];
    }

    // 复制V平面
    uint8_t* src_v = frame->data[2];
    for (int i = 0; i < frame->height / 2; i++) {
        memcpy(dst, src_v, frame->width / 2);
        dst += frame->width / 2;
        src_v += frame->linesize[2];
    }
}

ShmFrameBuffer::~ShmFrameBuffer() {
    delete[] data;
}

// 简化的GStreamer初始化
bool initialize_gstreamer() {
    if (!gst_is_initialized()) {
        gst_init(nullptr, nullptr);
    }
    gst_debug_set_default_threshold(GST_LEVEL_WARNING);
    return true;
}

// TCPStreamer实现
std::atomic<int> TCPStreamer::next_port_{ 5010 };
std::mutex TCPStreamer::port_allocation_mutex_;

TCPStreamer::TCPStreamer(const std::string& name, int port)
    : name_(name), width_(0), height_(0), fps_(0),
    pipeline_(nullptr), appsrc_(nullptr), bus_(nullptr), bus_watch_id_(0),
    running_(false), initialized_(false), need_data_(true),
    frame_count_(0) {

    if (port > 0) {
        port_ = port;
    }
    else {
        port_ = allocate_port();
    }

    if (!initialize_gstreamer()) {
        std::cerr << "GStreamer initialization failed" << std::endl;
    }
}

TCPStreamer::~TCPStreamer() {
    stop();
}

bool TCPStreamer::init(int width, int height, int fps) {
    width_ = width;
    height_ = height;
    fps_ = fps;

    if (!create_pipeline()) {
        std::cerr << "Failed to create pipeline for port " << port_ << std::endl;
        return false;
    }

    GstStateChangeReturn ret = gst_element_set_state(pipeline_, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        std::cerr << "Failed to start pipeline on port " << port_ << std::endl;
        stop();
        return false;
    }

    running_ = true;
    push_thread_ = std::thread(&TCPStreamer::push_frame_loop, this);
    initialized_ = true;

    std::cout << "TCPStreamer initialized on port " << port_ << std::endl;
    return true;
}

int TCPStreamer::allocate_port() {
    std::lock_guard<std::mutex> lock(port_allocation_mutex_);
    return next_port_++;
}


bool TCPStreamer::create_pipeline() {
    std::ostringstream pipeline_str;

    // 直接使用已知可工作的简单H.264 pipeline，避免失败的尝试
    pipeline_str << "appsrc name=mysrc "
        << "caps=\"video/x-raw,format=I420,width=" << width_
        << ",height=" << height_ << ",framerate=" << fps_ << "/1\" "
        << "is-live=true do-timestamp=true block=true max-buffers=3 ! "
        << "queue ! "
        << "x264enc tune=zerolatency speed-preset=veryfast ! "
        << "tcpserversink host=127.0.0.1 port=" << port_ << " sync=false";

    std::cout << "Creating H.264 pipeline: " << pipeline_str.str() << std::endl;

    GError* error = nullptr;
    pipeline_ = gst_parse_launch(pipeline_str.str().c_str(), &error);

    if (!pipeline_ || error) {
        if (error) {
            std::cerr << "Pipeline creation failed: " << error->message << std::endl;
            g_clear_error(&error);
        }
        return false;
    }

    // 获取appsrc元素
    appsrc_ = gst_bin_get_by_name(GST_BIN(pipeline_), "mysrc");
    if (!appsrc_) {
        std::cerr << "Failed to get appsrc element" << std::endl;
        gst_object_unref(pipeline_);
        pipeline_ = nullptr;
        return false;
    }

    // 配置appsrc属性
    GstCaps* caps = gst_caps_new_simple("video/x-raw",
        "format", G_TYPE_STRING, "I420",
        "width", G_TYPE_INT, width_,
        "height", G_TYPE_INT, height_,
        "framerate", GST_TYPE_FRACTION, fps_, 1,
        nullptr);

    g_object_set(G_OBJECT(appsrc_),
        "caps", caps,
        "is-live", TRUE,
        "do-timestamp", TRUE,
        "format", GST_FORMAT_TIME,
        "max-buffers", 3,
        "block", TRUE,
        "emit-signals", TRUE,
        nullptr);

    gst_caps_unref(caps);

    // 连接信号
    g_signal_connect(appsrc_, "need-data", G_CALLBACK(need_data_cb), this);
    g_signal_connect(appsrc_, "enough-data", G_CALLBACK(enough_data_cb), this);

    // 设置总线监听
    bus_ = gst_element_get_bus(pipeline_);
    if (bus_) {
        bus_watch_id_ = gst_bus_add_watch(bus_, bus_call, this);
    }

    std::cout << "H.264 pipeline created successfully for port " << port_ << std::endl;
    return true;
}

bool TCPStreamer::push_frame_to_appsrc() {
    if (!appsrc_ || !running_.load()) return false;

    std::shared_ptr<ShmFrameBuffer> frame_buffer;
    {
        std::unique_lock<std::mutex> lock(queue_mutex_);
        if (frame_queue_.empty()) return false;
        frame_buffer = frame_queue_.front();
        frame_queue_.pop();
    }

    size_t expected_size = width_ * height_ * 3 / 2;
    if (frame_buffer->size != expected_size) {
        std::cerr << "Frame size mismatch: expected " << expected_size
            << ", got " << frame_buffer->size << std::endl;
        return false;
    }

    GstBuffer* buffer = gst_buffer_new_allocate(nullptr, frame_buffer->size, nullptr);
    if (!buffer) {
        std::cerr << "Failed to allocate GstBuffer" << std::endl;
        return false;
    }

    GstMapInfo map;
    if (gst_buffer_map(buffer, &map, GST_MAP_WRITE)) {
        memcpy(map.data, frame_buffer->data, frame_buffer->size);
        gst_buffer_unmap(buffer, &map);
    }
    else {
        std::cerr << "Failed to map GstBuffer" << std::endl;
        gst_buffer_unref(buffer);
        return false;
    }

    // 设置精确的时间戳
    GstClockTime frame_duration = gst_util_uint64_scale(GST_SECOND, 1, fps_);
    GstClockTime timestamp = gst_util_uint64_scale(frame_count_, GST_SECOND, fps_);

    GST_BUFFER_PTS(buffer) = timestamp;
    GST_BUFFER_DTS(buffer) = timestamp;
    GST_BUFFER_DURATION(buffer) = frame_duration;

    // 设置关键帧标志
    if (frame_count_ % 30 == 0) {
        GST_BUFFER_FLAG_UNSET(buffer, GST_BUFFER_FLAG_DELTA_UNIT);
    }
    else {
        GST_BUFFER_FLAG_SET(buffer, GST_BUFFER_FLAG_DELTA_UNIT);
    }

    frame_count_++;

    GstFlowReturn ret = gst_app_src_push_buffer(GST_APP_SRC(appsrc_), buffer);

    if (ret != GST_FLOW_OK) {
        std::cerr << "Failed to push buffer to appsrc: " << ret << std::endl;
        return false;
    }

    return true;
}


bool TCPStreamer::send_frame(AVFrame* frame) {
    if (!running_.load() || !initialized_) return false;

    if (frame->format != AV_PIX_FMT_YUV420P ||
        frame->width != width_ || frame->height != height_) {
        return false;
    }

    bool can_accept = false;
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        if (need_data_.load() && frame_queue_.size() < MAX_QUEUE_SIZE) {
            can_accept = true;
        }
    }

    if (!can_accept) return false;

    auto frame_buffer = std::make_shared<ShmFrameBuffer>(frame);

    {
        std::lock_guard<std::mutex> lock(queue_mutex_);

        // 如果队列满了，移除最老的帧
        if (frame_queue_.size() >= MAX_QUEUE_SIZE) {
            frame_queue_.pop();
        }

        frame_queue_.push(frame_buffer);
    }

    queue_cv_.notify_one();
    return true;
}

void TCPStreamer::push_frame_loop() {
    while (running_.load()) {
        if (appsrc_ && need_data_.load()) {
            size_t queue_size = 0;
            {
                std::lock_guard<std::mutex> lock(queue_mutex_);
                queue_size = frame_queue_.size();
            }

            if (queue_size > 0) {
                push_frame_to_appsrc();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            else {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        }
        else {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
}

void TCPStreamer::stop() {
    if (!running_.load()) return;

    running_ = false;
    initialized_ = false;

    queue_cv_.notify_all();

    if (push_thread_.joinable()) {
        push_thread_.join();
    }

    if (pipeline_) {
        gst_element_set_state(pipeline_, GST_STATE_NULL);
        gst_object_unref(pipeline_);
        pipeline_ = nullptr;
    }

    if (bus_) {
        if (bus_watch_id_ > 0) {
            g_source_remove(bus_watch_id_);
            bus_watch_id_ = 0;
        }
        gst_object_unref(bus_);
        bus_ = nullptr;
    }

    appsrc_ = nullptr;

    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        while (!frame_queue_.empty()) {
            frame_queue_.pop();
        }
    }
}

// 回调函数
void TCPStreamer::need_data_cb(GstElement* appsrc, guint unused, gpointer user_data) {
    TCPStreamer* streamer = static_cast<TCPStreamer*>(user_data);
    streamer->need_data_ = true;
}

void TCPStreamer::enough_data_cb(GstElement* appsrc, gpointer user_data) {
    TCPStreamer* streamer = static_cast<TCPStreamer*>(user_data);
    streamer->need_data_ = false;
}

gboolean TCPStreamer::bus_call(GstBus* bus, GstMessage* msg, gpointer user_data) {
    TCPStreamer* streamer = static_cast<TCPStreamer*>(user_data);

    switch (GST_MESSAGE_TYPE(msg)) {
    case GST_MESSAGE_EOS:
        std::cout << "TCP:" << streamer->port_ << " End of stream" << std::endl;
        break;
    case GST_MESSAGE_ERROR: {
        GError* error;
        gchar* debug;
        gst_message_parse_error(msg, &error, &debug);
        std::cerr << "GStreamer error: " << error->message << std::endl;
        if (debug) {
            std::cerr << "Debug: " << debug << std::endl;
        }
        g_error_free(error);
        g_free(debug);
        break;
    }
    default:
        break;
    }

    return TRUE;
}
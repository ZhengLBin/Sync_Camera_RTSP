// shared_memory_streamer.cpp - 修复的原始I420 TCP流版本
#include "../includes/tcp_streamer.h"
#include <iostream>
#include <cstring>
#include <sstream>

// 静态端口分配
int TCPStreamer::next_port_ = 5010;

// ShmFrameBuffer构造保持不变
ShmFrameBuffer::ShmFrameBuffer(AVFrame* frame) : pts(frame->pts) {
    size = frame->width * frame->height * 3 / 2;
    data = new uint8_t[size];

    uint8_t* dst = data;
    uint8_t* src_y = frame->data[0];
    for (int i = 0; i < frame->height; i++) {
        memcpy(dst, src_y, frame->width);
        dst += frame->width;
        src_y += frame->linesize[0];
    }

    uint8_t* src_u = frame->data[1];
    for (int i = 0; i < frame->height / 2; i++) {
        memcpy(dst, src_u, frame->width / 2);
        dst += frame->width / 2;
        src_u += frame->linesize[1];
    }

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

// 全局GStreamer初始化
bool initialize_gstreamer_for_shm() {
    if (!gst_is_initialized()) {
        gst_init(nullptr, nullptr);
    }

    gst_debug_set_default_threshold(GST_LEVEL_WARNING);

    std::vector<std::string> required_elements = {
        "appsrc", "tcpserversink", "queue"
    };

    bool all_available = true;
    for (const auto& element_name : required_elements) {
        GstElement* element = gst_element_factory_make(element_name.c_str(), nullptr);
        if (element) {
            gst_object_unref(element);
        }
        else {
            std::cerr << "[ERROR] Missing GStreamer element: " << element_name << std::endl;
            all_available = false;
        }
    }

    return all_available;
}

// TCPStreamer构造函数
TCPStreamer::TCPStreamer(const std::string& name, int port)
    : name_(name), width_(0), height_(0), fps_(0),
    pipeline_(nullptr), appsrc_(nullptr), bus_(nullptr), bus_watch_id_(0),
    running_(false), initialized_(false), need_data_(true),
    frame_count_(0), dropped_frame_count_(0), total_memory_allocated_(0) {

    // 自动分配端口
    port_ = (port > 0) ? port : next_port_++;
    tcp_url_ = "tcp://localhost:" + std::to_string(port_);

    if (!initialize_gstreamer_for_shm()) {
        std::cerr << "[WARNING] GStreamer initialization issues detected" << std::endl;
    }
}

TCPStreamer::~TCPStreamer() {
    stop();
}

bool TCPStreamer::init(int width, int height, int fps) {
    width_ = width;
    height_ = height;
    fps_ = fps;

    std::cout << "[TCP] Initializing Raw I420 TCP streamer (" << width << "x" << height
        << "@" << fps << "fps) on port " << port_ << std::endl;

    if (!create_pipeline()) {
        std::cerr << "[ERROR] Failed to create GStreamer pipeline" << std::endl;
        return false;
    }

    GstStateChangeReturn ret = gst_element_set_state(pipeline_, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        std::cerr << "[ERROR] Failed to start GStreamer pipeline" << std::endl;
        stop();
        return false;
    }

    running_ = true;
    push_thread_ = std::thread(&TCPStreamer::push_frame_loop, this);

    initialized_ = true;

    // 调试信息
    print_pipeline_state();
    debug_caps_info();

    std::cout << "[TCP] Raw I420 TCP streamer started: " << tcp_url_ << std::endl;

    return true;
}


bool TCPStreamer::create_pipeline() {
    std::ostringstream pipeline_str;

    // 确保帧率信息正确传递的pipeline
    pipeline_str << "appsrc name=mysrc "
        << "caps=\"video/x-raw,format=I420,width=" << width_
        << ",height=" << height_ << ",framerate=" << fps_ << "/1\" "
        << "is-live=true "
        << "do-timestamp=true ! "  // 重新启用时间戳，确保帧率稳定
        << "tcpserversink host=127.0.0.1 port=" << port_
        << " sync=false";

    std::cout << "[TCP] Final I420 Pipeline: " << pipeline_str.str() << std::endl;

    GError* error = nullptr;
    pipeline_ = gst_parse_launch(pipeline_str.str().c_str(), &error);
    if (!pipeline_ || error) {
        std::cerr << "[ERROR] Failed to create pipeline: "
            << (error ? error->message : "unknown error") << std::endl;
        if (error) g_clear_error(&error);
        return false;
    }

    appsrc_ = gst_bin_get_by_name(GST_BIN(pipeline_), "mysrc");
    if (!appsrc_) {
        std::cerr << "[ERROR] Failed to get appsrc element" << std::endl;
        return false;
    }

    // 关键：确保caps完全固定，包括帧率
    GstCaps* caps = gst_caps_new_simple("video/x-raw",
        "format", G_TYPE_STRING, "I420",
        "width", G_TYPE_INT, width_,
        "height", G_TYPE_INT, height_,
        "framerate", GST_TYPE_FRACTION, fps_, 1,
        nullptr);

    // 验证caps是否固定
    if (!gst_caps_is_fixed(caps)) {
        std::cerr << "[ERROR] Caps are not fixed!" << std::endl;
        gchar* caps_str = gst_caps_to_string(caps);
        std::cerr << "[ERROR] Caps: " << caps_str << std::endl;
        g_free(caps_str);
        gst_caps_unref(caps);
        return false;
    }

    g_object_set(G_OBJECT(appsrc_),
        "caps", caps,
        "is-live", TRUE,
        "do-timestamp", TRUE,  // 启用时间戳确保正确的帧率
        "format", GST_FORMAT_TIME,
        nullptr);

    gst_caps_unref(caps);

    g_signal_connect(appsrc_, "need-data", G_CALLBACK(need_data_cb), this);
    g_signal_connect(appsrc_, "enough-data", G_CALLBACK(enough_data_cb), this);

    bus_ = gst_element_get_bus(pipeline_);
    bus_watch_id_ = gst_bus_add_watch(bus_, bus_call, this);

    return true;
}

// 修复时间戳以确保正确的帧率
bool TCPStreamer::push_frame_to_appsrc() {
    if (!appsrc_ || !running_.load()) {
        return false;
    }

    std::shared_ptr<ShmFrameBuffer> frame_buffer;

    {
        std::unique_lock<std::mutex> lock(queue_mutex_);
        if (frame_queue_.empty()) {
            return false;
        }
        frame_buffer = frame_queue_.front();
        frame_queue_.pop();
        total_memory_allocated_ -= frame_buffer->size;
    }

    size_t expected_size = width_ * height_ * 3 / 2;
    if (frame_buffer->size != expected_size) {
        std::cerr << "[ERROR] Frame size mismatch in push: expected "
            << expected_size << ", got " << frame_buffer->size << std::endl;
        return false;
    }

    GstBuffer* buffer = gst_buffer_new_allocate(nullptr, frame_buffer->size, nullptr);
    if (!buffer) {
        return false;
    }

    GstMapInfo map;
    if (gst_buffer_map(buffer, &map, GST_MAP_WRITE)) {
        memcpy(map.data, frame_buffer->data, frame_buffer->size);
        gst_buffer_unmap(buffer, &map);
    }
    else {
        gst_buffer_unref(buffer);
        return false;
    }

    // 关键修复：使用固定间隔的时间戳，确保稳定帧率
    static guint64 frame_number = 0;
    GstClockTime frame_duration = gst_util_uint64_scale(GST_SECOND, 1, fps_);
    GstClockTime timestamp = gst_util_uint64_scale(frame_number, GST_SECOND, fps_);

    GST_BUFFER_PTS(buffer) = timestamp;
    GST_BUFFER_DTS(buffer) = timestamp;
    GST_BUFFER_DURATION(buffer) = frame_duration;

    frame_number++;

    // 确保所有帧都是关键帧（对于原始视频）
    GST_BUFFER_FLAG_UNSET(buffer, GST_BUFFER_FLAG_DELTA_UNIT);

    GstFlowReturn ret = gst_app_src_push_buffer(GST_APP_SRC(appsrc_), buffer);
    if (ret != GST_FLOW_OK) {
        static int error_count = 0;
        if (error_count < 5) {
            std::cerr << "[ERROR] Failed to push buffer to TCP: " << ret
                << " (" << gst_flow_get_name(ret) << ")" << std::endl;
            error_count++;
        }
        return false;
    }

    return true;
}

// 改进的send_frame方法，添加更多检查
bool TCPStreamer::send_frame(AVFrame* frame) {
    if (!running_.load() || !initialized_) {
        return false;
    }

    if (frame->format != AV_PIX_FMT_YUV420P ||
        frame->width != width_ || frame->height != height_) {
        static int error_count = 0;
        if (error_count < 3) {
            std::cerr << "[ERROR] Frame format/size mismatch - Expected: I420 "
                << width_ << "x" << height_
                << ", Got: " << frame->format << " "
                << frame->width << "x" << frame->height << std::endl;
            error_count++;
        }
        return false;
    }

    bool can_accept_frame = false;
    size_t current_queue_size = 0;
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        current_queue_size = frame_queue_.size();

        if (need_data_.load() && current_queue_size < MAX_QUEUE_SIZE) {
            can_accept_frame = true;
        }
    }

    if (!can_accept_frame) {
        dropped_frame_count_++;
        return false;
    }

    auto frame_buffer = std::make_shared<ShmFrameBuffer>(frame);

    // 验证frame buffer大小
    size_t expected_size = width_ * height_ * 3 / 2;
    if (frame_buffer->size != expected_size) {
        std::cerr << "[ERROR] Frame buffer size mismatch: expected "
            << expected_size << ", got " << frame_buffer->size << std::endl;
        return false;
    }

    total_memory_allocated_ += frame_buffer->size;

    {
        std::lock_guard<std::mutex> lock(queue_mutex_);

        if (frame_queue_.size() >= MAX_QUEUE_SIZE) {
            auto old_buffer = frame_queue_.front();
            total_memory_allocated_ -= old_buffer->size;
            frame_queue_.pop();
            dropped_frame_count_++;
        }

        frame_queue_.push(frame_buffer);
    }

    frame_count_++;
    queue_cv_.notify_one();
    return true;
}

void TCPStreamer::push_frame_loop() {
    int push_count = 0;
    int failed_push_count = 0;
    auto start_time = std::chrono::steady_clock::now();
    auto last_stats_time = start_time;

    while (running_.load()) {
        bool has_appsrc = (appsrc_ != nullptr);
        bool needs_data = need_data_.load();
        size_t queue_size = 0;

        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            queue_size = frame_queue_.size();
        }

        if (has_appsrc && needs_data && queue_size > 0) {
            bool pushed = push_frame_to_appsrc();
            if (pushed) {
                push_count++;
            }
            else {
                failed_push_count++;
            }
        }

        auto current_time = std::chrono::steady_clock::now();

        if (std::chrono::duration_cast<std::chrono::seconds>(current_time - last_stats_time).count() >= 120) {
            std::cout << "[TCP:" << port_ << "] Pushed: " << push_count
                << ", Failed: " << failed_push_count
                << ", Queue: " << queue_size << "/" << MAX_QUEUE_SIZE << std::endl;
            last_stats_time = current_time;
        }

        if (has_appsrc && needs_data && queue_size > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        else {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
}



void TCPStreamer::stop() {
    if (!running_.load()) {
        return;
    }

    std::cout << "[TCP:" << port_ << "] Stopping..." << std::endl;
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
        size_t remaining_frames = frame_queue_.size();
        while (!frame_queue_.empty()) {
            frame_queue_.pop();
        }
        total_memory_allocated_ = 0;

        if (remaining_frames > 0) {
            std::cout << "[TCP:" << port_ << "] Cleared " << remaining_frames << " frames" << std::endl;
        }
    }

    std::cout << "[TCP:" << port_ << "] Stopped. Sent: " << frame_count_
        << ", Dropped: " << dropped_frame_count_ << std::endl;
}

// 添加调试方法
void TCPStreamer::print_pipeline_state() {
    if (!pipeline_) return;

    GstState state;
    GstState pending;
    GstStateChangeReturn ret = gst_element_get_state(pipeline_, &state, &pending, 0);

    std::cout << "[TCP:" << port_ << "] Pipeline state: ";
    switch (state) {
    case GST_STATE_NULL: std::cout << "NULL"; break;
    case GST_STATE_READY: std::cout << "READY"; break;
    case GST_STATE_PAUSED: std::cout << "PAUSED"; break;
    case GST_STATE_PLAYING: std::cout << "PLAYING"; break;
    default: std::cout << "UNKNOWN"; break;
    }
    std::cout << std::endl;
}

void TCPStreamer::debug_caps_info() {
    if (!appsrc_) return;

    GstCaps* caps;
    g_object_get(G_OBJECT(appsrc_), "caps", &caps, nullptr);
    if (caps) {
        gchar* caps_str = gst_caps_to_string(caps);
        std::cout << "[TCP:" << port_ << "] Appsrc caps: " << caps_str << std::endl;
        g_free(caps_str);
        gst_caps_unref(caps);
    }
}

// 回调函数保持不变
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
        std::cout << "[TCP:" << streamer->port_ << "] End of stream" << std::endl;
        break;
    case GST_MESSAGE_ERROR: {
        GError* error;
        gchar* debug;
        gst_message_parse_error(msg, &error, &debug);
        std::cerr << "[ERROR] GStreamer error: " << error->message << std::endl;
        if (debug) {
            std::cerr << "[DEBUG] " << debug << std::endl;
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
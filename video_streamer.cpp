// video_streamer.cpp - 支持客户端连接回调和延迟初始化
#include "video_streamer.h"
#include <iostream>
#include <cstring>
#include <sstream>

// 全局调试计数器
std::atomic<size_t> g_total_frame_buffers_created{ 0 };
std::atomic<size_t> g_total_frame_buffers_destroyed{ 0 };

// FrameBuffer implementation with debugging
FrameBuffer::FrameBuffer(AVFrame* frame) : pts(frame->pts) {
    size = frame->width * frame->height * 3 / 2;
    data = new uint8_t[size];
    g_total_frame_buffers_created++;

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

FrameBuffer::~FrameBuffer() {
    delete[] data;
    g_total_frame_buffers_destroyed++;
}

bool initialize_gstreamer_with_diagnostics() {
    std::cout << "=== GStreamer Diagnostics ===" << std::endl;

    if (gst_is_initialized()) {
        std::cout << "GStreamer is already initialized." << std::endl;
    }
    else {
        std::cout << "Initializing GStreamer..." << std::endl;
        gst_init(nullptr, nullptr);
    }

    guint major, minor, micro, nano;
    gst_version(&major, &minor, &micro, &nano);
    std::cout << "GStreamer Version: " << major << "." << minor << "." << micro << "." << nano << std::endl;

    // 设置日志级别为WARNING，减少噪音
    gst_debug_set_default_threshold(GST_LEVEL_WARNING);

    std::vector<std::string> core_plugins = {
        "coreelements", "videoconvert", "videotestsrc", "app", "x264"
    };

    for (const auto& plugin_name : core_plugins) {
        GstPlugin* plugin = gst_plugin_load_by_name(plugin_name.c_str());
        if (plugin) {
            std::cout << "Loaded plugin: " << plugin_name << std::endl;
            gst_object_unref(plugin);
        }
        else {
            std::cout << "Failed to load plugin: " << plugin_name << std::endl;
        }
    }

    std::vector<std::string> test_elements = {
        "appsrc", "videoconvert", "x264enc", "rtph264pay", "identity"
    };

    bool all_available = true;
    for (const auto& element_name : test_elements) {
        GstElement* element = gst_element_factory_make(element_name.c_str(), nullptr);
        if (element) {
            std::cout << "[OK] Created element '" << element_name << "'" << std::endl;
            gst_object_unref(element);
        }
        else {
            std::cout << "[FAIL] Failed to create element '" << element_name << "'" << std::endl;
            all_available = false;
        }
    }

    std::cout << "=============================" << std::endl;
    return all_available;
}

VideoStreamer::VideoStreamer(const std::string& rtp_ip, int rtp_port)
    : ip_(rtp_ip), port_(rtp_port), width_(0), height_(0), fps_(0),
    server_(nullptr), mounts_(nullptr), factory_(nullptr), loop_(nullptr), appsrc_(nullptr),
    running_(false), initialized_(false), need_data_(true), frame_count_(0),
    dropped_frame_count_(0), total_memory_allocated_(0), client_connected_(false) {

    if (!initialize_gstreamer_with_diagnostics()) {
        std::cerr << "GStreamer initialization warning: some components may not be available." << std::endl;
    }

    std::cout << "[VideoStreamer " << port_ << "] Constructor - memory tracking enabled" << std::endl;
}

VideoStreamer::~VideoStreamer() {
    stop();

    // 打印最终的内存统计
    std::cout << "[VideoStreamer " << port_ << "] Destructor - FrameBuffers created: "
        << g_total_frame_buffers_created.load()
        << ", destroyed: " << g_total_frame_buffers_destroyed.load() << std::endl;
}

bool VideoStreamer::init(int width, int height, int fps) {
    width_ = width;
    height_ = height;
    fps_ = fps;

    std::cout << "[VideoStreamer " << port_ << "] Initializing (size: "
        << width << "x" << height << ", fps: " << fps << ")" << std::endl;

    try {
        server_ = gst_rtsp_server_new();
        if (!server_) {
            std::cerr << "Failed to create RTSP server." << std::endl;
            return false;
        }

        // 服务器配置
        gst_rtsp_server_set_address(server_, "0.0.0.0");
        gst_rtsp_server_set_service(server_, std::to_string(port_).c_str());

        g_object_set(server_,
            "backlog", 10,
            nullptr);

        mounts_ = gst_rtsp_server_get_mount_points(server_);
        factory_ = gst_rtsp_media_factory_new();
        if (!factory_) {
            std::cerr << "Failed to create media factory." << std::endl;
            return false;
        }

        std::string launch_desc = create_optimized_pipeline();
        std::cout << "[VideoStreamer " << port_ << "] Pipeline: " << launch_desc << std::endl;

        // 验证pipeline有效性
        GError* error = nullptr;
        GstElement* test_pipeline = gst_parse_launch(launch_desc.c_str(), &error);
        if (!test_pipeline || error) {
            std::cerr << "Invalid pipeline: " << (error ? error->message : "unknown error") << std::endl;
            if (error) g_clear_error(&error);
            return false;
        }
        gst_object_unref(test_pipeline);

        gst_rtsp_media_factory_set_launch(factory_, launch_desc.c_str());
        gst_rtsp_media_factory_set_shared(factory_, FALSE);

        // 优化设置
        gst_rtsp_media_factory_set_latency(factory_, 0);
        gst_rtsp_media_factory_set_buffer_size(factory_, 1024 * 1024);

        gst_rtsp_media_factory_set_protocols(
            factory_,
            static_cast<GstRTSPLowerTrans>(GST_RTSP_LOWER_TRANS_TCP | GST_RTSP_LOWER_TRANS_UDP)
        );

        gst_rtsp_media_factory_set_permissions(factory_, nullptr);

        // 连接信号处理
        g_signal_connect(factory_, "media-configure", G_CALLBACK(media_configure_cb), this);
        g_signal_connect(factory_, "media-constructed", G_CALLBACK(media_constructed_cb), this);

        // 挂载到路径
        gst_rtsp_mount_points_add_factory(mounts_, "/stream", factory_);

        // 启动GStreamer主循环
        loop_ = g_main_loop_new(nullptr, FALSE);
        if (!loop_) {
            std::cerr << "Failed to create GMain loop" << std::endl;
            return false;
        }

        std::cout << "[VideoStreamer " << port_ << "] Starting GStreamer main loop..." << std::endl;
        gst_thread_ = std::thread(&VideoStreamer::gstreamer_main_loop, this);

        // 等待GStreamer启动
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        // 启动服务器
        guint server_id = gst_rtsp_server_attach(server_, nullptr);
        if (server_id == 0) {
            std::cerr << "Failed to attach RTSP server to port " << port_ << std::endl;
            return false;
        }

        std::cout << "[VideoStreamer " << port_ << "] RTSP server attached with ID: " << server_id << std::endl;

        running_ = true;
        push_thread_ = std::thread(&VideoStreamer::push_frame_loop, this);

        initialized_ = true;
        std::cout << "[VideoStreamer " << port_ << "] RTSP server started: " << get_rtsp_url() << std::endl;

        // 连接测试建议
        std::cout << "[VideoStreamer " << port_ << "] To test connection, use:" << std::endl;
        std::cout << "  ffplay -rtsp_transport tcp -fflags nobuffer -flags low_delay " << get_rtsp_url() << std::endl;
        std::cout << "  或者使用VLC: vlc " << get_rtsp_url() << std::endl;

        return true;

    }
    catch (const std::exception& e) {
        std::cerr << "[VideoStreamer " << port_ << "] Exception during initialization: " << e.what() << std::endl;
        return false;
    }
}

void VideoStreamer::set_client_callback(ClientCallback callback) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    client_callback_ = callback;
}

bool VideoStreamer::is_client_connected() const {
    return client_connected_.load();
}

void VideoStreamer::notify_client_status(bool connected) {
    bool prev_status = client_connected_.exchange(connected);

    // 只有状态真正改变时才通知
    if (prev_status != connected) {
        std::cout << "[VideoStreamer " << port_ << "] Client "
            << (connected ? "CONNECTED" : "DISCONNECTED") << std::endl;

        std::lock_guard<std::mutex> lock(callback_mutex_);
        if (client_callback_) {
            try {
                client_callback_(connected);
            }
            catch (const std::exception& e) {
                std::cerr << "[VideoStreamer " << port_ << "] Exception in client callback: "
                    << e.what() << std::endl;
            }
        }
    }
}

bool VideoStreamer::send_frame(AVFrame* frame) {
    if (!running_.load() || !initialized_) {
        return false;
    }

    if (frame->format != AV_PIX_FMT_YUV420P ||
        frame->width != width_ || frame->height != height_) {
        static int error_count = 0;
        if (error_count < 5) {
            std::cerr << "[VideoStreamer " << port_ << "] Frame format/size mismatch: "
                << "format=" << frame->format << " (expected " << AV_PIX_FMT_YUV420P << "), "
                << "size=" << frame->width << "x" << frame->height
                << " (expected " << width_ << "x" << height_ << ")" << std::endl;
            error_count++;
        }
        return false;
    }

    // 如果没有客户端连接，拒绝帧
    if (!client_connected_.load()) {
        dropped_frame_count_++;
        if (dropped_frame_count_ % 1000 == 0) {
            std::cout << "[VideoStreamer " << port_ << "] No client, dropped "
                << dropped_frame_count_ << " frames" << std::endl;
        }
        return false;
    }

    // 检查队列状态
    bool can_accept_frame = false;
    size_t current_queue_size = 0;
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        current_queue_size = frame_queue_.size();

        if (need_data_.load() && current_queue_size < 3) {
            can_accept_frame = true;
        }
    }

    if (!can_accept_frame) {
        dropped_frame_count_++;
        if (dropped_frame_count_ % 500 == 0) {
            std::string reason = need_data_.load() ? "queue full" : "GStreamer backpressure";
            std::cout << "[VideoStreamer " << port_ << "] Dropping frame (" << reason
                << "), queue: " << current_queue_size << "/3, dropped: " << dropped_frame_count_ << std::endl;
        }
        return false;
    }

    // 创建帧缓冲区
    auto frame_buffer = std::make_shared<FrameBuffer>(frame);
    total_memory_allocated_ += frame_buffer->size;

    {
        std::lock_guard<std::mutex> lock(queue_mutex_);

        // 防止队列过满
        if (frame_queue_.size() >= 3) {
            auto old_buffer = frame_queue_.front();
            total_memory_allocated_ -= old_buffer->size;
            frame_queue_.pop();
            dropped_frame_count_++;
        }

        frame_queue_.push(frame_buffer);
    }

    frame_count_++;
    queue_cv_.notify_one();

    // 减少日志频率
    if (frame_count_ % 2000 == 0) {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        std::cout << "[VideoStreamer " << port_ << "] Frame " << frame_count_
            << ", dropped " << dropped_frame_count_
            << ", queue: " << frame_queue_.size() << "/3"
            << ", mem: " << (total_memory_allocated_ / (1024 * 1024)) << "MB" << std::endl;
    }

    return true;
}

void VideoStreamer::stop() {
    if (!running_.load()) {
        return;
    }

    std::cout << "[VideoStreamer " << port_ << "] Stopping..." << std::endl;
    running_ = false;
    initialized_ = false;

    // 通知客户端断开
    notify_client_status(false);

    queue_cv_.notify_all();

    if (loop_) {
        g_main_loop_quit(loop_);
    }

    if (gst_thread_.joinable()) {
        gst_thread_.join();
    }

    if (push_thread_.joinable()) {
        push_thread_.join();
    }

    if (mounts_) {
        g_object_unref(mounts_);
        mounts_ = nullptr;
    }

    if (server_) {
        g_object_unref(server_);
        server_ = nullptr;
    }

    if (loop_) {
        g_main_loop_unref(loop_);
        loop_ = nullptr;
    }

    // 清理队列
    size_t cleared_memory = 0;
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        size_t remaining_frames = frame_queue_.size();
        while (!frame_queue_.empty()) {
            auto frame_buffer = frame_queue_.front();
            cleared_memory += frame_buffer->size;
            frame_queue_.pop();
        }
        total_memory_allocated_ = 0;

        if (remaining_frames > 0) {
            std::cout << "[VideoStreamer " << port_ << "] Cleared " << remaining_frames
                << " frames (" << (cleared_memory / (1024 * 1024)) << "MB)" << std::endl;
        }
    }

    appsrc_ = nullptr;
    std::cout << "[VideoStreamer " << port_ << "] Stopped. Total frames: " << frame_count_
        << ", dropped: " << dropped_frame_count_ << std::endl;
}

std::string VideoStreamer::get_rtsp_url() const {
    return "rtsp://" + ip_ + ":" + std::to_string(port_) + "/stream";
}

void VideoStreamer::gstreamer_main_loop() {
    std::cout << "[VideoStreamer " << port_ << "] Starting GStreamer main loop..." << std::endl;
    if (loop_) {
        g_main_loop_run(loop_);
        std::cout << "[VideoStreamer " << port_ << "] GStreamer main loop exited." << std::endl;
    }
    else {
        std::cerr << "[VideoStreamer " << port_ << "] ERROR: GMain loop is null!" << std::endl;
    }
}

void VideoStreamer::push_frame_loop() {
    std::cout << "[VideoStreamer " << port_ << "] Starting frame push thread..." << std::endl;

    int push_count = 0;
    int failed_push_count = 0;
    auto last_stats_time = std::chrono::steady_clock::now();

    while (running_.load()) {
        bool has_appsrc = (appsrc_ != nullptr);
        bool needs_data = need_data_.load();
        bool client_connected = client_connected_.load();
        size_t queue_size = 0;

        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            queue_size = frame_queue_.size();
        }

        if (has_appsrc && client_connected && needs_data && queue_size > 0) {
            bool pushed = push_frame_to_appsrc();
            if (pushed) {
                push_count++;
            }
            else {
                failed_push_count++;
            }
        }

        auto current_time = std::chrono::steady_clock::now();

        // 每60秒打印推送统计
        if (std::chrono::duration_cast<std::chrono::seconds>(current_time - last_stats_time).count() >= 60) {
            std::cout << "[VideoStreamer " << port_ << "] Push stats - Success: " << push_count
                << ", Failed: " << failed_push_count
                << ", Queue: " << queue_size << "/3"
                << ", Need data: " << needs_data
                << ", AppSrc: " << (has_appsrc ? "VALID" : "NULL")
                << ", Client: " << (client_connected ? "YES" : "NO") << std::endl;
            last_stats_time = current_time;
        }

        // 动态睡眠
        if (has_appsrc && client_connected && needs_data && queue_size > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        else {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }

    std::cout << "[VideoStreamer " << port_ << "] Frame push thread exited - Pushed: "
        << push_count << ", Failed: " << failed_push_count << std::endl;
}

bool VideoStreamer::push_frame_to_appsrc() {
    if (!appsrc_ || !running_.load() || !client_connected_.load()) {
        return false;
    }

    std::shared_ptr<FrameBuffer> frame_buffer;

    {
        std::unique_lock<std::mutex> lock(queue_mutex_);
        if (frame_queue_.empty()) {
            return false;
        }
        frame_buffer = frame_queue_.front();
        frame_queue_.pop();
        total_memory_allocated_ -= frame_buffer->size;
    }

    GstBuffer* buffer = gst_buffer_new_allocate(nullptr, frame_buffer->size, nullptr);
    if (!buffer) {
        std::cerr << "[VideoStreamer " << port_ << "] Failed to allocate GstBuffer." << std::endl;
        return false;
    }

    GstMapInfo map;
    if (gst_buffer_map(buffer, &map, GST_MAP_WRITE)) {
        memcpy(map.data, frame_buffer->data, frame_buffer->size);
        gst_buffer_unmap(buffer, &map);
    }
    else {
        std::cerr << "[VideoStreamer " << port_ << "] Failed to map GstBuffer." << std::endl;
        gst_buffer_unref(buffer);
        return false;
    }

    // 时间戳处理
    static std::chrono::steady_clock::time_point start_time = std::chrono::steady_clock::now();
    auto current_time = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(current_time - start_time);

    GstClockTime timestamp = elapsed.count();
    GST_BUFFER_PTS(buffer) = timestamp;
    GST_BUFFER_DTS(buffer) = timestamp;
    GST_BUFFER_DURATION(buffer) = GST_SECOND / fps_;

    GstFlowReturn ret = gst_app_src_push_buffer(GST_APP_SRC(appsrc_), buffer);
    if (ret != GST_FLOW_OK) {
        static int error_count = 0;
        if (error_count < 10) {
            std::cerr << "[VideoStreamer " << port_ << "] Failed to push buffer: " << ret << std::endl;
            error_count++;
        }
        return false;
    }

    return true;
}

std::string VideoStreamer::create_optimized_pipeline() const {
    std::ostringstream oss;
    oss << "appsrc name=mysrc ! "
        << "video/x-raw,format=I420,width=" << width_
        << ",height=" << height_ << ",framerate=" << fps_ << "/1 ! "
        << "queue max-size-buffers=3 max-size-time=100000000 leaky=downstream ! "
        << "x264enc tune=zerolatency bitrate=2000 speed-preset=ultrafast threads=1 ! "
        << "h264parse ! "
        << "rtph264pay name=pay0 config-interval=1 mtu=1200";
    return oss.str();
}

// GStreamer回调函数
void VideoStreamer::media_configure_cb(GstRTSPMediaFactory* factory,
    GstRTSPMedia* media,
    gpointer user_data) {
    VideoStreamer* streamer = static_cast<VideoStreamer*>(user_data);

    std::cout << "[VideoStreamer " << streamer->port_ << "] Configuring media for client..." << std::endl;

    GstElement* pipeline = gst_rtsp_media_get_element(media);
    GstElement* appsrc = gst_bin_get_by_name(GST_BIN(pipeline), "mysrc");

    if (!appsrc) {
        std::cerr << "[VideoStreamer " << streamer->port_ << "] Failed to find appsrc element." << std::endl;
        return;
    }

    streamer->appsrc_ = appsrc;

    g_object_set(G_OBJECT(appsrc),
        "format", GST_FORMAT_TIME,
        "is-live", TRUE,
        "do-timestamp", FALSE,
        "min-latency", G_GINT64_CONSTANT(0),
        "max-latency", G_GINT64_CONSTANT(50000000),  // 50ms
        "max-buffers", 3,
        "leaky-type", 2,   // 丢弃旧帧
        nullptr);

    g_signal_connect(appsrc, "need-data", G_CALLBACK(need_data_cb), user_data);
    g_signal_connect(appsrc, "enough-data", G_CALLBACK(enough_data_cb), user_data);

    gst_rtsp_media_set_latency(media, 50);

    std::cout << "[VideoStreamer " << streamer->port_ << "] Media configured successfully" << std::endl;
}

void VideoStreamer::media_constructed_cb(GstRTSPMediaFactory* factory,
    GstRTSPMedia* media,
    gpointer user_data) {
    VideoStreamer* streamer = static_cast<VideoStreamer*>(user_data);

    std::cout << "[VideoStreamer " << streamer->port_ << "] Media constructed - client connected!" << std::endl;

    // 通知客户端连接
    streamer->notify_client_status(true);

    // 监听媒体断开事件
    g_signal_connect(media, "unprepared", G_CALLBACK(media_unprepared_cb), user_data);
}

void VideoStreamer::media_unprepared_cb(GstRTSPMedia* media, gpointer user_data) {
    VideoStreamer* streamer = static_cast<VideoStreamer*>(user_data);

    std::cout << "[VideoStreamer " << streamer->port_ << "] Media unprepared - client disconnected!" << std::endl;

    // 通知客户端断开
    streamer->notify_client_status(false);
    streamer->appsrc_ = nullptr;
}

gboolean VideoStreamer::need_data_cb(GstElement* appsrc, guint unused, gpointer user_data) {
    VideoStreamer* streamer = static_cast<VideoStreamer*>(user_data);
    streamer->need_data_ = true;
    return TRUE;
}

void VideoStreamer::enough_data_cb(GstElement* appsrc, gpointer user_data) {
    VideoStreamer* streamer = static_cast<VideoStreamer*>(user_data);
    streamer->need_data_ = false;
}

std::string VideoStreamer::ffmpeg_errstr(int errnum) {
    return "Error code: " + std::to_string(errnum);
}
// sync_camera.cpp - 修复内存泄漏和帧平衡问题
#include "sync_camera.h"
#include <iostream>
#include <stdexcept>
#include <atomic>
#include <algorithm>

// 全局统计变量用于调试内存泄漏
std::atomic<size_t> g_total_frames_allocated{ 0 };
std::atomic<size_t> g_total_frames_freed{ 0 };
std::atomic<size_t> g_active_frame_count{ 0 };

// 新增：帧率监控
std::atomic<size_t> g_camera_frame_rates[2] = { 0, 0 };

DualCameraCapture::DualCameraCapture() {
    avdevice_register_all();
    std::cout << "[DualCameraCapture] Constructor - ready for delayed initialization" << std::endl;
}

DualCameraCapture::~DualCameraCapture() {
    std::cout << "[DualCameraCapture] Destructor called" << std::endl;
    full_reset();

    std::cout << "[DualCameraCapture] Final memory stats - Allocated: " << g_total_frames_allocated.load()
        << ", Freed: " << g_total_frames_freed.load()
        << ", Active: " << g_active_frame_count.load() << std::endl;

    if (g_active_frame_count.load() > 0) {
        std::cout << "[WARNING] Memory leak detected: " << g_active_frame_count.load()
            << " frames still active!" << std::endl;
    }
}

void DualCameraCapture::full_reset() {
    std::cout << "[DualCameraCapture] Performing full reset..." << std::endl;

    // 1. 停止所有线程
    stop();

    // 2. 确保所有线程已停止
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    size_t total_freed = 0;

    // 3. 清理摄像头资源
    for (int i = 0; i < static_cast<int>(cameras_.size()); ++i) {
        cleanup_camera(i);
    }
    cameras_.clear();

    // 4. 清理原始队列
    for (int i = 0; i < 2; ++i) {
        std::lock_guard<std::mutex> lock(mutex_);
        size_t queue_size = frame_queue_yuv[i].size();
        while (!frame_queue_yuv[i].empty()) {
            free_cloned_frame(&frame_queue_yuv[i].front().frame);
            frame_queue_yuv[i].pop_front();
            total_freed++;
        }
        if (queue_size > 0) {
            std::cout << "[RESET] Cleaned " << queue_size << " frames from raw queue " << i << std::endl;
        }
    }

    // 5. 清理同步队列
    {
        std::lock_guard<std::mutex> lock(mutex_);
        size_t sync_queue_size = synced_yuv_queue_.size();
        while (!synced_yuv_queue_.empty()) {
            auto& p = synced_yuv_queue_.front();
            free_cloned_frame(&p.first);
            free_cloned_frame(&p.second);
            synced_yuv_queue_.pop_front();
            total_freed += 2;
        }
        if (sync_queue_size > 0) {
            std::cout << "[RESET] Cleaned " << sync_queue_size << " frame pairs from sync queue" << std::endl;
        }
    }

    // 6. 重置状态
    initialized_ = false;
    running_ = false;
    capture_active_ = false;

    // 7. 重置统计
    g_camera_frame_rates[0] = 0;
    g_camera_frame_rates[1] = 0;

    std::cout << "[DualCameraCapture] Full reset completed, freed " << total_freed << " frames" << std::endl;
}

void DualCameraCapture::cleanup_camera(int index) {
    if (index >= static_cast<int>(cameras_.size())) return;

    auto& cam = cameras_[index];

    // 释放 SwsContext
    if (cam.sws_ctx_yuv) {
        sws_freeContext(cam.sws_ctx_yuv);
        cam.sws_ctx_yuv = nullptr;
    }

    // 释放帧缓存
    if (cam.yuv_frame) {
        av_frame_free(&cam.yuv_frame);
        cam.yuv_frame = nullptr;
    }
    if (cam.frame) {
        av_frame_free(&cam.frame);
        cam.frame = nullptr;
    }

    // 释放缓冲区
    if (cam.yuv_buffer) {
        av_freep(&cam.yuv_buffer);
        cam.yuv_buffer = nullptr;
    }

    // 释放解码器和格式上下文
    if (cam.codec_ctx) {
        avcodec_free_context(&cam.codec_ctx);
        cam.codec_ctx = nullptr;
    }
    if (cam.fmt_ctx) {
        avformat_close_input(&cam.fmt_ctx);
        cam.fmt_ctx = nullptr;
    }

    cam.video_stream_idx = -1;

    std::cout << "[DualCameraCapture] Camera " << index << " resources cleaned up" << std::endl;
}

// 改进的帧释放函数，增加详细跟踪
void DualCameraCapture::free_cloned_frame(AVFrame** frame) {
    if (frame && *frame) {
        if ((*frame)->data[0]) {
            av_freep(&(*frame)->data[0]);
        }
        av_frame_free(frame);

        size_t freed_count = g_total_frames_freed.fetch_add(1) + 1;
        size_t active_count = g_active_frame_count.fetch_sub(1) - 1;

        // 每释放1000帧打印一次统计
        if (freed_count % 1000 == 0) {
            std::cout << "[MEMORY] Freed " << freed_count << " frames, active: " << active_count << std::endl;
        }
    }
}

// 新增：激进的帧平衡函数
size_t DualCameraCapture::balance_frame_queues() {
    std::lock_guard<std::mutex> lock(mutex_);

    size_t queue0_size = frame_queue_yuv[0].size();
    size_t queue1_size = frame_queue_yuv[1].size();
    size_t balanced = 0;

    // 如果队列大小差异超过阈值，进行平衡
    const size_t BALANCE_THRESHOLD = 5;

    if (queue0_size > queue1_size + BALANCE_THRESHOLD) {
        // 队列0太多，丢弃多余的帧
        size_t excess = queue0_size - queue1_size - BALANCE_THRESHOLD;
        for (size_t i = 0; i < excess && !frame_queue_yuv[0].empty(); ++i) {
            free_cloned_frame(&frame_queue_yuv[0].front().frame);
            frame_queue_yuv[0].pop_front();
            balanced++;
        }
        if (balanced > 0) {
            std::cout << "[BALANCE] Dropped " << balanced << " frames from queue 0 (size was "
                << queue0_size << " vs " << queue1_size << ")" << std::endl;
        }
    }
    else if (queue1_size > queue0_size + BALANCE_THRESHOLD) {
        // 队列1太多，丢弃多余的帧
        size_t excess = queue1_size - queue0_size - BALANCE_THRESHOLD;
        for (size_t i = 0; i < excess && !frame_queue_yuv[1].empty(); ++i) {
            free_cloned_frame(&frame_queue_yuv[1].front().frame);
            frame_queue_yuv[1].pop_front();
            balanced++;
        }
        if (balanced > 0) {
            std::cout << "[BALANCE] Dropped " << balanced << " frames from queue 1 (size was "
                << queue1_size << " vs " << queue0_size << ")" << std::endl;
        }
    }

    return balanced;
}

// 新增：紧急内存清理
size_t DualCameraCapture::emergency_memory_cleanup() {
    if (!mutex_.try_lock()) {
        std::cout << "[EMERGENCY] Could not acquire lock, skipping cleanup" << std::endl;
        return 0;
    }
    std::lock_guard<std::mutex> lock(mutex_, std::adopt_lock);
    size_t total_cleared = 0;

    size_t before_active = g_active_frame_count.load();
    std::cout << "[EMERGENCY] Starting emergency cleanup - Active frames: " << before_active
        << ", Raw queues: " << frame_queue_yuv[0].size() << "/" << frame_queue_yuv[1].size()
        << ", Sync queue: " << synced_yuv_queue_.size() << std::endl;

    // 1. 清理原始队列，只保留最新的几帧
    for (int i = 0; i < 2; ++i) {
        size_t original_size = frame_queue_yuv[i].size();

        // 只保留最新的2帧
        while (frame_queue_yuv[i].size() > 2) {
            free_cloned_frame(&frame_queue_yuv[i].front().frame);
            frame_queue_yuv[i].pop_front();
            total_cleared++;
        }

        if (original_size > 2) {
            std::cout << "[EMERGENCY] Queue " << i << ": " << original_size << " -> " << frame_queue_yuv[i].size() << std::endl;
        }
    }

    // 2. 清理同步队列，只保留1个帧对
    size_t original_sync_size = synced_yuv_queue_.size();
    while (synced_yuv_queue_.size() > 1) {
        auto& p = synced_yuv_queue_.front();
        free_cloned_frame(&p.first);
        free_cloned_frame(&p.second);
        synced_yuv_queue_.pop_front();
        total_cleared += 2;
    }

    if (original_sync_size > 1) {
        std::cout << "[EMERGENCY] Sync queue: " << original_sync_size << " -> " << synced_yuv_queue_.size() << std::endl;
    }

    size_t after_active = g_active_frame_count.load();
    std::cout << "[EMERGENCY] Cleanup completed - Cleared " << total_cleared
        << " frames from queues, Active frames: " << before_active << " -> " << after_active
        << " (difference may indicate frames in transit)" << std::endl;

    return total_cleared;
}

size_t DualCameraCapture::force_clear_queues() {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t total_cleared = 0;

    // 清理原始队列
    for (int i = 0; i < 2; ++i) {
        size_t queue_size = frame_queue_yuv[i].size();
        while (!frame_queue_yuv[i].empty()) {
            free_cloned_frame(&frame_queue_yuv[i].front().frame);
            frame_queue_yuv[i].pop_front();
            total_cleared++;
        }
        if (queue_size > 0) {
            std::cout << "[FORCE_CLEAR] Cleared " << queue_size << " frames from raw queue " << i << std::endl;
        }
    }

    // 清理同步队列
    size_t sync_queue_size = synced_yuv_queue_.size();
    while (!synced_yuv_queue_.empty()) {
        auto& p = synced_yuv_queue_.front();
        free_cloned_frame(&p.first);
        free_cloned_frame(&p.second);
        synced_yuv_queue_.pop_front();
        total_cleared += 2;
    }
    if (sync_queue_size > 0) {
        std::cout << "[FORCE_CLEAR] Cleared " << sync_queue_size << " frame pairs from sync queue" << std::endl;
    }

    std::cout << "[FORCE_CLEAR] Total frames cleared: " << total_cleared << std::endl;
    return total_cleared;
}

size_t DualCameraCapture::clear_sync_queue_partial(size_t max_to_clear) {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t cleared = 0;

    while (!synced_yuv_queue_.empty() && cleared < max_to_clear) {
        auto& p = synced_yuv_queue_.front();
        free_cloned_frame(&p.first);
        free_cloned_frame(&p.second);
        synced_yuv_queue_.pop_front();
        cleared += 2;
    }

    return cleared;
}

MemoryStats DualCameraCapture::get_memory_stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    MemoryStats stats;
    stats.allocated_frames = g_total_frames_allocated.load();
    stats.freed_frames = g_total_frames_freed.load();
    stats.active_frames = g_active_frame_count.load();
    stats.raw_queue_size[0] = frame_queue_yuv[0].size();
    stats.raw_queue_size[1] = frame_queue_yuv[1].size();
    stats.sync_queue_size = synced_yuv_queue_.size();
    return stats;
}

bool DualCameraCapture::init(const std::vector<std::string>& device_paths) {
    if (initialized_) {
        std::cout << "[DualCameraCapture] Already initialized, performing reset first..." << std::endl;
        full_reset();
    }

    if (device_paths.size() != 2) {
        std::cerr << "Need exactly 2 camera devices" << std::endl;
        return false;
    }

    std::cout << "[DualCameraCapture] Initializing cameras..." << std::endl;
    cameras_.resize(2);

    for (int i = 0; i < 2; ++i) {
        if (!init_camera(i, device_paths[i])) {
            std::cerr << "[DualCameraCapture] Failed to initialize camera " << i << std::endl;
            // 清理已初始化的摄像头
            for (int j = 0; j < i; ++j) {
                cleanup_camera(j);
            }
            cameras_.clear();
            return false;
        }
    }

    initialized_ = true;
    std::cout << "[DualCameraCapture] All cameras initialized successfully" << std::endl;
    return true;
}

bool DualCameraCapture::init_camera(int index, const std::string& device_path) {
    auto& cam = cameras_[index];

    // 打开视频设备
    const AVInputFormat* input_format = av_find_input_format("dshow");
    AVDictionary* options = nullptr;

    // 进一步优化缓冲区设置
    av_dict_set(&options, "video_size", "640x480", 0);
    av_dict_set(&options, "framerate", "30", 0);
    // 进一步减少rtbufsize到1MB
    av_dict_set(&options, "rtbufsize", "1048576", 0);  // 1MB
    av_dict_set(&options, "probesize", "32", 0);
    av_dict_set(&options, "analyzeduration", "50000", 0);  // 50ms
    av_dict_set(&options, "fflags", "nobuffer", 0);
    av_dict_set(&options, "flags", "low_delay", 0);

    // 严格限制队列大小
    av_dict_set(&options, "thread_queue_size", "1", 0);
    av_dict_set(&options, "buffer_size", "262144", 0);   // 256KB输入缓冲区

    int ret = avformat_open_input(&cam.fmt_ctx, device_path.c_str(), input_format, &options);
    av_dict_free(&options);

    if (ret != 0) {
        char error_buf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, error_buf, sizeof(error_buf));
        std::cerr << "Could not open camera " << index << " at " << device_path
            << ": " << error_buf << std::endl;
        return false;
    }

    // 限制stream info查找时间
    AVDictionary* find_stream_options = nullptr;
    av_dict_set(&find_stream_options, "analyzeduration", "50000", 0);  // 50ms
    av_dict_set(&find_stream_options, "probesize", "32", 0);

    if (avformat_find_stream_info(cam.fmt_ctx, &find_stream_options) < 0) {
        std::cerr << "Could not find stream info for camera " << index << std::endl;
        av_dict_free(&find_stream_options);
        return false;
    }
    av_dict_free(&find_stream_options);

    // 查找视频流
    cam.video_stream_idx = -1;
    for (unsigned int i = 0; i < cam.fmt_ctx->nb_streams; i++) {
        if (cam.fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            cam.video_stream_idx = i;
            break;
        }
    }

    if (cam.video_stream_idx == -1) {
        std::cerr << "Could not find video stream in camera " << index << std::endl;
        return false;
    }

    // 获取解码器
    AVCodecParameters* codecpar = cam.fmt_ctx->streams[cam.video_stream_idx]->codecpar;

    std::cout << "Camera " << index << " - Format: "
        << av_get_pix_fmt_name((AVPixelFormat)codecpar->format)
        << ", Size: " << codecpar->width << "x" << codecpar->height
        << ", Color range: " << codecpar->color_range << std::endl;

    const AVCodec* codec = avcodec_find_decoder(codecpar->codec_id);
    if (!codec) {
        std::cerr << "Unsupported codec for camera " << index << std::endl;
        return false;
    }

    cam.codec_ctx = avcodec_alloc_context3(codec);
    if (!cam.codec_ctx) {
        std::cerr << "Could not allocate codec context for camera " << index << std::endl;
        return false;
    }

    if (avcodec_parameters_to_context(cam.codec_ctx, codecpar) < 0) {
        std::cerr << "Could not copy codec context for camera " << index << std::endl;
        return false;
    }

    // 设置低延迟选项和限制缓冲
    av_opt_set(cam.codec_ctx->priv_data, "preset", "ultrafast", 0);
    av_opt_set(cam.codec_ctx->priv_data, "tune", "zerolatency", 0);
    // 严格限制解码器内部缓冲
    cam.codec_ctx->thread_count = 1;
    cam.codec_ctx->delay = 0;

    if (avcodec_open2(cam.codec_ctx, codec, nullptr) < 0) {
        std::cerr << "Could not open codec for camera " << index << std::endl;
        return false;
    }

    // 分配帧缓冲区
    cam.frame = av_frame_alloc();
    if (!cam.frame) {
        std::cerr << "Could not allocate frame for camera " << index << std::endl;
        return false;
    }

    // 分配 YUV420P 帧缓冲区
    cam.yuv_frame = av_frame_alloc();
    if (!cam.yuv_frame) {
        std::cerr << "Could not allocate yuv frame for camera " << index << std::endl;
        return false;
    }

    // 计算所需缓冲区大小
    int yuv_size = av_image_get_buffer_size(AV_PIX_FMT_YUV420P, cam.codec_ctx->width, cam.codec_ctx->height, 32);
    cam.yuv_buffer = (uint8_t*)av_malloc(yuv_size);
    if (!cam.yuv_buffer) {
        std::cerr << "Could not allocate yuv buffer for camera " << index << std::endl;
        return false;
    }

    // 将 buffer 绑定到帧
    av_image_fill_arrays(cam.yuv_frame->data, cam.yuv_frame->linesize, cam.yuv_buffer,
        AV_PIX_FMT_YUV420P, cam.codec_ctx->width, cam.codec_ctx->height, 32);

    // 创建 SwsContext
    cam.sws_ctx_yuv = sws_getContext(
        cam.codec_ctx->width, cam.codec_ctx->height, cam.codec_ctx->pix_fmt,
        cam.codec_ctx->width, cam.codec_ctx->height, AV_PIX_FMT_YUV420P,
        SWS_BILINEAR, nullptr, nullptr, nullptr);

    if (!cam.sws_ctx_yuv) {
        std::cerr << "Could not create sws context for camera " << index << std::endl;
        return false;
    }

    std::cout << "Camera " << index << " initialized successfully (YUV buffer size: "
        << yuv_size << " bytes)" << std::endl;
    return true;
}

void DualCameraCapture::start() {
    if (!initialized_) {
        std::cerr << "[DualCameraCapture] Cannot start - not initialized" << std::endl;
        return;
    }

    if (running_.load()) {
        std::cout << "[DualCameraCapture] Already running" << std::endl;
        return;
    }

    std::cout << "[DualCameraCapture] Starting capture threads..." << std::endl;
    running_ = true;
    capture_active_ = true;

    // 启动捕获线程
    for (int i = 0; i < 2; ++i) {
        threads_.emplace_back(&DualCameraCapture::capture_thread, this, i);
    }

    // 启动同步线程
    sync_thread_ = std::thread(&DualCameraCapture::sync_loop, this);

    std::cout << "[DualCameraCapture] All threads started" << std::endl;
}

void DualCameraCapture::stop() {
    if (!running_.load()) {
        return;
    }

    std::cout << "[DualCameraCapture] Stopping capture..." << std::endl;
    running_ = false;
    capture_active_ = false;

    // 等待所有线程结束
    for (auto& t : threads_) {
        if (t.joinable()) t.join();
    }
    if (sync_thread_.joinable()) sync_thread_.join();
    threads_.clear();

    // 清理所有队列
    size_t cleared = force_clear_queues();
    std::cout << "[DualCameraCapture] All threads stopped, cleared " << cleared << " remaining frames" << std::endl;
}

void DualCameraCapture::pause_capture() {
    std::cout << "[DualCameraCapture] Pausing capture..." << std::endl;
    capture_active_ = false;
}

void DualCameraCapture::resume_capture() {
    std::cout << "[DualCameraCapture] Resuming capture..." << std::endl;
    capture_active_ = true;
}

void DualCameraCapture::capture_thread(int index) {
    if (index >= static_cast<int>(cameras_.size())) {
        std::cerr << "[Capture " << index << "] Invalid camera index" << std::endl;
        return;
    }

    auto& cam = cameras_[index];
    AVPacket* packet = av_packet_alloc();
    if (!packet) {
        std::cerr << "Could not allocate packet for camera " << index << std::endl;
        return;
    }

    int consecutive_failures = 0;
    const int MAX_CONSECUTIVE_FAILURES = 10;
    size_t frames_captured = 0;
    size_t frames_dropped = 0;
    size_t frames_paused = 0;
    size_t frames_discarded = 0;
    size_t frames_allocated = 0;  // 新增：此线程分配的帧数
    auto start_time = std::chrono::steady_clock::now();
    auto last_stats_time = start_time;
    auto last_flush_time = start_time;
    auto last_frame_rate_update = start_time;

    std::cout << "[Capture " << index << "] Thread started" << std::endl;

    while (running_) {
        // 如果capture暂停，休眠并继续
        if (!capture_active_.load()) {
            frames_paused++;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        auto current_time = std::chrono::steady_clock::now();

        // 定期冲刷解码器缓冲区，防止积压
        if (std::chrono::duration_cast<std::chrono::milliseconds>(current_time - last_flush_time).count() >= 200) {
            avcodec_flush_buffers(cam.codec_ctx);
            last_flush_time = current_time;
        }

        // 更新帧率统计
        if (std::chrono::duration_cast<std::chrono::seconds>(current_time - last_frame_rate_update).count() >= 1) {
            g_camera_frame_rates[index] = frames_captured;
            last_frame_rate_update = current_time;
        }

        int ret = av_read_frame(cam.fmt_ctx, packet);
        if (ret < 0) {
            consecutive_failures++;
            if (ret == AVERROR(EAGAIN)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            else if (consecutive_failures > MAX_CONSECUTIVE_FAILURES) {
                std::cerr << "[Capture " << index << "] Too many consecutive failures ("
                    << consecutive_failures << "), stopping thread" << std::endl;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        consecutive_failures = 0;

        if (packet->stream_index != cam.video_stream_idx) {
            av_packet_unref(packet);
            continue;
        }

        // 检查是否有消费者，以及内存压力
        bool has_consumers = false;
        bool memory_pressure = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            has_consumers = (synced_yuv_queue_.size() < MAX_SYNC_QUEUE_SIZE / 2);

            // 新增：检查内存压力
            size_t active_frames = g_active_frame_count.load();
            memory_pressure = (active_frames > 3000);  // 如果活跃帧数超过500，认为有内存压力
        }

        if (!has_consumers || memory_pressure) {
            frames_discarded++;
            av_packet_unref(packet);

            if (frames_discarded % 1000 == 0) {
                std::string reason = memory_pressure ? "memory pressure" : "no consumers";
                std::cout << "[Capture " << index << "] Discarded " << frames_discarded
                    << " packets (" << reason << "), active frames: " << g_active_frame_count.load() << std::endl;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }

        ret = avcodec_send_packet(cam.codec_ctx, packet);
        av_packet_unref(packet);

        if (ret < 0) {
            if (ret == AVERROR(EAGAIN)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            continue;
        }

        ret = avcodec_receive_frame(cam.codec_ctx, cam.frame);
        if (ret < 0) {
            if (ret == AVERROR(EAGAIN)) {
                continue;
            }
            else {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
        }

        int64_t pts = av_gettime();
        frames_captured++;

        {
            std::lock_guard<std::mutex> lock(mutex_);

            // 检查队列大小，更严格的限制
            size_t queue_size = frame_queue_yuv[index].size();
            if (queue_size >= 10) {  // 从30减少到10
                frames_dropped++;
                // 释放最旧的帧
                free_cloned_frame(&frame_queue_yuv[index].front().frame);
                frame_queue_yuv[index].pop_front();
            }

            // YUV420P 转换
            if (cam.sws_ctx_yuv) {
                sws_scale(cam.sws_ctx_yuv,
                    (const uint8_t* const*)cam.frame->data, cam.frame->linesize,
                    0, cam.frame->height,
                    cam.yuv_frame->data, cam.yuv_frame->linesize);

                cam.yuv_frame->width = cam.frame->width;
                cam.yuv_frame->height = cam.frame->height;
                cam.yuv_frame->format = AV_PIX_FMT_YUV420P;

                AVFrame* cloned_yuv = clone_frame(cam.yuv_frame);
                if (cloned_yuv) {
                    frame_queue_yuv[index].emplace_back(TimestampedFrame{ cloned_yuv, pts });

                    size_t allocated = g_total_frames_allocated.fetch_add(1) + 1;
                    size_t active = g_active_frame_count.fetch_add(1) + 1;
                    frames_allocated++;

                    // 每分配1000帧打印一次统计
                    if (allocated % 1000 == 0) {
                        std::cout << "[MEMORY] Thread " << index << " allocated " << allocated
                            << " frames, active: " << active << std::endl;
                    }
                }
                else {
                    std::cerr << "[Capture " << index << "] Failed to clone YUV frame" << std::endl;
                }
            }
        }

        // 每30秒打印统计
        if (std::chrono::duration_cast<std::chrono::seconds>(current_time - last_stats_time).count() >= 30) {
            std::lock_guard<std::mutex> lock(mutex_);
            size_t other_camera_frames = (index == 0) ? g_camera_frame_rates[1].load() : g_camera_frame_rates[0].load();

            std::cout << "[Capture " << index << "] Stats - Captured: " << frames_captured
                << ", Dropped: " << frames_dropped
                << ", Discarded: " << frames_discarded
                << ", Allocated: " << frames_allocated
                << ", Paused cycles: " << frames_paused
                << ", Raw queue size: " << frame_queue_yuv[index].size() << "/10"
                << ", Other camera rate: " << other_camera_frames
                << ", Active frames: " << g_active_frame_count.load() << std::endl;
            last_stats_time = current_time;
        }

        // 根据是否有消费者和内存压力动态调整睡眠时间
        if (has_consumers && !memory_pressure) {
            std::this_thread::sleep_for(std::chrono::microseconds(500));
        }
        else {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    av_packet_free(&packet);

    auto total_time = std::chrono::duration_cast<std::chrono::seconds>
        (std::chrono::steady_clock::now() - start_time).count();
    std::cout << "[Capture " << index << "] Thread exited - Total captured: " << frames_captured
        << ", Dropped: " << frames_dropped
        << ", Discarded: " << frames_discarded
        << ", Allocated: " << frames_allocated
        << ", Paused cycles: " << frames_paused
        << ", Average FPS: " << (total_time > 0 ? frames_captured / total_time : 0) << std::endl;
}

void DualCameraCapture::sync_loop() {
    size_t synced_pairs = 0;
    size_t dropped_frames = 0;
    size_t balanced_frames = 0;  // 新增：平衡丢弃的帧数
    auto start_time = std::chrono::steady_clock::now();
    auto last_stats_time = start_time;
    auto last_cleanup_time = start_time;
    auto last_balance_time = start_time;

    std::cout << "[Sync] Thread started" << std::endl;

    while (running_) {
        TimestampedFrame y0, y1;
        bool found_sync_pair = false;
        auto current_time = std::chrono::steady_clock::now();

        // 新增：每5秒进行一次帧平衡
        if (std::chrono::duration_cast<std::chrono::seconds>(current_time - last_balance_time).count() >= 5) {
            size_t balanced = balance_frame_queues();
            balanced_frames += balanced;
            last_balance_time = current_time;
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);

            // 检查原始队列是否有帧
            if (frame_queue_yuv[0].empty() || frame_queue_yuv[1].empty()) {
                // 队列为空，继续等待
            }
            else {
                // 获取最前面的帧
                y0 = frame_queue_yuv[0].front();
                y1 = frame_queue_yuv[1].front();

                int64_t dy = std::abs(y0.timestamp_us - y1.timestamp_us);
                if (dy <= SYNC_THRESHOLD_US) {
                    // 找到同步帧对
                    found_sync_pair = true;

                    // 从原始队列移除
                    frame_queue_yuv[0].pop_front();
                    frame_queue_yuv[1].pop_front();

                    // 控制同步队列大小，更严格的限制
                    if (synced_yuv_queue_.size() >= 2) {  // 从5减少到2
                        auto& old_pair = synced_yuv_queue_.front();
                        free_cloned_frame(&old_pair.first);
                        free_cloned_frame(&old_pair.second);
                        synced_yuv_queue_.pop_front();
                        dropped_frames += 2;
                    }

                    // 转移帧到同步队列
                    synced_yuv_queue_.emplace_back(y0.frame, y1.frame);
                    synced_pairs++;
                }
                else {
                    // 丢弃时间戳较早的那一帧
                    int idx = (y0.timestamp_us < y1.timestamp_us) ? 0 : 1;
                    free_cloned_frame(&frame_queue_yuv[idx].front().frame);
                    frame_queue_yuv[idx].pop_front();
                    dropped_frames++;
                }
            }
        }

        // 更频繁的清理，每1秒进行一次（从2秒改为1秒）
        if (std::chrono::duration_cast<std::chrono::seconds>(current_time - last_cleanup_time).count() >= 1) {
            std::lock_guard<std::mutex> lock(mutex_);
            size_t total_queue_size = synced_yuv_queue_.size() + frame_queue_yuv[0].size() + frame_queue_yuv[1].size();

            if (total_queue_size > 2) {  // 进一步降低阈值到2
                size_t before_sync = synced_yuv_queue_.size();
                while (synced_yuv_queue_.size() > 1) {
                    auto& old_pair = synced_yuv_queue_.front();
                    free_cloned_frame(&old_pair.first);
                    free_cloned_frame(&old_pair.second);
                    synced_yuv_queue_.pop_front();
                    dropped_frames += 2;
                }

                size_t cleared_sync = before_sync - synced_yuv_queue_.size();
                if (cleared_sync > 0) {
                    std::cout << "[Sync] Proactive cleanup: cleared " << cleared_sync
                        << " sync pairs, total dropped: " << dropped_frames << std::endl;
                }
            }
            last_cleanup_time = current_time;
        }

        // 每30秒打印统计
        if (std::chrono::duration_cast<std::chrono::seconds>(current_time - last_stats_time).count() >= 30) {
            std::lock_guard<std::mutex> lock(mutex_);
            size_t cam0_rate = g_camera_frame_rates[0].load();
            size_t cam1_rate = g_camera_frame_rates[1].load();

            std::cout << "[Sync] Stats - Synced pairs: " << synced_pairs
                << ", Dropped frames: " << dropped_frames
                << ", Balanced frames: " << balanced_frames
                << ", Sync queue size: " << synced_yuv_queue_.size() << "/2"
                << ", Raw queues: " << frame_queue_yuv[0].size() << "/" << frame_queue_yuv[1].size()
                << ", Camera rates: " << cam0_rate << "/" << cam1_rate
                << ", Active frames: " << g_active_frame_count.load() << std::endl;
            last_stats_time = current_time;
        }

        // 根据是否找到同步帧调整睡眠时间
        if (found_sync_pair) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        else {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    auto total_time = std::chrono::duration_cast<std::chrono::seconds>
        (std::chrono::steady_clock::now() - start_time).count();
    std::cout << "[Sync] Thread exited - Total synced pairs: " << synced_pairs
        << ", Dropped: " << dropped_frames
        << ", Balanced: " << balanced_frames
        << ", Average pairs/sec: " << (total_time > 0 ? synced_pairs / total_time : 0) << std::endl;
}

std::vector<AVFrame*> DualCameraCapture::get_sync_yuv420p_frames() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (synced_yuv_queue_.empty()) return {};

    auto p = synced_yuv_queue_.front();
    synced_yuv_queue_.pop_front();

    return { p.first, p.second };
}

size_t DualCameraCapture::get_sync_queue_size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return synced_yuv_queue_.size();
}

AVFrame* DualCameraCapture::clone_frame(const AVFrame* src) const {
    if (!src) {
        return nullptr;
    }

    AVFrame* dst = av_frame_alloc();
    if (!dst) {
        return nullptr;
    }

    dst->format = src->format;
    dst->width = src->width;
    dst->height = src->height;

    int ret = av_image_alloc(dst->data, dst->linesize,
        dst->width, dst->height,
        (AVPixelFormat)dst->format, 32);
    if (ret < 0) {
        av_frame_free(&dst);
        return nullptr;
    }

    av_image_copy(dst->data, dst->linesize,
        (const uint8_t* const*)src->data, src->linesize,
        (AVPixelFormat)dst->format,
        dst->width, dst->height);
    return dst;
}

// 新增：统一的帧释放接口，确保计数器正确更新
void DualCameraCapture::release_frame(AVFrame** frame) {
    if (frame && *frame) {
        // 检查这是否是我们clone的帧（有我们分配的data[0]）
        if ((*frame)->data[0]) {
            // 这是我们clone的帧，需要更新计数器
            av_freep(&(*frame)->data[0]);
            av_frame_free(frame);

            size_t freed_count = g_total_frames_freed.fetch_add(1) + 1;
            size_t active_count = g_active_frame_count.fetch_sub(1) - 1;

            // 每释放1000帧打印一次统计
            if (freed_count % 1000 == 0) {
                std::cout << "[RELEASE] Released " << freed_count << " frames, active: " << active_count << std::endl;
            }
        }
        else {
            // 这不是我们clone的帧，直接释放（不应该发生，但为了安全）
            av_frame_free(frame);
            std::cout << "[WARNING] Released non-cloned frame" << std::endl;
        }
    }
}
// sync_camera.cpp - 自适应双/三摄像头同步实现
#include "../includes/sync_camera.h"
#include <iostream>
#include <stdexcept>
#include <atomic>
#include <algorithm>
#include <chrono>

// 全局统计变量
std::atomic<size_t> g_total_frames_allocated{ 0 };
std::atomic<size_t> g_total_frames_freed{ 0 };
std::atomic<size_t> g_active_frame_count{ 0 };

//==============================================================================
// DualCameraCapture 实现（原双摄像头逻辑，优化到30fps）
//==============================================================================

DualCameraCapture::DualCameraCapture() {
    avdevice_register_all();
}

DualCameraCapture::~DualCameraCapture() {
    full_reset();

    size_t leaked = g_active_frame_count.load();
    if (leaked > 0) {
        std::cout << "[WARNING] Memory leak: " << leaked << " frames still active" << std::endl;
    }
}

void DualCameraCapture::full_reset() {
    stop();
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    size_t total_freed = 0;

    // 清理摄像头资源
    for (int i = 0; i < static_cast<int>(cameras_.size()); ++i) {
        cleanup_camera(i);
    }
    cameras_.clear();

    // 清理双摄像头队列
    for (int i = 0; i < 2; ++i) {
        std::lock_guard<std::mutex> lock(mutex_);
        while (!frame_queue_yuv[i].empty()) {
            free_cloned_frame(&frame_queue_yuv[i].front().frame);
            frame_queue_yuv[i].pop_front();
            total_freed++;
        }
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        while (!synced_yuv_queue_.empty()) {
            auto& p = synced_yuv_queue_.front();
            free_cloned_frame(&p.first);
            free_cloned_frame(&p.second);
            synced_yuv_queue_.pop_front();
            total_freed += 2;
        }
    }

    initialized_ = false;
    running_ = false;
    capture_active_ = false;

    if (total_freed > 0) {
        std::cout << "[DUAL RESET] Freed " << total_freed << " frames" << std::endl;
    }
}

void DualCameraCapture::cleanup_camera(int index) {
    if (index >= static_cast<int>(cameras_.size())) return;

    auto& cam = cameras_[index];

    if (cam.sws_ctx_yuv) {
        sws_freeContext(cam.sws_ctx_yuv);
        cam.sws_ctx_yuv = nullptr;
    }

    if (cam.yuv_frame) {
        av_frame_free(&cam.yuv_frame);
        cam.yuv_frame = nullptr;
    }
    if (cam.frame) {
        av_frame_free(&cam.frame);
        cam.frame = nullptr;
    }

    if (cam.yuv_buffer) {
        av_freep(&cam.yuv_buffer);
        cam.yuv_buffer = nullptr;
    }

    if (cam.codec_ctx) {
        avcodec_free_context(&cam.codec_ctx);
        cam.codec_ctx = nullptr;
    }
    if (cam.fmt_ctx) {
        avformat_close_input(&cam.fmt_ctx);
        cam.fmt_ctx = nullptr;
    }

    cam.video_stream_idx = -1;
}

void DualCameraCapture::free_cloned_frame(AVFrame** frame) {
    if (frame && *frame) {
        if ((*frame)->data[0]) {
            av_freep(&(*frame)->data[0]);
        }
        av_frame_free(frame);

        g_total_frames_freed.fetch_add(1);
        g_active_frame_count.fetch_sub(1);
    }
}

bool DualCameraCapture::init(const std::vector<std::string>& device_paths) {
    if (initialized_) {
        full_reset();
    }

    if (device_paths.size() != 2) {
        std::cerr << "[DUAL ERROR] Need exactly 2 camera devices" << std::endl;
        return false;
    }

    std::cout << "[DUAL] Initializing cameras..." << std::endl;
    cameras_.resize(2);

    for (int i = 0; i < 2; ++i) {
        if (!init_camera(i, device_paths[i])) {
            std::cerr << "[DUAL ERROR] Failed to initialize camera " << i << std::endl;
            for (int j = 0; j < i; ++j) {
                cleanup_camera(j);
            }
            cameras_.clear();
            return false;
        }
    }

    initialized_ = true;
    std::cout << "[DUAL] All cameras initialized - optimized for 30fps" << std::endl;
    return true;
}

bool DualCameraCapture::init_camera(int index, const std::string& device_path) {
    auto& cam = cameras_[index];

    // 打开视频设备
    const AVInputFormat* input_format = av_find_input_format("dshow");
    AVDictionary* options = nullptr;

    // 双摄像头模式：使用MJPEG格式，优化到30fps
    av_dict_set(&options, "video_size", "640x480", 0);
    av_dict_set(&options, "framerate", "30", 0);
    av_dict_set(&options, "vcodec", "mjpeg", 0);  // MJPEG带宽低
    av_dict_set(&options, "rtbufsize", "2097152", 0);  // 2MB缓冲
    av_dict_set(&options, "probesize", "32", 0);
    av_dict_set(&options, "analyzeduration", "50000", 0);
    av_dict_set(&options, "fflags", "nobuffer", 0);
    av_dict_set(&options, "flags", "low_delay", 0);
    av_dict_set(&options, "thread_queue_size", "2", 0);
    av_dict_set(&options, "buffer_size", "524288", 0);

    int ret = avformat_open_input(&cam.fmt_ctx, device_path.c_str(), input_format, &options);
    av_dict_free(&options);

    if (ret != 0) {
        std::cerr << "[DUAL ERROR] Could not open camera " << index << " at " << device_path << std::endl;
        return false;
    }

    // 限制stream info查找时间
    AVDictionary* find_stream_options = nullptr;
    av_dict_set(&find_stream_options, "analyzeduration", "50000", 0);
    av_dict_set(&find_stream_options, "probesize", "32", 0);

    if (avformat_find_stream_info(cam.fmt_ctx, &find_stream_options) < 0) {
        std::cerr << "[DUAL ERROR] Could not find stream info for camera " << index << std::endl;
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
        std::cerr << "[DUAL ERROR] Could not find video stream in camera " << index << std::endl;
        return false;
    }

    AVStream* vs = cam.fmt_ctx->streams[cam.video_stream_idx];
    std::cout << "[DUAL DEBUG] Camera " << index << " codec_id=" << vs->codecpar->codec_id
        << " format=" << vs->codecpar->format << std::endl;

    // 获取解码器
    AVCodecParameters* codecpar = cam.fmt_ctx->streams[cam.video_stream_idx]->codecpar;
    const AVCodec* codec = avcodec_find_decoder(codecpar->codec_id);
    if (!codec) {
        std::cerr << "[DUAL ERROR] Unsupported codec for camera " << index << std::endl;
        return false;
    }

    cam.codec_ctx = avcodec_alloc_context3(codec);
    if (!cam.codec_ctx) {
        std::cerr << "[DUAL ERROR] Could not allocate codec context for camera " << index << std::endl;
        return false;
    }

    if (avcodec_parameters_to_context(cam.codec_ctx, codecpar) < 0) {
        std::cerr << "[DUAL ERROR] Could not copy codec context for camera " << index << std::endl;
        return false;
    }

    // MJPEG解码器优化设置
    if (codecpar->codec_id == AV_CODEC_ID_MJPEG) {
        av_opt_set(cam.codec_ctx->priv_data, "lowres", "0", 0);
        cam.codec_ctx->thread_count = 2;  // MJPEG可以多线程
    }
    else {
        av_opt_set(cam.codec_ctx->priv_data, "preset", "ultrafast", 0);
        av_opt_set(cam.codec_ctx->priv_data, "tune", "zerolatency", 0);
        cam.codec_ctx->thread_count = 1;
    }

    cam.codec_ctx->delay = 0;

    if (avcodec_open2(cam.codec_ctx, codec, nullptr) < 0) {
        std::cerr << "[DUAL ERROR] Could not open codec for camera " << index << std::endl;
        return false;
    }

    // 分配帧缓冲区
    cam.frame = av_frame_alloc();
    cam.yuv_frame = av_frame_alloc();
    if (!cam.frame || !cam.yuv_frame) {
        std::cerr << "[DUAL ERROR] Could not allocate frames for camera " << index << std::endl;
        return false;
    }

    // 计算并分配YUV缓冲区
    int yuv_size = av_image_get_buffer_size(AV_PIX_FMT_YUV420P, cam.codec_ctx->width, cam.codec_ctx->height, 32);
    cam.yuv_buffer = (uint8_t*)av_malloc(yuv_size);
    if (!cam.yuv_buffer) {
        std::cerr << "[DUAL ERROR] Could not allocate yuv buffer for camera " << index << std::endl;
        return false;
    }

    // 绑定buffer到帧
    av_image_fill_arrays(cam.yuv_frame->data, cam.yuv_frame->linesize, cam.yuv_buffer,
        AV_PIX_FMT_YUV420P, cam.codec_ctx->width, cam.codec_ctx->height, 32);

    // 创建SwsContext
    enum AVPixelFormat src_pix_fmt = cam.codec_ctx->pix_fmt;
    if (src_pix_fmt == AV_PIX_FMT_NONE) {
        src_pix_fmt = AV_PIX_FMT_YUVJ420P;
    }

    cam.sws_ctx_yuv = sws_getContext(
        cam.codec_ctx->width, cam.codec_ctx->height, src_pix_fmt,
        cam.codec_ctx->width, cam.codec_ctx->height, AV_PIX_FMT_YUV420P,
        SWS_BILINEAR, nullptr, nullptr, nullptr);

    if (!cam.sws_ctx_yuv) {
        std::cerr << "[DUAL ERROR] Could not create sws context for camera " << index << std::endl;
        return false;
    }

    std::cout << "[DUAL] Camera " << index << " initialized ("
        << cam.codec_ctx->width << "x" << cam.codec_ctx->height
        << ", codec=" << avcodec_get_name(codecpar->codec_id) << ")" << std::endl;
    return true;
}

void DualCameraCapture::start() {
    if (!initialized_) {
        std::cerr << "[DUAL ERROR] Cannot start - not initialized" << std::endl;
        return;
    }

    if (running_.load()) {
        return;
    }

    std::cout << "[DUAL] Starting capture threads..." << std::endl;
    running_ = true;
    capture_active_ = true;

    // 启动两个捕获线程
    for (int i = 0; i < 2; ++i) {
        threads_.emplace_back(&DualCameraCapture::capture_thread, this, i);
    }

    // 启动双摄像头同步线程
    sync_thread_ = std::thread(&DualCameraCapture::sync_loop, this);

    std::cout << "[DUAL] All threads started" << std::endl;
}

void DualCameraCapture::stop() {
    if (!running_.load()) {
        return;
    }

    std::cout << "[DUAL] Stopping capture..." << std::endl;
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
    if (cleared > 0) {
        std::cout << "[DUAL] Cleared " << cleared << " remaining frames" << std::endl;
    }
}

void DualCameraCapture::pause_capture() {
    capture_active_ = false;
}

void DualCameraCapture::resume_capture() {
    capture_active_ = true;
}

void DualCameraCapture::capture_thread(int index) {
    if (index >= static_cast<int>(cameras_.size())) {
        std::cerr << "[DUAL ERROR] Invalid camera index " << index << std::endl;
        return;
    }

    auto& cam = cameras_[index];
    AVPacket* packet = av_packet_alloc();
    if (!packet) {
        std::cerr << "[DUAL ERROR] Could not allocate packet for camera " << index << std::endl;
        return;
    }

    int consecutive_failures = 0;
    const int MAX_CONSECUTIVE_FAILURES = 10;
    size_t frames_captured = 0;
    size_t frames_dropped = 0;
    auto start_time = std::chrono::steady_clock::now();
    auto last_stats_time = std::chrono::steady_clock::now();
    size_t last_frame_count_60s = 0;
    auto last_flush_time = start_time;

    while (running_) {
        if (!capture_active_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        auto current_time = std::chrono::steady_clock::now();

        // 定期冲刷解码器缓冲区
        if (std::chrono::duration_cast<std::chrono::milliseconds>(current_time - last_flush_time).count() >= 200) {
            avcodec_flush_buffers(cam.codec_ctx);
            last_flush_time = current_time;
        }

        int ret = av_read_frame(cam.fmt_ctx, packet);
        if (ret < 0) {
            consecutive_failures++;
            if (ret == AVERROR(EAGAIN)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            else if (consecutive_failures > MAX_CONSECUTIVE_FAILURES) {
                std::cerr << "[DUAL ERROR] Camera " << index << " too many failures, stopping" << std::endl;
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

        // 检查内存压力
        bool memory_pressure = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            size_t active_frames = g_active_frame_count.load();
            memory_pressure = (active_frames > 3000);
        }

        if (memory_pressure) {
            frames_dropped++;
            av_packet_unref(packet);
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

            // 限制队列大小
            size_t queue_size = frame_queue_yuv[index].size();
            if (queue_size >= 10) {
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

                    g_total_frames_allocated.fetch_add(1);
                    g_active_frame_count.fetch_add(1);
                }
            }
        }

        // 60秒统计
        auto elapsed_sec = std::chrono::duration_cast<std::chrono::seconds>(current_time - last_stats_time).count();
        if (elapsed_sec >= 60) {
            size_t frames_60s = frames_captured - last_frame_count_60s;
            double avg_fps = frames_60s / 60.0;
            std::lock_guard<std::mutex> lock(mutex_);
            std::cout << "[DUAL CAP" << index << "][60s] Captured: " << frames_60s
                << " frames, Avg FPS: " << avg_fps
                << ", Dropped: " << frames_dropped
                << ", Queue: " << frame_queue_yuv[index].size() << "/10"
                << std::endl;
            last_frame_count_60s = frames_captured;
            last_stats_time = current_time;
        }

        std::this_thread::sleep_for(std::chrono::microseconds(500));
    }

    av_packet_free(&packet);

    auto total_time = std::chrono::duration_cast<std::chrono::seconds>
        (std::chrono::steady_clock::now() - start_time).count();
    std::cout << "[DUAL CAP" << index << "] Thread exited - Captured: " << frames_captured
        << ", Dropped: " << frames_dropped
        << ", Avg FPS: " << (total_time > 0 ? frames_captured / total_time : 0) << std::endl;
}

void DualCameraCapture::sync_loop() {
    size_t synced_pairs = 0;
    size_t dropped_frames = 0;
    auto start_time = std::chrono::steady_clock::now();
    auto last_stats_time = start_time;

    std::cout << "[DUAL SYNC] Starting dual camera sync loop" << std::endl;

    while (running_) {
        TimestampedFrame y0, y1;
        bool found_sync_pair = false;
        auto current_time = std::chrono::steady_clock::now();

        {
            std::lock_guard<std::mutex> lock(mutex_);

            // 检查两个摄像头队列是否有帧
            if (!frame_queue_yuv[0].empty() && !frame_queue_yuv[1].empty()) {
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

                    // 控制同步队列大小
                    if (synced_yuv_queue_.size() >= 2) {
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

        // 每60秒打印同步统计
        if (std::chrono::duration_cast<std::chrono::seconds>(current_time - last_stats_time).count() >= 60) {
            std::lock_guard<std::mutex> lock(mutex_);
            std::cout << "[DUAL SYNC] Pairs: " << synced_pairs
                << ", Dropped: " << dropped_frames
                << ", Sync queue: " << synced_yuv_queue_.size() << "/2"
                << ", Raw queues: " << frame_queue_yuv[0].size() << "/" << frame_queue_yuv[1].size() << std::endl;
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
    std::cout << "[DUAL SYNC] Thread exited - Total pairs: " << synced_pairs
        << ", Dropped: " << dropped_frames
        << ", Avg pairs/sec: " << (total_time > 0 ? synced_pairs / total_time : 0) << std::endl;
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

MemoryStats DualCameraCapture::get_memory_stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    MemoryStats stats;
    stats.allocated_frames = g_total_frames_allocated.load();
    stats.freed_frames = g_total_frames_freed.load();
    stats.active_frames = g_active_frame_count.load();
    stats.raw_queue_size[0] = frame_queue_yuv[0].size();
    stats.raw_queue_size[1] = frame_queue_yuv[1].size();
    stats.raw_queue_size[2] = 0;  // 双摄像头模式没有第三个
    stats.sync_queue_size = synced_yuv_queue_.size();
    return stats;
}

size_t DualCameraCapture::emergency_memory_cleanup() {
    if (!mutex_.try_lock()) {
        return 0;
    }
    std::lock_guard<std::mutex> lock(mutex_, std::adopt_lock);
    size_t total_cleared = 0;

    // 清理原始队列，只保留最新的2帧
    for (int i = 0; i < 2; ++i) {
        while (frame_queue_yuv[i].size() > 2) {
            free_cloned_frame(&frame_queue_yuv[i].front().frame);
            frame_queue_yuv[i].pop_front();
            total_cleared++;
        }
    }

    // 清理同步队列，只保留1个帧对
    while (synced_yuv_queue_.size() > 1) {
        auto& p = synced_yuv_queue_.front();
        free_cloned_frame(&p.first);
        free_cloned_frame(&p.second);
        synced_yuv_queue_.pop_front();
        total_cleared += 2;
    }

    return total_cleared;
}

size_t DualCameraCapture::force_clear_queues() {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t total_cleared = 0;

    // 清理原始队列
    for (int i = 0; i < 2; ++i) {
        while (!frame_queue_yuv[i].empty()) {
            free_cloned_frame(&frame_queue_yuv[i].front().frame);
            frame_queue_yuv[i].pop_front();
            total_cleared++;
        }
    }

    // 清理同步队列
    while (!synced_yuv_queue_.empty()) {
        auto& p = synced_yuv_queue_.front();
        free_cloned_frame(&p.first);
        free_cloned_frame(&p.second);
        synced_yuv_queue_.pop_front();
        total_cleared += 2;
    }

    return total_cleared;
}

size_t DualCameraCapture::balance_frame_queues() {
    std::lock_guard<std::mutex> lock(mutex_);

    size_t queue0_size = frame_queue_yuv[0].size();
    size_t queue1_size = frame_queue_yuv[1].size();
    size_t balanced = 0;

    const size_t BALANCE_THRESHOLD = 5;

    if (queue0_size > queue1_size + BALANCE_THRESHOLD) {
        size_t excess = queue0_size - queue1_size - BALANCE_THRESHOLD;
        for (size_t i = 0; i < excess && !frame_queue_yuv[0].empty(); ++i) {
            free_cloned_frame(&frame_queue_yuv[0].front().frame);
            frame_queue_yuv[0].pop_front();
            balanced++;
        }
    }
    else if (queue1_size > queue0_size + BALANCE_THRESHOLD) {
        size_t excess = queue1_size - queue0_size - BALANCE_THRESHOLD;
        for (size_t i = 0; i < excess && !frame_queue_yuv[1].empty(); ++i) {
            free_cloned_frame(&frame_queue_yuv[1].front().frame);
            frame_queue_yuv[1].pop_front();
            balanced++;
        }
    }

    return balanced;
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

void DualCameraCapture::release_frame(AVFrame** frame) {
    if (frame && *frame) {
        if ((*frame)->data[0]) {
            av_freep(&(*frame)->data[0]);
            av_frame_free(frame);

            g_total_frames_freed.fetch_add(1);
            g_active_frame_count.fetch_sub(1);
        }
        else {
            av_frame_free(frame);
        }
    }
}

TripleCameraCapture::TripleCameraCapture() {
    avdevice_register_all();
}

TripleCameraCapture::~TripleCameraCapture() {
    full_reset();

    size_t leaked = g_active_frame_count.load();
    if (leaked > 0) {
        std::cout << "[WARNING] Memory leak: " << leaked << " frames still active" << std::endl;
    }
}

void TripleCameraCapture::full_reset() {
    stop();
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    size_t total_freed = 0;

    // 清理摄像头资源
    for (int i = 0; i < static_cast<int>(cameras_.size()); ++i) {
        cleanup_camera(i);
    }
    cameras_.clear();

    // 清理所有队列
    for (int i = 0; i < 3; ++i) {  // 改为3个摄像头
        std::lock_guard<std::mutex> lock(mutex_);
        while (!frame_queue_yuv[i].empty()) {
            free_cloned_frame(&frame_queue_yuv[i].front().frame);
            frame_queue_yuv[i].pop_front();
            total_freed++;
        }
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        while (!synced_yuv_queue_.empty()) {
            auto& triplet = synced_yuv_queue_.front();
            free_cloned_frame(&triplet.cam1);
            free_cloned_frame(&triplet.cam2);
            free_cloned_frame(&triplet.cam3);
            synced_yuv_queue_.pop_front();
            total_freed += 3;
        }
    }

    // 清理第三摄像头插值相关
    {
        std::lock_guard<std::mutex> lock(interpolation_mutex_);
        if (third_cam_prev_frame_) {
            free_cloned_frame(&third_cam_prev_frame_);
        }
        if (third_cam_next_frame_) {
            free_cloned_frame(&third_cam_next_frame_);
        }
    }

    initialized_ = false;
    running_ = false;
    capture_active_ = false;

    if (total_freed > 0) {
        std::cout << "[RESET] Freed " << total_freed << " frames" << std::endl;
    }
}

void TripleCameraCapture::cleanup_camera(int index) {
    if (index >= static_cast<int>(cameras_.size())) return;

    auto& cam = cameras_[index];

    if (cam.sws_ctx_yuv) {
        sws_freeContext(cam.sws_ctx_yuv);
        cam.sws_ctx_yuv = nullptr;
    }

    if (cam.yuv_frame) {
        av_frame_free(&cam.yuv_frame);
        cam.yuv_frame = nullptr;
    }
    if (cam.frame) {
        av_frame_free(&cam.frame);
        cam.frame = nullptr;
    }

    if (cam.yuv_buffer) {
        av_freep(&cam.yuv_buffer);
        cam.yuv_buffer = nullptr;
    }

    if (cam.codec_ctx) {
        avcodec_free_context(&cam.codec_ctx);
        cam.codec_ctx = nullptr;
    }
    if (cam.fmt_ctx) {
        avformat_close_input(&cam.fmt_ctx);
        cam.fmt_ctx = nullptr;
    }

    cam.video_stream_idx = -1;
}

void TripleCameraCapture::free_cloned_frame(AVFrame** frame) {
    if (frame && *frame) {
        if ((*frame)->data[0]) {
            av_freep(&(*frame)->data[0]);
        }
        av_frame_free(frame);

        g_total_frames_freed.fetch_add(1);
        g_active_frame_count.fetch_sub(1);
    }
}

// 简单线性插值函数
AVFrame* TripleCameraCapture::interpolate_frame(const AVFrame* prev, const AVFrame* next, float t) const {
    if (!prev || !next || t < 0.0f || t > 1.0f) {
        return nullptr;
    }

    AVFrame* result = av_frame_alloc();
    if (!result) return nullptr;

    result->format = prev->format;
    result->width = prev->width;
    result->height = prev->height;

    int ret = av_image_alloc(result->data, result->linesize,
        result->width, result->height,
        (AVPixelFormat)result->format, 32);
    if (ret < 0) {
        av_frame_free(&result);
        return nullptr;
    }

    // YUV420P格式的线性插值
    // Y平面
    int y_size = prev->width * prev->height;
    for (int i = 0; i < y_size; ++i) {
        result->data[0][i] = (uint8_t)(prev->data[0][i] * (1.0f - t) + next->data[0][i] * t);
    }

    // U平面
    int uv_size = (prev->width / 2) * (prev->height / 2);
    for (int i = 0; i < uv_size; ++i) {
        result->data[1][i] = (uint8_t)(prev->data[1][i] * (1.0f - t) + next->data[1][i] * t);
    }

    // V平面
    for (int i = 0; i < uv_size; ++i) {
        result->data[2][i] = (uint8_t)(prev->data[2][i] * (1.0f - t) + next->data[2][i] * t);
    }

    return result;
}

size_t TripleCameraCapture::emergency_memory_cleanup() {
    if (!mutex_.try_lock()) {
        return 0;
    }
    std::lock_guard<std::mutex> lock(mutex_, std::adopt_lock);
    size_t total_cleared = 0;

    // 清理原始队列，只保留最新的2帧
    for (int i = 0; i < 3; ++i) {
        while (frame_queue_yuv[i].size() > 2) {
            free_cloned_frame(&frame_queue_yuv[i].front().frame);
            frame_queue_yuv[i].pop_front();
            total_cleared++;
        }
    }

    // 清理同步队列，只保留1个帧三元组
    while (synced_yuv_queue_.size() > 1) {
        auto& triplet = synced_yuv_queue_.front();
        free_cloned_frame(&triplet.cam1);
        free_cloned_frame(&triplet.cam2);
        free_cloned_frame(&triplet.cam3);
        synced_yuv_queue_.pop_front();
        total_cleared += 3;
    }

    return total_cleared;
}

MemoryStats TripleCameraCapture::get_memory_stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    MemoryStats stats;
    stats.allocated_frames = g_total_frames_allocated.load();
    stats.freed_frames = g_total_frames_freed.load();
    stats.active_frames = g_active_frame_count.load();
    stats.raw_queue_size[0] = frame_queue_yuv[0].size();
    stats.raw_queue_size[1] = frame_queue_yuv[1].size();
    stats.raw_queue_size[2] = frame_queue_yuv[2].size();  // 第三个摄像头
    stats.sync_queue_size = synced_yuv_queue_.size();
    return stats;
}

bool TripleCameraCapture::init(const std::vector<std::string>& device_paths) {
    if (initialized_) {
        full_reset();
    }

    if (device_paths.size() != 3) {
        std::cerr << "[ERROR] Need exactly 3 camera devices" << std::endl;
        return false;
    }

    std::cout << "[CAMERA] Initializing triple cameras..." << std::endl;
    cameras_.resize(3);

    for (int i = 0; i < 3; ++i) {
        if (!init_camera(i, device_paths[i])) {
            std::cerr << "[ERROR] Failed to initialize camera " << i << std::endl;
            for (int j = 0; j < i; ++j) {
                cleanup_camera(j);
            }
            cameras_.clear();
            return false;
        }
    }

    initialized_ = true;
    std::cout << "[CAMERA] All three cameras initialized" << std::endl;
    return true;
}

bool TripleCameraCapture::init_camera(int index, const std::string& device_path) {
    auto& cam = cameras_[index];

    // 打开视频设备
    const AVInputFormat* input_format = av_find_input_format("dshow");
    AVDictionary* options = nullptr;

    // 第三个摄像头(索引2)使用较低参数，其他使用MJPEG
    if (index == 2) {
        // USB 2.0 Camera - 较低质量但稳定的配置
        av_dict_set(&options, "video_size", "640x480", 0);
        av_dict_set(&options, "framerate", "15", 0);  // 尝试15fps而不是10fps
        av_dict_set(&options, "vcodec", "mjpeg", 0);
        av_dict_set(&options, "rtbufsize", "1048576", 0);  // 1MB
        av_dict_set(&options, "buffer_size", "262144", 0); // 256KB
    }
    else {
        // 主摄像头和辅摄像头 - 使用MJPEG 30fps
        av_dict_set(&options, "video_size", "640x480", 0);
        av_dict_set(&options, "framerate", "30", 0);
        av_dict_set(&options, "vcodec", "mjpeg", 0);
        av_dict_set(&options, "rtbufsize", "2097152", 0);
        av_dict_set(&options, "buffer_size", "524288", 0);
    }

    av_dict_set(&options, "probesize", "32", 0);
    av_dict_set(&options, "analyzeduration", "50000", 0);
    av_dict_set(&options, "fflags", "nobuffer", 0);
    av_dict_set(&options, "flags", "low_delay", 0);
    av_dict_set(&options, "thread_queue_size", "2", 0);

    int ret = avformat_open_input(&cam.fmt_ctx, device_path.c_str(), input_format, &options);
    av_dict_free(&options);

    if (ret != 0) {
        std::cerr << "[ERROR] Could not open camera " << index << " at " << device_path << std::endl;
        return false;
    }

    // 限制stream info查找时间
    AVDictionary* find_stream_options = nullptr;
    av_dict_set(&find_stream_options, "analyzeduration", "50000", 0);
    av_dict_set(&find_stream_options, "probesize", "32", 0);

    if (avformat_find_stream_info(cam.fmt_ctx, &find_stream_options) < 0) {
        std::cerr << "[ERROR] Could not find stream info for camera " << index << std::endl;
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
        std::cerr << "[ERROR] Could not find video stream in camera " << index << std::endl;
        return false;
    }

    AVStream* vs = cam.fmt_ctx->streams[cam.video_stream_idx];
    std::cout << "[DEBUG] Camera " << index << " codec_id=" << vs->codecpar->codec_id
        << " format=" << vs->codecpar->format << std::endl;

    // 获取解码器
    AVCodecParameters* codecpar = cam.fmt_ctx->streams[cam.video_stream_idx]->codecpar;
    const AVCodec* codec = avcodec_find_decoder(codecpar->codec_id);
    if (!codec) {
        std::cerr << "[ERROR] Unsupported codec for camera " << index << std::endl;
        return false;
    }

    cam.codec_ctx = avcodec_alloc_context3(codec);
    if (!cam.codec_ctx) {
        std::cerr << "[ERROR] Could not allocate codec context for camera " << index << std::endl;
        return false;
    }

    if (avcodec_parameters_to_context(cam.codec_ctx, codecpar) < 0) {
        std::cerr << "[ERROR] Could not copy codec context for camera " << index << std::endl;
        return false;
    }

    // MJPEG解码器优化设置
    if (codecpar->codec_id == AV_CODEC_ID_MJPEG) {
        av_opt_set(cam.codec_ctx->priv_data, "lowres", "0", 0);
        cam.codec_ctx->thread_count = 2;
    }
    else {
        av_opt_set(cam.codec_ctx->priv_data, "preset", "ultrafast", 0);
        av_opt_set(cam.codec_ctx->priv_data, "tune", "zerolatency", 0);
        cam.codec_ctx->thread_count = 1;
    }

    cam.codec_ctx->delay = 0;

    if (avcodec_open2(cam.codec_ctx, codec, nullptr) < 0) {
        std::cerr << "[ERROR] Could not open codec for camera " << index << std::endl;
        return false;
    }

    // 分配帧缓冲区
    cam.frame = av_frame_alloc();
    cam.yuv_frame = av_frame_alloc();
    if (!cam.frame || !cam.yuv_frame) {
        std::cerr << "[ERROR] Could not allocate frames for camera " << index << std::endl;
        return false;
    }

    // 计算并分配YUV缓冲区
    int yuv_size = av_image_get_buffer_size(AV_PIX_FMT_YUV420P, cam.codec_ctx->width, cam.codec_ctx->height, 32);
    cam.yuv_buffer = (uint8_t*)av_malloc(yuv_size);
    if (!cam.yuv_buffer) {
        std::cerr << "[ERROR] Could not allocate yuv buffer for camera " << index << std::endl;
        return false;
    }

    // 绑定buffer到帧
    av_image_fill_arrays(cam.yuv_frame->data, cam.yuv_frame->linesize, cam.yuv_buffer,
        AV_PIX_FMT_YUV420P, cam.codec_ctx->width, cam.codec_ctx->height, 32);

    // 创建SwsContext
    enum AVPixelFormat src_pix_fmt = cam.codec_ctx->pix_fmt;
    if (src_pix_fmt == AV_PIX_FMT_NONE) {
        src_pix_fmt = AV_PIX_FMT_YUVJ420P;
    }

    cam.sws_ctx_yuv = sws_getContext(
        cam.codec_ctx->width, cam.codec_ctx->height, src_pix_fmt,
        cam.codec_ctx->width, cam.codec_ctx->height, AV_PIX_FMT_YUV420P,
        SWS_BILINEAR, nullptr, nullptr, nullptr);

    if (!cam.sws_ctx_yuv) {
        std::cerr << "[ERROR] Could not create sws context for camera " << index << std::endl;
        return false;
    }

    std::cout << "[CAMERA] Camera " << index << " initialized ("
        << cam.codec_ctx->width << "x" << cam.codec_ctx->height
        << ", codec=" << avcodec_get_name(codecpar->codec_id) << ")" << std::endl;
    return true;
}

void TripleCameraCapture::start() {
    if (!initialized_) {
        std::cerr << "[ERROR] Cannot start - not initialized" << std::endl;
        return;
    }

    if (running_.load()) {
        return;
    }

    std::cout << "[CAMERA] Starting capture threads..." << std::endl;
    running_ = true;
    capture_active_ = true;

    // 启动三个捕获线程
    for (int i = 0; i < 3; ++i) {
        threads_.emplace_back(&TripleCameraCapture::capture_thread, this, i);
    }

    // 启动第三摄像头插值线程
    interpolation_thread_ = std::thread(&TripleCameraCapture::interpolation_loop, this);

    // 启动三摄像头同步线程
    sync_thread_ = std::thread(&TripleCameraCapture::sync_loop, this);

    std::cout << "[CAMERA] All threads started (including interpolation)" << std::endl;
}

void TripleCameraCapture::stop() {
    if (!running_.load()) {
        return;
    }

    std::cout << "[CAMERA] Stopping capture..." << std::endl;
    running_ = false;
    capture_active_ = false;

    // 等待所有线程结束
    for (auto& t : threads_) {
        if (t.joinable()) t.join();
    }
    if (interpolation_thread_.joinable()) interpolation_thread_.join();
    if (sync_thread_.joinable()) sync_thread_.join();
    threads_.clear();

    std::cout << "[CAMERA] All threads stopped" << std::endl;
}

// 第三摄像头插值线程
void TripleCameraCapture::interpolation_loop() {
    std::cout << "[INTERPOLATION] Starting interpolation thread for camera 2" << std::endl;

    size_t interpolated_frames = 0;
    size_t real_frames_used = 0;
    auto start_time = std::chrono::steady_clock::now();
    auto last_stats_time = start_time;

    while (running_) {
        // 检查是否有新的真实帧
        TimestampedFrame real_frame;
        bool has_real_frame = false;

        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!frame_queue_yuv[2].empty()) {  // 第三摄像头队列
                real_frame = frame_queue_yuv[2].front();
                frame_queue_yuv[2].pop_front();
                has_real_frame = true;
                real_frames_used++;
            }
        }

        if (has_real_frame) {
            std::lock_guard<std::mutex> lock(interpolation_mutex_);

            // 更新帧对
            if (third_cam_prev_frame_) {
                free_cloned_frame(&third_cam_prev_frame_);
            }
            third_cam_prev_frame_ = third_cam_next_frame_;
            third_cam_next_frame_ = real_frame.frame;
            third_cam_next_timestamp_ = real_frame.timestamp_us;

            if (third_cam_prev_frame_ && third_cam_next_frame_) {
                // 生成插值帧
                int64_t time_diff = third_cam_next_timestamp_ - third_cam_prev_timestamp_;
                if (time_diff > 0 && time_diff < 200000) {  // 小于200ms的间隔才插值
                    // 生成2个插值帧（在33%和66%位置）
                    for (int i = 1; i <= 2; ++i) {
                        float t = i / 3.0f;  // 0.33 或 0.66
                        AVFrame* interpolated = interpolate_frame(third_cam_prev_frame_, third_cam_next_frame_, t);
                        if (interpolated) {
                            int64_t interp_timestamp = third_cam_prev_timestamp_ + (int64_t)(time_diff * t);

                            // 添加到插值队列
                            {
                                std::lock_guard<std::mutex> queue_lock(mutex_);
                                if (interpolated_queue_.size() < 20) {  // 限制插值队列大小
                                    interpolated_queue_.emplace_back(TimestampedFrame{ interpolated, interp_timestamp });
                                    interpolated_frames++;

                                    g_total_frames_allocated.fetch_add(1);
                                    g_active_frame_count.fetch_add(1);
                                }
                                else {
                                    free_cloned_frame(&interpolated);
                                }
                            }
                        }
                    }
                }
            }

            third_cam_prev_timestamp_ = third_cam_next_timestamp_;
        }

        auto current_time = std::chrono::steady_clock::now();

        // 每60秒打印插值统计
        if (std::chrono::duration_cast<std::chrono::seconds>(current_time - last_stats_time).count() >= 60) {
            std::lock_guard<std::mutex> lock(mutex_);
            std::cout << "[INTERPOLATION] Real frames: " << real_frames_used
                << ", Interpolated: " << interpolated_frames
                << ", Queue size: " << interpolated_queue_.size() << std::endl;
            last_stats_time = current_time;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    std::cout << "[INTERPOLATION] Thread exited" << std::endl;
}

// 修改后的同步循环，支持三摄像头
void TripleCameraCapture::sync_loop() {
    size_t synced_triplets = 0;
    size_t dropped_frames = 0;
    auto start_time = std::chrono::steady_clock::now();
    auto last_stats_time = start_time;

    std::cout << "[SYNC] Starting triple camera sync loop" << std::endl;

    while (running_) {
        TimestampedFrame y0, y1, y2;
        bool found_sync_triplet = false;
        auto current_time = std::chrono::steady_clock::now();

        {
            std::lock_guard<std::mutex> lock(mutex_);

            // 检查前两个摄像头和插值队列是否有帧
            if (!frame_queue_yuv[0].empty() && !frame_queue_yuv[1].empty() && !interpolated_queue_.empty()) {
                y0 = frame_queue_yuv[0].front();
                y1 = frame_queue_yuv[1].front();

                // 从插值队列中找最接近的帧
                auto best_match = interpolated_queue_.begin();
                int64_t min_diff = LLONG_MAX;

                for (auto it = interpolated_queue_.begin(); it != interpolated_queue_.end(); ++it) {
                    int64_t avg_timestamp = (y0.timestamp_us + y1.timestamp_us) / 2;
                    int64_t diff = std::abs(it->timestamp_us - avg_timestamp);
                    if (diff < min_diff) {
                        min_diff = diff;
                        best_match = it;
                    }
                }

                if (min_diff <= SYNC_THRESHOLD_US * 2) {  // 放宽第三摄像头的同步阈值
                    y2 = *best_match;
                    interpolated_queue_.erase(best_match);

                    // 检查前两个摄像头是否同步
                    int64_t dy = std::abs(y0.timestamp_us - y1.timestamp_us);
                    if (dy <= SYNC_THRESHOLD_US) {
                        found_sync_triplet = true;

                        // 从原始队列移除
                        frame_queue_yuv[0].pop_front();
                        frame_queue_yuv[1].pop_front();

                        // 控制同步队列大小
                        if (synced_yuv_queue_.size() >= 2) {
                            auto& old_triplet = synced_yuv_queue_.front();
                            free_cloned_frame(&old_triplet.cam1);
                            free_cloned_frame(&old_triplet.cam2);
                            free_cloned_frame(&old_triplet.cam3);
                            synced_yuv_queue_.pop_front();
                            dropped_frames += 3;
                        }

                        // 添加到同步队列
                        synced_yuv_queue_.emplace_back(FrameTriplet{ y0.frame, y1.frame, y2.frame });
                        synced_triplets++;
                    }
                    else {
                        // 前两个摄像头不同步，释放第三摄像头帧
                        free_cloned_frame(&y2.frame);

                        // 丢弃时间戳较早的那一帧
                        int idx = (y0.timestamp_us < y1.timestamp_us) ? 0 : 1;
                        free_cloned_frame(&frame_queue_yuv[idx].front().frame);
                        frame_queue_yuv[idx].pop_front();
                        dropped_frames++;
                    }
                }
                else {
                    // 没有合适的第三摄像头帧，丢弃最早的帧
                    if (y0.timestamp_us < y1.timestamp_us) {
                        free_cloned_frame(&frame_queue_yuv[0].front().frame);
                        frame_queue_yuv[0].pop_front();
                    }
                    else {
                        free_cloned_frame(&frame_queue_yuv[1].front().frame);
                        frame_queue_yuv[1].pop_front();
                    }
                    dropped_frames++;
                }
            }
        }

        // 每60秒打印同步统计
        if (std::chrono::duration_cast<std::chrono::seconds>(current_time - last_stats_time).count() >= 60) {
            std::lock_guard<std::mutex> lock(mutex_);
            std::cout << "[SYNC] Triplets: " << synced_triplets
                << ", Dropped: " << dropped_frames
                << ", Sync queue: " << synced_yuv_queue_.size() << "/2"
                << ", Raw queues: " << frame_queue_yuv[0].size() << "/"
                << frame_queue_yuv[1].size() << "/" << interpolated_queue_.size() << std::endl;
            last_stats_time = current_time;
        }

        // 根据是否找到同步帧调整睡眠时间
        if (found_sync_triplet) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        else {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    auto total_time = std::chrono::duration_cast<std::chrono::seconds>
        (std::chrono::steady_clock::now() - start_time).count();
    std::cout << "[SYNC] Thread exited - Total triplets: " << synced_triplets
        << ", Dropped: " << dropped_frames
        << ", Avg triplets/sec: " << (total_time > 0 ? synced_triplets / total_time : 0) << std::endl;
}

// 捕获线程（与之前类似，但支持3个摄像头）
void TripleCameraCapture::capture_thread(int index) {
    if (index >= static_cast<int>(cameras_.size())) {
        std::cerr << "[ERROR] Invalid camera index " << index << std::endl;
        return;
    }

    auto& cam = cameras_[index];
    AVPacket* packet = av_packet_alloc();
    if (!packet) {
        std::cerr << "[ERROR] Could not allocate packet for camera " << index << std::endl;
        return;
    }

    int consecutive_failures = 0;
    const int MAX_CONSECUTIVE_FAILURES = 10;
    size_t frames_captured = 0;
    size_t frames_dropped = 0;
    auto start_time = std::chrono::steady_clock::now();
    auto last_stats_time = std::chrono::steady_clock::now();
    size_t last_frame_count_60s = 0;
    auto last_flush_time = start_time;

    while (running_) {
        if (!capture_active_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        auto current_time = std::chrono::steady_clock::now();

        // 定期冲刷解码器缓冲区
        if (std::chrono::duration_cast<std::chrono::milliseconds>(current_time - last_flush_time).count() >= 200) {
            avcodec_flush_buffers(cam.codec_ctx);
            last_flush_time = current_time;
        }

        int ret = av_read_frame(cam.fmt_ctx, packet);
        if (ret < 0) {
            consecutive_failures++;
            if (ret == AVERROR(EAGAIN)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            else if (consecutive_failures > MAX_CONSECUTIVE_FAILURES) {
                std::cerr << "[ERROR] Camera " << index << " too many failures, stopping" << std::endl;
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

        // 检查内存压力
        bool memory_pressure = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            size_t active_frames = g_active_frame_count.load();
            memory_pressure = (active_frames > 4000);  // 增加阈值适应三摄像头
        }

        if (memory_pressure) {
            frames_dropped++;
            av_packet_unref(packet);
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

            // 限制队列大小
            size_t queue_size = frame_queue_yuv[index].size();
            size_t max_queue_size = (index == 2) ? 5 : 10;  // 第三摄像头较小队列

            if (queue_size >= max_queue_size) {
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

                    g_total_frames_allocated.fetch_add(1);
                    g_active_frame_count.fetch_add(1);
                }
            }
        }

        // 60秒统计
        auto elapsed_sec = std::chrono::duration_cast<std::chrono::seconds>(current_time - last_stats_time).count();
        if (elapsed_sec >= 60) {
            size_t frames_60s = frames_captured - last_frame_count_60s;
            double avg_fps = frames_60s / 60.0;
            std::lock_guard<std::mutex> lock(mutex_);
            std::cout << "[CAP" << index << "][60s] Captured: " << frames_60s
                << " frames, Avg FPS: " << avg_fps
                << ", Dropped: " << frames_dropped
                << ", Queue: " << frame_queue_yuv[index].size() << "/"
                << std::endl;
            last_frame_count_60s = frames_captured;
            last_stats_time = current_time;
        }

        std::this_thread::sleep_for(std::chrono::microseconds(500));
    }

    av_packet_free(&packet);

    auto total_time = std::chrono::duration_cast<std::chrono::seconds>
        (std::chrono::steady_clock::now() - start_time).count();
    std::cout << "[CAP" << index << "] Thread exited - Captured: " << frames_captured
        << ", Dropped: " << frames_dropped
        << ", Avg FPS: " << (total_time > 0 ? frames_captured / total_time : 0) << std::endl;
}

std::vector<AVFrame*> TripleCameraCapture::get_sync_yuv420p_frames() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (synced_yuv_queue_.empty()) return {};

    auto triplet = synced_yuv_queue_.front();
    synced_yuv_queue_.pop_front();

    return { triplet.cam1, triplet.cam2, triplet.cam3 };
}

void TripleCameraCapture::pause_capture() {
    capture_active_ = false;
}

void TripleCameraCapture::resume_capture() {
    capture_active_ = true;
}

size_t TripleCameraCapture::get_sync_queue_size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return synced_yuv_queue_.size();
}

AVFrame* TripleCameraCapture::clone_frame(const AVFrame* src) const {
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

void TripleCameraCapture::release_frame(AVFrame** frame) {
    if (frame && *frame) {
        if ((*frame)->data[0]) {
            av_freep(&(*frame)->data[0]);
            av_frame_free(frame);

            g_total_frames_freed.fetch_add(1);
            g_active_frame_count.fetch_sub(1);
        }
        else {
            av_frame_free(frame);
        }
    }
}
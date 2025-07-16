// multi_camera_sync.cpp - 去除插值功能的优化版本 - 最终修复版本
#include "../includes/multi_camera_sync.h"
#include <iostream>
#include <stdexcept>
#include <atomic>
#include <algorithm>
#include <chrono>

// 全局统计变量
std::atomic<size_t> g_total_frames_allocated{ 0 };
std::atomic<size_t> g_total_frames_freed{ 0 };
std::atomic<size_t> g_active_frame_count{ 0 };

// 调试级别控制
#define DEBUG_LEVEL_NONE 0
#define DEBUG_LEVEL_ERROR 1
#define DEBUG_LEVEL_INFO 2
#define DEBUG_LEVEL_VERBOSE 3

#ifndef DEBUG_LEVEL
#define DEBUG_LEVEL DEBUG_LEVEL_INFO
#endif

#define DEBUG_PRINT(level, ...) \
    do { if (DEBUG_LEVEL >= level) { std::cout << __VA_ARGS__ << std::endl; } } while(0)

//==============================================================================
// StatsManager 实现 - 只提供方法实现，不重新定义类
//==============================================================================
StatsManager::StatsManager(size_t camera_count) : camera_stats_(camera_count) {
    start_time_ = last_report_time_ = std::chrono::steady_clock::now();
}

void StatsManager::update_camera_stats(int camera_id, const std::string& event, size_t count) {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    if (camera_id >= 0 && camera_id < static_cast<int>(camera_stats_.size())) {
        auto& stats = camera_stats_[camera_id];
        if (event == "captured") stats.frames_captured += count;
        else if (event == "dropped_queue") stats.frames_dropped_queue += count;
        else if (event == "dropped_memory") stats.frames_dropped_memory += count;
        else if (event == "decode_fail") stats.decode_failures += count;
        else if (event == "read_fail") stats.read_failures += count;
        else if (event == "skipped") stats.frames_skipped += count;
    }
}

void StatsManager::update_sync_stats(const std::string& event, size_t count) {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    if (event == "attempt") sync_stats_.sync_attempts += count;
    else if (event == "success") sync_stats_.sync_success += count;
    else if (event == "fail_no_frames") sync_stats_.sync_failures_no_frames += count;
    else if (event == "fail_timestamp") sync_stats_.sync_failures_timestamp += count;
    else if (event == "memory_cleanup") sync_stats_.memory_cleanups += count;
}

bool StatsManager::should_report(int interval_seconds) {
    auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::seconds>(now - last_report_time_).count() >= interval_seconds) {
        last_report_time_ = now;
        return true;
    }
    return false;
}

void StatsManager::print_summary(const std::string& prefix) {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - start_time_).count();

    DEBUG_PRINT(DEBUG_LEVEL_INFO, "[STATS" << prefix << "] Runtime: " << elapsed << "s");

    for (size_t i = 0; i < camera_stats_.size(); ++i) {
        const auto& stats = camera_stats_[i];
        double fps = elapsed > 0 ? static_cast<double>(stats.frames_captured) / elapsed : 0;
        DEBUG_PRINT(DEBUG_LEVEL_INFO, "[CAM" << i << "] FPS: " << fps
            << ", Captured: " << stats.frames_captured
            << ", Dropped: " << (stats.frames_dropped_queue + stats.frames_dropped_memory)
            << ", Failures: " << (stats.decode_failures + stats.read_failures));
    }

    double sync_rate = sync_stats_.sync_attempts > 0 ?
        static_cast<double>(sync_stats_.sync_success) / sync_stats_.sync_attempts * 100.0 : 0;
    DEBUG_PRINT(DEBUG_LEVEL_INFO, "[SYNC] Success rate: " << sync_rate << "%");
}

//==============================================================================
// PerformanceOptimizer 实现
//==============================================================================
PerformanceOptimizer::PerformanceOptimizer() {
    last_adjustment_time_ = std::chrono::steady_clock::now();
}

void PerformanceOptimizer::update_sync_stats(bool success) {
    total_attempts_.fetch_add(1);
    if (success) {
        total_success_.fetch_add(1);
    }

    // 每100次尝试后更新同步率
    if (total_attempts_.load() % 100 == 0) {
        double rate = total_success_.load() > 0 ?
            static_cast<double>(total_success_.load()) / total_attempts_.load() : 0.0;
        current_sync_rate_.store(rate);
        adjust_frame_skip_strategy(rate);
    }
}

bool PerformanceOptimizer::should_skip_frame(size_t camera_index, size_t queue_size) {
    int skip_factor = frame_skip_factor_.load();
    if (skip_factor <= 1) return false;

    // 基于队列大小和跳帧因子决定是否跳过
    return (queue_size > 5) && ((camera_index + queue_size) % skip_factor == 0);
}

double PerformanceOptimizer::get_sync_rate() const {
    return current_sync_rate_.load();
}

size_t PerformanceOptimizer::get_total_attempts() const {
    return total_attempts_.load();
}

size_t PerformanceOptimizer::get_total_success() const {
    return total_success_.load();
}

void PerformanceOptimizer::adjust_frame_skip_strategy(double sync_rate) {
    auto now = std::chrono::steady_clock::now();
    auto time_since_last = std::chrono::duration_cast<std::chrono::seconds>(now - last_adjustment_time_).count();

    if (time_since_last < 10) return; // 至少10秒才调整一次

    if (sync_rate < 0.6) {
        frame_skip_factor_ = 3; // 低同步率，增加跳帧
    }
    else if (sync_rate < 0.8) {
        frame_skip_factor_ = 2; // 中等同步率，适度跳帧
    }
    else {
        frame_skip_factor_ = 1; // 高同步率，不跳帧
    }

    last_adjustment_time_ = now;
}

//==============================================================================
// 同步策略实现 - 简化版本（去除插值策略）
//==============================================================================
std::vector<AVFrame*> TimestampSyncStrategy::find_sync_frames(
    const std::vector<std::deque<TimestampedFrame>>& queues,
    const SyncConfig& config) {

    if (queues.size() < 2) return {};

    for (const auto& queue : queues) {
        if (queue.empty()) return {};
    }

    std::vector<TimestampedFrame> candidates;
    for (const auto& queue : queues) {
        candidates.push_back(queue.front());
    }

    int64_t avg_timestamp = 0;
    for (const auto& frame : candidates) {
        avg_timestamp += frame.timestamp_us;
    }
    avg_timestamp /= candidates.size();

    for (const auto& frame : candidates) {
        if (std::abs(frame.timestamp_us - avg_timestamp) > config.sync_threshold_us) {
            return {};
        }
    }

    std::vector<AVFrame*> sync_frames;
    for (const auto& frame : candidates) {
        sync_frames.push_back(frame.frame);
    }
    return sync_frames;
}

//==============================================================================
// MultiCameraCapture 实现 - 去除插值功能，支持rawvideo
//==============================================================================
MultiCameraCapture::MultiCameraCapture() {
    avdevice_register_all();
    sync_strategy_ = std::make_unique<TimestampSyncStrategy>();
}

MultiCameraCapture::~MultiCameraCapture() {
    full_reset();
    size_t leaked = g_active_frame_count.load();
    if (leaked > 0) {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR, "[WARNING] Memory leak: " << leaked << " frames");
    }
}

bool MultiCameraCapture::init(const std::vector<std::string>& device_paths, const SyncConfig& config) {
    if (initialized_) full_reset();
    if (device_paths.empty()) return false;

    camera_count_ = device_paths.size();
    config_ = config;
    stats_manager_ = std::make_unique<StatsManager>(camera_count_);

    DEBUG_PRINT(DEBUG_LEVEL_INFO, "[MULTI] Initializing " << camera_count_ << " cameras");

    cameras_.resize(camera_count_);
    frame_queues_.resize(camera_count_);

    for (size_t i = 0; i < camera_count_; ++i) {
        if (!init_camera(static_cast<int>(i), device_paths[i])) {
            DEBUG_PRINT(DEBUG_LEVEL_ERROR, "[ERROR] Failed to init camera " << i);
            for (size_t j = 0; j < i; ++j) {
                cleanup_camera(static_cast<int>(j));
            }
            return false;
        }
    }

    initialized_ = true;
    DEBUG_PRINT(DEBUG_LEVEL_INFO, "[MULTI] All cameras initialized successfully");
    return true;
}

// 关键修复函数：支持rawvideo和mjpeg的摄像头初始化
bool MultiCameraCapture::init_camera(int index, const std::string& device_path) {
    auto& cam = cameras_[index];
    cam.index = index;
    cam.device_path = device_path;

    const AVInputFormat* input_format = av_find_input_format("dshow");
    AVDictionary* options = nullptr;

    // 关键修复：支持rawvideo和mjpeg的摄像头配置
    av_dict_set(&options, "video_size", "640x480", 0);
    av_dict_set(&options, "framerate", "30", 0);

    // 重要修复：不强制指定编码格式，让设备自选最佳格式
    // av_dict_set(&options, "vcodec", "mjpeg", 0);  // 注释掉强制mjpeg

    av_dict_set(&options, "rtbufsize", "16777216", 0);     // 16MB缓冲区
    av_dict_set(&options, "buffer_size", "4194304", 0);    // 4MB缓冲区
    av_dict_set(&options, "probesize", "32", 0);
    av_dict_set(&options, "analyzeduration", "200000", 0);
    av_dict_set(&options, "fflags", "nobuffer", 0);
    av_dict_set(&options, "flags", "low_delay", 0);
    av_dict_set(&options, "thread_queue_size", "8", 0);
    av_dict_set(&options, "use_wallclock_as_timestamps", "1", 0);

    DEBUG_PRINT(DEBUG_LEVEL_VERBOSE, "[INIT] Opening camera " << index << " at " << device_path);

    int ret = avformat_open_input(&cam.fmt_ctx, device_path.c_str(), input_format, &options);
    av_dict_free(&options);

    if (ret != 0) {
        char error_buf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, error_buf, sizeof(error_buf));
        DEBUG_PRINT(DEBUG_LEVEL_ERROR, "[ERROR] Could not open camera " << index
            << " (" << device_path << "): " << error_buf);

        // 提供更详细的错误信息
        if (ret == AVERROR(EIO)) {
            DEBUG_PRINT(DEBUG_LEVEL_ERROR, "[ERROR] This may be caused by:");
            DEBUG_PRINT(DEBUG_LEVEL_ERROR, "  1. Device already in use by another application");
            DEBUG_PRINT(DEBUG_LEVEL_ERROR, "  2. Same physical device referenced multiple times");
            DEBUG_PRINT(DEBUG_LEVEL_ERROR, "  3. Insufficient permissions or driver issues");
        }

        return false;
    }

    if (avformat_find_stream_info(cam.fmt_ctx, nullptr) < 0) {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR, "[ERROR] Could not find stream info for camera " << index);
        avformat_close_input(&cam.fmt_ctx);
        return false;
    }

    // 查找视频流
    cam.video_stream_idx = -1;
    for (unsigned int i = 0; i < cam.fmt_ctx->nb_streams; i++) {
        if (cam.fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            cam.video_stream_idx = i;
            break;
        }
    }

    if (cam.video_stream_idx == -1) {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR, "[ERROR] No video stream found for camera " << index);
        avformat_close_input(&cam.fmt_ctx);
        return false;
    }

    AVCodecParameters* codecpar = cam.fmt_ctx->streams[cam.video_stream_idx]->codecpar;

    // 显示实际检测到的编码格式
    DEBUG_PRINT(DEBUG_LEVEL_INFO, "[INFO] Camera " << index << " codec: "
        << avcodec_get_name(codecpar->codec_id) << " (" << codecpar->width
        << "x" << codecpar->height << ")");

    const AVCodec* codec = avcodec_find_decoder(codecpar->codec_id);
    if (!codec) {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR, "[ERROR] Could not find decoder for camera " << index);
        avformat_close_input(&cam.fmt_ctx);
        return false;
    }

    cam.codec_ctx = avcodec_alloc_context3(codec);
    if (!cam.codec_ctx) {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR, "[ERROR] Could not allocate codec context for camera " << index);
        avformat_close_input(&cam.fmt_ctx);
        return false;
    }

    if (avcodec_parameters_to_context(cam.codec_ctx, codecpar) < 0) {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR, "[ERROR] Could not copy codec params for camera " << index);
        avcodec_free_context(&cam.codec_ctx);
        avformat_close_input(&cam.fmt_ctx);
        return false;
    }

    // 根据编码格式优化解码器配置
    if (codecpar->codec_id == AV_CODEC_ID_RAWVIDEO) {
        cam.codec_ctx->thread_count = 1;  // rawvideo不需要多线程
        DEBUG_PRINT(DEBUG_LEVEL_INFO, "[INFO] Camera " << index << " using rawvideo codec, single-threaded");
    }
    else {
        cam.codec_ctx->thread_count = 2;  // 其他编码格式使用多线程
        DEBUG_PRINT(DEBUG_LEVEL_INFO, "[INFO] Camera " << index << " using "
            << avcodec_get_name(codecpar->codec_id) << " codec, multi-threaded");
    }

    cam.codec_ctx->delay = 0;

    if (avcodec_open2(cam.codec_ctx, codec, nullptr) < 0) {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR, "[ERROR] Could not open codec for camera " << index);
        avcodec_free_context(&cam.codec_ctx);
        avformat_close_input(&cam.fmt_ctx);
        return false;
    }

    // 分配帧缓冲区
    cam.frame = av_frame_alloc();
    cam.yuv_frame = av_frame_alloc();
    if (!cam.frame || !cam.yuv_frame) {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR, "[ERROR] Could not allocate frames for camera " << index);
        if (cam.frame) av_frame_free(&cam.frame);
        if (cam.yuv_frame) av_frame_free(&cam.yuv_frame);
        avcodec_free_context(&cam.codec_ctx);
        avformat_close_input(&cam.fmt_ctx);
        return false;
    }

    int yuv_size = av_image_get_buffer_size(AV_PIX_FMT_YUV420P, cam.codec_ctx->width, cam.codec_ctx->height, 32);
    cam.yuv_buffer = (uint8_t*)av_malloc(yuv_size);
    if (!cam.yuv_buffer) {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR, "[ERROR] Could not allocate YUV buffer for camera " << index);
        av_frame_free(&cam.frame);
        av_frame_free(&cam.yuv_frame);
        avcodec_free_context(&cam.codec_ctx);
        avformat_close_input(&cam.fmt_ctx);
        return false;
    }

    av_image_fill_arrays(cam.yuv_frame->data, cam.yuv_frame->linesize, cam.yuv_buffer,
        AV_PIX_FMT_YUV420P, cam.codec_ctx->width, cam.codec_ctx->height, 32);

    // 创建SwsContext - 智能检测像素格式，支持rawvideo
    enum AVPixelFormat src_pix_fmt = cam.codec_ctx->pix_fmt;

    // 对于rawvideo，可能需要特殊处理像素格式
    if (src_pix_fmt == AV_PIX_FMT_NONE) {
        // 尝试常见的rawvideo格式
        if (codecpar->codec_id == AV_CODEC_ID_RAWVIDEO) {
            // 根据位深度推测格式
            if (codecpar->bits_per_coded_sample == 24) {
                src_pix_fmt = AV_PIX_FMT_RGB24;
                DEBUG_PRINT(DEBUG_LEVEL_INFO, "[INFO] Assuming RGB24 for 24-bit rawvideo");
            }
            else if (codecpar->bits_per_coded_sample == 16) {
                src_pix_fmt = AV_PIX_FMT_RGB565;
                DEBUG_PRINT(DEBUG_LEVEL_INFO, "[INFO] Assuming RGB565 for 16-bit rawvideo");
            }
            else {
                src_pix_fmt = AV_PIX_FMT_YUYV422;  // 常见的USB摄像头格式
                DEBUG_PRINT(DEBUG_LEVEL_INFO, "[INFO] Assuming YUYV422 for rawvideo");
            }
        }
        else {
            src_pix_fmt = AV_PIX_FMT_YUVJ420P;  // 默认回退
            DEBUG_PRINT(DEBUG_LEVEL_INFO, "[INFO] Using default YUVJ420P");
        }
    }
    else {
        DEBUG_PRINT(DEBUG_LEVEL_INFO, "[INFO] Using detected pixel format: " << av_get_pix_fmt_name(src_pix_fmt));
    }

    cam.sws_ctx_yuv = sws_getContext(
        cam.codec_ctx->width, cam.codec_ctx->height, src_pix_fmt,
        cam.codec_ctx->width, cam.codec_ctx->height, AV_PIX_FMT_YUV420P,
        SWS_BILINEAR, nullptr, nullptr, nullptr);

    if (!cam.sws_ctx_yuv) {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR, "[ERROR] Could not create SWS context for camera " << index);
        DEBUG_PRINT(DEBUG_LEVEL_ERROR, "[ERROR] Source format: " << av_get_pix_fmt_name(src_pix_fmt));

        // 尝试备用像素格式
        DEBUG_PRINT(DEBUG_LEVEL_INFO, "[INFO] Trying backup pixel formats...");
        std::vector<AVPixelFormat> backup_formats = {
            AV_PIX_FMT_YUYV422,
            AV_PIX_FMT_RGB24,
            AV_PIX_FMT_BGR24,
            AV_PIX_FMT_YUVJ420P,
            AV_PIX_FMT_YUV420P
        };

        bool sws_created = false;
        for (auto backup_fmt : backup_formats) {
            cam.sws_ctx_yuv = sws_getContext(
                cam.codec_ctx->width, cam.codec_ctx->height, backup_fmt,
                cam.codec_ctx->width, cam.codec_ctx->height, AV_PIX_FMT_YUV420P,
                SWS_BILINEAR, nullptr, nullptr, nullptr);

            if (cam.sws_ctx_yuv) {
                DEBUG_PRINT(DEBUG_LEVEL_INFO, "[INFO] Successfully created SWS context with backup format: "
                    << av_get_pix_fmt_name(backup_fmt));
                sws_created = true;
                break;
            }
        }

        if (!sws_created) {
            DEBUG_PRINT(DEBUG_LEVEL_ERROR, "[ERROR] All pixel formats failed for camera " << index);
            av_freep(&cam.yuv_buffer);
            av_frame_free(&cam.frame);
            av_frame_free(&cam.yuv_frame);
            avcodec_free_context(&cam.codec_ctx);
            avformat_close_input(&cam.fmt_ctx);
            return false;
        }
    }

    DEBUG_PRINT(DEBUG_LEVEL_INFO, "[SUCCESS] Camera " << index << " initialized ("
        << cam.codec_ctx->width << "x" << cam.codec_ctx->height << ", "
        << avcodec_get_name(cam.codec_ctx->codec_id) << ")");

    return true;
}

void MultiCameraCapture::start() {
    if (!initialized_ || running_.load()) return;

    DEBUG_PRINT(DEBUG_LEVEL_INFO, "[MULTI] Starting capture threads");
    running_ = true;
    capture_active_ = true;

    capture_threads_.reserve(camera_count_);
    for (size_t i = 0; i < camera_count_; ++i) {
        capture_threads_.emplace_back(&MultiCameraCapture::capture_thread, this, static_cast<int>(i));
    }

    sync_thread_ = std::thread(&MultiCameraCapture::sync_loop, this);

    DEBUG_PRINT(DEBUG_LEVEL_INFO, "[MULTI] All threads started");
}

void MultiCameraCapture::stop() {
    if (!running_.load()) return;

    DEBUG_PRINT(DEBUG_LEVEL_INFO, "[MULTI] Stopping capture");
    running_ = false;
    capture_active_ = false;

    for (auto& t : capture_threads_) {
        if (t.joinable()) t.join();
    }
    if (sync_thread_.joinable()) sync_thread_.join();

    capture_threads_.clear();

    size_t cleared = force_clear_all_queues();
    if (cleared > 0) {
        DEBUG_PRINT(DEBUG_LEVEL_INFO, "[MULTI] Cleared " << cleared << " remaining frames");
    }

    if (stats_manager_) {
        stats_manager_->print_summary(" FINAL");
    }
}

void MultiCameraCapture::capture_thread(int camera_index) {
    if (camera_index < 0 || static_cast<size_t>(camera_index) >= cameras_.size()) return;

    auto& cam = cameras_[static_cast<size_t>(camera_index)];
    AVPacket* packet = av_packet_alloc();
    if (!packet) return;

    int consecutive_failures = 0;
    const int MAX_CONSECUTIVE_FAILURES = 10;

    DEBUG_PRINT(DEBUG_LEVEL_VERBOSE, "[CAP" << camera_index << "] Starting capture thread");

    while (running_) {
        if (!capture_active_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        int ret = av_read_frame(cam.fmt_ctx, packet);
        if (ret < 0) {
            consecutive_failures++;
            stats_manager_->update_camera_stats(camera_index, "read_fail");
            if (ret == AVERROR(EAGAIN)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            else if (consecutive_failures > MAX_CONSECUTIVE_FAILURES) {
                DEBUG_PRINT(DEBUG_LEVEL_ERROR, "[CAP" << camera_index << "] Too many failures, stopping");
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

        // 内存压力检查 - 针对多摄像头优化，增加阈值
        size_t memory_threshold = camera_count_ >= 4 ? 8000 : 5000; // 提高阈值
        if (g_active_frame_count.load() > memory_threshold) {
            stats_manager_->update_camera_stats(camera_index, "dropped_memory");
            av_packet_unref(packet);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }

        ret = avcodec_send_packet(cam.codec_ctx, packet);
        av_packet_unref(packet);

        if (ret < 0) {
            stats_manager_->update_camera_stats(camera_index, "decode_fail");
            if (ret == AVERROR(EAGAIN)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            continue;
        }

        ret = avcodec_receive_frame(cam.codec_ctx, cam.frame);
        if (ret < 0) {
            if (ret != AVERROR(EAGAIN)) {
                stats_manager_->update_camera_stats(camera_index, "decode_fail");
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            continue;
        }

        int64_t pts = av_gettime();
        stats_manager_->update_camera_stats(camera_index, "captured");

        {
            std::lock_guard<std::mutex> lock(queue_mutex_);

            // 动态队列大小控制 - 增加队列大小以配合更大的缓冲区
            size_t max_queue_size = camera_count_ >= 4 ? 12 : 15; // 增加队列大小

            if (frame_queues_[camera_index].size() >= max_queue_size) {
                stats_manager_->update_camera_stats(camera_index, "dropped_queue");
                free_cloned_frame(&frame_queues_[camera_index].front().frame);
                frame_queues_[camera_index].pop_front();
            }

            // YUV420P转换
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
                    frame_queues_[camera_index].emplace_back(TimestampedFrame{ cloned_yuv, pts });
                    g_total_frames_allocated.fetch_add(1);
                    g_active_frame_count.fetch_add(1);
                }
            }
        }

        // 定期统计报告
        if (stats_manager_->should_report()) {
            stats_manager_->print_summary();
        }

        std::this_thread::sleep_for(std::chrono::microseconds(500));
    }

    av_packet_free(&packet);
    DEBUG_PRINT(DEBUG_LEVEL_VERBOSE, "[CAP" << camera_index << "] Capture thread stopped");
}

void MultiCameraCapture::sync_loop() {
    // 根据摄像头数量调整同步间隔
    auto base_sync_interval = std::chrono::milliseconds(
        camera_count_ >= 4 ? 30 : (camera_count_ >= 3 ? 25 : 20) // 稍微增加间隔
    );
    auto current_sync_interval = base_sync_interval;
    int consecutive_failures = 0;

    DEBUG_PRINT(DEBUG_LEVEL_VERBOSE, "[SYNC] Starting sync loop for " << camera_count_ << " cameras");

    while (running_) {
        auto current_time = std::chrono::steady_clock::now();
        static auto last_sync_time = current_time;

        auto time_since_last_sync = current_time - last_sync_time;
        if (time_since_last_sync < current_sync_interval) {
            std::this_thread::sleep_for(current_sync_interval - time_since_last_sync);
            continue;
        }
        last_sync_time = current_time;

        std::vector<AVFrame*> sync_frames;
        bool found_sync = false;

        stats_manager_->update_sync_stats("attempt");

        // 检查是否可以进行同步
        bool can_sync = check_sync_conditions(sync_frames);

        if (can_sync) {
            found_sync = true;
            consecutive_failures = 0;
            current_sync_interval = base_sync_interval;
            stats_manager_->update_sync_stats("success");

            // 添加到同步队列
            add_to_sync_queue(sync_frames);
        }
        else {
            consecutive_failures++;
            if (consecutive_failures > 20) { // 增加阈值
                current_sync_interval = std::chrono::milliseconds(40);
            }
            else if (consecutive_failures > 40) {
                current_sync_interval = std::chrono::milliseconds(60);
            }

            // 智能丢帧
            smart_frame_dropping();
        }

        // 队列管理
        manage_queues();
    }

    DEBUG_PRINT(DEBUG_LEVEL_VERBOSE, "[SYNC] Sync loop stopped");
}

bool MultiCameraCapture::check_sync_conditions(std::vector<AVFrame*>& sync_frames) {
    std::lock_guard<std::mutex> lock(queue_mutex_);

    // 检查所有摄像头队列都有帧
    for (size_t i = 0; i < camera_count_; ++i) {
        if (frame_queues_[i].empty()) {
            stats_manager_->update_sync_stats("fail_no_frames");
            return false;
        }
    }

    // 时间戳同步检查 - 放宽同步阈值以适应更大的缓冲区
    std::vector<int64_t> timestamps;
    for (size_t i = 0; i < camera_count_; ++i) {
        timestamps.push_back(frame_queues_[i].front().timestamp_us);
    }

    if (timestamps.size() < 2) return false;

    int64_t max_diff = *std::max_element(timestamps.begin(), timestamps.end()) -
        *std::min_element(timestamps.begin(), timestamps.end());

    // 根据摄像头数量调整同步阈值 - 增加容忍度
    int64_t threshold = camera_count_ >= 4 ? 800000 : (camera_count_ >= 3 ? 600000 : 400000); // 增加阈值
    if (max_diff > threshold) {
        stats_manager_->update_sync_stats("fail_timestamp");
        return false;
    }

    // 准备同步帧
    for (size_t i = 0; i < camera_count_; ++i) {
        sync_frames.push_back(frame_queues_[i].front().frame);
        frame_queues_[i].pop_front();
    }

    return true;
}

void MultiCameraCapture::add_to_sync_queue(const std::vector<AVFrame*>& frames) {
    std::lock_guard<std::mutex> lock(queue_mutex_);

    // 根据摄像头数量调整同步队列大小 - 增加容量
    size_t max_sync_queue = camera_count_ >= 4 ? 15 : (camera_count_ >= 3 ? 18 : 20);
    if (synced_frame_queue_.size() >= max_sync_queue) {
        auto& old_frames = synced_frame_queue_.front();
        for (auto* frame : old_frames) {
            free_cloned_frame(&frame);
        }
        synced_frame_queue_.pop_front();
    }

    synced_frame_queue_.push_back(frames);
}

void MultiCameraCapture::smart_frame_dropping() {
    std::lock_guard<std::mutex> lock(queue_mutex_);

    // 找出队列最长的摄像头并丢弃最旧的帧
    size_t max_size = 0;
    size_t max_camera = SIZE_MAX;

    for (size_t i = 0; i < camera_count_; ++i) {
        if (frame_queues_[i].size() > max_size) {
            max_size = frame_queues_[i].size();
            max_camera = i;
        }
    }

    size_t drop_threshold = camera_count_ >= 4 ? 20 : 25; // 增加丢帧阈值
    if (max_camera != SIZE_MAX && max_size > drop_threshold) {
        free_cloned_frame(&frame_queues_[max_camera].front().frame);
        frame_queues_[max_camera].pop_front();
    }
}

void MultiCameraCapture::manage_queues() {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    // 在没有插值的情况下，这个函数主要用于监控
    // 可以在这里添加其他队列管理逻辑
}

// 获取同步帧的方法
std::vector<AVFrame*> MultiCameraCapture::get_sync_yuv420p_frames() {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    if (synced_frame_queue_.empty()) return {};

    auto frames = synced_frame_queue_.front();
    synced_frame_queue_.pop_front();
    return frames;
}

void MultiCameraCapture::set_sync_strategy(std::unique_ptr<SyncStrategy> strategy) {
    sync_strategy_ = std::move(strategy);
}

MemoryStats MultiCameraCapture::get_memory_stats() const {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    MemoryStats stats;
    stats.allocated_frames = g_total_frames_allocated.load();
    stats.freed_frames = g_total_frames_freed.load();
    stats.active_frames = g_active_frame_count.load();

    stats.raw_queue_sizes.resize(camera_count_);
    for (size_t i = 0; i < camera_count_; ++i) {
        stats.raw_queue_sizes[i] = frame_queues_[i].size();
    }
    stats.sync_queue_size = synced_frame_queue_.size();
    // 不再有插值队列
    stats.interpolated_queue_size = 0;
    return stats;
}

size_t MultiCameraCapture::get_sync_queue_size() const {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    return synced_frame_queue_.size();
}

void MultiCameraCapture::free_cloned_frame(AVFrame** frame) {
    if (frame && *frame) {
        if ((*frame)->data[0]) {
            av_freep(&(*frame)->data[0]);
        }
        av_frame_free(frame);
        *frame = nullptr;

        g_total_frames_freed.fetch_add(1);
        if (g_active_frame_count.load() > 0) {
            g_active_frame_count.fetch_sub(1);
        }
    }
}

AVFrame* MultiCameraCapture::clone_frame(const AVFrame* src) const {
    if (!src) return nullptr;

    AVFrame* dst = av_frame_alloc();
    if (!dst) return nullptr;

    dst->format = src->format;
    dst->width = src->width;
    dst->height = src->height;

    int ret = av_image_alloc(dst->data, dst->linesize,
        dst->width, dst->height, (AVPixelFormat)dst->format, 32);
    if (ret < 0) {
        av_frame_free(&dst);
        return nullptr;
    }

    av_image_copy(dst->data, dst->linesize,
        (const uint8_t* const*)src->data, src->linesize,
        (AVPixelFormat)dst->format, dst->width, dst->height);
    return dst;
}

void MultiCameraCapture::release_frame(AVFrame** frame) {
    free_cloned_frame(frame);
}

void MultiCameraCapture::cleanup_camera(int index) {
    if (index < 0 || static_cast<size_t>(index) >= cameras_.size()) return;

    auto& cam = cameras_[static_cast<size_t>(index)];

    if (cam.sws_ctx_yuv) {
        sws_freeContext(cam.sws_ctx_yuv);
        cam.sws_ctx_yuv = nullptr;
    }

    if (cam.yuv_frame) av_frame_free(&cam.yuv_frame);
    if (cam.frame) av_frame_free(&cam.frame);
    if (cam.yuv_buffer) av_freep(&cam.yuv_buffer);
    if (cam.codec_ctx) avcodec_free_context(&cam.codec_ctx);
    if (cam.fmt_ctx) avformat_close_input(&cam.fmt_ctx);

    cam.video_stream_idx = -1;
}

void MultiCameraCapture::full_reset() {
    stop();
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // 清理摄像头资源
    for (size_t i = 0; i < cameras_.size(); ++i) {
        cleanup_camera(static_cast<int>(i));
    }
    cameras_.clear();

    // 清理所有队列
    size_t total_freed = force_clear_all_queues();

    initialized_ = false;
    running_ = false;
    capture_active_ = false;
    camera_count_ = 0;

    if (total_freed > 0) {
        DEBUG_PRINT(DEBUG_LEVEL_INFO, "[RESET] Freed " << total_freed << " frames");
    }
}

size_t MultiCameraCapture::force_clear_all_queues() {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    size_t total_cleared = 0;

    // 清理原始队列
    for (auto& queue : frame_queues_) {
        while (!queue.empty()) {
            free_cloned_frame(&queue.front().frame);
            queue.pop_front();
            total_cleared++;
        }
    }

    // 清理同步队列
    while (!synced_frame_queue_.empty()) {
        auto& frames = synced_frame_queue_.front();
        for (auto* frame : frames) {
            free_cloned_frame(&frame);
        }
        synced_frame_queue_.pop_front();
        total_cleared += frames.size();
    }

    return total_cleared;
}

size_t MultiCameraCapture::emergency_memory_cleanup() {
    if (!queue_mutex_.try_lock()) return 0;
    std::lock_guard<std::mutex> lock(queue_mutex_, std::adopt_lock);

    size_t total_cleared = 0;

    // 清理原始队列，只保留最新的3帧（增加保留数量）
    for (auto& queue : frame_queues_) {
        while (queue.size() > 3) {
            free_cloned_frame(&queue.front().frame);
            queue.pop_front();
            total_cleared++;
        }
    }

    // 清理同步队列，只保留2个帧组
    while (synced_frame_queue_.size() > 2) {
        auto& frames = synced_frame_queue_.front();
        for (auto* frame : frames) {
            free_cloned_frame(&frame);
        }
        synced_frame_queue_.pop_front();
        total_cleared += frames.size();
    }

    return total_cleared;
}

// 其他辅助函数
size_t MultiCameraCapture::clear_queue(int camera_index, size_t max_to_clear) {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    if (camera_index < 0 || static_cast<size_t>(camera_index) >= frame_queues_.size()) return 0;

    size_t cleared = 0;
    auto& queue = frame_queues_[static_cast<size_t>(camera_index)];

    while (!queue.empty() && cleared < max_to_clear) {
        free_cloned_frame(&queue.front().frame);
        queue.pop_front();
        cleared++;
    }
    return cleared;
}

size_t MultiCameraCapture::balance_queues() {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    size_t total_balanced = 0;

    if (frame_queues_.size() < 2) return 0;

    size_t total_size = 0;
    for (const auto& queue : frame_queues_) {
        total_size += queue.size();
    }
    size_t avg_size = total_size / frame_queues_.size();

    for (auto& queue : frame_queues_) {
        while (queue.size() > avg_size + 3) { // 增加容忍度
            free_cloned_frame(&queue.front().frame);
            queue.pop_front();
            total_balanced++;
        }
    }
    return total_balanced;
}

void MultiCameraCapture::pause_capture() {
    capture_active_ = false;
}

void MultiCameraCapture::resume_capture() {
    capture_active_ = true;
}

void MultiCameraCapture::update_config(const SyncConfig& config) {
    config_ = config;
}

//==============================================================================
// 工厂函数实现 - 去除插值相关参数，优化缓冲区配置
//==============================================================================
namespace CameraCaptureFactory {
    std::unique_ptr<MultiCameraCapture> create_dual_camera(
        const std::vector<std::string>& device_paths) {

        if (device_paths.size() != 2) {
            throw std::invalid_argument("Dual camera requires exactly 2 device paths");
        }

        SyncConfig config;
        config.target_fps = 30;
        config.max_queue_size = 15;  // 增加队列大小
        config.max_sync_queue_size = 5;
        config.sync_threshold_us = 300000;  // 放宽同步阈值

        auto capture = std::make_unique<MultiCameraCapture>();
        if (!capture->init(device_paths, config)) {
            return nullptr;
        }
        return capture;
    }

    std::unique_ptr<MultiCameraCapture> create_triple_camera(
        const std::vector<std::string>& device_paths) {

        if (device_paths.size() != 3) {
            throw std::invalid_argument("Triple camera requires exactly 3 device paths");
        }

        SyncConfig config;
        config.target_fps = 28;
        config.max_queue_size = 18;  // 增加队列大小
        config.max_sync_queue_size = 8;
        config.sync_threshold_us = 500000;  // 放宽同步阈值

        auto capture = std::make_unique<MultiCameraCapture>();
        if (!capture->init(device_paths, config)) {
            return nullptr;
        }
        return capture;
    }

    std::unique_ptr<MultiCameraCapture> create_quad_camera(
        const std::vector<std::string>& device_paths) {

        if (device_paths.size() != 4) {
            throw std::invalid_argument("Quad camera requires exactly 4 device paths");
        }

        SyncConfig config;
        config.target_fps = 25;
        config.max_queue_size = 20;  // 显著增加队列大小
        config.max_sync_queue_size = 12;
        config.sync_threshold_us = 600000;  // 放宽同步阈值以适应4摄像头

        auto capture = std::make_unique<MultiCameraCapture>();
        if (!capture->init(device_paths, config)) {
            return nullptr;
        }
        return capture;
    }
}
// multi_camera_sync.cpp - 无插值版本：专注解决Camera 0帧捕获问题
#include "../includes/multi_camera_sync.h"
#include <iostream>
#include <stdexcept>
#include <atomic>
#include <algorithm>
#include <chrono>
#include <functional>
#include <numeric>    
#include <array>

// 全局统计变量
std::atomic<size_t> g_total_frames_allocated{ 0 };
std::atomic<size_t> g_total_frames_freed{ 0 };
std::atomic<size_t> g_active_frame_count{ 0 };

// 简化调试级别
#define DEBUG_ERROR 1
#define DEBUG_INFO 2
#ifndef DEBUG_LEVEL
#define DEBUG_LEVEL DEBUG_INFO
#endif

#define DEBUG_PRINT(level, ...) \
    do { if (DEBUG_LEVEL >= level) { std::cout << __VA_ARGS__ << std::endl; } } while(0)

//==============================================================================
// StatsManager 实现
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

    DEBUG_PRINT(DEBUG_INFO, "[STATS" << prefix << "] Runtime: " << elapsed << "s");

    for (size_t i = 0; i < camera_stats_.size(); ++i) {
        const auto& stats = camera_stats_[i];
        double fps = elapsed > 0 ? static_cast<double>(stats.frames_captured) / elapsed : 0;
        DEBUG_PRINT(DEBUG_INFO, "[CAM" << i << "] FPS: " << fps
            << ", Captured: " << stats.frames_captured
            << ", Dropped: " << (stats.frames_dropped_queue + stats.frames_dropped_memory)
            << ", Read fails: " << stats.read_failures);
    }

    double sync_rate = sync_stats_.sync_attempts > 0 ?
        static_cast<double>(sync_stats_.sync_success) / sync_stats_.sync_attempts * 100.0 : 0;
    DEBUG_PRINT(DEBUG_INFO, "[SYNC] Success rate: " << sync_rate << "%");
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

    if (total_attempts_.load() % 100 == 0) {
        double rate = total_success_.load() > 0 ?
            static_cast<double>(total_success_.load()) / total_attempts_.load() : 0.0;
        current_sync_rate_.store(rate);
    }
}

bool PerformanceOptimizer::should_skip_frame(size_t camera_index, size_t queue_size) {
    return false; // 禁用跳帧
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
    // 不调整，保持稳定
}

//==============================================================================
// 同步策略实现
//==============================================================================
std::vector<AVFrame*> TimestampSyncStrategy::find_sync_frames(
    const std::vector<std::deque<TimestampedFrame>>& queues,
    const SyncConfig& config) {

    if (queues.size() < 2) return {};

    for (const auto& queue : queues) {
        if (queue.empty()) return {};
    }

    std::vector<AVFrame*> sync_frames;
    for (const auto& queue : queues) {
        sync_frames.push_back(queue.front().frame);
    }
    return sync_frames;
}

//==============================================================================
// MultiCameraCapture 实现 - 无插值版本
//==============================================================================
MultiCameraCapture::MultiCameraCapture() {
    avdevice_register_all();
    sync_strategy_ = std::make_unique<TimestampSyncStrategy>();
    optimizer_ = std::make_unique<PerformanceOptimizer>();
}

MultiCameraCapture::~MultiCameraCapture() {
    full_reset();
    size_t leaked = g_active_frame_count.load();
    if (leaked > 0) {
        DEBUG_PRINT(DEBUG_ERROR, "[WARNING] Memory leak: " << leaked << " frames");
    }
}

void MultiCameraCapture::init_interpolation_system() {
    // 空实现，不使用插值
}

bool MultiCameraCapture::init(const std::vector<std::string>& device_paths, const SyncConfig& config) {
    if (initialized_) full_reset();
    if (device_paths.empty()) return false;

    camera_count_ = device_paths.size();
    config_ = config;
    stats_manager_ = std::make_unique<StatsManager>(camera_count_);

    DEBUG_PRINT(DEBUG_INFO, "[INIT] Initializing " << camera_count_ << " cameras (NO INTERPOLATION)");

    cameras_.resize(camera_count_);
    frame_queues_.resize(camera_count_);

    for (size_t i = 0; i < camera_count_; ++i) {
        if (!init_camera(static_cast<int>(i), device_paths[i])) {
            DEBUG_PRINT(DEBUG_ERROR, "[ERROR] Failed to init camera " << i);
            for (size_t j = 0; j < i; ++j) {
                cleanup_camera(static_cast<int>(j));
            }
            return false;
        }
    }

    initialized_ = true;
    DEBUG_PRINT(DEBUG_INFO, "[INIT] All cameras initialized successfully");
    return true;
}

bool MultiCameraCapture::init_camera(int index, const std::string& device_path) {
    auto& cam = cameras_[index];
    cam.index = index;
    cam.device_path = device_path;

    const AVInputFormat* input_format = av_find_input_format("dshow");
    AVDictionary* options = nullptr;

    // ✅ 修复：基于设备路径而非名称判断MJPEG
    bool is_mjpeg_camera = (device_path.find("USB Camera") != std::string::npos &&
        device_path.find("@device_pnp") == std::string::npos);
    bool is_camera0 = (index == 0);
    bool is_camera3 = (index == 3);
    bool is_complex_path = (device_path.find("@device_pnp") != std::string::npos);

    DEBUG_PRINT(DEBUG_INFO, "[INIT CAM" << index << "] Type: "
        << (is_mjpeg_camera ? "MJPEG" : "RAWVIDEO")
        << (is_camera0 ? " - CAMERA 0 SPECIAL CONFIG" : "")
        << (is_camera3 ? " - CAMERA 3 COMPLEX PATH" : "")
        << (is_complex_path ? " - USING DEVICE PATH" : ""));

    if (is_mjpeg_camera) {
        // MJPEG摄像头配置
        av_dict_set(&options, "vcodec", "mjpeg", 0);
        av_dict_set(&options, "video_size", "640x480", 0);
        av_dict_set(&options, "framerate", "30", 0);
        av_dict_set(&options, "rtbufsize", "134217728", 0);
        av_dict_set(&options, "buffer_size", "33554432", 0);
        av_dict_set(&options, "probesize", "32", 0);
        av_dict_set(&options, "analyzeduration", "100000", 0);
        av_dict_set(&options, "fflags", "nobuffer+flush_packets", 0);
        av_dict_set(&options, "flags", "low_delay", 0);
        av_dict_set(&options, "thread_queue_size", "64", 0);
        av_dict_set(&options, "max_delay", "0", 0);
    }
    else {
        // RAWVIDEO摄像头配置
        av_dict_set(&options, "video_size", "640x480", 0);
        av_dict_set(&options, "framerate", "30", 0);

        // Camera 0和Camera 3特殊优化配置
        if (is_camera0 || is_camera3) {
            av_dict_set(&options, "rtbufsize", "268435456", 0);    // 双倍缓冲区
            av_dict_set(&options, "buffer_size", "67108864", 0);   // 双倍缓冲区
            av_dict_set(&options, "probesize", "128", 0);          // 增加探测大小
            av_dict_set(&options, "analyzeduration", "1000000", 0); // 增加分析时间
            av_dict_set(&options, "thread_queue_size", "128", 0);  // 双倍线程队列

            if (is_camera3) {
                // Camera 3额外配置
                av_dict_set(&options, "pixel_format", "yuyv422", 0);  // 明确像素格式
                av_dict_set(&options, "input_format", "rawvideo", 0); // 明确输入格式
                DEBUG_PRINT(DEBUG_INFO, "[CAM3] Using enhanced configuration for complex path");
            }

            DEBUG_PRINT(DEBUG_INFO, "[CAM" << index << "] Using enhanced buffer configuration");
        }
        else {
            av_dict_set(&options, "rtbufsize", "134217728", 0);
            av_dict_set(&options, "buffer_size", "33554432", 0);
            av_dict_set(&options, "probesize", "64", 0);
            av_dict_set(&options, "analyzeduration", "500000", 0);
            av_dict_set(&options, "thread_queue_size", "64", 0);
        }

        av_dict_set(&options, "fflags", "nobuffer+flush_packets+discardcorrupt", 0);
        av_dict_set(&options, "flags", "low_delay", 0);
        av_dict_set(&options, "use_wallclock_as_timestamps", "1", 0);
    }

    DEBUG_PRINT(DEBUG_INFO, "[CAM" << index << "] Opening device: " << device_path.substr(0, 50)
        << (device_path.length() > 50 ? "..." : ""));

    int ret = avformat_open_input(&cam.fmt_ctx, device_path.c_str(), input_format, &options);
    av_dict_free(&options);

    if (ret != 0) {
        char error_buf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, error_buf, sizeof(error_buf));
        DEBUG_PRINT(DEBUG_ERROR, "[ERROR CAM" << index << "] Init failed: " << error_buf);

        // Camera 3特殊错误处理
        if (is_camera3) {
            DEBUG_PRINT(DEBUG_ERROR, "[CAM3 ERROR] Complex path failed, device may be busy or incompatible");
            DEBUG_PRINT(DEBUG_ERROR, "[CAM3 ERROR] Full path: " << device_path);
        }
        return false;
    }

    if (avformat_find_stream_info(cam.fmt_ctx, nullptr) < 0) {
        DEBUG_PRINT(DEBUG_ERROR, "[ERROR CAM" << index << "] Could not find stream info");
        if (is_camera3) {
            DEBUG_PRINT(DEBUG_ERROR, "[CAM3 ERROR] Stream info failed for complex path device");
        }
        avformat_close_input(&cam.fmt_ctx);
        return false;
    }

    cam.video_stream_idx = -1;
    for (unsigned int i = 0; i < cam.fmt_ctx->nb_streams; i++) {
        if (cam.fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            cam.video_stream_idx = i;
            break;
        }
    }

    if (cam.video_stream_idx == -1) {
        DEBUG_PRINT(DEBUG_ERROR, "[ERROR CAM" << index << "] No video stream found");
        avformat_close_input(&cam.fmt_ctx);
        return false;
    }

    AVCodecParameters* codecpar = cam.fmt_ctx->streams[cam.video_stream_idx]->codecpar;
    const AVCodec* codec = avcodec_find_decoder(codecpar->codec_id);
    if (!codec) {
        DEBUG_PRINT(DEBUG_ERROR, "[ERROR CAM" << index << "] Could not find decoder");
        avformat_close_input(&cam.fmt_ctx);
        return false;
    }

    cam.codec_ctx = avcodec_alloc_context3(codec);
    if (!cam.codec_ctx) {
        DEBUG_PRINT(DEBUG_ERROR, "[ERROR CAM" << index << "] Could not allocate codec context");
        avformat_close_input(&cam.fmt_ctx);
        return false;
    }

    if (avcodec_parameters_to_context(cam.codec_ctx, codecpar) < 0) {
        DEBUG_PRINT(DEBUG_ERROR, "[ERROR CAM" << index << "] Could not copy codec params");
        avcodec_free_context(&cam.codec_ctx);
        avformat_close_input(&cam.fmt_ctx);
        return false;
    }

    cam.codec_ctx->thread_count = 1;
    cam.codec_ctx->delay = 0;

    if (avcodec_open2(cam.codec_ctx, codec, nullptr) < 0) {
        DEBUG_PRINT(DEBUG_ERROR, "[ERROR CAM" << index << "] Could not open codec");
        avcodec_free_context(&cam.codec_ctx);
        avformat_close_input(&cam.fmt_ctx);
        return false;
    }

    // 分配帧缓冲区
    cam.frame = av_frame_alloc();
    cam.yuv_frame = av_frame_alloc();
    if (!cam.frame || !cam.yuv_frame) {
        DEBUG_PRINT(DEBUG_ERROR, "[ERROR CAM" << index << "] Could not allocate frames");
        return false;
    }

    int yuv_size = av_image_get_buffer_size(AV_PIX_FMT_YUV420P, cam.codec_ctx->width, cam.codec_ctx->height, 32);
    cam.yuv_buffer = (uint8_t*)av_malloc(yuv_size);
    if (!cam.yuv_buffer) {
        DEBUG_PRINT(DEBUG_ERROR, "[ERROR CAM" << index << "] Could not allocate YUV buffer");
        return false;
    }

    av_image_fill_arrays(cam.yuv_frame->data, cam.yuv_frame->linesize, cam.yuv_buffer,
        AV_PIX_FMT_YUV420P, cam.codec_ctx->width, cam.codec_ctx->height, 32);

    enum AVPixelFormat src_pix_fmt = cam.codec_ctx->pix_fmt;
    cam.sws_ctx_yuv = sws_getContext(
        cam.codec_ctx->width, cam.codec_ctx->height, src_pix_fmt,
        cam.codec_ctx->width, cam.codec_ctx->height, AV_PIX_FMT_YUV420P,
        SWS_BILINEAR, nullptr, nullptr, nullptr);

    if (!cam.sws_ctx_yuv) {
        DEBUG_PRINT(DEBUG_ERROR, "[ERROR CAM" << index << "] Could not create SWS context");
        return false;
    }

    // 输出实际获得的格式信息
    std::string actual_codec = avcodec_get_name(codecpar->codec_id);
    std::string pixel_format_name = av_get_pix_fmt_name((AVPixelFormat)codecpar->format);

    DEBUG_PRINT(DEBUG_INFO, "[SUCCESS CAM" << index << "] "
        << actual_codec << " (" << codecpar->width << "x" << codecpar->height
        << ", " << pixel_format_name << ")"
        << (is_camera0 ? " - CAMERA 0 READY" : ""));
    return true;
}

void MultiCameraCapture::start() {
    if (!initialized_ || running_.load()) return;

    DEBUG_PRINT(DEBUG_INFO, "[START] Starting camera capture (pure frame capture, no interpolation)");
    running_ = true;
    capture_active_ = true;

    capture_threads_.reserve(camera_count_);
    for (size_t i = 0; i < camera_count_; ++i) {
        capture_threads_.emplace_back(&MultiCameraCapture::capture_thread, this, static_cast<int>(i));
    }

    sync_thread_ = std::thread(&MultiCameraCapture::sync_loop, this);
    DEBUG_PRINT(DEBUG_INFO, "[START] All threads started successfully");
}

void MultiCameraCapture::stop() {
    if (!running_.load()) return;

    DEBUG_PRINT(DEBUG_INFO, "[STOP] Stopping capture system");
    running_ = false;
    capture_active_ = false;

    for (auto& t : capture_threads_) {
        if (t.joinable()) t.join();
    }
    if (sync_thread_.joinable()) sync_thread_.join();

    capture_threads_.clear();

    size_t cleared = force_clear_all_queues();
    if (cleared > 0) {
        DEBUG_PRINT(DEBUG_INFO, "[STOP] Cleared " << cleared << " remaining frames");
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

    // 时间戳标准化
    static std::atomic<int64_t> global_start_time{ 0 };
    static std::array<std::atomic<int64_t>, 4> camera_start_time{ 0 };
    static std::array<std::atomic<int>, 4> frame_counter{ 0 };
    static std::array<bool, 4> timing_initialized{ false };

    const int64_t TARGET_FRAME_INTERVAL_US = 33333; // 30fps
    bool is_mjpeg = (cam.codec_ctx->codec_id == AV_CODEC_ID_MJPEG);
    bool is_camera0 = (camera_index == 0);
    bool is_camera3 = (camera_index == 3);

    // Camera 0和Camera 3特殊监控变量
    static int cam0_successful_reads = 0;
    static int cam0_failed_reads = 0;
    static auto cam0_last_success = std::chrono::steady_clock::now();

    static int cam3_successful_reads = 0;
    static int cam3_failed_reads = 0;
    static auto cam3_last_success = std::chrono::steady_clock::now();
    static auto cam3_last_attempt = std::chrono::steady_clock::now();

    DEBUG_PRINT(DEBUG_INFO, "[CAP" << camera_index << "] Thread started ("
        << (is_mjpeg ? "MJPEG" : "RAWVIDEO")
        << (is_camera0 ? " - CAMERA 0 PRIORITY" : "")
        << (is_camera3 ? " - CAMERA 3 PRIORITY" : "") << ")");

    while (running_) {
        if (!capture_active_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }

        // ✅ Camera 3增强读取策略 - 比Camera 0更激进
        int max_attempts;
        if (is_camera3) {
            max_attempts = 15; // Camera 3最多尝试15次
        }
        else if (is_camera0) {
            max_attempts = 10; // Camera 0保持10次
        }
        else {
            max_attempts = is_mjpeg ? 5 : 3;
        }

        bool packet_read = false;

        for (int attempt = 0; attempt < max_attempts && !packet_read; ++attempt) {
            int ret = av_read_frame(cam.fmt_ctx, packet);

            if (ret >= 0 && packet->stream_index == cam.video_stream_idx) {
                packet_read = true;
                if (is_camera0) {
                    cam0_successful_reads++;
                    cam0_last_success = std::chrono::steady_clock::now();
                }
                else if (is_camera3) {
                    cam3_successful_reads++;
                    cam3_last_success = std::chrono::steady_clock::now();
                }
                break;
            }

            if (ret < 0) {
                stats_manager_->update_camera_stats(camera_index, "read_fail");
                if (is_camera0) {
                    cam0_failed_reads++;
                }
                else if (is_camera3) {
                    cam3_failed_reads++;
                }

                if (ret == AVERROR(EAGAIN)) {
                    if (is_camera3) {
                        std::this_thread::sleep_for(std::chrono::nanoseconds(500)); // 极短等待
                    }
                    else if (is_camera0) {
                        std::this_thread::sleep_for(std::chrono::microseconds(1));
                    }
                    else {
                        std::this_thread::sleep_for(std::chrono::microseconds(10));
                    }
                    continue;
                }
                else if (ret == AVERROR_EOF) {
                    DEBUG_PRINT(DEBUG_ERROR, "[CAP" << camera_index << "] End of stream");
                    if (is_camera3) {
                        DEBUG_PRINT(DEBUG_ERROR, "[CAM3 CRITICAL] End of stream - device disconnected?");
                    }
                    av_packet_free(&packet);
                    return;
                }
            }

            av_packet_unref(packet);

            // Camera 3特殊重试策略
            if (is_camera3) {
                std::this_thread::sleep_for(std::chrono::microseconds(1)); // 极短间隔重试
            }
            else if (is_camera0) {
                std::this_thread::sleep_for(std::chrono::microseconds(5));
            }
            else {
                std::this_thread::sleep_for(std::chrono::microseconds(50));
            }
        }

        // ✅ Camera 3详细错误监控
        if (!packet_read) {
            if (is_camera3) {
                static int cam3_empty_cycles = 0;
                cam3_empty_cycles++;

                auto now = std::chrono::steady_clock::now();
                cam3_last_attempt = now;

                if (cam3_empty_cycles % 50 == 0) { // 更频繁的报告
                    auto time_since_success = std::chrono::duration_cast<std::chrono::seconds>(
                        now - cam3_last_success).count();

                    DEBUG_PRINT(DEBUG_ERROR, "[CAM3 ALERT] Failed " << cam3_empty_cycles
                        << " cycles, " << time_since_success << "s since last success");
                    DEBUG_PRINT(DEBUG_ERROR, "[CAM3 STATUS] Success/Fail: "
                        << cam3_successful_reads << "/" << cam3_failed_reads);
                }

                // 每1000次失败尝试重启Camera 3
                if (cam3_empty_cycles % 1000 == 0) {
                    DEBUG_PRINT(DEBUG_ERROR, "[CAM3 RESTART] Attempting device restart after "
                        << cam3_empty_cycles << " failures");
                    // 这里可以添加设备重启逻辑
                }
            }
            else if (is_camera0) {
                static int cam0_empty_cycles = 0;
                cam0_empty_cycles++;
                if (cam0_empty_cycles % 100 == 0) {
                    DEBUG_PRINT(DEBUG_ERROR, "[CAM0 ALERT] Failed to read packet for "
                        << cam0_empty_cycles << " cycles");
                }
            }
            continue;
        }

        // 解码数据包
        int ret = avcodec_send_packet(cam.codec_ctx, packet);
        av_packet_unref(packet);

        if (ret < 0) {
            stats_manager_->update_camera_stats(camera_index, "decode_fail");
            if (is_camera3) {
                static int cam3_decode_fails = 0;
                cam3_decode_fails++;
                if (cam3_decode_fails % 10 == 0) {
                    DEBUG_PRINT(DEBUG_ERROR, "[CAM3 DECODE] Decode failure #" << cam3_decode_fails);
                }
            }
            continue;
        }

        while (true) {
            ret = avcodec_receive_frame(cam.codec_ctx, cam.frame);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                break;
            }
            else if (ret < 0) {
                stats_manager_->update_camera_stats(camera_index, "decode_fail");
                break;
            }

            // 成功解码，时间戳标准化
            int current_frame_count = frame_counter[camera_index].fetch_add(1) + 1;
            int64_t normalized_timestamp;

            if (!timing_initialized[camera_index]) {
                if (global_start_time.load() == 0) {
                    global_start_time.store(av_gettime());
                    camera_start_time[camera_index].store(global_start_time.load());
                    normalized_timestamp = 0;
                }
                else {
                    camera_start_time[camera_index].store(global_start_time.load());
                    normalized_timestamp = (current_frame_count - 1) * TARGET_FRAME_INTERVAL_US;
                }
                timing_initialized[camera_index] = true;
            }
            else {
                normalized_timestamp = (current_frame_count - 1) * TARGET_FRAME_INTERVAL_US;
            }

            stats_manager_->update_camera_stats(camera_index, "captured");

            // 添加帧到队列
            {
                std::lock_guard<std::mutex> lock(queue_mutex_);

                // ✅ Camera 3特殊队列管理
                size_t max_queue_size;
                if (is_camera3) {
                    max_queue_size = config_.max_queue_size * 4; // Camera 3特大队列
                }
                else {
                    max_queue_size = config_.max_queue_size * 2; // 其他摄像头2倍
                }

                // 保守的队列清理
                if (frame_queues_[camera_index].size() >= max_queue_size) {
                    size_t frames_to_remove = 1; // 所有摄像头都只清理1帧

                    for (size_t i = 0; i < frames_to_remove && !frame_queues_[camera_index].empty(); ++i) {
                        free_cloned_frame(&frame_queues_[camera_index].front().frame);
                        frame_queues_[camera_index].pop_front();
                        stats_manager_->update_camera_stats(camera_index, "dropped_queue");
                    }
                }

                // YUV转换并添加到队列
                if (cam.sws_ctx_yuv) {
                    int sws_ret = sws_scale(cam.sws_ctx_yuv,
                        (const uint8_t* const*)cam.frame->data, cam.frame->linesize,
                        0, cam.frame->height,
                        cam.yuv_frame->data, cam.yuv_frame->linesize);

                    if (sws_ret > 0) {
                        cam.yuv_frame->width = cam.frame->width;
                        cam.yuv_frame->height = cam.frame->height;
                        cam.yuv_frame->format = AV_PIX_FMT_YUV420P;

                        AVFrame* cloned_yuv = clone_frame(cam.yuv_frame);
                        if (cloned_yuv) {
                            frame_queues_[camera_index].emplace_back(
                                TimestampedFrame{ cloned_yuv, normalized_timestamp }
                            );
                            g_total_frames_allocated.fetch_add(1);
                            g_active_frame_count.fetch_add(1);

                            // Camera 0和Camera 3成功添加帧的监控
                            if (is_camera0 && current_frame_count % 20 == 0) {
                                DEBUG_PRINT(DEBUG_INFO, "[CAM0 SUCCESS] Frame " << current_frame_count
                                    << " added, queue: " << frame_queues_[camera_index].size());
                            }
                            else if (is_camera3 && current_frame_count % 10 == 0) {
                                DEBUG_PRINT(DEBUG_INFO, "[CAM3 SUCCESS] Frame " << current_frame_count
                                    << " added, queue: " << frame_queues_[camera_index].size());
                            }
                        }
                        else {
                            if (is_camera0) {
                                DEBUG_PRINT(DEBUG_ERROR, "[CAM0 ERROR] Failed to clone frame!");
                            }
                            else if (is_camera3) {
                                DEBUG_PRINT(DEBUG_ERROR, "[CAM3 ERROR] Failed to clone frame!");
                            }
                        }
                    }
                    else {
                        if (is_camera0) {
                            DEBUG_PRINT(DEBUG_ERROR, "[CAM0 ERROR] SWS scale failed: " << sws_ret);
                        }
                        else if (is_camera3) {
                            DEBUG_PRINT(DEBUG_ERROR, "[CAM3 ERROR] SWS scale failed: " << sws_ret);
                        }
                    }
                }
            }

            if (is_mjpeg) break; // MJPEG通常一次只产生一帧
        }

        // ✅ Camera 3状态报告
        if (is_camera3) {
            static auto last_cam3_report = std::chrono::steady_clock::now();
            auto now = std::chrono::steady_clock::now();

            if (std::chrono::duration_cast<std::chrono::seconds>(now - last_cam3_report).count() >= 3) {
                double success_rate = (cam3_successful_reads + cam3_failed_reads) > 0 ?
                    static_cast<double>(cam3_successful_reads) / (cam3_successful_reads + cam3_failed_reads) * 100.0 : 0.0;

                std::lock_guard<std::mutex> lock(queue_mutex_);
                DEBUG_PRINT(DEBUG_INFO, "[CAM3 REPORT] Success rate: " << success_rate
                    << "%, Queue: " << frame_queues_[3].size()
                    << ", Reads: " << cam3_successful_reads << "/" << (cam3_successful_reads + cam3_failed_reads));
                last_cam3_report = now;
            }
        }

        // Camera 0状态报告（保持原有逻辑）
        if (is_camera0) {
            static auto last_cam0_report = std::chrono::steady_clock::now();
            auto now = std::chrono::steady_clock::now();

            if (std::chrono::duration_cast<std::chrono::seconds>(now - last_cam0_report).count() >= 5) {
                double success_rate = (cam0_successful_reads + cam0_failed_reads) > 0 ?
                    static_cast<double>(cam0_successful_reads) / (cam0_successful_reads + cam0_failed_reads) * 100.0 : 0.0;

                std::lock_guard<std::mutex> lock(queue_mutex_);
                DEBUG_PRINT(DEBUG_INFO, "[CAM0 REPORT] Success rate: " << success_rate
                    << "%, Queue: " << frame_queues_[0].size()
                    << ", Reads: " << cam0_successful_reads << "/" << (cam0_successful_reads + cam0_failed_reads));
                last_cam0_report = now;
            }
        }

        // ✅ 线程睡眠时间 - Camera 3最高优先级
        if (is_camera3) {
            std::this_thread::sleep_for(std::chrono::nanoseconds(500));  // Camera 3极速响应
        }
        else if (is_camera0) {
            std::this_thread::sleep_for(std::chrono::microseconds(1));   // Camera 0次之
        }
        else if (is_mjpeg) {
            std::this_thread::sleep_for(std::chrono::microseconds(5));
        }
        else {
            std::this_thread::sleep_for(std::chrono::microseconds(10));
        }
    }

    av_packet_free(&packet);
    timing_initialized[camera_index] = false;
    frame_counter[camera_index].store(0);

    if (is_camera0) {
        DEBUG_PRINT(DEBUG_INFO, "[CAM0 FINAL] Successful: " << cam0_successful_reads
            << ", Failed: " << cam0_failed_reads);
    }
    else if (is_camera3) {
        DEBUG_PRINT(DEBUG_INFO, "[CAM3 FINAL] Successful: " << cam3_successful_reads
            << ", Failed: " << cam3_failed_reads);
    }

    DEBUG_PRINT(DEBUG_INFO, "[CAP" << camera_index << "] Thread stopped");
}

void MultiCameraCapture::sync_loop() {
    auto sync_interval = std::chrono::milliseconds(20); // 稍微宽松的同步间隔
    auto last_sync_time = std::chrono::steady_clock::now();

    DEBUG_PRINT(DEBUG_INFO, "[SYNC] Pure sync loop started (no interpolation)");

    while (running_) {
        auto current_time = std::chrono::steady_clock::now();
        auto time_since_last_sync = current_time - last_sync_time;

        if (time_since_last_sync < sync_interval) {
            std::this_thread::sleep_for(sync_interval - time_since_last_sync);
            continue;
        }
        last_sync_time = current_time;

        std::vector<AVFrame*> sync_frames;
        bool found_sync = false;

        stats_manager_->update_sync_stats("attempt");

        // ✅ 宽松的同步检查：允许Camera 3缺失，但其他摄像头必须有帧
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);

            // 计算有帧的摄像头
            std::vector<bool> camera_has_frames(camera_count_, false);
            size_t cameras_with_frames = 0;

            for (size_t i = 0; i < camera_count_; ++i) {
                if (!frame_queues_[i].empty()) {
                    camera_has_frames[i] = true;
                    cameras_with_frames++;
                }
            }

            // 检查同步条件
            bool basic_sync_ok = false;

            // 条件1：至少前3个摄像头（0,1,2）中有2个有帧
            size_t first_three_with_frames = 0;
            for (size_t i = 0; i < 3 && i < camera_count_; ++i) {
                if (camera_has_frames[i]) {
                    first_three_with_frames++;
                }
            }

            if (first_three_with_frames >= 2) {
                basic_sync_ok = true;
            }

            // 条件2：Camera 3单独有帧也可以尝试同步（如果其他摄像头都有帧）
            if (!basic_sync_ok && camera_has_frames[3] && cameras_with_frames >= 3) {
                basic_sync_ok = true;
            }

            if (basic_sync_ok) {
                found_sync = true;

                // 备用帧机制：为没有帧的摄像头使用最后一帧
                static std::vector<AVFrame*> last_frames(4, nullptr);

                for (size_t i = 0; i < camera_count_; ++i) {
                    TimestampedFrame selected_frame;

                    if (!frame_queues_[i].empty()) {
                        // 有新帧，正常使用
                        selected_frame = frame_queues_[i].front();
                        frame_queues_[i].pop_front();

                        // 更新备用帧
                        if (last_frames[i]) {
                            free_cloned_frame(&last_frames[i]);
                        }
                        last_frames[i] = clone_frame(selected_frame.frame);

                        sync_frames.push_back(selected_frame.frame);
                    }
                    else if (last_frames[i]) {
                        // 使用备用帧
                        selected_frame.frame = clone_frame(last_frames[i]);
                        selected_frame.timestamp_us = av_gettime();
                        sync_frames.push_back(selected_frame.frame);

                        if (i == 3) {
                            static int cam3_backup_count = 0;
                            cam3_backup_count++;
                            if (cam3_backup_count % 50 == 0) {
                                DEBUG_PRINT(DEBUG_INFO, "[CAM3 BACKUP] Using backup frame #" << cam3_backup_count);
                            }
                        }
                    }
                    else {
                        // 完全没有帧，创建黑帧
                        AVFrame* black_frame = av_frame_alloc();
                        if (black_frame) {
                            black_frame->format = AV_PIX_FMT_YUV420P;
                            black_frame->width = 640;
                            black_frame->height = 480;

                            if (av_image_alloc(black_frame->data, black_frame->linesize,
                                640, 480, AV_PIX_FMT_YUV420P, 32) >= 0) {

                                // 创建黑色帧
                                memset(black_frame->data[0], 16, 640 * 480);     // Y分量
                                memset(black_frame->data[1], 128, 640 * 480 / 4); // U分量
                                memset(black_frame->data[2], 128, 640 * 480 / 4); // V分量

                                sync_frames.push_back(black_frame);

                                if (i == 3) {
                                    static int cam3_black_count = 0;
                                    cam3_black_count++;
                                    if (cam3_black_count % 100 == 0) {
                                        DEBUG_PRINT(DEBUG_ERROR, "[CAM3 BLACK] Using black frame #" << cam3_black_count);
                                    }
                                }
                            }
                            else {
                                av_frame_free(&black_frame);
                                found_sync = false;
                                break;
                            }
                        }
                        else {
                            found_sync = false;
                            break;
                        }
                    }
                }
            }
        }

        if (found_sync) {
            stats_manager_->update_sync_stats("success");
            if (optimizer_) {
                optimizer_->update_sync_stats(true);
            }
            add_to_sync_queue(sync_frames);
        }
        else {
            if (optimizer_) {
                optimizer_->update_sync_stats(false);
            }
            stats_manager_->update_sync_stats("fail_no_frames");
        }

        // ✅ 更频繁的队列状态报告，特别关注Camera 3
        static int debug_counter = 0;
        debug_counter++;
        if (debug_counter % 150 == 0) { // 更频繁报告
            std::lock_guard<std::mutex> lock(queue_mutex_);
            std::cout << "[PURE SYNC] ";
            for (size_t i = 0; i < camera_count_; ++i) {
                std::string cam_type = (i == 0) ? "CAM0" :
                    (i == 2) ? "MJPEG" :
                    (i == 3) ? "CAM3" : "NORM";
                std::cout << "C" << i << "(" << cam_type << "):" << frame_queues_[i].size() << " ";
            }

            double sync_rate = optimizer_ ? optimizer_->get_sync_rate() * 100 : 0;
            std::cout << "| Sync: " << static_cast<int>(sync_rate) << "%";

            // ✅ Camera 3特殊状态显示
            if (frame_queues_[3].empty()) {
                std::cout << " [CAM3 EMPTY!]";
            }

            std::cout << std::endl;
        }

        // ✅ Camera 3专门诊断报告
        static int cam3_diagnostic_counter = 0;
        cam3_diagnostic_counter++;
        if (cam3_diagnostic_counter % 1000 == 0) {
            std::lock_guard<std::mutex> lock(queue_mutex_);

            bool cam3_empty = frame_queues_[3].empty();
            size_t cam3_queue_size = frame_queues_[3].size();

            if (cam3_empty) {
                DEBUG_PRINT(DEBUG_ERROR, "[CAM3 DIAGNOSTIC] Queue empty for "
                    << (cam3_diagnostic_counter * 20) << "ms");
            }
            else {
                DEBUG_PRINT(DEBUG_INFO, "[CAM3 DIAGNOSTIC] Queue healthy: "
                    << cam3_queue_size << " frames");
            }
        }
    }

    DEBUG_PRINT(DEBUG_INFO, "[SYNC] Pure sync loop stopped");
}

void MultiCameraCapture::add_to_sync_queue(const std::vector<AVFrame*>& frames) {
    std::lock_guard<std::mutex> lock(queue_mutex_);

    if (synced_frame_queue_.size() >= config_.max_sync_queue_size) {
        auto& old_frames = synced_frame_queue_.front();
        for (auto* frame : old_frames) {
            free_cloned_frame(&frame);
        }
        synced_frame_queue_.pop_front();
    }

    synced_frame_queue_.push_back(frames);
}

std::vector<AVFrame*> MultiCameraCapture::get_sync_yuv420p_frames() {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    if (synced_frame_queue_.empty()) return {};

    auto frames = synced_frame_queue_.front();
    synced_frame_queue_.pop_front();
    return frames;
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
    return stats;
}

size_t MultiCameraCapture::get_sync_queue_size() const {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    return synced_frame_queue_.size();
}

InterpolationStats MultiCameraCapture::get_interpolation_stats() const {
    // 返回空统计，不使用插值
    return InterpolationStats{};
}

void MultiCameraCapture::enable_interpolation_for_camera(int camera_index, bool enable) {
    // 空实现，不使用插值
}

// 内存管理和清理函数
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

    for (size_t i = 0; i < cameras_.size(); ++i) {
        cleanup_camera(static_cast<int>(i));
    }
    cameras_.clear();

    size_t total_freed = force_clear_all_queues();

    initialized_ = false;
    running_ = false;
    capture_active_ = false;
    camera_count_ = 0;

    if (total_freed > 0) {
        DEBUG_PRINT(DEBUG_INFO, "[RESET] Freed " << total_freed << " frames");
    }
}

size_t MultiCameraCapture::force_clear_all_queues() {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    size_t total_cleared = 0;

    for (auto& queue : frame_queues_) {
        while (!queue.empty()) {
            free_cloned_frame(&queue.front().frame);
            queue.pop_front();
            total_cleared++;
        }
    }

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
    size_t keep_frames = 5; // 统一保留5帧

    for (auto& queue : frame_queues_) {
        while (queue.size() > keep_frames) {
            free_cloned_frame(&queue.front().frame);
            queue.pop_front();
            total_cleared++;
        }
    }

    while (synced_frame_queue_.size() > 3) {
        auto& frames = synced_frame_queue_.front();
        for (auto* frame : frames) {
            free_cloned_frame(&frame);
        }
        synced_frame_queue_.pop_front();
        total_cleared += frames.size();
    }

    return total_cleared;
}

// 基础控制和查询函数
void MultiCameraCapture::pause_capture() {
    capture_active_ = false;
}

void MultiCameraCapture::resume_capture() {
    capture_active_ = true;
}

void MultiCameraCapture::update_config(const SyncConfig& config) {
    config_ = config;
}

size_t MultiCameraCapture::get_camera_count() const {
    return camera_count_;
}

void MultiCameraCapture::set_sync_strategy(std::unique_ptr<SyncStrategy> strategy) {
    sync_strategy_ = std::move(strategy);
}
// multi_camera_sync.cpp - 精简版本
#include "../includes/multi_camera_sync.h"
#include <iostream>
#include <stdexcept>
#include <atomic>
#include <algorithm>
#include <chrono>
#include <functional>
#include <numeric>

// 全局统计变量
std::atomic<size_t> g_total_frames_allocated{ 0 };
std::atomic<size_t> g_total_frames_freed{ 0 };
std::atomic<size_t> g_active_frame_count{ 0 };

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
    // 精简的统计输出，只在需要时调用
    std::lock_guard<std::mutex> lock(stats_mutex_);
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - start_time_).count();

    double sync_rate = sync_stats_.sync_attempts > 0 ?
        static_cast<double>(sync_stats_.sync_success) / sync_stats_.sync_attempts * 100.0 : 0;

    std::cout << "[FINAL] Runtime: " << elapsed << "s, Sync rate: " << sync_rate << "%" << std::endl;
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
    const SyncConfig& config)
{
    const size_t N = queues.size();
    if (N == 0) return {};

    // 检查所有队列是否有帧
    for (const auto& q : queues) {
        if (q.empty()) return {};
    }

    // 计算容差
    const double fps = (config.expected_fps > 0.1 ? config.expected_fps : 25.0);
    const int64_t tol_us = (config.timestamp_tolerance_us > 0)
        ? config.timestamp_tolerance_us
        : static_cast<int64_t>(3.0 * 1e6 / fps);

    // 获取队首时间戳
    std::vector<int64_t> heads(N);
    for (size_t i = 0; i < N; ++i) {
        heads[i] = queues[i].front().timestamp_us;
    }

    // 使用min-max方式检查同步
    int64_t t_min = *std::min_element(heads.begin(), heads.end());
    int64_t t_max = *std::max_element(heads.begin(), heads.end());

    if (t_max - t_min <= tol_us) {
        // 同步成功，返回帧指针
        std::vector<AVFrame*> sync_group(N);
        for (size_t i = 0; i < N; ++i) {
            sync_group[i] = queues[i].front().frame;
        }
        return sync_group;
    }

    return {}; // 同步失败
}

//==============================================================================
// MultiCameraCapture 实现
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
        std::cerr << "[WARNING] Memory leak: " << leaked << " frames" << std::endl;
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

    cameras_.resize(camera_count_);
    frame_queues_.resize(camera_count_);

    for (size_t i = 0; i < camera_count_; ++i) {
        if (!init_camera(static_cast<int>(i), device_paths[i])) {
            std::cerr << "[ERROR] Failed to init camera " << i << std::endl;
            for (size_t j = 0; j < i; ++j) {
                cleanup_camera(static_cast<int>(j));
            }
            return false;
        }
    }

    initialized_ = true;
    return true;
}

bool MultiCameraCapture::init_camera(int index, const std::string& device_path) {
    auto& cam = cameras_[index];
    cam.index = index;
    cam.device_path = device_path;

    const AVInputFormat* input_format = av_find_input_format("dshow");
    AVDictionary* options = nullptr;

    // 根据摄像头类型设置参数
    bool is_mjpeg_camera = (device_path.find("USB Camera") != std::string::npos &&
        device_path.find("@device_pnp") == std::string::npos);
    bool is_raw_camera = (device_path.find("@device_pnp") != std::string::npos);

    if (is_mjpeg_camera) {
        // MJPEG 摄像头配置
        av_dict_set(&options, "vcodec", "mjpeg", 0);
        av_dict_set(&options, "video_size", "640x480", 0);
        av_dict_set(&options, "framerate", "30", 0);
        av_dict_set(&options, "rtbufsize", "512M", 0);
        av_dict_set(&options, "buffer_size", "33554432", 0);
        av_dict_set(&options, "probesize", "32", 0);
        av_dict_set(&options, "analyzeduration", "100000", 0);
        av_dict_set(&options, "fflags", "nobuffer+flush_packets", 0);
        av_dict_set(&options, "flags", "low_delay", 0);
        av_dict_set(&options, "thread_queue_size", "64", 0);
        av_dict_set(&options, "max_delay", "0", 0);
        av_dict_set(&options, "use_wallclock_as_timestamps", "1", 0);
    }
    else if (is_raw_camera) {
        // RAW 摄像头配置
        av_dict_set(&options, "video_size", "640x480", 0);
        av_dict_set(&options, "framerate", "30", 0);
        av_dict_set(&options, "rtbufsize", "512M", 0);
        av_dict_set(&options, "buffer_size", "67108864", 0);
        av_dict_set(&options, "probesize", "128", 0);
        av_dict_set(&options, "analyzeduration", "1000000", 0);
        av_dict_set(&options, "thread_queue_size", "128", 0);
        av_dict_set(&options, "pixel_format", "yuyv422", 0);
        av_dict_set(&options, "input_format", "rawvideo", 0);
        av_dict_set(&options, "fflags", "nobuffer+flush_packets+discardcorrupt", 0);
        av_dict_set(&options, "flags", "low_delay", 0);
        av_dict_set(&options, "use_wallclock_as_timestamps", "1", 0);
    }
    else {
        // 默认配置
        av_dict_set(&options, "video_size", "640x480", 0);
        av_dict_set(&options, "framerate", "30", 0);
        av_dict_set(&options, "rtbufsize", "512M", 0);
        av_dict_set(&options, "buffer_size", "33554432", 0);
        av_dict_set(&options, "probesize", "64", 0);
        av_dict_set(&options, "analyzeduration", "500000", 0);
        av_dict_set(&options, "thread_queue_size", "64", 0);
        av_dict_set(&options, "fflags", "nobuffer+flush_packets", 0);
        av_dict_set(&options, "flags", "low_delay", 0);
        av_dict_set(&options, "use_wallclock_as_timestamps", "1", 0);
    }

    // 打开摄像头
    int ret = avformat_open_input(&cam.fmt_ctx, device_path.c_str(), input_format, &options);
    av_dict_free(&options);

    if (ret != 0) {
        char error_buf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, error_buf, sizeof(error_buf));
        std::cerr << "[ERROR] Camera " << index << " init failed: " << error_buf << std::endl;
        return false;
    }

    if (avformat_find_stream_info(cam.fmt_ctx, nullptr) < 0) {
        std::cerr << "[ERROR] Could not find stream info for camera " << index << std::endl;
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
        std::cerr << "[ERROR] No video stream found for camera " << index << std::endl;
        avformat_close_input(&cam.fmt_ctx);
        return false;
    }

    AVCodecParameters* codecpar = cam.fmt_ctx->streams[cam.video_stream_idx]->codecpar;
    const AVCodec* codec = avcodec_find_decoder(codecpar->codec_id);
    if (!codec) {
        std::cerr << "[ERROR] Could not find decoder for camera " << index << std::endl;
        avformat_close_input(&cam.fmt_ctx);
        return false;
    }

    cam.codec_ctx = avcodec_alloc_context3(codec);
    if (!cam.codec_ctx) {
        std::cerr << "[ERROR] Could not allocate codec context for camera " << index << std::endl;
        avformat_close_input(&cam.fmt_ctx);
        return false;
    }

    if (avcodec_parameters_to_context(cam.codec_ctx, codecpar) < 0) {
        std::cerr << "[ERROR] Could not copy codec params for camera " << index << std::endl;
        avcodec_free_context(&cam.codec_ctx);
        avformat_close_input(&cam.fmt_ctx);
        return false;
    }

    cam.codec_ctx->thread_count = 1;
    cam.codec_ctx->delay = 0;

    if (avcodec_open2(cam.codec_ctx, codec, nullptr) < 0) {
        std::cerr << "[ERROR] Could not open codec for camera " << index << std::endl;
        avcodec_free_context(&cam.codec_ctx);
        avformat_close_input(&cam.fmt_ctx);
        return false;
    }

    cam.frame = av_frame_alloc();
    cam.yuv_frame = av_frame_alloc();
    if (!cam.frame || !cam.yuv_frame) {
        std::cerr << "[ERROR] Could not allocate frames for camera " << index << std::endl;
        return false;
    }

    int yuv_size = av_image_get_buffer_size(AV_PIX_FMT_YUV420P,
        cam.codec_ctx->width, cam.codec_ctx->height, 32);
    cam.yuv_buffer = (uint8_t*)av_malloc(yuv_size);
    if (!cam.yuv_buffer) {
        std::cerr << "[ERROR] Could not allocate YUV buffer for camera " << index << std::endl;
        return false;
    }

    av_image_fill_arrays(cam.yuv_frame->data, cam.yuv_frame->linesize,
        cam.yuv_buffer, AV_PIX_FMT_YUV420P,
        cam.codec_ctx->width, cam.codec_ctx->height, 32);

    cam.sws_ctx_yuv = sws_getContext(
        cam.codec_ctx->width, cam.codec_ctx->height, cam.codec_ctx->pix_fmt,
        cam.codec_ctx->width, cam.codec_ctx->height, AV_PIX_FMT_YUV420P,
        SWS_BILINEAR, nullptr, nullptr, nullptr);

    if (!cam.sws_ctx_yuv) {
        std::cerr << "[ERROR] Could not create SWS context for camera " << index << std::endl;
        return false;
    }

    return true;
}

void MultiCameraCapture::start() {
    if (!initialized_ || running_.load()) return;

    running_ = true;
    capture_active_ = true;

    capture_threads_.reserve(camera_count_);
    for (size_t i = 0; i < camera_count_; ++i) {
        capture_threads_.emplace_back(&MultiCameraCapture::capture_thread, this, static_cast<int>(i));
    }

    sync_thread_ = std::thread(&MultiCameraCapture::sync_loop, this);
}

void MultiCameraCapture::stop() {
    if (!running_.load()) return;

    running_ = false;
    capture_active_ = false;

    for (auto& t : capture_threads_) {
        if (t.joinable()) t.join();
    }
    if (sync_thread_.joinable()) sync_thread_.join();

    capture_threads_.clear();

    size_t cleared = force_clear_all_queues();
    if (cleared > 0) {
        std::cout << "[STOP] Cleared " << cleared << " remaining frames" << std::endl;
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

    // 时间戳基准
    static int64_t program_start_time = 0;
    static std::once_flag start_time_flag;
    std::call_once(start_time_flag, []() {
        program_start_time = av_gettime_relative();
        });

    while (running_) {
        if (!capture_active_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }

        int ret = av_read_frame(cam.fmt_ctx, packet);
        if (ret < 0) {
            stats_manager_->update_camera_stats(camera_index, "read_fail");
            av_packet_unref(packet);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        int64_t ts_us = av_gettime_relative() - program_start_time;

        if (packet->stream_index != cam.video_stream_idx) {
            av_packet_unref(packet);
            continue;
        }

        ret = avcodec_send_packet(cam.codec_ctx, packet);
        av_packet_unref(packet);
        if (ret < 0) {
            stats_manager_->update_camera_stats(camera_index, "decode_fail");
            continue;
        }

        while (true) {
            ret = avcodec_receive_frame(cam.codec_ctx, cam.frame);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
            if (ret < 0) {
                stats_manager_->update_camera_stats(camera_index, "decode_fail");
                break;
            }

            // 转换为YUV420P
            if (cam.sws_ctx_yuv) {
                int sws_ret = sws_scale(cam.sws_ctx_yuv,
                    (const uint8_t* const*)cam.frame->data, cam.frame->linesize,
                    0, cam.frame->height,
                    cam.yuv_frame->data, cam.yuv_frame->linesize);
                if (sws_ret > 0) {
                    cam.yuv_frame->width = cam.frame->width;
                    cam.yuv_frame->height = cam.frame->height;
                    cam.yuv_frame->format = AV_PIX_FMT_YUV420P;

                    AVFrame* cloned = clone_frame(cam.yuv_frame);
                    if (cloned) {
                        std::lock_guard<std::mutex> lock(queue_mutex_);

                        // 队列管理
                        const size_t max_q = std::max<size_t>(config_.max_queue_size, 8);
                        while (frame_queues_[camera_index].size() >= max_q ||
                            (!frame_queues_[camera_index].empty() &&
                                ts_us - frame_queues_[camera_index].front().timestamp_us > config_.max_queue_age_us)) {
                            free_cloned_frame(&frame_queues_[camera_index].front().frame);
                            frame_queues_[camera_index].pop_front();
                            stats_manager_->update_camera_stats(camera_index, "dropped_queue");
                        }

                        frame_queues_[camera_index].push_back({ cloned, ts_us });
                        g_total_frames_allocated.fetch_add(1);
                        g_active_frame_count.fetch_add(1);
                        stats_manager_->update_camera_stats(camera_index, "captured");
                    }
                }
            }
        }
    }

    av_packet_free(&packet);
}

void MultiCameraCapture::sync_loop() {
    const auto tick = std::chrono::milliseconds(3);
    const size_t N = frame_queues_.size();
    if (N == 0) {
        std::cerr << "[ERROR] No frame queues, sync_loop exiting" << std::endl;
        return;
    }

    const double fps = (config_.expected_fps > 0 ? config_.expected_fps : 25.0);
    const int64_t tol_us = (config_.timestamp_tolerance_us > 0 ?
        config_.timestamp_tolerance_us : static_cast<int64_t>(3.0 * 1e6 / fps));

    while (running_) {
        std::this_thread::sleep_for(tick);
        stats_manager_->update_sync_stats("attempt");

        std::vector<AVFrame*> group(N, nullptr);
        std::lock_guard<std::mutex> lock(queue_mutex_);

        // 检查是否所有队列都有帧
        bool any_empty = false;
        std::vector<int64_t> heads(N);
        for (size_t i = 0; i < N; ++i) {
            if (frame_queues_[i].empty()) {
                any_empty = true;
                break;
            }
            heads[i] = frame_queues_[i].front().timestamp_us;
        }
        if (any_empty) continue;

        int64_t t_min = *std::min_element(heads.begin(), heads.end());
        int64_t t_max = *std::max_element(heads.begin(), heads.end());

        if (t_max - t_min <= 3 * tol_us) {
            // 同步成功
            for (size_t i = 0; i < N; ++i) {
                group[i] = frame_queues_[i].front().frame;
                frame_queues_[i].pop_front();
            }

            if (synced_frame_queue_.size() >= config_.max_sync_queue_size) {
                auto& old_frames = synced_frame_queue_.front();
                for (auto* f : old_frames) free_cloned_frame(&f);
                synced_frame_queue_.pop_front();
            }

            synced_frame_queue_.push_back(group);
            stats_manager_->update_sync_stats("success");
            optimizer_->update_sync_stats(true);
        }
        else {
            // 丢弃过早的帧
            int64_t drop_threshold = t_max - tol_us;
            for (size_t i = 0; i < N; ++i) {
                while (!frame_queues_[i].empty() &&
                    frame_queues_[i].front().timestamp_us < drop_threshold) {
                    free_cloned_frame(&frame_queues_[i].front().frame);
                    frame_queues_[i].pop_front();
                    stats_manager_->update_camera_stats(static_cast<int>(i), "skipped");
                }
            }
            stats_manager_->update_sync_stats("fail_timestamp");
            optimizer_->update_sync_stats(false);
        }
    }
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
    return InterpolationStats{};
}

void MultiCameraCapture::enable_interpolation_for_camera(int camera_index, bool enable) {
    // 空实现
}

// 内存管理函数
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
        std::cout << "[RESET] Freed " << total_freed << " frames" << std::endl;
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
    size_t keep_frames = 3; // 只保留3帧

    for (auto& queue : frame_queues_) {
        while (queue.size() > keep_frames) {
            free_cloned_frame(&queue.front().frame);
            queue.pop_front();
            total_cleared++;
        }
    }

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

// 基础控制函数
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
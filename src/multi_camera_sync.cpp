// multi_camera_sync.cpp - 优化版本 (从1500行优化到约800行)
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
// 统计管理器 - 集中管理所有统计功能
//==============================================================================
class StatsManager {
private:
    mutable std::mutex stats_mutex_;
    std::chrono::steady_clock::time_point start_time_;
    std::chrono::steady_clock::time_point last_report_time_;

    struct CameraStats {
        size_t frames_captured = 0;
        size_t frames_dropped_queue = 0;
        size_t frames_dropped_memory = 0;
        size_t decode_failures = 0;
        size_t read_failures = 0;
    };

    struct SyncStats {
        size_t sync_attempts = 0;
        size_t sync_success = 0;
        size_t sync_failures_no_frames = 0;
        size_t sync_failures_timestamp = 0;
        size_t sync_failures_no_interp = 0;
    };

    std::vector<CameraStats> camera_stats_;
    SyncStats sync_stats_;
    size_t interpolated_frames_ = 0;

public:
    StatsManager(size_t camera_count) : camera_stats_(camera_count) {
        start_time_ = last_report_time_ = std::chrono::steady_clock::now();
    }

    void update_camera_stats(int camera_id, const std::string& event, size_t count = 1) {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        if (camera_id >= 0 && camera_id < static_cast<int>(camera_stats_.size())) {
            auto& stats = camera_stats_[camera_id];
            if (event == "captured") stats.frames_captured += count;
            else if (event == "dropped_queue") stats.frames_dropped_queue += count;
            else if (event == "dropped_memory") stats.frames_dropped_memory += count;
            else if (event == "decode_fail") stats.decode_failures += count;
            else if (event == "read_fail") stats.read_failures += count;
        }
    }

    void update_sync_stats(const std::string& event, size_t count = 1) {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        if (event == "attempt") sync_stats_.sync_attempts += count;
        else if (event == "success") sync_stats_.sync_success += count;
        else if (event == "fail_no_frames") sync_stats_.sync_failures_no_frames += count;
        else if (event == "fail_timestamp") sync_stats_.sync_failures_timestamp += count;
        else if (event == "fail_no_interp") sync_stats_.sync_failures_no_interp += count;
    }

    void update_interpolation_stats(size_t count = 1) {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        interpolated_frames_ += count;
    }

    bool should_report(int interval_seconds = 30) {
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - last_report_time_).count() >= interval_seconds) {
            last_report_time_ = now;
            return true;
        }
        return false;
    }

    void print_summary(const std::string& prefix = "") {
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
        DEBUG_PRINT(DEBUG_LEVEL_INFO, "[SYNC] Success rate: " << sync_rate << "%, Interpolated: " << interpolated_frames_);
    }
};

//==============================================================================
// 同步策略实现 - 简化版本
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

std::vector<AVFrame*> InterpolationSyncStrategy::find_sync_frames(
    const std::vector<std::deque<TimestampedFrame>>& queues,
    const SyncConfig& config) {

    if (queues.size() < 2) return {};

    // 前两个摄像头使用正常同步
    if (queues[0].empty() || queues[1].empty()) return {};

    auto frame0 = queues[0].front();
    auto frame1 = queues[1].front();
    int64_t time_diff = std::abs(frame0.timestamp_us - frame1.timestamp_us);

    if (time_diff <= config.sync_threshold_us) {
        return { frame0.frame, frame1.frame, nullptr }; // 第三个位置留给插值
    }
    return {};
}

AVFrame* InterpolationSyncStrategy::interpolate_frame(const AVFrame* prev, const AVFrame* next, float t) const {
    if (!prev || !next || t < 0.0f || t > 1.0f) return nullptr;

    AVFrame* result = av_frame_alloc();
    if (!result) return nullptr;

    result->format = prev->format;
    result->width = prev->width;
    result->height = prev->height;

    int ret = av_image_alloc(result->data, result->linesize,
        result->width, result->height, (AVPixelFormat)result->format, 32);
    if (ret < 0) {
        av_frame_free(&result);
        return nullptr;
    }

    // YUV420P线性插值
    int y_size = prev->width * prev->height;
    for (int i = 0; i < y_size; ++i) {
        result->data[0][i] = (uint8_t)(prev->data[0][i] * (1.0f - t) + next->data[0][i] * t);
    }

    int uv_size = (prev->width / 2) * (prev->height / 2);
    for (int i = 0; i < uv_size; ++i) {
        result->data[1][i] = (uint8_t)(prev->data[1][i] * (1.0f - t) + next->data[1][i] * t);
        result->data[2][i] = (uint8_t)(prev->data[2][i] * (1.0f - t) + next->data[2][i] * t);
    }

    return result;
}

//==============================================================================
// MultiCameraCapture 实现 - 大幅简化
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

bool MultiCameraCapture::init_camera(int index, const std::string& device_path) {
    auto& cam = cameras_[index];
    cam.index = index;
    cam.device_path = device_path;

    const AVInputFormat* input_format = av_find_input_format("dshow");
    AVDictionary* options = nullptr;

    // 统一的摄像头配置，减少复杂的特殊处理
    av_dict_set(&options, "video_size", "640x480", 0);
    av_dict_set(&options, "framerate", "30", 0);
    av_dict_set(&options, "vcodec", "mjpeg", 0);
    av_dict_set(&options, "rtbufsize", "4194304", 0);
    av_dict_set(&options, "buffer_size", "1048576", 0);
    av_dict_set(&options, "probesize", "32", 0);
    av_dict_set(&options, "analyzeduration", "50000", 0);
    av_dict_set(&options, "fflags", "nobuffer", 0);
    av_dict_set(&options, "flags", "low_delay", 0);
    av_dict_set(&options, "thread_queue_size", "3", 0);

    DEBUG_PRINT(DEBUG_LEVEL_VERBOSE, "[INIT] Opening camera " << index << " at " << device_path);

    int ret = avformat_open_input(&cam.fmt_ctx, device_path.c_str(), input_format, &options);
    av_dict_free(&options);

    if (ret != 0) {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR, "[ERROR] Could not open camera " << index);
        return false;
    }

    if (avformat_find_stream_info(cam.fmt_ctx, nullptr) < 0) {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR, "[ERROR] Could not find stream info for camera " << index);
        return false;
    }

    // 查找视频流和设置解码器 (简化版本)
    cam.video_stream_idx = -1;
    for (unsigned int i = 0; i < cam.fmt_ctx->nb_streams; i++) {
        if (cam.fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            cam.video_stream_idx = i;
            break;
        }
    }

    if (cam.video_stream_idx == -1) return false;

    AVCodecParameters* codecpar = cam.fmt_ctx->streams[cam.video_stream_idx]->codecpar;
    const AVCodec* codec = avcodec_find_decoder(codecpar->codec_id);
    if (!codec) return false;

    cam.codec_ctx = avcodec_alloc_context3(codec);
    if (!cam.codec_ctx) return false;

    if (avcodec_parameters_to_context(cam.codec_ctx, codecpar) < 0) return false;

    // 简化的解码器配置
    cam.codec_ctx->thread_count = 1;
    cam.codec_ctx->delay = 0;

    if (avcodec_open2(cam.codec_ctx, codec, nullptr) < 0) return false;

    // 分配帧缓冲区
    cam.frame = av_frame_alloc();
    cam.yuv_frame = av_frame_alloc();
    if (!cam.frame || !cam.yuv_frame) return false;

    int yuv_size = av_image_get_buffer_size(AV_PIX_FMT_YUV420P, cam.codec_ctx->width, cam.codec_ctx->height, 32);
    cam.yuv_buffer = (uint8_t*)av_malloc(yuv_size);
    if (!cam.yuv_buffer) return false;

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

    if (!cam.sws_ctx_yuv) return false;

    DEBUG_PRINT(DEBUG_LEVEL_INFO, "[SUCCESS] Camera " << index << " initialized ("
        << cam.codec_ctx->width << "x" << cam.codec_ctx->height << ")");

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

    if (config_.enable_interpolation && !config_.interpolation_cameras.empty()) {
        interpolation_thread_ = std::thread(&MultiCameraCapture::interpolation_loop, this);
    }

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
    if (interpolation_thread_.joinable()) interpolation_thread_.join();

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
    if (camera_index >= static_cast<int>(cameras_.size())) return;

    auto& cam = cameras_[camera_index];
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

        // 内存压力检查
        if (g_active_frame_count.load() > (camera_count_ <= 2 ? 3000 : 4000)) {
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

            size_t max_queue_size = (camera_index == 2) ? 5 : 10;

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
    auto base_sync_interval = std::chrono::milliseconds(camera_count_ >= 4 ? 20 : 15);
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
            if (consecutive_failures > 15) {
                current_sync_interval = std::chrono::milliseconds(35);
            }
            else if (consecutive_failures > 30) {
                current_sync_interval = std::chrono::milliseconds(50);
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

    // 检查基本条件
    for (size_t i = 0; i < camera_count_; ++i) {
        if (i == 2) continue; // 跳过插值摄像头
        if (frame_queues_[i].empty()) {
            stats_manager_->update_sync_stats("fail_no_frames");
            return false;
        }
    }

    // 时间戳同步检查
    std::vector<int64_t> timestamps;
    for (size_t i = 0; i < camera_count_; ++i) {
        if (i == 2) continue;
        timestamps.push_back(frame_queues_[i].front().timestamp_us);
    }

    if (timestamps.size() < 2) return false;

    int64_t max_diff = *std::max_element(timestamps.begin(), timestamps.end()) -
        *std::min_element(timestamps.begin(), timestamps.end());

    int64_t threshold = camera_count_ >= 4 ? 800000 : 600000; // 更宽松的阈值
    if (max_diff > threshold) {
        stats_manager_->update_sync_stats("fail_timestamp");
        return false;
    }

    // 准备同步帧
    for (size_t i = 0; i < camera_count_; ++i) {
        if (i == 2) {
            // 从插值队列获取
            if (interpolated_queue_.empty()) {
                stats_manager_->update_sync_stats("fail_no_interp");
                return false;
            }
            sync_frames.push_back(interpolated_queue_.front().frame);
            interpolated_queue_.pop_front();
        }
        else {
            sync_frames.push_back(frame_queues_[i].front().frame);
            frame_queues_[i].pop_front();
        }
    }

    return true;
}

void MultiCameraCapture::add_to_sync_queue(const std::vector<AVFrame*>& frames) {
    std::lock_guard<std::mutex> lock(queue_mutex_);

    size_t max_sync_queue = camera_count_ >= 4 ? 15 : 12;
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
    int max_camera = -1;

    for (size_t i = 0; i < camera_count_; ++i) {
        if (i == 2) continue; // 跳过插值摄像头
        if (frame_queues_[i].size() > max_size) {
            max_size = frame_queues_[i].size();
            max_camera = i;
        }
    }

    if (max_camera >= 0 && max_size > 20) {
        free_cloned_frame(&frame_queues_[max_camera].front().frame);
        frame_queues_[max_camera].pop_front();
    }
}

void MultiCameraCapture::manage_queues() {
    std::lock_guard<std::mutex> lock(queue_mutex_);

    // 管理插值队列
    size_t max_interp_queue = camera_count_ >= 4 ? 40 : 30;
    while (interpolated_queue_.size() > max_interp_queue) {
        free_cloned_frame(&interpolated_queue_.front().frame);
        interpolated_queue_.pop_front();
    }
}

void MultiCameraCapture::interpolation_loop() {
    if (!config_.enable_interpolation || config_.interpolation_cameras.empty()) return;

    int camera_idx = config_.interpolation_cameras[0];
    const int INTERPOLATION_FACTOR = 4;

    DEBUG_PRINT(DEBUG_LEVEL_VERBOSE, "[INTERP] Starting interpolation for camera " << camera_idx);

    while (running_) {
        TimestampedFrame real_frame;
        bool has_real_frame = false;

        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            if (!frame_queues_[camera_idx].empty()) {
                real_frame = frame_queues_[camera_idx].front();
                frame_queues_[camera_idx].pop_front();
                has_real_frame = true;
            }
        }

        if (has_real_frame) {
            std::lock_guard<std::mutex> lock(interpolation_mutex_);

            auto& frame_pair = interpolation_frame_pairs_[camera_idx];
            auto& timestamp_pair = interpolation_timestamps_[camera_idx];

            if (frame_pair.first) {
                free_cloned_frame(&frame_pair.first);
            }
            frame_pair.first = frame_pair.second;
            frame_pair.second = real_frame.frame;
            timestamp_pair.first = timestamp_pair.second;
            timestamp_pair.second = real_frame.timestamp_us;

            if (frame_pair.first && frame_pair.second) {
                int64_t time_diff = timestamp_pair.second - timestamp_pair.first;

                if (time_diff > 0 && time_diff < 500000) {
                    // 生成插值帧
                    for (int i = 1; i <= INTERPOLATION_FACTOR; ++i) {
                        float t = i / static_cast<float>(INTERPOLATION_FACTOR + 1);
                        AVFrame* interpolated = interpolate_frame_simple(frame_pair.first, frame_pair.second, t);
                        if (interpolated) {
                            int64_t interp_timestamp = timestamp_pair.first + (int64_t)(time_diff * t);

                            {
                                std::lock_guard<std::mutex> queue_lock(queue_mutex_);
                                if (interpolated_queue_.size() < 40) {
                                    interpolated_queue_.emplace_back(TimestampedFrame{ interpolated, interp_timestamp });
                                    stats_manager_->update_interpolation_stats();
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
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    DEBUG_PRINT(DEBUG_LEVEL_VERBOSE, "[INTERP] Interpolation loop stopped");
}

// 简化的插值函数
AVFrame* MultiCameraCapture::interpolate_frame_simple(const AVFrame* prev, const AVFrame* next, float t) const {
    if (!prev || !next || t < 0.0f || t > 1.0f) return nullptr;

    AVFrame* result = av_frame_alloc();
    if (!result) return nullptr;

    result->format = prev->format;
    result->width = prev->width;
    result->height = prev->height;

    int ret = av_image_alloc(result->data, result->linesize,
        result->width, result->height, (AVPixelFormat)result->format, 32);
    if (ret < 0) {
        av_frame_free(&result);
        return nullptr;
    }

    // YUV420P线性插值
    int y_size = prev->width * prev->height;
    for (int i = 0; i < y_size; ++i) {
        result->data[0][i] = (uint8_t)(prev->data[0][i] * (1.0f - t) + next->data[0][i] * t);
    }

    int uv_size = (prev->width / 2) * (prev->height / 2);
    for (int i = 0; i < uv_size; ++i) {
        result->data[1][i] = (uint8_t)(prev->data[1][i] * (1.0f - t) + next->data[1][i] * t);
        result->data[2][i] = (uint8_t)(prev->data[2][i] * (1.0f - t) + next->data[2][i] * t);
    }

    return result;
}

// 其他必要的方法实现...
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
    stats.interpolated_queue_size = interpolated_queue_.size();
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
    if (index >= static_cast<int>(cameras_.size())) return;

    auto& cam = cameras_[index];

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
    for (int i = 0; i < static_cast<int>(cameras_.size()); ++i) {
        cleanup_camera(i);
    }
    cameras_.clear();

    // 清理所有队列
    size_t total_freed = force_clear_all_queues();

    // 清理插值相关
    {
        std::lock_guard<std::mutex> lock(interpolation_mutex_);
        for (auto& pair : interpolation_frame_pairs_) {
            if (pair.second.first) free_cloned_frame(&pair.second.first);
            if (pair.second.second) free_cloned_frame(&pair.second.second);
        }
        interpolation_frame_pairs_.clear();
        interpolation_timestamps_.clear();
    }

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

    // 清理插值队列
    while (!interpolated_queue_.empty()) {
        free_cloned_frame(&interpolated_queue_.front().frame);
        interpolated_queue_.pop_front();
        total_cleared++;
    }

    return total_cleared;
}

size_t MultiCameraCapture::emergency_memory_cleanup() {
    if (!queue_mutex_.try_lock()) return 0;
    std::lock_guard<std::mutex> lock(queue_mutex_, std::adopt_lock);

    size_t total_cleared = 0;

    // 清理原始队列，只保留最新的2帧
    for (auto& queue : frame_queues_) {
        while (queue.size() > 2) {
            free_cloned_frame(&queue.front().frame);
            queue.pop_front();
            total_cleared++;
        }
    }

    // 清理同步队列，只保留1个帧组
    while (synced_frame_queue_.size() > 1) {
        auto& frames = synced_frame_queue_.front();
        for (auto* frame : frames) {
            free_cloned_frame(&frame);
        }
        synced_frame_queue_.pop_front();
        total_cleared += frames.size();
    }

    // 清理插值队列
    while (interpolated_queue_.size() > 5) {
        free_cloned_frame(&interpolated_queue_.front().frame);
        interpolated_queue_.pop_front();
        total_cleared++;
    }

    return total_cleared;
}

// 其他辅助函数的简化实现...
size_t MultiCameraCapture::clear_queue(int camera_index, size_t max_to_clear) {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    if (camera_index >= static_cast<int>(frame_queues_.size())) return 0;

    size_t cleared = 0;
    auto& queue = frame_queues_[camera_index];

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
        while (queue.size() > avg_size + 2) {
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
// 工厂函数实现
//==============================================================================
namespace CameraCaptureFactory {
    std::unique_ptr<MultiCameraCapture> create_dual_camera(
        const std::vector<std::string>& device_paths) {

        if (device_paths.size() != 2) {
            throw std::invalid_argument("Dual camera requires exactly 2 device paths");
        }

        SyncConfig config;
        config.target_fps = 30;
        config.max_queue_size = 10;
        config.max_sync_queue_size = 2;
        config.sync_threshold_us = 150000;
        config.enable_interpolation = false;

        auto capture = std::make_unique<MultiCameraCapture>();
        if (!capture->init(device_paths, config)) {
            return nullptr;
        }
        return capture;
    }

    std::unique_ptr<MultiCameraCapture> create_triple_camera_with_interpolation(
        const std::vector<std::string>& device_paths,
        int interpolation_camera_index) {

        if (device_paths.size() != 3) {
            throw std::invalid_argument("Triple camera requires exactly 3 device paths");
        }

        SyncConfig config;
        config.target_fps = 20;
        config.max_queue_size = 15;
        config.max_sync_queue_size = 5;
        config.sync_threshold_us = 300000;
        config.enable_interpolation = true;
        config.interpolation_cameras = { interpolation_camera_index };

        auto capture = std::make_unique<MultiCameraCapture>();
        if (!capture->init(device_paths, config)) {
            return nullptr;
        }
        return capture;
    }

    std::unique_ptr<MultiCameraCapture> create_quad_camera_with_interpolation(
        const std::vector<std::string>& device_paths,
        int interpolation_camera_index) {

        if (device_paths.size() != 4) {
            throw std::invalid_argument("Quad camera requires exactly 4 device paths");
        }

        SyncConfig config;
        config.target_fps = 18;
        config.max_queue_size = 20;
        config.max_sync_queue_size = 8;
        config.sync_threshold_us = 400000;
        config.enable_interpolation = true;
        config.interpolation_cameras = { interpolation_camera_index };

        auto capture = std::make_unique<MultiCameraCapture>();
        if (!capture->init(device_paths, config)) {
            return nullptr;
        }
        return capture;
    }
}
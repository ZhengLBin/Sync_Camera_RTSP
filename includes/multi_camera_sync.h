// multi_camera_sync.h - 简化版本（无插值系统）
#pragma once

#include <memory>
#include <vector>
#include <deque>
#include <mutex>
#include <thread>
#include <atomic>
#include <chrono>
#include <string>
#include <functional>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavdevice/avdevice.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
#include <libavutil/time.h>
}

//==============================================================================
// 配置结构体
//==============================================================================
struct SyncConfig {
    int target_fps = 30;
    size_t max_queue_size = 15;
    size_t max_sync_queue_size = 5;
    int64_t sync_threshold_us = 400000;

    // 新增优化参数
    size_t frame_drop_threshold = 20;
    size_t emergency_cleanup_threshold = 40;
    int balance_interval_ms = 50;
    bool enable_smart_sync = true;
    bool enable_aggressive_cleanup = true;
};

//==============================================================================
// 统计结构体
//==============================================================================
struct CameraStats {
    size_t frames_captured = 0;
    size_t frames_dropped_queue = 0;
    size_t frames_dropped_memory = 0;
    size_t frames_skipped = 0;
    size_t decode_failures = 0;
    size_t read_failures = 0;
};

struct SyncStats {
    size_t sync_attempts = 0;
    size_t sync_success = 0;
    size_t sync_failures_no_frames = 0;
    size_t sync_failures_timestamp = 0;
    size_t memory_cleanups = 0;
};

struct MemoryStats {
    size_t allocated_frames = 0;
    size_t freed_frames = 0;
    size_t active_frames = 0;
    std::vector<size_t> raw_queue_sizes;
    size_t sync_queue_size = 0;
    size_t interpolated_queue_size = 0;
};

//==============================================================================
// 时间戳帧结构体
//==============================================================================
struct TimestampedFrame {
    AVFrame* frame;
    int64_t timestamp_us;

    TimestampedFrame() : frame(nullptr), timestamp_us(0) {}
    TimestampedFrame(AVFrame* f, int64_t ts) : frame(f), timestamp_us(ts) {}
    TimestampedFrame(const TimestampedFrame& other) : frame(other.frame), timestamp_us(other.timestamp_us) {}
    TimestampedFrame& operator=(const TimestampedFrame& other) {
        if (this != &other) {
            frame = other.frame;
            timestamp_us = other.timestamp_us;
        }
        return *this;
    }
};

//==============================================================================
// 统计管理器
//==============================================================================
class StatsManager {
public:
    explicit StatsManager(size_t camera_count);

    void update_camera_stats(int camera_id, const std::string& event, size_t count = 1);
    void update_sync_stats(const std::string& event, size_t count = 1);
    bool should_report(int interval_seconds = 30);
    void print_summary(const std::string& prefix = "");

private:
    std::vector<CameraStats> camera_stats_;
    SyncStats sync_stats_;
    std::chrono::steady_clock::time_point start_time_;
    std::chrono::steady_clock::time_point last_report_time_;
    std::mutex stats_mutex_;
};

//==============================================================================
// 性能优化器
//==============================================================================
class PerformanceOptimizer {
public:
    PerformanceOptimizer();

    void update_sync_stats(bool success);
    bool should_skip_frame(size_t camera_index, size_t queue_size);
    double get_sync_rate() const;
    size_t get_total_attempts() const;
    size_t get_total_success() const;

private:
    void adjust_frame_skip_strategy(double sync_rate);

    std::atomic<size_t> total_attempts_{ 0 };
    std::atomic<size_t> total_success_{ 0 };
    std::atomic<double> current_sync_rate_{ 0.0 };
    std::atomic<int> frame_skip_factor_{ 1 };
    std::chrono::steady_clock::time_point last_adjustment_time_;
};

//==============================================================================
// 同步策略接口
//==============================================================================
class SyncStrategy {
public:
    virtual ~SyncStrategy() = default;
    virtual std::vector<AVFrame*> find_sync_frames(
        const std::vector<std::deque<TimestampedFrame>>& queues,
        const SyncConfig& config) = 0;
};

class TimestampSyncStrategy : public SyncStrategy {
public:
    std::vector<AVFrame*> find_sync_frames(
        const std::vector<std::deque<TimestampedFrame>>& queues,
        const SyncConfig& config) override;
};

//==============================================================================
// 摄像头信息结构体
//==============================================================================
struct CameraInfo {
    int index = -1;
    std::string device_path;

    AVFormatContext* fmt_ctx = nullptr;
    AVCodecContext* codec_ctx = nullptr;
    int video_stream_idx = -1;

    AVFrame* frame = nullptr;
    AVFrame* yuv_frame = nullptr;
    uint8_t* yuv_buffer = nullptr;

    SwsContext* sws_ctx_yuv = nullptr;
};

struct InterpolationStats {
    std::vector<size_t> original_frames;
    std::vector<size_t> interpolated_frames;
    std::vector<double> interpolation_rates;
    std::vector<size_t> current_queue_sizes;
    size_t total_interpolations = 0;
};

//==============================================================================
// 多摄像头捕获器
//==============================================================================
class MultiCameraCapture {
public:
    MultiCameraCapture();
    ~MultiCameraCapture();

    // 基础接口
    bool init(const std::vector<std::string>& device_paths, const SyncConfig& config);
    void start();
    void stop();

    // 帧获取接口
    std::vector<AVFrame*> get_sync_yuv420p_frames();
    void release_frame(AVFrame** frame);

    // 配置和策略
    void set_sync_strategy(std::unique_ptr<SyncStrategy> strategy);
    void update_config(const SyncConfig& config);

    // 状态查询
    size_t get_camera_count() const;
    size_t get_sync_queue_size() const;
    MemoryStats get_memory_stats() const;

    // 控制接口
    void pause_capture();
    void resume_capture();
    size_t emergency_memory_cleanup();

    // 插值接口（兼容性保留，空实现）
    void enable_interpolation_for_camera(int camera_index, bool enable = true);
    InterpolationStats get_interpolation_stats() const;

private:
    // 初始化和清理
    bool init_camera(int index, const std::string& device_path);
    void cleanup_camera(int index);
    void full_reset();

    // 线程函数
    void capture_thread(int camera_index);
    void sync_loop();

    // 同步相关
    void add_to_sync_queue(const std::vector<AVFrame*>& frames);

    // 队列管理
    size_t force_clear_all_queues();

    // 内存管理
    AVFrame* clone_frame(const AVFrame* src) const;
    void free_cloned_frame(AVFrame** frame);

    // 插值相关（空实现，兼容性保留）
    void init_interpolation_system();

    // 成员变量
    std::vector<CameraInfo> cameras_;
    std::vector<std::deque<TimestampedFrame>> frame_queues_;
    std::deque<std::vector<AVFrame*>> synced_frame_queue_;

    std::vector<std::thread> capture_threads_;
    std::thread sync_thread_;

    mutable std::mutex queue_mutex_;

    std::atomic<bool> running_{ false };
    std::atomic<bool> capture_active_{ false };
    bool initialized_ = false;

    size_t camera_count_ = 0;
    SyncConfig config_;

    // 管理器和优化器
    std::unique_ptr<SyncStrategy> sync_strategy_;
    std::unique_ptr<StatsManager> stats_manager_;
    std::unique_ptr<PerformanceOptimizer> optimizer_;
};

//==============================================================================
// 工厂函数
//==============================================================================
namespace CameraCaptureFactory {
    std::unique_ptr<MultiCameraCapture> create_dual_camera(
        const std::vector<std::string>& device_paths);

    std::unique_ptr<MultiCameraCapture> create_triple_camera(
        const std::vector<std::string>& device_paths);

    std::unique_ptr<MultiCameraCapture> create_quad_camera(
        const std::vector<std::string>& device_paths);
}
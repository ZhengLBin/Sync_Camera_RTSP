// multi_camera_sync.h - 去除插值功能的优化版本 - 最终修复版本
#pragma once

#include <vector>
#include <string>
#include <memory>
#include <thread>
#include <mutex>
#include <atomic>
#include <deque>
#include <chrono>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavdevice/avdevice.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
#include <libavutil/time.h>
}

//==============================================================================
// 核心数据结构
//==============================================================================

// 时间戳帧结构
struct TimestampedFrame {
    AVFrame* frame;
    int64_t timestamp_us;

    TimestampedFrame() : frame(nullptr), timestamp_us(0) {}
    TimestampedFrame(AVFrame* f, int64_t ts) : frame(f), timestamp_us(ts) {}
};

// 同步配置
struct SyncConfig {
    int target_fps = 30;
    size_t max_queue_size = 15;
    size_t max_sync_queue_size = 5;
    int64_t sync_threshold_us = 300000;  // 300ms同步阈值

    SyncConfig() = default;
};

// 摄像头统计信息
struct CameraStats {
    size_t frames_captured = 0;
    size_t frames_dropped_queue = 0;
    size_t frames_dropped_memory = 0;
    size_t decode_failures = 0;
    size_t read_failures = 0;
    size_t frames_skipped = 0;

    CameraStats() = default;
};

// 同步统计信息
struct SyncStats {
    size_t sync_attempts = 0;
    size_t sync_success = 0;
    size_t sync_failures_no_frames = 0;
    size_t sync_failures_timestamp = 0;
    size_t memory_cleanups = 0;

    SyncStats() = default;
};

// 内存统计信息
struct MemoryStats {
    size_t allocated_frames = 0;
    size_t freed_frames = 0;
    size_t active_frames = 0;
    std::vector<size_t> raw_queue_sizes;
    size_t sync_queue_size = 0;
    size_t interpolated_queue_size = 0;  // 保留字段，但不再使用

    MemoryStats() = default;
};

// 摄像头数据结构
struct CameraData {
    int index = -1;
    std::string device_path;

    AVFormatContext* fmt_ctx = nullptr;
    AVCodecContext* codec_ctx = nullptr;
    int video_stream_idx = -1;

    AVFrame* frame = nullptr;
    AVFrame* yuv_frame = nullptr;
    uint8_t* yuv_buffer = nullptr;

    SwsContext* sws_ctx_yuv = nullptr;

    CameraData() = default;
    ~CameraData() = default;

    // 禁用拷贝构造和赋值
    CameraData(const CameraData&) = delete;
    CameraData& operator=(const CameraData&) = delete;

    // 允许移动构造和赋值
    CameraData(CameraData&& other) noexcept {
        *this = std::move(other);
    }

    CameraData& operator=(CameraData&& other) noexcept {
        if (this != &other) {
            index = other.index;
            device_path = std::move(other.device_path);
            fmt_ctx = other.fmt_ctx;
            codec_ctx = other.codec_ctx;
            video_stream_idx = other.video_stream_idx;
            frame = other.frame;
            yuv_frame = other.yuv_frame;
            yuv_buffer = other.yuv_buffer;
            sws_ctx_yuv = other.sws_ctx_yuv;

            // 重置源对象
            other.index = -1;
            other.fmt_ctx = nullptr;
            other.codec_ctx = nullptr;
            other.video_stream_idx = -1;
            other.frame = nullptr;
            other.yuv_frame = nullptr;
            other.yuv_buffer = nullptr;
            other.sws_ctx_yuv = nullptr;
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
    ~StatsManager() = default;

    void update_camera_stats(int camera_id, const std::string& event, size_t count = 1);
    void update_sync_stats(const std::string& event, size_t count = 1);
    bool should_report(int interval_seconds = 30);
    void print_summary(const std::string& prefix = "");

private:
    std::vector<CameraStats> camera_stats_;
    SyncStats sync_stats_;
    std::chrono::steady_clock::time_point start_time_;
    std::chrono::steady_clock::time_point last_report_time_;
    mutable std::mutex stats_mutex_;
};

//==============================================================================
// 性能优化器
//==============================================================================
class PerformanceOptimizer {
public:
    PerformanceOptimizer();
    ~PerformanceOptimizer() = default;

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

// 时间戳同步策略实现
class TimestampSyncStrategy : public SyncStrategy {
public:
    std::vector<AVFrame*> find_sync_frames(
        const std::vector<std::deque<TimestampedFrame>>& queues,
        const SyncConfig& config) override;
};

//==============================================================================
// 多摄像头捕获器主类
//==============================================================================
class MultiCameraCapture {
public:
    MultiCameraCapture();
    ~MultiCameraCapture();

    // 禁用拷贝构造和赋值
    MultiCameraCapture(const MultiCameraCapture&) = delete;
    MultiCameraCapture& operator=(const MultiCameraCapture&) = delete;

    // 初始化和控制方法
    bool init(const std::vector<std::string>& device_paths, const SyncConfig& config = SyncConfig{});
    void start();
    void stop();
    void pause_capture();
    void resume_capture();

    // 配置更新
    void update_config(const SyncConfig& config);
    void set_sync_strategy(std::unique_ptr<SyncStrategy> strategy);

    // 帧获取方法
    std::vector<AVFrame*> get_sync_yuv420p_frames();
    void release_frame(AVFrame** frame);

    // 状态查询方法
    size_t get_camera_count() const { return camera_count_; }
    bool is_running() const { return running_.load(); }
    bool is_initialized() const { return initialized_; }

    // 内存和队列管理
    MemoryStats get_memory_stats() const;
    size_t get_sync_queue_size() const;
    size_t emergency_memory_cleanup();
    size_t clear_queue(int camera_index, size_t max_to_clear = SIZE_MAX);
    size_t balance_queues();

private:
    // 初始化和清理方法
    bool init_camera(int index, const std::string& device_path);
    void cleanup_camera(int index);
    void full_reset();

    // 线程函数
    void capture_thread(int camera_index);
    void sync_loop();

    // 同步相关方法
    bool check_sync_conditions(std::vector<AVFrame*>& sync_frames);
    void add_to_sync_queue(const std::vector<AVFrame*>& frames);
    void smart_frame_dropping();
    void manage_queues();

    // 内存管理
    AVFrame* clone_frame(const AVFrame* src) const;
    void free_cloned_frame(AVFrame** frame);
    size_t force_clear_all_queues();

    // 成员变量
    std::vector<CameraData> cameras_;
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

    std::unique_ptr<SyncStrategy> sync_strategy_;
    std::unique_ptr<StatsManager> stats_manager_;
};

//==============================================================================
// 工厂函数命名空间
//==============================================================================
namespace CameraCaptureFactory {
    std::unique_ptr<MultiCameraCapture> create_dual_camera(
        const std::vector<std::string>& device_paths);

    std::unique_ptr<MultiCameraCapture> create_triple_camera(
        const std::vector<std::string>& device_paths);

    std::unique_ptr<MultiCameraCapture> create_quad_camera(
        const std::vector<std::string>& device_paths);
}

//==============================================================================
// 全局统计变量声明
//==============================================================================
extern std::atomic<size_t> g_total_frames_allocated;
extern std::atomic<size_t> g_total_frames_freed;
extern std::atomic<size_t> g_active_frame_count;
#pragma once

#include <vector>
#include <string>
#include <memory>
#include <deque>
#include <mutex>
#include <thread>
#include <atomic>
#include <chrono>

#include "../includes/tcp_streamer.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavdevice/avdevice.h>
#include <libavutil/imgutils.h>
#include <libavutil/time.h>
#include <libswscale/swscale.h>
}

// 前向声明
class StatsManager;
class PerformanceOptimizer;

//==============================================================================
// 配置结构体
//==============================================================================
struct SyncConfig {
    int target_fps = 25;
    double expected_fps = 25.0;
    size_t max_queue_size = 50;
    size_t max_sync_queue_size = 20;
    int64_t sync_threshold_us = 5000000;
    int64_t timestamp_tolerance_us = 200000;
    int64_t max_queue_age_us = 10000000;
    size_t frame_drop_threshold = 50;
    size_t emergency_cleanup_threshold = 100;
    int balance_interval_ms = 100;
    bool enable_smart_sync = true;
    bool enable_aggressive_cleanup = false;
};

//==============================================================================
// 统计结构体
//==============================================================================
struct CameraStats {
    size_t frames_captured = 0;
    size_t frames_dropped_queue = 0;
    size_t frames_dropped_memory = 0;
    size_t decode_failures = 0;
    size_t read_failures = 0;
    size_t frames_skipped = 0;
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
};

struct InterpolationStats {
    // 空结构体，保持接口兼容性
};

//==============================================================================
// 时间戳帧结构
//==============================================================================
struct TimestampedFrame {
    AVFrame* frame;
    int64_t timestamp_us;
};

//==============================================================================
// 摄像头信息结构
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

//==============================================================================
// 统计管理器
//==============================================================================
class StatsManager {
public:
    explicit StatsManager(size_t camera_count);

    void update_camera_stats(int camera_id, const std::string& event, size_t count = 1);
    void update_sync_stats(const std::string& event, size_t count = 1);
    bool should_report(int interval_seconds = 60);
    void print_summary(const std::string& prefix = "");

private:
    std::vector<CameraStats> camera_stats_;
    SyncStats sync_stats_;
    mutable std::mutex stats_mutex_;
    std::chrono::steady_clock::time_point start_time_;
    std::chrono::steady_clock::time_point last_report_time_;
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

    std::atomic<double> current_sync_rate_{ 0.0 };
    std::atomic<size_t> total_attempts_{ 0 };
    std::atomic<size_t> total_success_{ 0 };
    std::chrono::steady_clock::time_point last_adjustment_time_;
};

//==============================================================================
// 多摄像头捕获器主类
//==============================================================================
class MultiCameraCapture {
public:
    MultiCameraCapture();
    ~MultiCameraCapture();

    // 基础控制
    bool init(const std::vector<std::string>& device_paths, const SyncConfig& config);
    void start();
    void stop();
    void pause_capture();
    void resume_capture();

    // 帧获取
    std::vector<AVFrame*> get_sync_yuv420p_frames();
    void release_frame(AVFrame** frame);

    // 状态查询
    size_t get_camera_count() const;
    size_t get_sync_queue_size() const;
    MemoryStats get_memory_stats() const;
    InterpolationStats get_interpolation_stats() const;

    // 配置
    void update_config(const SyncConfig& config);

    // 内存管理
    size_t emergency_memory_cleanup();

    // 兼容性方法（空实现）
    void enable_interpolation_for_camera(int camera_index, bool enable);


private:
    // 初始化和清理
    bool init_camera(int index, const std::string& device_path);
    void cleanup_camera(int index);
    void full_reset();

    // 线程函数
    void capture_thread(int camera_index);
    void sync_loop();

    // 内存管理
    AVFrame* clone_frame(const AVFrame* src) const;
    void free_cloned_frame(AVFrame** frame);
    size_t force_clear_all_queues();

    // 成员变量
    bool initialized_ = false;
    std::atomic<bool> running_{ false };
    std::atomic<bool> capture_active_{ false };
    size_t camera_count_ = 0;
    SyncConfig config_;

    // 摄像头相关
    std::vector<CameraInfo> cameras_;
    std::vector<std::deque<TimestampedFrame>> frame_queues_;
    std::deque<std::vector<AVFrame*>> synced_frame_queue_;

    // 线程管理
    std::vector<std::thread> capture_threads_;
    std::thread sync_thread_;

    // 同步控制
    mutable std::mutex queue_mutex_;

    // 组件
    std::unique_ptr<StatsManager> stats_manager_;
    std::unique_ptr<PerformanceOptimizer> optimizer_;

};

//==============================================================================
// 工厂模式 - 创建不同配置的多摄像头捕获器
//==============================================================================
namespace CameraCaptureFactory {
    std::unique_ptr<MultiCameraCapture> create_dual_camera(
        const std::vector<std::string>& device_paths);

    std::unique_ptr<MultiCameraCapture> create_triple_camera(
        const std::vector<std::string>& device_paths);

    std::unique_ptr<MultiCameraCapture> create_quad_camera(
        const std::vector<std::string>& device_paths);
}
// multi_camera_sync.h - 优化版本多摄像头同步头文件
#pragma once

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/time.h>
#include <libavdevice/avdevice.h>
}

#include <vector>
#include <deque>
#include <thread>
#include <mutex>
#include <atomic>
#include <memory>
#include <functional>
#include <map>
#include <string>

// 前向声明
class StatsManager;

// 时间戳帧结构
struct TimestampedFrame {
    AVFrame* frame;
    int64_t timestamp_us;
};

// 摄像头信息结构
struct CameraInfo {
    AVFormatContext* fmt_ctx = nullptr;
    AVCodecContext* codec_ctx = nullptr;
    AVFrame* frame = nullptr;
    AVFrame* yuv_frame = nullptr;
    uint8_t* yuv_buffer = nullptr;
    SwsContext* sws_ctx_yuv = nullptr;
    int video_stream_idx = -1;
    std::string device_path;
    int index = -1;
};

// 同步配置结构
struct SyncConfig {
    int64_t sync_threshold_us = 50000;  // 同步阈值（微秒）
    size_t max_queue_size = 10;         // 最大队列大小
    size_t max_sync_queue_size = 2;     // 最大同步队列大小
    int target_fps = 30;                // 目标输出帧率
    bool enable_interpolation = false;   // 是否启用插值（针对特定摄像头）
    std::vector<int> interpolation_cameras; // 需要插值的摄像头索引（通常是性能差的摄像头）
};

// 内存统计结构
struct MemoryStats {
    size_t allocated_frames = 0;
    size_t freed_frames = 0;
    size_t active_frames = 0;
    std::vector<size_t> raw_queue_sizes;
    size_t sync_queue_size = 0;
    size_t interpolated_queue_size = 0;
};

// 同步策略基类
class SyncStrategy {
public:
    virtual ~SyncStrategy() = default;
    virtual std::vector<AVFrame*> find_sync_frames(
        const std::vector<std::deque<TimestampedFrame>>& queues,
        const SyncConfig& config) = 0;
};

// 简单时间戳同步策略
class TimestampSyncStrategy : public SyncStrategy {
public:
    std::vector<AVFrame*> find_sync_frames(
        const std::vector<std::deque<TimestampedFrame>>& queues,
        const SyncConfig& config) override;
};

// 插值同步策略（用于不同帧率的摄像头）
class InterpolationSyncStrategy : public SyncStrategy {
public:
    std::vector<AVFrame*> find_sync_frames(
        const std::vector<std::deque<TimestampedFrame>>& queues,
        const SyncConfig& config) override;

private:
    AVFrame* interpolate_frame(const AVFrame* prev, const AVFrame* next, float t) const;
};

// 通用多摄像头捕获类
class MultiCameraCapture {
public:
    MultiCameraCapture();
    ~MultiCameraCapture();

    // 基本接口
    bool init(const std::vector<std::string>& device_paths, const SyncConfig& config = SyncConfig{});
    void start();
    void stop();
    void pause_capture();
    void resume_capture();

    // 帧获取和释放
    std::vector<AVFrame*> get_sync_yuv420p_frames();
    void release_frame(AVFrame** frame);

    // 配置管理
    void set_sync_strategy(std::unique_ptr<SyncStrategy> strategy);
    void update_config(const SyncConfig& config);
    SyncConfig get_config() const { return config_; }

    // 统计和监控
    MemoryStats get_memory_stats() const;
    size_t get_sync_queue_size() const;
    size_t emergency_memory_cleanup();

    // 摄像头数量
    size_t get_camera_count() const { return cameras_.size(); }

private:
    // 配置
    SyncConfig config_;
    size_t camera_count_ = 0;

    // 摄像头资源
    std::vector<CameraInfo> cameras_;
    std::unique_ptr<SyncStrategy> sync_strategy_;

    // 统计管理器（新增）
    std::unique_ptr<StatsManager> stats_manager_;

    // 状态控制
    std::atomic<bool> initialized_{ false };
    std::atomic<bool> running_{ false };
    std::atomic<bool> capture_active_{ false };

    // 线程管理
    std::vector<std::thread> capture_threads_;
    std::thread sync_thread_;
    std::thread interpolation_thread_;

    // 数据队列
    mutable std::mutex queue_mutex_;
    std::vector<std::deque<TimestampedFrame>> frame_queues_;  // 每个摄像头一个队列
    std::deque<std::vector<AVFrame*>> synced_frame_queue_;    // 同步后的帧组队列

    // 插值相关
    mutable std::mutex interpolation_mutex_;
    std::deque<TimestampedFrame> interpolated_queue_;
    std::map<int, std::pair<AVFrame*, AVFrame*>> interpolation_frame_pairs_; // 摄像头索引 -> (prev, next)
    std::map<int, std::pair<int64_t, int64_t>> interpolation_timestamps_;

    // 内部方法
    bool init_camera(int index, const std::string& device_path);
    void cleanup_camera(int index);
    void full_reset();
    void free_cloned_frame(AVFrame** frame);

    // 线程函数
    void capture_thread(int camera_index);
    void sync_loop();
    void interpolation_loop();

    // 帧处理
    AVFrame* clone_frame(const AVFrame* src) const;
    AVFrame* interpolate_frame_simple(const AVFrame* prev, const AVFrame* next, float t) const;

    // 内存管理
    size_t clear_queue(int camera_index, size_t max_to_clear = SIZE_MAX);
    size_t balance_queues();
    size_t force_clear_all_queues();

    // 新增的同步相关方法
    bool check_sync_conditions(std::vector<AVFrame*>& sync_frames);
    void add_to_sync_queue(const std::vector<AVFrame*>& frames);
    void smart_frame_dropping();
    void manage_queues();

    // 删除的方法（被StatsManager替代）
    // void print_stats() const;  // 已删除 - 由StatsManager处理
    // bool is_queue_size_exceeded(int camera_index) const;  // 已删除 - 逻辑已内联

    // 禁用拷贝构造和赋值
    MultiCameraCapture(const MultiCameraCapture&) = delete;
    MultiCameraCapture& operator=(const MultiCameraCapture&) = delete;
};

// 便利工厂函数
namespace CameraCaptureFactory {
    // 创建双摄像头捕获
    std::unique_ptr<MultiCameraCapture> create_dual_camera(
        const std::vector<std::string>& device_paths);

    // 创建三摄像头捕获（带插值）
    std::unique_ptr<MultiCameraCapture> create_triple_camera_with_interpolation(
        const std::vector<std::string>& device_paths,
        int interpolation_camera_index = 2);

    // 创建四摄像头捕获（带插值）
    std::unique_ptr<MultiCameraCapture> create_quad_camera_with_interpolation(
        const std::vector<std::string>& device_paths,
        int interpolation_camera_index);
}
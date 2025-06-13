// sync_camera.h - 支持延迟初始化和内存泄漏修复
#pragma once

extern "C" {
#include <libavdevice/avdevice.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/time.h>
#include <libswscale/swscale.h>
}

#include <vector>
#include <thread>
#include <mutex>
#include <atomic>
#include <deque>
#include <string>
#include <chrono>

// 带时间戳的帧结构
struct TimestampedFrame {
    AVFrame* frame;
    int64_t timestamp_us;
};

// 内存统计结构
struct MemoryStats {
    size_t allocated_frames;
    size_t freed_frames;
    size_t active_frames;
    size_t raw_queue_size[2];
    size_t sync_queue_size;
};

// 摄像头信息结构
struct CameraInfo {
    AVFormatContext* fmt_ctx = nullptr;
    AVCodecContext* codec_ctx = nullptr;
    SwsContext* sws_ctx_yuv = nullptr;
    AVFrame* frame = nullptr;
    AVFrame* yuv_frame = nullptr;
    uint8_t* yuv_buffer = nullptr;
    int video_stream_idx = -1;
};

class DualCameraCapture {
public:
    DualCameraCapture();
    ~DualCameraCapture();

    // 基本操作
    bool init(const std::vector<std::string>& device_paths);
    void start();
    void stop();
    void pause_capture();
    void resume_capture();

    // 完全重置，释放所有资源
    void full_reset();

    // 检查状态
    bool is_initialized() const { return initialized_; }
    bool is_running() const { return running_.load(); }

    // 帧获取
    std::vector<AVFrame*> get_sync_yuv420p_frames();

    // 队列管理
    size_t force_clear_queues();
    size_t clear_sync_queue_partial(size_t max_to_clear);
    size_t get_sync_queue_size() const;

    // 新增：内存管理功能
    size_t balance_frame_queues();           // 平衡两个摄像头的帧队列
    size_t emergency_memory_cleanup();       // 紧急内存清理

    // 统计信息
    MemoryStats get_memory_stats() const;
    void release_frame(AVFrame** frame);

private:
    bool init_camera(int index, const std::string& device_path);
    void capture_thread(int index);
    void sync_loop();
    AVFrame* clone_frame(const AVFrame* src) const;
    void free_cloned_frame(AVFrame** frame);

    // 清理单个摄像头资源
    void cleanup_camera(int index);

    // 摄像头数据
    std::vector<CameraInfo> cameras_;
    bool initialized_ = false;

    // 线程控制
    std::vector<std::thread> threads_;
    std::thread sync_thread_;
    std::atomic<bool> running_{ false };
    std::atomic<bool> capture_active_{ false };

    // 队列和同步
    mutable std::mutex mutex_;
    std::deque<TimestampedFrame> frame_queue_yuv[2];
    std::deque<std::pair<AVFrame*, AVFrame*>> synced_yuv_queue_;

    // 配置常量（更严格的限制）
    static constexpr size_t MAX_RAW_QUEUE_SIZE = 10;     // 从30减少到10
    static constexpr size_t MAX_SYNC_QUEUE_SIZE = 2;     // 从5减少到2
    static constexpr int64_t SYNC_THRESHOLD_US = 50000;  // 50ms
};
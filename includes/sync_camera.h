// sync_camera.h - 自适应摄像头同步头文件
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

// 时间戳帧结构
struct TimestampedFrame {
    AVFrame* frame;
    int64_t timestamp_us;
};

// 三摄像头帧组结构
struct FrameTriplet {
    AVFrame* cam1;  // 主摄像头
    AVFrame* cam2;  // 辅摄像头  
    AVFrame* cam3;  // 第三摄像头（插值后）
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
};

// 内存统计结构
struct MemoryStats {
    size_t allocated_frames = 0;
    size_t freed_frames = 0;
    size_t active_frames = 0;
    size_t raw_queue_size[3] = { 0, 0, 0 };  // 最多三个摄像头队列
    size_t sync_queue_size = 0;
};

// 摄像头捕获基类
class BaseCameraCapture {
public:
    virtual ~BaseCameraCapture() = default;

    // 纯虚函数接口
    virtual bool init(const std::vector<std::string>& device_paths) = 0;
    virtual void start() = 0;
    virtual void stop() = 0;
    virtual void pause_capture() = 0;
    virtual void resume_capture() = 0;

    // 帧获取和释放
    virtual std::vector<AVFrame*> get_sync_yuv420p_frames() = 0;
    virtual void release_frame(AVFrame** frame) = 0;

    // 统计和监控
    virtual MemoryStats get_memory_stats() const = 0;
    virtual size_t get_sync_queue_size() const = 0;
    virtual size_t emergency_memory_cleanup() = 0;
};

// 双摄像头捕获类（原来的实现）
class DualCameraCapture : public BaseCameraCapture {
public:
    DualCameraCapture();
    ~DualCameraCapture() override;

    // 基类接口实现
    bool init(const std::vector<std::string>& device_paths) override;
    void start() override;
    void stop() override;
    void pause_capture() override;
    void resume_capture() override;

    // 帧获取和释放
    std::vector<AVFrame*> get_sync_yuv420p_frames() override;
    void release_frame(AVFrame** frame) override;

    // 统计和监控
    MemoryStats get_memory_stats() const override;
    size_t get_sync_queue_size() const override;
    size_t emergency_memory_cleanup() override;

private:
    // 同步阈值（微秒）
    static constexpr int64_t SYNC_THRESHOLD_US = 50000;  // 50ms

    // 成员变量
    std::vector<CameraInfo> cameras_;
    std::atomic<bool> initialized_{ false };
    std::atomic<bool> running_{ false };
    std::atomic<bool> capture_active_{ false };

    // 线程管理
    std::vector<std::thread> threads_;           // 捕获线程
    std::thread sync_thread_;                    // 同步线程

    // 队列和同步
    mutable std::mutex mutex_;
    std::deque<TimestampedFrame> frame_queue_yuv[2];      // 两个原始帧队列
    std::deque<std::pair<AVFrame*, AVFrame*>> synced_yuv_queue_;  // 同步后的帧对队列

    // 内部方法
    bool init_camera(int index, const std::string& device_path);
    void cleanup_camera(int index);
    void full_reset();
    void free_cloned_frame(AVFrame** frame);

    // 线程函数
    void capture_thread(int index);
    void sync_loop();

    // 帧处理
    AVFrame* clone_frame(const AVFrame* src) const;

    // 内存管理
    size_t balance_frame_queues();
    size_t force_clear_queues();
    size_t clear_sync_queue_partial(size_t max_to_clear);

    // 禁用拷贝构造和赋值
    DualCameraCapture(const DualCameraCapture&) = delete;
    DualCameraCapture& operator=(const DualCameraCapture&) = delete;
};

// 三摄像头捕获类
class TripleCameraCapture : public BaseCameraCapture {
public:
    TripleCameraCapture();
    ~TripleCameraCapture() override;

    // 基类接口实现
    bool init(const std::vector<std::string>& device_paths) override;
    void start() override;
    void stop() override;
    void pause_capture() override;
    void resume_capture() override;

    // 帧获取和释放
    std::vector<AVFrame*> get_sync_yuv420p_frames() override;
    void release_frame(AVFrame** frame) override;

    // 统计和监控
    MemoryStats get_memory_stats() const override;
    size_t get_sync_queue_size() const override;
    size_t emergency_memory_cleanup() override;

private:
    // 同步阈值（微秒）
    static constexpr int64_t SYNC_THRESHOLD_US = 50000;  // 50ms

    // 成员变量
    std::vector<CameraInfo> cameras_;
    std::atomic<bool> initialized_{ false };
    std::atomic<bool> running_{ false };
    std::atomic<bool> capture_active_{ false };

    // 线程管理
    std::vector<std::thread> threads_;           // 捕获线程
    std::thread sync_thread_;                    // 同步线程
    std::thread interpolation_thread_;           // 插值线程

    // 队列和同步
    mutable std::mutex mutex_;
    std::deque<TimestampedFrame> frame_queue_yuv[3];  // 三个原始帧队列
    std::deque<FrameTriplet> synced_yuv_queue_;       // 同步后的三元组队列

    // 第三摄像头插值相关
    mutable std::mutex interpolation_mutex_;
    AVFrame* third_cam_prev_frame_ = nullptr;     // 第三摄像头前一帧
    AVFrame* third_cam_next_frame_ = nullptr;     // 第三摄像头后一帧
    int64_t third_cam_prev_timestamp_ = 0;        // 前一帧时间戳
    int64_t third_cam_next_timestamp_ = 0;        // 后一帧时间戳
    std::deque<TimestampedFrame> interpolated_queue_;  // 插值帧队列

    // 内部方法
    bool init_camera(int index, const std::string& device_path);
    void cleanup_camera(int index);
    void full_reset();
    void free_cloned_frame(AVFrame** frame);

    // 线程函数
    void capture_thread(int index);
    void sync_loop();
    void interpolation_loop();  // 插值线程

    // 帧处理
    AVFrame* clone_frame(const AVFrame* src) const;
    AVFrame* interpolate_frame(const AVFrame* prev, const AVFrame* next, float t) const;  // 帧插值

    // 内存管理
    size_t balance_frame_queues();
    size_t force_clear_queues();
    size_t clear_sync_queue_partial(size_t max_to_clear);

    // 禁用拷贝构造和赋值
    TripleCameraCapture(const TripleCameraCapture&) = delete;
    TripleCameraCapture& operator=(const TripleCameraCapture&) = delete;
};
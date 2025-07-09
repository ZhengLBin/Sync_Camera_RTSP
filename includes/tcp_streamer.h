// shared_memory_streamer.h - TCP版本
#ifndef SHARED_MEMORY_STREAMER_H
#define SHARED_MEMORY_STREAMER_H

#include <string>
#include <thread>
#include <atomic>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <memory>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
}

#include <gst/gst.h>
#include <gst/app/gstappsrc.h>

// 帧缓冲区类
class ShmFrameBuffer {
public:
    explicit ShmFrameBuffer(AVFrame* frame);
    ~ShmFrameBuffer();

    uint8_t* data;
    size_t size;
    int64_t pts;



private:
    ShmFrameBuffer(const ShmFrameBuffer&) = delete;
    ShmFrameBuffer& operator=(const ShmFrameBuffer&) = delete;
};

// TCP流媒体器类（替代共享内存）
class TCPStreamer {
public:
    explicit TCPStreamer(const std::string& name, int port = 0);
    ~TCPStreamer();

    // 初始化TCP流
    bool init(int width, int height, int fps);

    // 发送帧数据
    bool send_frame(AVFrame* frame);
    void print_pipeline_state();
    void debug_caps_info();
    // 停止流媒体
    void stop();

    // 获取连接信息
    std::string get_socket_path() const { return tcp_url_; }
    int get_port() const { return port_; }

    // 获取统计信息
    size_t get_frame_count() const { return frame_count_.load(); }
    size_t get_dropped_count() const { return dropped_frame_count_.load(); }

private:
    // 创建pipeline
    bool create_pipeline();

    // 帧推送循环
    void push_frame_loop();

    // 推送单帧到appsrc
    bool push_frame_to_appsrc();

    // GStreamer回调函数
    static void need_data_cb(GstElement* appsrc, guint unused, gpointer user_data);
    static void enough_data_cb(GstElement* appsrc, gpointer user_data);
    static gboolean bus_call(GstBus* bus, GstMessage* msg, gpointer user_data);

    // 配置参数
    std::string name_;
    std::string tcp_url_;
    int port_;
    int width_;
    int height_;
    int fps_;

    // GStreamer组件
    GstElement* pipeline_;
    GstElement* appsrc_;
    GstBus* bus_;
    guint bus_watch_id_;

    // 状态控制
    std::atomic<bool> running_;
    std::atomic<bool> initialized_;
    std::atomic<bool> need_data_;

    // 线程管理
    std::thread push_thread_;

    // 帧队列
    std::queue<std::shared_ptr<ShmFrameBuffer>> frame_queue_;
    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;

    // 统计信息
    std::atomic<size_t> frame_count_;
    std::atomic<size_t> dropped_frame_count_;
    std::atomic<size_t> total_memory_allocated_;

    // 常量
    static const size_t MAX_QUEUE_SIZE = 3;
    static int next_port_;
};

// 全局GStreamer初始化函数
bool initialize_gstreamer_for_shm();

#endif // SHARED_MEMORY_STREAMER_H
// shared_memory_streamer.h - TCP streaming header
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

class TCPStreamer {
public:
    explicit TCPStreamer(const std::string& name, const std::string& host, int port);
    ~TCPStreamer();

    bool init(int width, int height, int fps);
    bool send_frame(AVFrame* frame);
    void print_pipeline_state();
    void debug_caps_info();
    void stop();
    bool is_port_in_use(int port);
    static int allocate_port();

    std::string get_socket_path() const { return tcp_url_; }
    int get_port() const { return port_; }

    size_t get_frame_count() const { return frame_count_.load(); }
    size_t get_dropped_count() const { return dropped_frame_count_.load(); }

private:
    bool create_pipeline();
    void push_frame_loop();
    bool push_frame_to_appsrc();

    static void need_data_cb(GstElement* appsrc, guint unused, gpointer user_data);
    static void enough_data_cb(GstElement* appsrc, gpointer user_data);
    static gboolean bus_call(GstBus* bus, GstMessage* msg, gpointer user_data);

    std::string name_;
    std::string tcp_url_;
    std::string host_;
    int port_;
    int width_;
    int height_;
    int fps_;

    GstElement* pipeline_;
    GstElement* appsrc_;
    GstBus* bus_;
    guint bus_watch_id_;

    std::atomic<bool> running_;
    std::atomic<bool> initialized_;
    std::atomic<bool> need_data_;

    std::thread push_thread_;

    std::queue<std::shared_ptr<ShmFrameBuffer>> frame_queue_;
    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;

    std::atomic<size_t> frame_count_;
    std::atomic<size_t> dropped_frame_count_;
    std::atomic<size_t> total_memory_allocated_;

    static const size_t MAX_QUEUE_SIZE = 3;
    static std::atomic<int> next_port_;
    static std::mutex port_allocation_mutex_;
};

bool initialize_gstreamer_for_shm();

#endif // SHARED_MEMORY_STREAMER_H
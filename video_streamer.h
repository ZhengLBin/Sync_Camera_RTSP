// video_streamer.h - ֧�ֿͻ������ӻص��İ汾
#pragma once

#include <gst/gst.h>
#include <gst/rtsp-server/rtsp-server.h>
#include <gst/app/gstappsrc.h>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/time.h>
}

#include <memory>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <string>
#include <functional>

class FrameBuffer {
public:
    explicit FrameBuffer(AVFrame* frame);
    ~FrameBuffer();

    uint8_t* data;
    size_t size;
    int64_t pts;
};

bool initialize_gstreamer_with_diagnostics();

class VideoStreamer {
public:
    // �ͻ�������״̬�ص�����
    using ClientCallback = std::function<void(bool connected)>;

    VideoStreamer(const std::string& rtp_ip, int rtp_port);
    ~VideoStreamer();

    bool init(int width, int height, int fps);
    bool send_frame(AVFrame* frame);
    void stop();
    std::string get_rtsp_url() const;

    // ���������ÿͻ�������״̬�ص�
    void set_client_callback(ClientCallback callback);

    // ��������ȡ�ͻ�������״̬
    bool is_client_connected() const;

private:
    void gstreamer_main_loop();
    void push_frame_loop();
    bool push_frame_to_appsrc();
    std::string create_optimized_pipeline() const;

    // ������֪ͨ�ͻ���״̬�仯
    void notify_client_status(bool connected);

    // GStreamer�ص�����
    static void media_configure_cb(GstRTSPMediaFactory* factory, GstRTSPMedia* media, gpointer user_data);
    static void media_constructed_cb(GstRTSPMediaFactory* factory, GstRTSPMedia* media, gpointer user_data);
    static void media_unprepared_cb(GstRTSPMedia* media, gpointer user_data);
    static gboolean need_data_cb(GstElement* appsrc, guint unused, gpointer user_data);
    static void enough_data_cb(GstElement* appsrc, gpointer user_data);

    static std::string ffmpeg_errstr(int errnum);

    // ���ò���
    std::string ip_;
    int port_;
    int width_;
    int height_;
    int fps_;

    // GStreamer����
    GstRTSPServer* server_;
    GstRTSPMountPoints* mounts_;
    GstRTSPMediaFactory* factory_;
    GMainLoop* loop_;
    GstElement* appsrc_;

    // �̺߳�ͬ��
    std::thread gst_thread_;
    std::thread push_thread_;
    std::atomic<bool> running_;
    std::atomic<bool> initialized_;
    std::atomic<bool> need_data_;
    std::atomic<bool> client_connected_;

    // �ͻ���״̬�ص�
    ClientCallback client_callback_;
    std::mutex callback_mutex_;

    // ֡����
    std::queue<std::shared_ptr<FrameBuffer>> frame_queue_;
    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;

    // ͳ����Ϣ
    std::atomic<size_t> frame_count_;
    std::atomic<size_t> dropped_frame_count_;
    std::atomic<size_t> total_memory_allocated_;
};
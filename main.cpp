// main.cpp - �޸��ڴ�й©��ʵ�������ڴ����
// #include <vld.h>
#include "sync_camera.h"
#include "video_streamer.h"
#include <iostream>
#include <chrono>
#include <csignal>
#include <iomanip>

// Windows�ڴ���
#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#endif

// ȫ���˳���־
std::atomic<bool> g_should_exit{ false };

// �ڴ�������ؽṹ
struct MemoryMonitor {
    size_t baseline_mb = 0;
    size_t last_mb = 0;
    size_t peak_mb = 0;
    double growth_rate_mb_per_sec = 0.0;
    std::chrono::steady_clock::time_point last_check_time;
    int consecutive_high_growth = 0;

    void update(size_t current_mb) {
        auto now = std::chrono::steady_clock::now();

        if (last_mb > 0) {
            auto elapsed_sec = std::chrono::duration<double>(now - last_check_time).count();
            if (elapsed_sec > 0) {
                growth_rate_mb_per_sec = static_cast<double>(current_mb - last_mb) / elapsed_sec;
            }
        }

        if (current_mb > peak_mb) {
            peak_mb = current_mb;
        }

        // �������������
        if (growth_rate_mb_per_sec > 30.0) {  // ÿ����������10MB
            consecutive_high_growth++;
        }
        else {
            consecutive_high_growth = 0;
        }

        last_mb = current_mb;
        last_check_time = now;
    }

    bool is_critical() const {
        return consecutive_high_growth >= 5 || growth_rate_mb_per_sec > 100.0;
    }

    bool is_warning() const {
        return consecutive_high_growth >= 3 || growth_rate_mb_per_sec > 50.0;
    }
};

// �źŴ�����
void signal_handler(int signal) {
    if (signal == SIGINT || signal == SIGTERM) {
        std::cout << "\nReceived signal " << signal << ", shutting down gracefully..." << std::endl;
        g_should_exit = true;
    }
}

// ��ȡ��ǰ�����ڴ�ʹ������MB��
size_t get_memory_usage_mb() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
        return pmc.WorkingSetSize / (1024 * 1024);
    }
#endif
    return 0;
}

int main() {
    // �����źŴ���
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // ����FFmpeg��־����
    av_log_set_level(AV_LOG_ERROR);

    std::cout << "=== Advanced Memory Management System ===" << std::endl;
    std::cout << "Features: Delayed initialization, frame balancing, smart memory cleanup" << std::endl;

    // ��ʼ���ڴ���
    MemoryMonitor memory_monitor;
    memory_monitor.baseline_mb = get_memory_usage_mb();
    memory_monitor.last_check_time = std::chrono::steady_clock::now();

    // �ȳ�ʼ����Ƶ��������������ʼ������ͷ
    std::cout << "Initializing video streamers..." << std::endl;
    VideoStreamer left_streamer("192.168.16.247", 5004);
    VideoStreamer right_streamer("192.168.16.247", 5006);

    if (!left_streamer.init(640, 480, 30)) {
        std::cerr << "Failed to initialize left_streamer" << std::endl;
        return -1;
    }

    if (!right_streamer.init(640, 480, 30)) {
        std::cerr << "Failed to initialize right_streamer" << std::endl;
        return -1;
    }

    std::cout << "RTSP servers started successfully!" << std::endl;

    // ����ͷ��ر���
    std::unique_ptr<DualCameraCapture> capture;
    std::atomic<bool> cameras_initialized{ false };
    std::atomic<bool> cameras_running{ false };

    // �ͻ�������״̬���
    std::atomic<bool> left_client_connected{ false };
    std::atomic<bool> right_client_connected{ false };
    std::atomic<int> total_clients{ 0 };

    // ���ÿͻ���״̬�ص�
    left_streamer.set_client_callback([&](bool connected) {
        left_client_connected = connected;
        int old_count = total_clients.load();
        total_clients = (left_client_connected.load() ? 1 : 0) + (right_client_connected.load() ? 1 : 0);

        if (connected && old_count == 0) {
            std::cout << "[Main] First client connected to left stream!" << std::endl;
        }
        else if (!connected && total_clients.load() == 0) {
            std::cout << "[Main] Last client disconnected from left stream!" << std::endl;
        }
        });

    right_streamer.set_client_callback([&](bool connected) {
        right_client_connected = connected;
        int old_count = total_clients.load();
        total_clients = (left_client_connected.load() ? 1 : 0) + (right_client_connected.load() ? 1 : 0);

        if (connected && old_count == 0) {
            std::cout << "[Main] First client connected to right stream!" << std::endl;
        }
        else if (!connected && total_clients.load() == 0) {
            std::cout << "[Main] Last client disconnected from right stream!" << std::endl;
        }
        });

    // �������̣߳�֧�ֶ�̬����/ֹͣ����ͷ��
    std::thread streaming_thread([&]() {
        std::cout << "[Streaming] Thread started - waiting for clients..." << std::endl;

        uint64_t frame_pair_count = 0;
        uint64_t success_count = 0;
        uint64_t fail_count = 0;
        uint64_t memory_cleanup_count = 0;
        auto thread_start = std::chrono::steady_clock::now();
        auto last_stats_time = thread_start;
        auto last_camera_check = thread_start;
        auto last_memory_emergency_check = thread_start;

        while (!g_should_exit.load()) {
            try {
                auto current_time = std::chrono::steady_clock::now();
                int current_clients = total_clients.load();
                bool cams_initialized = cameras_initialized.load();
                bool cams_running = cameras_running.load();

                // �����ڴ��飨ÿ2�룩
                if (std::chrono::duration_cast<std::chrono::seconds>(current_time - last_memory_emergency_check).count() >= 2) {
                    if (capture && cams_running) {
                        auto memory_stats = capture->get_memory_stats();
                        size_t current_memory = get_memory_usage_mb();

                        // ���ڻ�Ծ֡���Ľ�������
                        if (memory_stats.active_frames > 5000) {
                            std::cout << "[EMERGENCY] Active frames: " << memory_stats.active_frames
                                << ", triggering emergency cleanup!" << std::endl;

                            size_t cleared = capture->emergency_memory_cleanup();
                            memory_cleanup_count += cleared;

                            std::cout << "[EMERGENCY] Cleared " << cleared << " frames, memory: "
                                << get_memory_usage_mb() << "MB" << std::endl;
                        }
                        // �����ڴ�ʹ�����Ľ�������
                        else if (current_memory > 1000) {  // ����600MB
                            std::cout << "[EMERGENCY] Memory usage: " << current_memory
                                << "MB, triggering emergency cleanup!" << std::endl;

                            size_t cleared = capture->emergency_memory_cleanup();
                            memory_cleanup_count += cleared;
                        }
                        // Ԥ����֡ƽ��
                        else if (memory_stats.active_frames > 2000) {
                            size_t balanced = capture->balance_frame_queues();
                            if (balanced > 0) {
                                memory_cleanup_count += balanced;
                                std::cout << "[BALANCE] Balanced " << balanced << " frames preventively" << std::endl;
                            }
                        }
                    }
                    last_memory_emergency_check = current_time;
                }

                // ÿ2����һ������ͷ״̬
                if (std::chrono::duration_cast<std::chrono::seconds>(current_time - last_camera_check).count() >= 2) {
                    // ���1���пͻ��˵�����ͷδ��ʼ�� - ��������ͷ
                    if (current_clients > 0 && !cams_initialized) {
                        std::cout << "[Streaming] Clients detected (" << current_clients << "), initializing cameras..." << std::endl;

                        try {
                            capture = std::make_unique<DualCameraCapture>();
                            if (capture->init({ "video=USB 2.0 Camera", "video=CyberTrack H3" })) {
                                capture->start();
                                cameras_initialized = true;
                                cameras_running = true;
                                std::cout << "[Streaming] Cameras initialized and started successfully!" << std::endl;
                            }
                            else {
                                std::cerr << "[Streaming] Failed to initialize cameras!" << std::endl;
                                capture.reset();
                            }
                        }
                        catch (const std::exception& e) {
                            std::cerr << "[Streaming] Exception during camera initialization: " << e.what() << std::endl;
                            capture.reset();
                        }
                    }
                    // ���2���޿ͻ��˵�����ͷ������ - ֹͣ����ͷ
                    else if (current_clients == 0 && cams_initialized) {
                        std::cout << "[Streaming] No clients, shutting down cameras..." << std::endl;

                        if (capture) {
                            capture->stop();
                            capture.reset();
                        }
                        cameras_initialized = false;
                        cameras_running = false;

                        // ǿ����������
                        std::this_thread::sleep_for(std::chrono::seconds(1));
                        size_t memory_after = get_memory_usage_mb();
                        std::cout << "[Streaming] Cameras shut down, memory: " << memory_after << "MB" << std::endl;
                    }

                    last_camera_check = current_time;
                }

                // �������ͷ���������пͻ��ˣ�����֡����
                if (cams_running && current_clients > 0 && capture) {
                    auto frames = capture->get_sync_yuv420p_frames();
                    if (frames.size() == 2 && frames[0] && frames[1]) {
                        // ����PTS��ȷ��ͬ��
                        frames[0]->pts = frame_pair_count;
                        frames[1]->pts = frame_pair_count;

                        bool left_ok = left_streamer.send_frame(frames[0]);
                        bool right_ok = right_streamer.send_frame(frames[1]);

                        frame_pair_count++;

                        if (left_ok || right_ok) {
                            success_count++;
                        }
                        else {
                            fail_count++;
                        }

                        // �ͷ�֡�ڴ� - ʹ��ͳһ�ӿ�ȷ����������ȷ
                        capture->release_frame(&frames[0]);
                        capture->release_frame(&frames[1]);

                        // ÿ30���ӡͳ����Ϣ
                        if (std::chrono::duration_cast<std::chrono::seconds>(current_time - last_stats_time).count() >= 30) {
                            auto total_elapsed = std::chrono::duration_cast<std::chrono::seconds>(current_time - thread_start).count();
                            double fps = total_elapsed > 0 ? static_cast<double>(frame_pair_count) / total_elapsed : 0;
                            size_t memory_mb = get_memory_usage_mb();
                            size_t sync_queue_size = capture ? capture->get_sync_queue_size() : 0;
                            auto memory_stats = capture ? capture->get_memory_stats() : MemoryStats{};

                            std::cout << "[Streaming] Stats - Pairs: " << frame_pair_count
                                << " (success: " << success_count << ", fail: " << fail_count << ")"
                                << ", FPS: " << fps
                                << ", Runtime: " << total_elapsed << "s"
                                << ", Memory: " << memory_mb << "MB"
                                << ", Queue: " << sync_queue_size
                                << ", Active frames: " << memory_stats.active_frames
                                << ", Clients: " << current_clients
                                << " (L=" << (left_client_connected.load() ? "Y" : "N")
                                << " R=" << (right_client_connected.load() ? "Y" : "N") << ")"
                                << ", Cleanups: " << memory_cleanup_count << std::endl;

                            last_stats_time = current_time;
                        }

                        // �пͻ���ʱ���ٴ���
                        std::this_thread::sleep_for(std::chrono::milliseconds(2));
                    }
                    else {
                        // û��֡���ݣ��ͷ��κλ�ȡ����֡
                        for (auto frame : frames) {
                            if (frame) capture->release_frame(&frame);
                        }
                        // �ȴ���������
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    }
                }
                else {
                    // �޿ͻ��˻�����ͷδ����ʱ����Ƶ���
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                }

            }
            catch (const std::exception& e) {
                std::cerr << "[Streaming] Exception: " << e.what() << std::endl;
                std::this_thread::sleep_for(std::chrono::milliseconds(1000));
            }
        }

        // �߳��˳�ʱ��������ͷ
        if (capture) {
            std::cout << "[Streaming] Thread exiting, stopping cameras..." << std::endl;
            capture->stop();
            capture.reset();
        }

        auto total_time = std::chrono::duration_cast<std::chrono::seconds>
            (std::chrono::steady_clock::now() - thread_start).count();
        std::cout << "[Streaming] Thread exited - Total frames: " << frame_pair_count
            << " (success: " << success_count << ", fail: " << fail_count << ")"
            << ", Cleanups: " << memory_cleanup_count
            << ", Average FPS: " << (total_time > 0 ? static_cast<double>(frame_pair_count) / total_time : 0) << std::endl;
        });

    // ��ʾRTSP������Ϣ
    std::cout << "\n=== RTSP Stream Information ===" << std::endl;
    std::cout << "Left camera:  " << left_streamer.get_rtsp_url() << std::endl;
    std::cout << "Right camera: " << right_streamer.get_rtsp_url() << std::endl;
    std::cout << "\n=== Test Commands ===" << std::endl;
    std::cout << "Left:  ffplay -rtsp_transport tcp -fflags nobuffer -flags low_delay " << left_streamer.get_rtsp_url() << std::endl;
    std::cout << "Right: ffplay -rtsp_transport tcp -fflags nobuffer -flags low_delay " << right_streamer.get_rtsp_url() << std::endl;
    std::cout << "\n=== Alternative Players ===" << std::endl;
    std::cout << "VLC:   vlc " << left_streamer.get_rtsp_url() << std::endl;
    std::cout << "VLC:   vlc " << right_streamer.get_rtsp_url() << std::endl;
    std::cout << "\n=== Synchronized Playback ===" << std::endl;
    std::cout << "ffplay -rtsp_transport tcp -flags low_delay " << left_streamer.get_rtsp_url()
        << " & ffplay -rtsp_transport tcp -flags low_delay " << right_streamer.get_rtsp_url() << std::endl;
    std::cout << "\n=== Advanced Features ===" << std::endl;
    std::cout << "- Delayed camera initialization (starts only when clients connect)" << std::endl;
    std::cout << "- Automatic frame balancing between cameras" << std::endl;
    std::cout << "- Smart memory monitoring and emergency cleanup" << std::endl;
    std::cout << "- Automatic camera shutdown when all clients disconnect" << std::endl;
    std::cout << "\nPress Ctrl+C to exit..." << std::endl;
    std::cout << "===============================================" << std::endl;

    // �����ѭ��
    int wait_count = 0;
    while (!g_should_exit.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        wait_count++;

        // �����ڴ���
        size_t current_memory = get_memory_usage_mb();
        memory_monitor.update(current_memory);

        // ÿ3����һ���ڴ���������
        if (wait_count % 3 == 0) {
            if (memory_monitor.is_critical()) {
                std::cout << "[CRITICAL] Memory growth rate: " << memory_monitor.growth_rate_mb_per_sec
                    << " MB/s (consecutive: " << memory_monitor.consecutive_high_growth << ")" << std::endl;

                // ���������ͷ���У�ִ�н�������
                if (capture && cameras_running.load()) {
                    size_t cleared = capture->emergency_memory_cleanup();
                    std::cout << "[CRITICAL] Emergency cleanup cleared " << cleared << " frames" << std::endl;
                }
            }
            else if (memory_monitor.is_warning()) {
                std::cout << "[WARNING] Memory growth rate: " << memory_monitor.growth_rate_mb_per_sec
                    << " MB/s, monitoring closely..." << std::endl;

                // Ԥ����ƽ�⣨ֻ����������ʱ��ִ�У�
                if (capture && cameras_running.load() && memory_monitor.is_warning()) {
                    size_t balanced = capture->balance_frame_queues();
                    if (balanced > 0) {
                        std::cout << "[WARNING] Preventive balancing cleared " << balanced << " frames" << std::endl;
                    }
                }
            }
        }

        // ÿ30���ӡһ��״̬
        if (wait_count % 30 == 0) {
            int current_clients = total_clients.load();
            bool cams_init = cameras_initialized.load();
            bool cams_run = cameras_running.load();

            std::cout << "[Main] Runtime: " << wait_count << "s"
                << ", Memory: " << current_memory << "MB"
                << " (baseline: " << memory_monitor.baseline_mb << "MB"
                << ", peak: " << memory_monitor.peak_mb << "MB"
                << ", growth: " << std::fixed << std::setprecision(1) << memory_monitor.growth_rate_mb_per_sec << " MB/s)"
                << ", Clients: " << current_clients
                << " (L=" << (left_client_connected.load() ? "Y" : "N")
                << " R=" << (right_client_connected.load() ? "Y" : "N") << ")"
                << ", Cameras: " << (cams_init ? (cams_run ? "RUNNING" : "INIT") : "OFF");

            if (capture && cams_run) {
                auto stats = capture->get_memory_stats();
                std::cout << ", Active frames: " << stats.active_frames;
            }

            std::cout << ". Press Ctrl+C to exit." << std::endl;
        }
    }

    // ������Դ
    std::cout << "\n[Main] Shutting down..." << std::endl;

    // �ȴ��������߳̽���
    std::cout << "[Main] Waiting for streaming thread..." << std::endl;
    if (streaming_thread.joinable()) {
        streaming_thread.join();
    }

    // ֹͣ��ý�������
    std::cout << "[Main] Stopping streamers..." << std::endl;
    left_streamer.stop();
    right_streamer.stop();

    // ��ӡ�����ڴ�ʹ�����
    size_t final_memory_mb = get_memory_usage_mb();
    std::cout << "[Main] Final memory usage: " << final_memory_mb << "MB"
        << " (peak: " << memory_monitor.peak_mb << "MB)" << std::endl;
    std::cout << "[Main] Cleanup completed. Program exited normally." << std::endl;
    return 0;
}
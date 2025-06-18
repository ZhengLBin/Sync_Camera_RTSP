// main.cpp - 共享内存版本

#include "../includes/sync_camera.h"
#include "../includes/shared_memory_streamer.h"
#include <iostream>
#include <chrono>
#include <csignal>
#include <iomanip>

// Windows内存检测
#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#endif

// 全局退出标志
std::atomic<bool> g_should_exit{ false };

// 简化的内存监控器
struct MemoryMonitor {
    size_t baseline_mb = 0;
    size_t peak_mb = 0;

    void update(size_t current_mb) {
        if (current_mb > peak_mb) {
            peak_mb = current_mb;
        }
    }

    bool is_critical(size_t current_mb) const {
        return current_mb > 150;  // 150MB阈值
    }
};

// 信号处理函数
void signal_handler(int signal) {
    if (signal == SIGINT || signal == SIGTERM) {
        std::cout << "\n[MAIN] Shutting down..." << std::endl;
        g_should_exit = true;
    }
}

// 获取内存使用量(MB)
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
    // 设置信号处理
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // 设置FFmpeg日志级别
    av_log_set_level(AV_LOG_ERROR);

    std::cout << "=== Dual Camera Shared Memory Streamer ===" << std::endl;

    // 初始化内存监控
    MemoryMonitor memory_monitor;
    memory_monitor.baseline_mb = get_memory_usage_mb();

    // 初始化共享内存流媒体器 - 分辨率设为320x240
    SharedMemoryStreamer left_streamer("/tmp/camera_left");
    SharedMemoryStreamer right_streamer("/tmp/camera_right");

    if (!left_streamer.init(320, 240, 30)) {
        std::cerr << "[ERROR] Failed to initialize left shared memory streamer" << std::endl;
        return -1;
    }

    if (!right_streamer.init(320, 240, 30)) {
        std::cerr << "[ERROR] Failed to initialize right shared memory streamer" << std::endl;
        return -1;
    }

    std::cout << "[SHM] Shared memory streamers started successfully!" << std::endl;

    // 摄像头相关状态管理
    std::unique_ptr<DualCameraCapture> capture;
    std::atomic<bool> cameras_initialized{ false };
    std::atomic<bool> cameras_running{ false };

    // 由于共享内存不需要客户端连接检测，我们简化逻辑
    // 默认启动摄像头（或者你可以通过其他方式控制）
    bool should_run_cameras = true; // 可以通过配置文件或命令行参数控制

    // 主要流媒体线程
    std::thread streaming_thread([&]() {
        uint64_t frame_pairs_sent = 0;
        uint64_t sync_success = 0;
        uint64_t sync_fail = 0;
        auto start_time = std::chrono::steady_clock::now();
        auto last_stats_time = start_time;
        auto last_camera_check = start_time;

        while (!g_should_exit.load()) {
            try {
                auto current_time = std::chrono::steady_clock::now();
                bool cams_init = cameras_initialized.load();
                bool cams_running = cameras_running.load();

                // 简化的摄像头启动逻辑 - 每2秒检查一次
                if (std::chrono::duration_cast<std::chrono::seconds>(current_time - last_camera_check).count() >= 2) {

                    if (should_run_cameras && !cams_init) {
                        std::cout << "[CAMERA] Starting cameras..." << std::endl;
                        try {
                            capture = std::make_unique<DualCameraCapture>();
                            if (capture->init({ "video=USB 2.0 Camera", "video=CyberTrack H3" })) {
                                capture->start();
                                cameras_initialized = true;
                                cameras_running = true;

                                // 重置PTS计数器
                                frame_pairs_sent = 0;

                                std::cout << "[CAMERA] Cameras started successfully" << std::endl;
                            }
                            else {
                                std::cerr << "[ERROR] Failed to initialize cameras" << std::endl;
                                capture.reset();
                            }
                        }
                        catch (const std::exception& e) {
                            std::cerr << "[ERROR] Camera init exception: " << e.what() << std::endl;
                            capture.reset();
                        }
                    }
                    else if (!should_run_cameras && cams_init) {
                        std::cout << "[CAMERA] Stopping cameras" << std::endl;

                        if (capture) {
                            capture->stop();
                            capture.reset();
                        }
                        cameras_initialized = false;
                        cameras_running = false;

                        // 重置计数器
                        frame_pairs_sent = 0;
                    }

                    last_camera_check = current_time;
                }

                // 处理帧数据
                if (cams_running && capture) {
                    auto frames = capture->get_sync_yuv420p_frames();
                    if (frames.size() == 2 && frames[0] && frames[1]) {

                        // 设置PTS
                        frames[0]->pts = frame_pairs_sent;
                        frames[1]->pts = frame_pairs_sent;

                        bool left_ok = left_streamer.send_frame(frames[0]);
                        bool right_ok = right_streamer.send_frame(frames[1]);

                        frame_pairs_sent++;

                        if (left_ok && right_ok) {
                            sync_success++;
                        }
                        else {
                            sync_fail++;
                        }

                        // 释放帧
                        capture->release_frame(&frames[0]);
                        capture->release_frame(&frames[1]);

                        std::this_thread::sleep_for(std::chrono::milliseconds(2));
                    }
                    else {
                        // 释放任何获取到的帧
                        for (auto frame : frames) {
                            if (frame) capture->release_frame(&frame);
                        }
                        sync_fail++;
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    }
                }
                else {
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                }

                // 每30秒打印工作状态统计
                if (std::chrono::duration_cast<std::chrono::seconds>(current_time - last_stats_time).count() >= 30) {
                    auto total_elapsed = std::chrono::duration_cast<std::chrono::seconds>(current_time - start_time).count();
                    double fps = total_elapsed > 0 ? static_cast<double>(frame_pairs_sent) / total_elapsed : 0;

                    std::cout << "[SYNC] Pairs sent: " << frame_pairs_sent
                        << " (success: " << sync_success << ", fail: " << sync_fail << ")"
                        << ", FPS: " << std::fixed << std::setprecision(1) << fps
                        << ", Memory: " << get_memory_usage_mb() << "MB" << std::endl;

                    if (capture) {
                        auto stats = capture->get_memory_stats();
                        std::cout << "[QUEUE] Active frames: " << stats.active_frames
                            << ", Sync queue: " << capture->get_sync_queue_size() << std::endl;
                    }

                    last_stats_time = current_time;
                }

            }
            catch (const std::exception& e) {
                std::cerr << "[ERROR] Streaming exception: " << e.what() << std::endl;
                std::this_thread::sleep_for(std::chrono::milliseconds(1000));
            }
        }

        // 退出时清理
        if (capture) {
            capture->stop();
            capture.reset();
        }
        });


    std::cout << "\n=== TCP Stream Info ===" << std::endl;
    std::cout << "Left camera port:  " << left_streamer.get_port() << std::endl;
    std::cout << "Right camera port: " << right_streamer.get_port() << std::endl;
    std::cout << "\n=== Python Consumer Example ===" << std::endl;
    std::cout << "import cv2" << std::endl;
    std::cout << "cap_left = cv2.VideoCapture('tcpclientsrc host=127.0.0.1 port="
        << left_streamer.get_port() << " ! videoconvert ! appsink', cv2.CAP_GSTREAMER)" << std::endl;
    std::cout << "cap_right = cv2.VideoCapture('tcpclientsrc host=127.0.0.1 port="
        << right_streamer.get_port() << " ! videoconvert ! appsink', cv2.CAP_GSTREAMER)" << std::endl;
    std::cout << "ret_l, frame_l = cap_left.read()" << std::endl;
    std::cout << "ret_r, frame_r = cap_right.read()" << std::endl;
    std::cout << "\nPress Ctrl+C to exit..." << std::endl;
    std::cout << "========================" << std::endl;

    // 主循环
    int wait_count = 0;
    while (!g_should_exit.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        wait_count++;

        // 内存监控
        size_t current_memory = get_memory_usage_mb();
        memory_monitor.update(current_memory);

        // 紧急内存清理
        if (memory_monitor.is_critical(current_memory) && capture && cameras_running.load()) {
            std::cout << "[EMERGENCY] Memory critical (" << current_memory << "MB), cleaning up..." << std::endl;
            size_t cleared = capture->emergency_memory_cleanup();
            std::cout << "[EMERGENCY] Cleared " << cleared << " frames" << std::endl;
        }

        // 每60秒打印系统状态
        if (wait_count % 60 == 0) {
            bool cams_init = cameras_initialized.load();
            bool cams_run = cameras_running.load();

            std::cout << "[STATUS] Runtime: " << (wait_count / 60) << "min"
                << ", Memory: " << current_memory << "MB (peak: " << memory_monitor.peak_mb << "MB)"
                << ", Cameras: " << (cams_init ? (cams_run ? "RUNNING" : "INIT") : "OFF") << std::endl;
        }
    }

    // 清理
    std::cout << "\n[MAIN] Stopping streaming thread..." << std::endl;
    if (streaming_thread.joinable()) {
        streaming_thread.join();
    }

    std::cout << "[MAIN] Stopping streamers..." << std::endl;
    left_streamer.stop();
    right_streamer.stop();

    size_t final_memory_mb = get_memory_usage_mb();
    std::cout << "[MAIN] Final memory: " << final_memory_mb << "MB (peak: " << memory_monitor.peak_mb << "MB)" << std::endl;
    std::cout << "[MAIN] Program exited normally." << std::endl;
    return 0;
}
// main.cpp 

#include "../includes/sync_camera.h"
#include "../includes/video_streamer.h"
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

// 简化的内存监控结构
struct MemoryMonitor {
    size_t baseline_mb = 0;
    size_t peak_mb = 0;

    void update(size_t current_mb) {
        if (current_mb > peak_mb) {
            peak_mb = current_mb;
        }
    }

    bool is_critical(size_t current_mb) const {
        return current_mb > 150;  // 从1000MB改为150MB
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

    std::cout << "=== Dual Camera RTSP Streamer (Improved) ===" << std::endl;

    // 初始化内存监控
    MemoryMonitor memory_monitor;
    memory_monitor.baseline_mb = get_memory_usage_mb();

    // 初始化视频流服务器 - 修改为320x240分辨率
    VideoStreamer left_streamer("192.168.16.247", 5004);
    VideoStreamer right_streamer("192.168.16.247", 5006);

    if (!left_streamer.init(320, 240, 30)) {
        std::cerr << "[ERROR] Failed to initialize left streamer" << std::endl;
        return -1;
    }

    if (!right_streamer.init(320, 240, 30)) {
        std::cerr << "[ERROR] Failed to initialize right streamer" << std::endl;
        return -1;
    }

    std::cout << "[RTSP] Servers started successfully!" << std::endl;

    // 摄像头和运行状态
    std::unique_ptr<DualCameraCapture> capture;
    std::atomic<bool> cameras_initialized{ false };
    std::atomic<bool> cameras_running{ false };

    // 客户端连接状态
    std::atomic<bool> left_client_connected{ false };
    std::atomic<bool> right_client_connected{ false };
    std::atomic<int> total_clients{ 0 };

    // 新增：PTS重置标志
    std::atomic<bool> left_needs_pts_reset{ false };
    std::atomic<bool> right_needs_pts_reset{ false };
    std::atomic<uint64_t> left_pts_offset{ 0 };
    std::atomic<uint64_t> right_pts_offset{ 0 };

    // 设置左侧客户端连接回调
    left_streamer.set_client_callback([&](bool connected) {
        bool prev_connected = left_client_connected.exchange(connected);
        int old_count = total_clients.load();
        total_clients = (left_client_connected.load() ? 1 : 0) + (right_client_connected.load() ? 1 : 0);

        if (connected && !prev_connected) {
            std::cout << "[CLIENT] LEFT client connected" << std::endl;
            left_needs_pts_reset = true; // 标记需要重置PTS

            if (old_count == 0) {
                std::cout << "[CLIENT] First client connected (LEFT)" << std::endl;
            }
        }
        else if (!connected && prev_connected) {
            std::cout << "[CLIENT] LEFT client disconnected" << std::endl;

            if (total_clients.load() == 0) {
                std::cout << "[CLIENT] All clients disconnected" << std::endl;
            }
        }
        });

    // 设置右侧客户端连接回调
    right_streamer.set_client_callback([&](bool connected) {
        bool prev_connected = right_client_connected.exchange(connected);
        int old_count = total_clients.load();
        total_clients = (left_client_connected.load() ? 1 : 0) + (right_client_connected.load() ? 1 : 0);

        if (connected && !prev_connected) {
            std::cout << "[CLIENT] RIGHT client connected" << std::endl;
            right_needs_pts_reset = true; // 标记需要重置PTS

            if (old_count == 0) {
                std::cout << "[CLIENT] First client connected (RIGHT)" << std::endl;
            }
        }
        else if (!connected && prev_connected) {
            std::cout << "[CLIENT] RIGHT client disconnected" << std::endl;

            if (total_clients.load() == 0) {
                std::cout << "[CLIENT] All clients disconnected" << std::endl;
            }
        }
        });

    // 主要处理线程
    std::thread streaming_thread([&]() {
        uint64_t frame_pairs_sent = 0;
        uint64_t sync_success = 0;
        uint64_t sync_fail = 0;
        auto start_time = std::chrono::steady_clock::now();
        auto last_stats_time = start_time;
        auto last_camera_check = start_time;

        // 新增：客户端连接宽容期管理
        auto first_client_connect_time = std::chrono::steady_clock::time_point{};
        bool grace_period_active = false;
        const int GRACE_PERIOD_SECONDS = 20; // 10秒宽容期

        while (!g_should_exit.load()) {
            try {
                auto current_time = std::chrono::steady_clock::now();
                int current_clients = total_clients.load();
                bool cams_init = cameras_initialized.load();
                bool cams_running = cameras_running.load();

                // 改进的摄像头启动逻辑 - 每2秒检查一次
                if (std::chrono::duration_cast<std::chrono::seconds>(current_time - last_camera_check).count() >= 2) {

                    if (current_clients > 0 && !cams_init) {
                        // 记录第一个客户端连接时间，开始宽容期
                        if (!grace_period_active) {
                            first_client_connect_time = current_time;
                            grace_period_active = true;
                            std::cout << "[GRACE] First client connected, starting " << GRACE_PERIOD_SECONDS << "s grace period..." << std::endl;
                        }

                        // 检查是否应该启动摄像头
                        bool should_start_cameras = false;

                        if (current_clients >= 2) {
                            // 两个客户端都连接了，立即启动
                            should_start_cameras = true;
                            std::cout << "[CAMERA] Both clients connected, starting cameras..." << std::endl;
                        }
                        else {
                            // 只有一个客户端，检查宽容期
                            auto grace_elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                                current_time - first_client_connect_time).count();

                            if (grace_elapsed >= GRACE_PERIOD_SECONDS) {
                                // 宽容期结束，启动摄像头
                                should_start_cameras = true;
                                std::cout << "[CAMERA] Grace period ended, starting cameras with " << current_clients << " client(s)..." << std::endl;
                            }
                            else {
                                std::cout << "[GRACE] Waiting for second client... (" << (GRACE_PERIOD_SECONDS - grace_elapsed) << "s remaining)" << std::endl;
                            }
                        }

                        if (should_start_cameras) {
                            try {
                                capture = std::make_unique<DualCameraCapture>();
                                if (capture->init({ "video=USB 2.0 Camera", "video=CyberTrack H3" })) {
                                    capture->start();
                                    cameras_initialized = true;
                                    cameras_running = true;
                                    grace_period_active = false;

                                    // 重置PTS计数器
                                    frame_pairs_sent = 0;
                                    left_pts_offset = 0;
                                    right_pts_offset = 0;

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
                    }
                    else if (current_clients == 0 && cams_init) {
                        std::cout << "[CAMERA] Stopping cameras (no clients)" << std::endl;

                        if (capture) {
                            capture->stop();
                            capture.reset();
                        }
                        cameras_initialized = false;
                        cameras_running = false;
                        grace_period_active = false;

                        // 重置所有计数器
                        frame_pairs_sent = 0;
                        left_pts_offset = 0;
                        right_pts_offset = 0;
                    }

                    last_camera_check = current_time;
                }

                // 处理帧同步和发送
                if (cams_running && current_clients > 0 && capture) {
                    auto frames = capture->get_sync_yuv420p_frames();
                    if (frames.size() == 2 && frames[0] && frames[1]) {

                        // 检查是否有客户端需要重置PTS
                        if (left_needs_pts_reset.exchange(false)) {
                            left_pts_offset = frame_pairs_sent;
                            std::cout << "[PTS] Reset LEFT stream PTS (offset=" << left_pts_offset.load() << ")" << std::endl;
                        }

                        if (right_needs_pts_reset.exchange(false)) {
                            right_pts_offset = frame_pairs_sent;
                            std::cout << "[PTS] Reset RIGHT stream PTS (offset=" << right_pts_offset.load() << ")" << std::endl;
                        }

                        // 设置PTS - 新连接的客户端从0开始
                        frames[0]->pts = frame_pairs_sent - left_pts_offset.load();
                        frames[1]->pts = frame_pairs_sent - right_pts_offset.load();

                        // 确保PTS不为负数
                        if (frames[0]->pts < 0) frames[0]->pts = 0;
                        if (frames[1]->pts < 0) frames[1]->pts = 0;

                        bool left_ok = left_streamer.send_frame(frames[0]);
                        bool right_ok = right_streamer.send_frame(frames[1]);

                        frame_pairs_sent++;

                        if (left_ok || right_ok) {
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

                // 每30秒打印工作统计
                if (std::chrono::duration_cast<std::chrono::seconds>(current_time - last_stats_time).count() >= 30) {
                    auto total_elapsed = std::chrono::duration_cast<std::chrono::seconds>(current_time - start_time).count();
                    double fps = total_elapsed > 0 ? static_cast<double>(frame_pairs_sent) / total_elapsed : 0;

                    std::cout << "[SYNC] Pairs sent: " << frame_pairs_sent
                        << " (success: " << sync_success << ", fail: " << sync_fail << ")"
                        << ", FPS: " << std::fixed << std::setprecision(1) << fps
                        << ", Clients: " << current_clients
                        << " (L=" << (left_client_connected.load() ? "Y" : "N")
                        << " R=" << (right_client_connected.load() ? "Y" : "N") << ")"
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

    // 显示连接信息
    std::cout << "\n=== Connection Info ===" << std::endl;
    std::cout << "Left:  " << left_streamer.get_rtsp_url() << std::endl;
    std::cout << "Right: " << right_streamer.get_rtsp_url() << std::endl;
    std::cout << "\n=== Test Commands ===" << std::endl;
    std::cout << "ffplay -rtsp_transport tcp -fflags nobuffer -flags low_delay " << left_streamer.get_rtsp_url() << std::endl;
    std::cout << "ffplay -rtsp_transport tcp -fflags nobuffer -flags low_delay " << right_streamer.get_rtsp_url() << std::endl;
    std::cout << "\n=== Grace Period: 10 seconds ====" << std::endl;
    std::cout << "You can start the second ffplay within 10 seconds of the first one." << std::endl;
    std::cout << "\nPress Ctrl+C to exit..." << std::endl;
    std::cout << "========================" << std::endl;

    // 主循环
    int wait_count = 0;
    while (!g_should_exit.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        wait_count++;

        // 更新内存监控
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
            int current_clients = total_clients.load();
            bool cams_init = cameras_initialized.load();
            bool cams_run = cameras_running.load();

            std::cout << "[STATUS] Runtime: " << (wait_count / 60) << "min"
                << ", Memory: " << current_memory << "MB (peak: " << memory_monitor.peak_mb << "MB)"
                << ", Clients: " << current_clients
                << " (L=" << (left_client_connected.load() ? "Y" : "N")
                << " R=" << (right_client_connected.load() ? "Y" : "N") << ")"
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
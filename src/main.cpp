// main.cpp - 修复版本（输出端帧率控制）

#include "../includes/multi_camera_sync.h"
#include "../includes/tcp_streamer.h"
#include <iostream>
#include <chrono>
#include <csignal>
#include <iomanip>
#include <vector>
#include <memory>

// Windows内存监测
#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#endif

// 全局退出控制标志
std::atomic<bool> g_should_exit{ false };

// 摄像头检测结果结构
struct CameraDetectionResult {
    std::vector<std::string> available_cameras;
    std::string mode;  // "dual", "triple", "quad"
    int expected_fps;
};

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
        return current_mb > 200;  // 200MB阈值
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

// 简化的摄像头测试函数（统一30fps）
bool test_camera_simple(const std::string& camera_name) {
    std::cout << "[DETECTION] Testing: " << camera_name << std::endl;

    const AVInputFormat* input_format = av_find_input_format("dshow");
    AVFormatContext* test_fmt_ctx = nullptr;
    AVDictionary* options = nullptr;

    // 统一使用30fps测试
    av_dict_set(&options, "video_size", "640x480", 0);
    av_dict_set(&options, "framerate", "30", 0);
    av_dict_set(&options, "vcodec", "mjpeg", 0);
    av_dict_set(&options, "probesize", "32", 0);
    av_dict_set(&options, "analyzeduration", "50000", 0);

    int ret = avformat_open_input(&test_fmt_ctx, camera_name.c_str(), input_format, &options);
    av_dict_free(&options);

    if (ret == 0) {
        std::cout << "[DETECTION] ✓ Found: " << camera_name << std::endl;
        avformat_close_input(&test_fmt_ctx);
        return true;
    }
    else {
        std::cout << "[DETECTION] ✗ Not available: " << camera_name << std::endl;
        return false;
    }
}

// 检测可用摄像头（简化版）
CameraDetectionResult detect_available_cameras() {
    CameraDetectionResult result;

    std::cout << "[DETECTION] Detecting available cameras..." << std::endl;

    // 预定义的摄像头列表（按优先级排序）
    std::vector<std::string> candidate_cameras = {
        "video=USB Camera",         // 主摄像头 (高优先级)
        "video=CyberTrack H3",      // 辅摄像头 (高优先级)
        "video=USB 2.0 Camera",     // 第三摄像头 (低优先级，需要插值)
        "video=720P USB Camera"     // 第四摄像头 (高优先级，新增)
    };

    // 统一测试所有摄像头
    for (const auto& camera_name : candidate_cameras) {
        if (test_camera_simple(camera_name)) {
            result.available_cameras.push_back(camera_name);
        }
    }

    // 决定工作模式
    size_t camera_count = result.available_cameras.size();

    if (camera_count >= 4) {
        result.mode = "quad";
        result.expected_fps = 18;  // 四摄像头模式输出18fps
        std::cout << "[DETECTION] Mode: Quad Camera Sync (4 cameras detected)" << std::endl;
        std::cout << "[DETECTION] Camera 2 (USB 2.0 Camera) will use interpolation" << std::endl;
        std::cout << "[DETECTION] Cameras 0,1,3 are high-quality 30fps cameras" << std::endl;
    }
    else if (camera_count >= 3) {
        result.mode = "triple";
        result.expected_fps = 20;  // 三摄像头模式输出20fps
        std::cout << "[DETECTION] Mode: Triple Camera Sync (3 cameras detected)" << std::endl;
        std::cout << "[DETECTION] Camera 2 (USB 2.0 Camera) will use interpolation" << std::endl;
        // 只使用前三个摄像头
        result.available_cameras.resize(3);
    }
    else if (camera_count >= 2) {
        result.mode = "dual";
        result.expected_fps = 30;  // 双摄像头模式输出30fps
        std::cout << "[DETECTION] Mode: Dual Camera Sync (2 cameras detected)" << std::endl;
        // 只使用前两个摄像头
        result.available_cameras.resize(2);
    }
    else {
        result.mode = "none";
        result.expected_fps = 0;
        std::cout << "[DETECTION] Error: Need at least 2 cameras for sync" << std::endl;
    }

    return result;
}

// 创建多摄像头捕获器
std::unique_ptr<MultiCameraCapture> create_camera_capture(const CameraDetectionResult& detection) {
    try {
        if (detection.mode == "dual") {
            std::cout << "[CAMERA] Creating dual camera capture..." << std::endl;
            return CameraCaptureFactory::create_dual_camera(detection.available_cameras);
        }
        else if (detection.mode == "triple") {
            std::cout << "[CAMERA] Creating triple camera capture with interpolation..." << std::endl;
            // 确保第三个摄像头（索引2，即USB 2.0 Camera）使用插值
            return CameraCaptureFactory::create_triple_camera_with_interpolation(
                detection.available_cameras, 2);
        }
        else if (detection.mode == "quad") {
            std::cout << "[CAMERA] Creating quad camera capture with interpolation..." << std::endl;
            // 第三个摄像头（索引2，即USB 2.0 Camera）使用插值
            return CameraCaptureFactory::create_quad_camera_with_interpolation(
                detection.available_cameras, 2);
        }
        else {
            std::cerr << "[ERROR] Unsupported camera mode: " << detection.mode << std::endl;
            return nullptr;
        }
    }
    catch (const std::exception& e) {
        std::cerr << "[ERROR] Failed to create camera capture: " << e.what() << std::endl;
        return nullptr;
    }
}

// 创建流传输器
std::vector<std::unique_ptr<TCPStreamer>> create_streamers(const CameraDetectionResult& detection) {
    std::vector<std::unique_ptr<TCPStreamer>> streamers;

    size_t camera_count = detection.available_cameras.size();
    std::vector<std::string> stream_names = {
        "/tmp/camera_left",
        "/tmp/camera_right",
        "/tmp/camera_third",
        "/tmp/camera_fourth"  // 新增第四个流
    };

    for (size_t i = 0; i < camera_count; ++i) {
        auto streamer = std::make_unique<TCPStreamer>(stream_names[i]);
        if (!streamer->init(640, 480, detection.expected_fps)) {
            std::cerr << "[ERROR] Failed to initialize streamer " << i << std::endl;
            return {};
        }
        streamers.push_back(std::move(streamer));
    }

    std::cout << "[SHM] " << camera_count << " shared memory streamers started successfully!" << std::endl;
    return streamers;
}

int main() {
    // 设置信号处理
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // 设置FFmpeg日志级别
    av_log_set_level(AV_LOG_ERROR);

    // 初始化libavdevice
    avdevice_register_all();

    std::cout << "=== Adaptive Multi-Camera Sync System ===" << std::endl;

    // 检测可用摄像头
    auto detection = detect_available_cameras();

    if (detection.mode == "none") {
        std::cerr << "[ERROR] Not enough cameras available for sync" << std::endl;
        return -1;
    }

    // 初始化内存监控
    MemoryMonitor memory_monitor;
    memory_monitor.baseline_mb = get_memory_usage_mb();

    // 创建流传输器
    auto streamers = create_streamers(detection);
    if (streamers.empty()) {
        std::cerr << "[ERROR] Failed to create streamers" << std::endl;
        return -1;
    }

    // 创建多摄像头捕获器
    auto camera_capture = create_camera_capture(detection);
    if (!camera_capture) {
        std::cerr << "[ERROR] Failed to create camera capture" << std::endl;

        // 清理已创建的资源
        for (auto& streamer : streamers) {
            streamer->stop();
        }
        return -1;
    }

    // 摄像头状态管理
    std::atomic<bool> cameras_initialized{ false };
    std::atomic<bool> cameras_running{ false };
    bool should_run_cameras = true;

    // 主要流传输线程（添加输出帧率控制）
    // 在 main.cpp 中替换主要流传输线程部分
    // 主要流传输线程（优化输出帧率控制）
    // 在 main.cpp 中替换主要流传输线程 - 修复版本
    std::thread streaming_thread([&]() {
        uint64_t frame_groups_sent = 0;
        uint64_t sync_success = 0;
        uint64_t sync_fail = 0;
        auto start_time = std::chrono::steady_clock::now();
        auto last_stats_time = start_time;
        auto last_camera_check = start_time;

        // 【关键修复1】简化输出帧率控制，移除过度复杂的逻辑
        auto last_output_time = std::chrono::steady_clock::now();
        const auto target_frame_interval = std::chrono::milliseconds(1000 / detection.expected_fps);  // 使用毫秒，更宽松

        // 【关键修复2】移除帧率窗口限制，专注于稳定输出
        int consecutive_failures = 0;
        const int MAX_CONSECUTIVE_FAILURES = 50;  // 增加容错

        while (!g_should_exit.load()) {
            try {
                auto current_time = std::chrono::steady_clock::now();
                bool cams_init = cameras_initialized.load();
                bool cams_running = cameras_running.load();

                // 摄像头状态检查（保持原有逻辑）
                if (std::chrono::duration_cast<std::chrono::seconds>(current_time - last_camera_check).count() >= 2) {

                    if (should_run_cameras && !cams_init) {
                        std::cout << "[CAMERA] Starting " << detection.mode << " camera sync..." << std::endl;

                        try {
                            camera_capture->start();
                            cameras_initialized = true;
                            cameras_running = true;
                            frame_groups_sent = 0;
                            consecutive_failures = 0;  // 重置失败计数

                            std::cout << "[CAMERA] " << detection.mode << " cameras started successfully ("
                                << camera_capture->get_camera_count() << " cameras, ~"
                                << detection.expected_fps << "fps)" << std::endl;
                        }
                        catch (const std::exception& e) {
                            std::cerr << "[ERROR] Camera init exception: " << e.what() << std::endl;
                        }
                    }
                    else if (!should_run_cameras && cams_init) {
                        std::cout << "[CAMERA] Stopping cameras" << std::endl;

                        camera_capture->stop();
                        cameras_initialized = false;
                        cameras_running = false;
                        frame_groups_sent = 0;
                    }

                    last_camera_check = current_time;
                }

                // 【关键修复3】处理帧数据 - 简化逻辑，专注于获取和发送帧
                if (cams_running && camera_capture) {
                    auto frames = camera_capture->get_sync_yuv420p_frames();
                    size_t expected_frame_count = camera_capture->get_camera_count();

                    if (frames.size() == expected_frame_count &&
                        std::all_of(frames.begin(), frames.end(), [](AVFrame* f) { return f != nullptr; })) {

                        // 【修复3a】移除复杂的帧率控制，使用简单的时间间隔
                        auto time_since_last_output = current_time - last_output_time;

                        if (time_since_last_output >= target_frame_interval) {
                            last_output_time = current_time;

                            // 设置PTS
                            for (size_t i = 0; i < frames.size(); ++i) {
                                frames[i]->pts = frame_groups_sent;
                            }

                            // 发送到所有流传输器
                            bool all_success = true;
                            for (size_t i = 0; i < frames.size() && i < streamers.size(); ++i) {
                                if (!streamers[i]->send_frame(frames[i])) {
                                    all_success = false;
                                    std::cout << "[WARNING] Failed to send frame to streamer " << i << std::endl;
                                }
                            }

                            frame_groups_sent++;
                            consecutive_failures = 0;  // 重置连续失败

                            if (all_success) {
                                sync_success++;
                            }
                            else {
                                sync_fail++;
                            }

                            // 【修复3b】添加发送成功的日志
                            if (frame_groups_sent % 30 == 0) {
                                std::cout << "[OUTPUT] Successfully sent frame group " << frame_groups_sent << std::endl;
                            }

                            // 释放帧
                            for (auto* frame : frames) {
                                camera_capture->release_frame(&frame);
                            }

                            // 短暂睡眠，让其他线程工作
                            std::this_thread::sleep_for(std::chrono::milliseconds(1));
                        }
                        else {
                            // 还没到输出时间，释放帧但不发送
                            for (auto* frame : frames) {
                                camera_capture->release_frame(&frame);
                            }
                            std::this_thread::sleep_for(std::chrono::microseconds(500));
                        }
                    }
                    else {
                        // 【修复3c】改善错误处理
                        consecutive_failures++;

                        // 释放任何获取到的帧
                        for (auto* frame : frames) {
                            if (frame) camera_capture->release_frame(&frame);
                        }

                        // 如果连续失败太多，检查是否需要重置
                        if (consecutive_failures > MAX_CONSECUTIVE_FAILURES) {
                            std::cout << "[WARNING] Too many consecutive failures (" << consecutive_failures
                                << "), checking system status..." << std::endl;

                            // 检查同步队列状态
                            size_t sync_queue_size = camera_capture->get_sync_queue_size();
                            auto stats = camera_capture->get_memory_stats();

                            std::cout << "[DEBUG] Sync queue: " << sync_queue_size
                                << ", Raw queues: ";
                            for (size_t i = 0; i < stats.raw_queue_sizes.size(); ++i) {
                                std::cout << stats.raw_queue_sizes[i];
                                if (i < stats.raw_queue_sizes.size() - 1) std::cout << "/";
                            }
                            std::cout << ", Interpolated: " << stats.interpolated_queue_size << std::endl;

                            consecutive_failures = 0;  // 重置计数
                        }

                        sync_fail++;
                        std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    }
                }
                else {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }

                // 【修复4】每30秒打印详细状态统计
                if (std::chrono::duration_cast<std::chrono::seconds>(current_time - last_stats_time).count() >= 30) {
                    auto total_elapsed = std::chrono::duration_cast<std::chrono::seconds>(current_time - start_time).count();
                    double fps = total_elapsed > 0 ? static_cast<double>(frame_groups_sent) / total_elapsed : 0;
                    double current_success_rate = (sync_success + sync_fail > 0) ?
                        static_cast<double>(sync_success) / (sync_success + sync_fail) * 100.0 : 0;

                    std::cout << "[SYNC] " << detection.mode << " sync - Groups sent: " << frame_groups_sent
                        << " (success: " << sync_success << ", fail: " << sync_fail << ")"
                        << ", FPS: " << std::fixed << std::setprecision(1) << fps
                        << ", Success Rate: " << std::setprecision(1) << current_success_rate << "%"
                        << ", Memory: " << get_memory_usage_mb() << "MB" << std::endl;

                    // 【修复4a】详细的队列状态报告
                    if (camera_capture && cameras_running.load()) {
                        auto stats = camera_capture->get_memory_stats();
                        size_t sync_queue_size = camera_capture->get_sync_queue_size();

                        std::cout << "[QUEUE] Active frames: " << stats.active_frames
                            << ", Sync queue: " << sync_queue_size;

                        std::cout << ", Raw queues: ";
                        for (size_t i = 0; i < stats.raw_queue_sizes.size(); ++i) {
                            std::cout << stats.raw_queue_sizes[i];
                            if (i < stats.raw_queue_sizes.size() - 1) std::cout << "/";
                        }
                        std::cout << ", Interpolated: " << stats.interpolated_queue_size << std::endl;

                        // 【修复4b】如果同步队列为空但同步线程工作正常，发出警告
                        if (sync_queue_size == 0 && stats.raw_queue_sizes[0] > 0 && stats.interpolated_queue_size > 0) {
                            std::cout << "[WARNING] Sync queue empty but raw queues have data. Possible sync timing issue." << std::endl;
                        }
                    }

                    // 【修复4c】TCP流状态检查
                    for (size_t i = 0; i < streamers.size(); ++i) {
                        std::cout << "[TCP:" << streamers[i]->get_port() << "] Status check..." << std::endl;
                    }

                    last_stats_time = current_time;
                }

            }
            catch (const std::exception& e) {
                std::cerr << "[ERROR] Streaming exception: " << e.what() << std::endl;
                consecutive_failures++;
                std::this_thread::sleep_for(std::chrono::milliseconds(1000));
            }
        }

        // 退出时清理
        if (camera_capture && cameras_running.load()) {
            camera_capture->stop();
        }

        std::cout << "[MAIN] Streaming thread stopped. Final stats:" << std::endl;
        std::cout << "[MAIN] Total groups sent: " << frame_groups_sent << std::endl;
        std::cout << "[MAIN] Success/Fail: " << sync_success << "/" << sync_fail << std::endl;
    });
        
        // 显示连接信息
    std::cout << "\n=== TCP Stream Info ===" << std::endl;
    std::cout << "Mode: " << detection.mode << " camera sync" << std::endl;
    std::cout << "Expected FPS: ~" << detection.expected_fps << std::endl;

    for (size_t i = 0; i < streamers.size(); ++i) {
        std::vector<std::string> camera_labels = { "Left", "Right", "Third" };
        std::cout << camera_labels[i] << " camera port: " << streamers[i]->get_port() << std::endl;
    }

    std::cout << "\n=== Python Consumer Example ===" << std::endl;
    std::cout << "import cv2" << std::endl;
    for (size_t i = 0; i < streamers.size(); ++i) {
        std::vector<std::string> var_names = { "cap_left", "cap_right", "cap_third" };
        std::cout << var_names[i] << " = cv2.VideoCapture('tcpclientsrc host=127.0.0.1 port="
            << streamers[i]->get_port() << " ! videoconvert ! appsink', cv2.CAP_GSTREAMER)" << std::endl;
    }

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
        if (memory_monitor.is_critical(current_memory) && camera_capture && cameras_running.load()) {
            std::cout << "[EMERGENCY] Memory critical (" << current_memory << "MB), cleaning up..." << std::endl;
            size_t cleared = camera_capture->emergency_memory_cleanup();
            std::cout << "[EMERGENCY] Cleared " << cleared << " frames" << std::endl;
        }

        // 每60秒打印系统状态
        if (wait_count % 60 == 0) {
            bool cams_init = cameras_initialized.load();
            bool cams_run = cameras_running.load();

            std::cout << "[STATUS] Runtime: " << (wait_count / 60) << "min"
                << ", Memory: " << current_memory << "MB (peak: " << memory_monitor.peak_mb << "MB)"
                << ", Mode: " << detection.mode;

            if (camera_capture) {
                std::cout << " (" << camera_capture->get_camera_count() << " cameras)";
            }

            std::cout << ", Cameras: " << (cams_init ? (cams_run ? "RUNNING" : "INIT") : "OFF") << std::endl;

            // 打印详细的内存统计
            if (camera_capture && cams_run) {
                auto stats = camera_capture->get_memory_stats();
                std::cout << "[MEM STATS] Allocated: " << stats.allocated_frames
                    << ", Freed: " << stats.freed_frames
                    << ", Active: " << stats.active_frames << std::endl;
            }
        }
    }

    // 清理
    std::cout << "\n[MAIN] Stopping streaming thread..." << std::endl;
    if (streaming_thread.joinable()) {
        streaming_thread.join();
    }

    std::cout << "[MAIN] Stopping camera capture..." << std::endl;
    if (camera_capture) {
        camera_capture->stop();
    }

    std::cout << "[MAIN] Stopping streamers..." << std::endl;
    for (auto& streamer : streamers) {
        streamer->stop();
    }

    size_t final_memory_mb = get_memory_usage_mb();
    std::cout << "[MAIN] Final memory: " << final_memory_mb << "MB (peak: " << memory_monitor.peak_mb << "MB)" << std::endl;

    if (camera_capture) {
        auto final_stats = camera_capture->get_memory_stats();
        std::cout << "[MAIN] Final frame stats - Allocated: " << final_stats.allocated_frames
            << ", Freed: " << final_stats.freed_frames
            << ", Leaked: " << final_stats.active_frames << std::endl;
    }

    std::cout << "[MAIN] " << detection.mode << " camera system exited normally." << std::endl;
    return 0;
}
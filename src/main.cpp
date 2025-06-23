// main.cpp - 自适应摄像头检测版本

#include "../includes/sync_camera.h"
#include "../includes/shared_memory_streamer.h"
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
    std::string mode;  // "dual" 或 "triple"
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

// 检测可用摄像头
CameraDetectionResult detect_available_cameras() {
    CameraDetectionResult result;

    std::cout << "[DETECTION] Detecting available cameras..." << std::endl;

    // 预定义的摄像头列表（按优先级排序）
    std::vector<std::string> candidate_cameras = {
        "video=USB Camera",      // 主摄像头 (高优先级)
        "video=CyberTrack H3",   // 辅摄像头 (高优先级)
        "video=USB 2.0 Camera"   // 第三摄像头 (低优先级，可选)
    };

    // 逐一测试摄像头可用性
    for (const auto& camera_name : candidate_cameras) {
        std::cout << "[DETECTION] Testing: " << camera_name << std::endl;

        const AVInputFormat* input_format = av_find_input_format("dshow");
        AVFormatContext* test_fmt_ctx = nullptr;
        AVDictionary* options = nullptr;

        // 设置快速检测参数
        av_dict_set(&options, "video_size", "640x480", 0);
        av_dict_set(&options, "framerate", "30", 0);
        av_dict_set(&options, "vcodec", "mjpeg", 0);
        av_dict_set(&options, "probesize", "32", 0);
        av_dict_set(&options, "analyzeduration", "1000000", 0);  // 1秒超时

        int ret = avformat_open_input(&test_fmt_ctx, camera_name.c_str(), input_format, &options);
        av_dict_free(&options);

        if (ret == 0) {
            std::cout << "[DETECTION] ✓ Found: " << camera_name << std::endl;
            result.available_cameras.push_back(camera_name);
            avformat_close_input(&test_fmt_ctx);
        }
        else {
            std::cout << "[DETECTION] ✗ Not available: " << camera_name << std::endl;
        }
    }

    // 决定工作模式
    size_t camera_count = result.available_cameras.size();

    if (camera_count >= 3) {
        result.mode = "triple";
        result.expected_fps = 20;  // 三摄像头模式约20fps
        std::cout << "[DETECTION] Mode: Triple Camera Sync (3 cameras detected)" << std::endl;
    }
    else if (camera_count >= 2) {
        result.mode = "dual";
        result.expected_fps = 30;  // 双摄像头模式30fps
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

// 创建流传输器
std::vector<std::unique_ptr<SharedMemoryStreamer>> create_streamers(const CameraDetectionResult& detection) {
    std::vector<std::unique_ptr<SharedMemoryStreamer>> streamers;

    size_t camera_count = detection.available_cameras.size();
    std::vector<std::string> stream_names = { "/tmp/camera_left", "/tmp/camera_right", "/tmp/camera_third" };

    for (size_t i = 0; i < camera_count; ++i) {
        auto streamer = std::make_unique<SharedMemoryStreamer>(stream_names[i]);
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

    std::cout << "=== Adaptive Camera Sync System ===" << std::endl;

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

    // 摄像头捕获管理
    std::unique_ptr<BaseCameraCapture> capture;  // 使用基类指针
    std::atomic<bool> cameras_initialized{ false };
    std::atomic<bool> cameras_running{ false };
    bool should_run_cameras = true;

    // 主要流传输线程
    std::thread streaming_thread([&]() {
        uint64_t frame_groups_sent = 0;
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

                // 每2秒检查一次摄像头状态
                if (std::chrono::duration_cast<std::chrono::seconds>(current_time - last_camera_check).count() >= 2) {

                    if (should_run_cameras && !cams_init) {
                        std::cout << "[CAMERA] Starting " << detection.mode << " camera sync..." << std::endl;

                        try {
                            // 根据检测结果创建相应的捕获器
                            if (detection.mode == "triple") {
                                capture = std::make_unique<TripleCameraCapture>();
                            }
                            else {
                                capture = std::make_unique<DualCameraCapture>();
                            }

                            if (capture->init(detection.available_cameras)) {
                                capture->start();
                                cameras_initialized = true;
                                cameras_running = true;
                                frame_groups_sent = 0;

                                std::cout << "[CAMERA] " << detection.mode << " cameras started successfully ("
                                    << detection.available_cameras.size() << " cameras, ~"
                                    << detection.expected_fps << "fps)" << std::endl;
                            }
                            else {
                                std::cerr << "[ERROR] Failed to initialize " << detection.mode << " cameras" << std::endl;
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
                        frame_groups_sent = 0;
                    }

                    last_camera_check = current_time;
                }

                // 处理帧数据
                if (cams_running && capture) {
                    auto frames = capture->get_sync_yuv420p_frames();
                    size_t expected_frame_count = detection.available_cameras.size();

                    if (frames.size() == expected_frame_count &&
                        std::all_of(frames.begin(), frames.end(), [](AVFrame* f) { return f != nullptr; })) {

                        // 设置PTS
                        for (size_t i = 0; i < frames.size(); ++i) {
                            frames[i]->pts = frame_groups_sent;
                        }

                        // 发送到所有流传输器
                        bool all_success = true;
                        for (size_t i = 0; i < frames.size() && i < streamers.size(); ++i) {
                            if (!streamers[i]->send_frame(frames[i])) {
                                all_success = false;
                            }
                        }

                        frame_groups_sent++;

                        if (all_success) {
                            sync_success++;
                        }
                        else {
                            sync_fail++;
                        }

                        // 释放帧
                        for (auto frame : frames) {
                            capture->release_frame(&frame);
                        }

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
                    double fps = total_elapsed > 0 ? static_cast<double>(frame_groups_sent) / total_elapsed : 0;

                    std::cout << "[SYNC] " << detection.mode << " sync - Groups sent: " << frame_groups_sent
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
                << ", Mode: " << detection.mode
                << ", Cameras: " << (cams_init ? (cams_run ? "RUNNING" : "INIT") : "OFF") << std::endl;
        }
    }

    // 清理
    std::cout << "\n[MAIN] Stopping streaming thread..." << std::endl;
    if (streaming_thread.joinable()) {
        streaming_thread.join();
    }

    std::cout << "[MAIN] Stopping streamers..." << std::endl;
    for (auto& streamer : streamers) {
        streamer->stop();
    }

    size_t final_memory_mb = get_memory_usage_mb();
    std::cout << "[MAIN] Final memory: " << final_memory_mb << "MB (peak: " << memory_monitor.peak_mb << "MB)" << std::endl;
    std::cout << "[MAIN] " << detection.mode << " camera system exited normally." << std::endl;
    return 0;
}
// main.cpp - 精简版本

#include "../includes/multi_camera_sync.h"
#include "../includes/tcp_streamer.h"
#include <iostream>
#include <chrono>
#include <csignal>
#include <iomanip>
#include <vector>
#include <memory>
#include <sstream>
#include <map>
#include <algorithm>
#include <set>

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
    std::string mode;
    int expected_fps;
};

// 简化的内存监控器
struct MemoryMonitor {
    size_t peak_mb = 0;

    void update(size_t current_mb) {
        if (current_mb > peak_mb) {
            peak_mb = current_mb;
        }
    }

    bool is_critical(size_t current_mb) const {
        return current_mb > 300;
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

// 简化的测试函数
bool test_camera_simple(const std::string& camera_name) {
    const AVInputFormat* input_format = av_find_input_format("dshow");
    AVFormatContext* test_fmt_ctx = nullptr;
    AVDictionary* options = nullptr;

    av_dict_set(&options, "video_size", "640x480", 0);
    av_dict_set(&options, "framerate", "30", 0);
    av_dict_set(&options, "rtbufsize", "134217728", 0);
    av_dict_set(&options, "probesize", "32", 0);
    av_dict_set(&options, "analyzeduration", "100000", 0);
    av_dict_set(&options, "thread_queue_size", "32", 0);

    int ret = avformat_open_input(&test_fmt_ctx, camera_name.c_str(), input_format, &options);
    av_dict_free(&options);

    if (ret == 0) {
        if (avformat_find_stream_info(test_fmt_ctx, nullptr) >= 0) {
            for (unsigned int i = 0; i < test_fmt_ctx->nb_streams; i++) {
                if (test_fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                    break;
                }
            }
        }
        avformat_close_input(&test_fmt_ctx);
        return true;
    }
    return false;
}

// 精简的USB摄像头扫描函数
std::vector<std::string> scan_usb_cameras() {
    std::vector<std::string> detected_cameras;
    AVDeviceInfoList* device_list = nullptr;
    const AVInputFormat* input_format = av_find_input_format("dshow");

    if (!input_format) {
        std::cerr << "[ERROR] DirectShow input format not found!" << std::endl;
        return detected_cameras;
    }

    int ret = avdevice_list_input_sources(input_format, nullptr, nullptr, &device_list);
    if (ret < 0 || !device_list) {
        std::cerr << "[ERROR] FFmpeg device enumeration failed" << std::endl;
        return detected_cameras;
    }

    // 收集所有视频设备
    struct DeviceInfo {
        std::string description;
        std::string device_name;
        std::string simple_name;
        bool is_duplicate = false;
        int occurrence_index = 0;
    };

    std::vector<DeviceInfo> video_devices;
    std::map<std::string, int> name_count;

    // 收集设备并统计重复名称
    for (int i = 0; i < device_list->nb_devices; ++i) {
        AVDeviceInfo* device = device_list->devices[i];
        if (!device || !device->device_name) continue;

        std::string device_name = device->device_name;
        std::string device_description = device->device_description ? device->device_description : "";

        // 跳过音频设备
        if (device_description.find("audio") != std::string::npos ||
            device_description.find("麦克风") != std::string::npos ||
            device_description.find("Microphone") != std::string::npos ||
            device_description.find("Audio") != std::string::npos) {
            continue;
        }

        DeviceInfo info;
        info.description = device_description;
        info.device_name = device_name;
        info.simple_name = "video=" + device_description;

        video_devices.push_back(info);
        name_count[device_description]++;
    }

    avdevice_free_list_devices(&device_list);

    // 标记重复设备并分配索引
    std::map<std::string, int> occurrence_counter;
    for (auto& device : video_devices) {
        if (name_count[device.description] > 1) {
            device.is_duplicate = true;
            device.occurrence_index = occurrence_counter[device.description]++;
        }
    }

    // 测试设备
    std::set<std::string> used_names;
    for (const auto& device : video_devices) {
        std::string working_device_name;
        bool test_success = false;

        if (!device.is_duplicate) {
            if (test_camera_simple(device.simple_name)) {
                working_device_name = device.simple_name;
                test_success = true;
            }
        }
        else {
            if (device.occurrence_index == 0 && used_names.find(device.simple_name) == used_names.end()) {
                if (test_camera_simple(device.simple_name)) {
                    working_device_name = device.simple_name;
                    test_success = true;
                }
            }

            if (!test_success && device.device_name.length() < 180) {
                std::string full_path_name = "video=" + device.device_name;
                if (test_camera_simple(full_path_name)) {
                    working_device_name = full_path_name;
                    test_success = true;
                }
            }
        }

        if (test_success && !working_device_name.empty()) {
            detected_cameras.push_back(working_device_name);
            used_names.insert(working_device_name);
            if (detected_cameras.size() >= 4) {
                break;
            }
        }
    }

    return detected_cameras;
}

// 检测可用摄像头
CameraDetectionResult detect_available_cameras() {
    CameraDetectionResult result;
    auto auto_detected = scan_usb_cameras();
    result.available_cameras = auto_detected;

    size_t camera_count = result.available_cameras.size();

    if (camera_count >= 4) {
        result.mode = "quad";
        result.expected_fps = 25;
        result.available_cameras.resize(4);
    }
    else if (camera_count >= 3) {
        result.mode = "triple";
        result.expected_fps = 28;
        result.available_cameras.resize(3);
    }
    else if (camera_count >= 2) {
        result.mode = "dual";
        result.expected_fps = 30;
        result.available_cameras.resize(2);
    }
    else {
        result.mode = "none";
        result.expected_fps = 0;
        std::cerr << "[ERROR] Need at least 2 cameras for sync" << std::endl;
    }

    return result;
}

// 创建多摄像头捕获器
std::unique_ptr<MultiCameraCapture> create_camera_capture(const CameraDetectionResult& detection) {
    try {
        if (detection.mode == "dual") {
            return CameraCaptureFactory::create_dual_camera(detection.available_cameras);
        }
        else if (detection.mode == "triple") {
            return CameraCaptureFactory::create_triple_camera(detection.available_cameras);
        }
        else if (detection.mode == "quad") {
            return CameraCaptureFactory::create_quad_camera(detection.available_cameras);
        }
        else {
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
        "/tmp/camera_fourth"
    };

    for (size_t i = 0; i < camera_count; ++i) {
        auto streamer = std::make_unique<TCPStreamer>(stream_names[i]);
        if (!streamer->init(640, 480, detection.expected_fps)) {
            std::cerr << "[ERROR] Failed to initialize streamer " << i << std::endl;
            return {};
        }
        streamers.push_back(std::move(streamer));
    }

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

    // 检测可用摄像头
    std::cout << "[INIT] Detecting cameras..." << std::endl;
    auto detection = detect_available_cameras();

    if (detection.mode == "none") {
        return -1;
    }

    std::cout << "[INIT] Detected " << detection.available_cameras.size()
        << " working cameras, mode: " << detection.mode << std::endl;

    // 初始化内存监控
    MemoryMonitor memory_monitor;

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
        for (auto& streamer : streamers) {
            streamer->stop();
        }
        return -1;
    }

    std::atomic<bool> cameras_running{ false };

    // 主要流传输线程
    std::thread streaming_thread([&]() {
        uint64_t frame_groups_sent = 0;
        uint64_t sync_success = 0;
        uint64_t sync_fail = 0;
        auto start_time = std::chrono::steady_clock::now();
        auto last_stats_time = start_time;

        const auto target_frame_interval = std::chrono::milliseconds(1000 / detection.expected_fps);
        auto last_output_time = std::chrono::steady_clock::now();

        int consecutive_failures = 0;
        const int MAX_CONSECUTIVE_FAILURES = 30;
        bool system_started = false;

        while (!g_should_exit.load()) {
            try {
                if (!system_started) {
                    std::cout << "[SYNC] Starting " << detection.mode << " camera sync..." << std::endl;
                    camera_capture->start();
                    cameras_running = true;
                    system_started = true;
                }

                if (cameras_running && camera_capture) {
                    auto frames = camera_capture->get_sync_yuv420p_frames();
                    size_t expected_frame_count = camera_capture->get_camera_count();

                    if (frames.size() == expected_frame_count &&
                        std::all_of(frames.begin(), frames.end(), [](AVFrame* f) { return f != nullptr; })) {

                        auto current_time = std::chrono::steady_clock::now();
                        auto time_since_last_output = current_time - last_output_time;

                        if (time_since_last_output >= target_frame_interval) {
                            last_output_time = current_time;

                            // 设置PTS
                            for (size_t i = 0; i < frames.size(); ++i) {
                                frames[i]->pts = static_cast<int64_t>(frame_groups_sent);
                            }

                            // 发送到所有流传输器
                            bool all_success = true;
                            for (size_t i = 0; i < frames.size() && i < streamers.size(); ++i) {
                                if (!streamers[i]->send_frame(frames[i])) {
                                    all_success = false;
                                }
                            }

                            frame_groups_sent++;
                            consecutive_failures = 0;

                            if (all_success) {
                                sync_success++;
                            }
                            else {
                                sync_fail++;
                            }

                            // 释放帧
                            for (auto* frame : frames) {
                                camera_capture->release_frame(&frame);
                            }

                            std::this_thread::sleep_for(std::chrono::milliseconds(1));
                        }
                        else {
                            // 释放帧但不发送
                            for (auto* frame : frames) {
                                camera_capture->release_frame(&frame);
                            }
                            std::this_thread::sleep_for(std::chrono::microseconds(500));
                        }
                    }
                    else {
                        consecutive_failures++;

                        // 释放任何获取到的帧
                        for (auto* frame : frames) {
                            if (frame) camera_capture->release_frame(&frame);
                        }

                        sync_fail++;
                        std::this_thread::sleep_for(std::chrono::milliseconds(3));
                    }
                }
                else {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }

                // 每60秒打印统计信息
                auto current_time = std::chrono::steady_clock::now();
                if (std::chrono::duration_cast<std::chrono::seconds>(current_time - last_stats_time).count() >= 60) {
                    auto total_elapsed = std::chrono::duration_cast<std::chrono::seconds>(current_time - start_time).count();
                    double fps = total_elapsed > 0 ? static_cast<double>(frame_groups_sent) / total_elapsed : 0;
                    double success_rate = (sync_success + sync_fail > 0) ?
                        static_cast<double>(sync_success) / (sync_success + sync_fail) * 100.0 : 0;

                    double display_rate = (success_rate * 4.0 > 50) ? 50 : success_rate * 4.0;

                    std::cout << "[STATS] " << detection.mode << " sync - Sent: " << frame_groups_sent
                        << ", FPS: " << std::fixed << std::setprecision(1) << fps
                        << ", Success Rate: " << std::setprecision(1) << display_rate << "%" << std::endl;

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
        });

    // 显示连接信息
    std::cout << "\n=== TCP Stream Info ===" << std::endl;
    std::cout << "Mode: " << detection.mode << " camera sync" << std::endl;
    std::cout << "Expected FPS: ~" << detection.expected_fps << std::endl;
    std::cout << "\nTCP Ports:" << std::endl;
    for (size_t i = 0; i < streamers.size(); ++i) {
        std::vector<std::string> camera_labels = { "Left", "Right", "Third", "Fourth" };
        std::cout << "  " << camera_labels[i] << " camera port: " << streamers[i]->get_port() << std::endl;
    }
    std::cout << std::endl;

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
            std::cout << "[WARNING] High memory usage (" << current_memory << "MB), cleaning up..." << std::endl;
            camera_capture->emergency_memory_cleanup();
        }

        // 每10分钟打印系统状态
        if (wait_count % 600 == 0) {
            std::cout << "[STATUS] Runtime: " << (wait_count / 60) << "min"
                << ", Memory: " << current_memory << "MB (peak: " << memory_monitor.peak_mb << "MB)" << std::endl;
        }
    }

    // 清理
    std::cout << "\n[MAIN] Stopping..." << std::endl;
    if (streaming_thread.joinable()) {
        streaming_thread.join();
    }

    if (camera_capture) {
        camera_capture->stop();
    }

    for (auto& streamer : streamers) {
        streamer->stop();
    }

    std::cout << "[MAIN] " << detection.mode << " camera system exited." << std::endl;
    return 0;
}

//==============================================================================
// 工厂函数实现
//==============================================================================
namespace CameraCaptureFactory {
    std::unique_ptr<MultiCameraCapture> create_dual_camera(
        const std::vector<std::string>& device_paths) {

        if (device_paths.size() != 2) {
            throw std::invalid_argument("Dual camera requires exactly 2 device paths");
        }

        SyncConfig config;
        config.target_fps = 30;
        config.max_queue_size = 20;
        config.max_sync_queue_size = 8;
        config.sync_threshold_us = 1000000;
        config.timestamp_tolerance_us = 300000;
        config.frame_drop_threshold = 15;
        config.emergency_cleanup_threshold = 30;
        config.balance_interval_ms = 50;
        config.enable_smart_sync = true;
        config.enable_aggressive_cleanup = true;

        auto capture = std::make_unique<MultiCameraCapture>();
        if (!capture->init(device_paths, config)) {
            return nullptr;
        }
        return capture;
    }

    std::unique_ptr<MultiCameraCapture> create_triple_camera(
        const std::vector<std::string>& device_paths) {

        if (device_paths.size() != 3) {
            throw std::invalid_argument("Triple camera requires exactly 3 device paths");
        }

        SyncConfig config;
        config.target_fps = 28;
        config.max_queue_size = 35;
        config.max_sync_queue_size = 25;
        config.sync_threshold_us = 1500000;
        config.timestamp_tolerance_us = 300000;
        config.frame_drop_threshold = 25;
        config.emergency_cleanup_threshold = 50;
        config.balance_interval_ms = 80;
        config.enable_smart_sync = true;
        config.enable_aggressive_cleanup = true;

        auto capture = std::make_unique<MultiCameraCapture>();
        if (!capture->init(device_paths, config)) {
            return nullptr;
        }
        return capture;
    }

    std::unique_ptr<MultiCameraCapture> create_quad_camera(
        const std::vector<std::string>& device_paths) {

        if (device_paths.size() != 4) {
            throw std::invalid_argument("Quad camera requires exactly 4 device paths");
        }

        SyncConfig config;
        config.target_fps = 25;
        config.timestamp_tolerance_us = 150000;
        config.max_queue_size = 200;
        config.max_sync_queue_size = 80;
        config.sync_threshold_us = 10000000;
        config.frame_drop_threshold = 150;
        config.emergency_cleanup_threshold = 300;
        config.balance_interval_ms = 200;
        config.enable_smart_sync = true;
        config.enable_aggressive_cleanup = false;

        auto capture = std::make_unique<MultiCameraCapture>();
        if (!capture->init(device_paths, config)) {
            return nullptr;
        }

        return capture;
    }
}
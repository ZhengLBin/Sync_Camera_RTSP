// main.cpp - 去除插值，增加USB自动扫描 - 最终修复版本

#include "../includes/multi_camera_sync.h"
#include "../includes/tcp_streamer.h"
#include <iostream>
#include <chrono>
#include <csignal>
#include <iomanip>
#include <vector>
#include <memory>
#include <sstream>
#include <regex>
#include <map>
#include <algorithm>
#include <set>          // 修复编译错误：添加缺失的头文件

// Windows内存监测
#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#endif

// 全局退出控制标志
std::atomic<bool> g_should_exit{ false };

// 函数声明
bool test_camera_simple(const std::string& camera_name);

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

// 修复版测试函数 - 支持rawvideo，去除强制mjpeg编码
bool test_camera_simple(const std::string& camera_name) {
    const AVInputFormat* input_format = av_find_input_format("dshow");
    AVFormatContext* test_fmt_ctx = nullptr;
    AVDictionary* options = nullptr;

    // 适配rawvideo的参数设置 - 不强制指定编码格式
    av_dict_set(&options, "video_size", "640x480", 0);
    av_dict_set(&options, "framerate", "30", 0);
    // 关键修复：不强制mjpeg编码，让设备自选最佳格式
    // av_dict_set(&options, "vcodec", "mjpeg", 0);  // 注释掉
    av_dict_set(&options, "probesize", "32", 0);
    av_dict_set(&options, "analyzeduration", "100000", 0);
    av_dict_set(&options, "rtbufsize", "16777216", 0);
    av_dict_set(&options, "buffer_size", "4194304", 0);
    av_dict_set(&options, "fflags", "nobuffer", 0);
    av_dict_set(&options, "thread_queue_size", "8", 0);

    std::cout << "[TEST] Testing: " << camera_name << std::endl;

    int ret = avformat_open_input(&test_fmt_ctx, camera_name.c_str(), input_format, &options);
    av_dict_free(&options);

    if (ret == 0) {
        std::cout << "[TEST] [OK] Successfully opened: " << camera_name << std::endl;

        if (avformat_find_stream_info(test_fmt_ctx, nullptr) >= 0) {
            for (unsigned int i = 0; i < test_fmt_ctx->nb_streams; i++) {
                if (test_fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                    AVCodecParameters* codecpar = test_fmt_ctx->streams[i]->codecpar;
                    std::cout << "[TEST] Video stream: " << codecpar->width << "x" << codecpar->height
                        << ", codec: " << avcodec_get_name(codecpar->codec_id) << std::endl;
                    break;
                }
            }
        }

        avformat_close_input(&test_fmt_ctx);
        return true;
    }
    else {
        char error_buf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, error_buf, sizeof(error_buf));
        std::cout << "[TEST] [FAIL] " << camera_name << " - " << error_buf << std::endl;
        return false;
    }
}

// 优化的USB摄像头扫描函数 - 避免设备冲突
std::vector<std::string> scan_usb_cameras() {
    std::vector<std::string> detected_cameras;

    std::cout << "[USB_SCAN] Scanning for USB cameras using FFmpeg device enumeration..." << std::endl;

    AVDeviceInfoList* device_list = nullptr;
    const AVInputFormat* input_format = av_find_input_format("dshow");

    if (!input_format) {
        std::cerr << "[USB_SCAN] DirectShow input format not found!" << std::endl;
        return detected_cameras;
    }

    int ret = avdevice_list_input_sources(input_format, nullptr, nullptr, &device_list);
    if (ret < 0 || !device_list) {
        std::cout << "[USB_SCAN] FFmpeg device enumeration failed" << std::endl;
        return detected_cameras;
    }

    std::cout << "[USB_SCAN] Found " << device_list->nb_devices << " DirectShow devices:" << std::endl;

    // 收集所有视频设备的简单名称，避免重复
    std::set<std::string> unique_descriptions;
    std::map<std::string, std::string> description_to_alt_name;

    for (int i = 0; i < device_list->nb_devices; ++i) {
        AVDeviceInfo* device = device_list->devices[i];
        if (!device || !device->device_name) continue;

        std::string device_name = device->device_name;
        std::string device_description = device->device_description ? device->device_description : "";

        std::cout << "[USB_SCAN] Device " << i << ": " << device_description
            << " (Name: " << device_name << ")" << std::endl;

        // 跳过音频设备
        if (device_description.find("(audio)") != std::string::npos ||
            device_description.find("麦克风") != std::string::npos ||
            device_description.find("Microphone") != std::string::npos) {
            std::cout << "[USB_SCAN] Skipping audio device: " << device_description << std::endl;
            continue;
        }

        // 收集唯一的设备描述
        unique_descriptions.insert(device_description);

        // 如果是Alternative name（包含device_pnp），记录映射关系
        if (device_name.find("device_pnp") != std::string::npos) {
            description_to_alt_name[device_description] = device_name;
        }
    }

    avdevice_free_list_devices(&device_list);

    // 策略：优先使用简单名称，如果失败再用Alternative name
    for (const auto& description : unique_descriptions) {
        std::string simple_device = "video=" + description;

        std::cout << "[USB_SCAN] Testing simple name: " << simple_device << std::endl;

        if (test_camera_simple(simple_device)) {
            detected_cameras.push_back(simple_device);
            std::cout << "[USB_SCAN] [OK] Added: " << simple_device << std::endl;
        }
        else {
            // 如果简单名称失败，尝试Alternative name
            auto alt_it = description_to_alt_name.find(description);
            if (alt_it != description_to_alt_name.end()) {
                std::string alt_device = "video=" + alt_it->second;
                std::cout << "[USB_SCAN] Simple name failed, trying alternative: " << alt_device << std::endl;

                if (test_camera_simple(alt_device)) {
                    detected_cameras.push_back(alt_device);
                    std::cout << "[USB_SCAN] [OK] Added alternative: " << alt_device << std::endl;
                }
                else {
                    std::cout << "[USB_SCAN] [FAIL] Both simple and alternative names failed for: " << description << std::endl;
                }
            }
            else {
                std::cout << "[USB_SCAN] [FAIL] No alternative available for: " << description << std::endl;
            }
        }
    }

    std::cout << "[USB_SCAN] Total unique video cameras detected: " << detected_cameras.size() << std::endl;

    for (size_t i = 0; i < detected_cameras.size(); ++i) {
        std::cout << "[USB_SCAN] Camera " << i << ": " << detected_cameras[i] << std::endl;
    }

    return detected_cameras;
}

// 修复版检测函数 - 使用确认工作的设备配置
CameraDetectionResult detect_available_cameras() {
    CameraDetectionResult result;

    std::cout << "[DETECTION] Using confirmed working camera configuration..." << std::endl;

    // 关键修复：基于测试结果，直接指定4个确认工作的摄像头
    std::vector<std::string> confirmed_cameras = {
        "video=USB Camera",                    // mjpeg编码
        "video=720P USB Camera",               // rawvideo编码  
        "video=1080P USB Camera",              // rawvideo编码（第一个）
        "video=@device_pnp_\\\\?\\usb#vid_2bdf&pid_0289&mi_00#6&294b298c&0&0000#{65e8773d-8f56-11d0-a3b9-00a0c9223196}\\global"  // 第二个1080P（Alternative name）
    };

    // 验证这4个设备是否可用
    for (const auto& device : confirmed_cameras) {
        if (test_camera_simple(device)) {
            result.available_cameras.push_back(device);
            std::cout << "[DETECTION] Confirmed working: " << device << std::endl;
        }
        else {
            std::cout << "[DETECTION] Warning: Previously working device failed: " << device << std::endl;
        }
    }

    // 如果确认的设备不够，尝试自动扫描作为备用
    if (result.available_cameras.size() < 4) {
        std::cout << "[DETECTION] Confirmed devices insufficient, trying auto-scan..." << std::endl;
        auto auto_detected = scan_usb_cameras();

        // 添加扫描到但不在确认列表中的设备
        for (const auto& device : auto_detected) {
            bool already_added = false;
            for (const auto& existing : result.available_cameras) {
                if (existing == device) {
                    already_added = true;
                    break;
                }
            }

            if (!already_added && result.available_cameras.size() < 4) {
                result.available_cameras.push_back(device);
                std::cout << "[DETECTION] Added from auto-scan: " << device << std::endl;
            }
        }
    }

    // 决定工作模式
    size_t camera_count = result.available_cameras.size();

    std::cout << "[DETECTION] Total working cameras found: " << camera_count << std::endl;
    for (size_t i = 0; i < camera_count; ++i) {
        std::cout << "[DETECTION] Camera " << i << ": " << result.available_cameras[i] << std::endl;
    }

    if (camera_count >= 4) {
        result.mode = "quad";
        result.expected_fps = 25;
        result.available_cameras.resize(4);  // 只使用前4个
        std::cout << "[DETECTION] Mode: Quad Camera Sync (4 cameras)" << std::endl;
    }
    else if (camera_count >= 3) {
        result.mode = "triple";
        result.expected_fps = 28;
        result.available_cameras.resize(3);
        std::cout << "[DETECTION] Mode: Triple Camera Sync (3 cameras)" << std::endl;
    }
    else if (camera_count >= 2) {
        result.mode = "dual";
        result.expected_fps = 30;
        result.available_cameras.resize(2);
        std::cout << "[DETECTION] Mode: Dual Camera Sync (2 cameras)" << std::endl;
    }
    else {
        result.mode = "none";
        result.expected_fps = 0;
        std::cout << "[DETECTION] Error: Need at least 2 cameras for sync" << std::endl;
    }

    return result;
}

// 创建多摄像头捕获器（去除插值相关代码）
std::unique_ptr<MultiCameraCapture> create_camera_capture(const CameraDetectionResult& detection) {
    try {
        if (detection.mode == "dual") {
            std::cout << "[CAMERA] Creating dual camera capture..." << std::endl;
            return CameraCaptureFactory::create_dual_camera(detection.available_cameras);
        }
        else if (detection.mode == "triple") {
            std::cout << "[CAMERA] Creating triple camera capture..." << std::endl;
            return CameraCaptureFactory::create_triple_camera(detection.available_cameras);
        }
        else if (detection.mode == "quad") {
            std::cout << "[CAMERA] Creating quad camera capture..." << std::endl;
            return CameraCaptureFactory::create_quad_camera(detection.available_cameras);
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

    std::cout << "=== Adaptive Multi-Camera Sync System (No Interpolation) ===" << std::endl;

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

    // 主要流传输线程（简化的输出帧率控制）
    std::thread streaming_thread([&]() {
        uint64_t frame_groups_sent = 0;
        uint64_t sync_success = 0;
        uint64_t sync_fail = 0;
        auto start_time = std::chrono::steady_clock::now();
        auto last_stats_time = start_time;
        auto last_camera_check = start_time;

        // 简化输出帧率控制
        auto last_output_time = std::chrono::steady_clock::now();
        const auto target_frame_interval = std::chrono::milliseconds(1000 / detection.expected_fps);

        int consecutive_failures = 0;
        const int MAX_CONSECUTIVE_FAILURES = 50;

        while (!g_should_exit.load()) {
            try {
                auto current_time = std::chrono::steady_clock::now();
                bool cams_init = cameras_initialized.load();
                bool cams_running = cameras_running.load();

                // 摄像头状态检查
                if (std::chrono::duration_cast<std::chrono::seconds>(current_time - last_camera_check).count() >= 2) {

                    if (should_run_cameras && !cams_init) {
                        std::cout << "[CAMERA] Starting " << detection.mode << " camera sync..." << std::endl;

                        try {
                            camera_capture->start();
                            cameras_initialized = true;
                            cameras_running = true;
                            frame_groups_sent = 0;
                            consecutive_failures = 0;

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

                // 处理帧数据
                if (cams_running && camera_capture) {
                    auto frames = camera_capture->get_sync_yuv420p_frames();
                    size_t expected_frame_count = camera_capture->get_camera_count();

                    if (frames.size() == expected_frame_count &&
                        std::all_of(frames.begin(), frames.end(), [](AVFrame* f) { return f != nullptr; })) {

                        // 简单的时间间隔控制
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
                                    std::cout << "[WARNING] Failed to send frame to streamer " << i << std::endl;
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
                            // 还没到输出时间，释放帧但不发送
                            for (auto* frame : frames) {
                                camera_capture->release_frame(&frame);
                            }
                            std::this_thread::sleep_for(std::chrono::microseconds(500));
                        }
                    }
                    else {
                        // 错误处理
                        consecutive_failures++;

                        // 释放任何获取到的帧
                        for (auto* frame : frames) {
                            if (frame) camera_capture->release_frame(&frame);
                        }

                        // 如果连续失败太多，检查系统状态
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
                            std::cout << std::endl;

                            consecutive_failures = 0;
                        }

                        sync_fail++;
                        std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    }
                }
                else {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }

                // 每30秒打印详细状态统计
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

                    // 详细的队列状态报告
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
                        std::cout << std::endl;
                    }

                    // TCP流状态检查
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
    std::cout << "Detected Cameras:" << std::endl;

    for (size_t i = 0; i < detection.available_cameras.size(); ++i) {
        std::cout << "  Camera " << i << ": " << detection.available_cameras[i] << std::endl;
    }

    std::cout << "\nTCP Ports:" << std::endl;
    for (size_t i = 0; i < streamers.size(); ++i) {
        std::vector<std::string> camera_labels = { "Left", "Right", "Third", "Fourth" };
        std::cout << "  " << camera_labels[i] << " camera port: " << streamers[i]->get_port() << std::endl;
    }

    std::cout << "\n=== Python Consumer Example ===" << std::endl;
    std::cout << "import cv2" << std::endl;
    for (size_t i = 0; i < streamers.size(); ++i) {
        std::vector<std::string> var_names = { "cap_left", "cap_right", "cap_third", "cap_fourth" };
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
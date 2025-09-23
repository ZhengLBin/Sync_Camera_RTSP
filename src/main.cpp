#include "../includes/multi_camera_sync.h"
#include "../includes/tcp_streamer.h"
#include "../includes/camera_input_source.h"
#include <iostream>
#include <chrono>
#include <csignal>
#include <vector>
#include <memory>

std::atomic<bool> g_should_exit{ false };

void signal_handler(int signal) {
    if (signal == SIGINT || signal == SIGTERM) {
        std::cout << "\nShutting down..." << std::endl;
        g_should_exit = true;
    }
}

CameraInputMode select_camera_mode() {
    std::cout << "\n=== Camera Mode Selection ===" << std::endl;
    std::cout << "1. USB Cameras" << std::endl;
    std::cout << "2. RTSP Streams" << std::endl;
    std::cout << "Select (1-2): ";

    int choice;
    while (true) {
        std::cin >> choice;
        if (std::cin.fail()) {
            std::cin.clear();
            std::cin.ignore(10000, '\n');
            std::cout << "Invalid input. Enter 1 or 2: ";
            continue;
        }
        if (choice == 1) return CameraInputMode::USB_CAMERAS;
        else if (choice == 2) return CameraInputMode::RTSP_STREAMS;
        else std::cout << "Invalid choice. Enter 1 or 2: ";
    }
}

std::vector<std::string> configure_rtsp_urls() {
    std::cout << "\n=== RTSP Configuration ===" << std::endl;
    std::cout << "1. Use default URLs (192.168.16.240)" << std::endl;
    std::cout << "2. Enter custom URLs" << std::endl;
    std::cout << "Select (1-2): ";

    int choice;
    std::cin >> choice;

    std::vector<std::string> urls;
    if (choice == 1) {
        urls = {
            "rtsp://admin:haikang123@192.168.16.240:554/Streaming/Channels/101?transportmode=unicast",
            "rtsp://admin:haikang123@192.168.16.240:554/Streaming/Channels/201?transportmode=unicast"
        };
    }
    else {
        std::cout << "Enter number of streams (2-4): ";
        int count;
        std::cin >> count;
        count = std::max(2, std::min(4, count));

        std::cin.ignore();
        for (int i = 0; i < count; ++i) {
            std::string url;
            std::cout << "Enter RTSP URL " << (i + 1) << ": ";
            std::getline(std::cin, url);
            if (!url.empty()) {
                urls.push_back(url);
            }
        }
    }

    // 🔍 详细验证RTSP URL
    std::cout << "\n=== RTSP URLs Verification ===" << std::endl;
    for (size_t i = 0; i < urls.size(); ++i) {
        std::cout << "Camera " << i << ": " << urls[i] << std::endl;

        // 检查URL是否真的不同
        if (i > 0) {
            for (size_t j = 0; j < i; ++j) {
                if (urls[i] == urls[j]) {
                    std::cerr << "⚠️  WARNING: Camera " << i << " and Camera " << j << " have IDENTICAL URLs!" << std::endl;
                }
            }
        }
    }
    std::cout << "===============================" << std::endl;

    return urls;
}

CameraDetectionResult detect_cameras(CameraInputMode mode) {
    std::unique_ptr<CameraInputSource> source;

    if (mode == CameraInputMode::USB_CAMERAS) {
        std::cout << "Detecting USB cameras..." << std::endl;
        source = CameraSourceFactory::create_usb_source();
    }
    else {
        std::cout << "Configuring RTSP streams..." << std::endl;
        auto rtsp_urls = configure_rtsp_urls();
        source = CameraSourceFactory::create_rtsp_source(rtsp_urls);
    }

    if (!source) return CameraDetectionResult{};
    return source->detect_cameras();
}

std::unique_ptr<MultiCameraCapture> create_camera_capture(const CameraDetectionResult& detection) {
    try {
        SyncConfig config;
        config.target_fps = detection.expected_fps;

        if (detection.mode == "dual") {
            config.max_queue_size = 20;
            config.max_sync_queue_size = 8;
            config.sync_threshold_us = 1000000;
        }
        else if (detection.mode == "triple") {
            config.max_queue_size = 35;
            config.max_sync_queue_size = 25;
            config.sync_threshold_us = 1500000;
        }
        else if (detection.mode == "quad") {
            config.max_queue_size = 200;
            config.max_sync_queue_size = 80;
            config.sync_threshold_us = 10000000;
        }
        else {
            return nullptr;
        }

        auto capture = std::make_unique<MultiCameraCapture>();
        if (!capture->init(detection.available_cameras, config)) {
            return nullptr;
        }
        return capture;
    }
    catch (const std::exception& e) {
        std::cerr << "Failed to create camera capture: " << e.what() << std::endl;
        return nullptr;
    }
}

std::vector<std::unique_ptr<TCPStreamer>> create_streamers(const CameraDetectionResult& detection) {
    std::vector<std::unique_ptr<TCPStreamer>> streamers;
    size_t camera_count = detection.available_cameras.size();

    // 根据输入模式选择分辨率
    int width, height;
    if (detection.input_mode == CameraInputMode::RTSP_STREAMS) {
        width = 1280;
        height = 720;
    }
    else {
        width = 640;
        height = 480;
    }

    std::vector<std::string> names = { "left", "right", "third", "fourth" };
    const int base_port = 6010;

    for (size_t i = 0; i < camera_count; ++i) {
        int port = base_port + static_cast<int>(i);
        auto streamer = std::make_unique<TCPStreamer>(names[i], port);

        if (!streamer->init(width, height, detection.expected_fps)) {
            std::cerr << "Failed to initialize streamer " << i << " on port " << port << std::endl;
            return {};
        }

        std::cout << "Streamer " << i << " (" << names[i] << ") on port " << port << std::endl;
        streamers.push_back(std::move(streamer));
    }

    return streamers;
}

int main() {
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    av_log_set_level(AV_LOG_ERROR);
    avdevice_register_all();

    // 选择模式并检测摄像头
    CameraInputMode mode = select_camera_mode();
    auto detection = detect_cameras(mode);

    if (detection.mode == "none") {
        std::cerr << "No working cameras found" << std::endl;
        return -1;
    }

    std::cout << "Detected " << detection.available_cameras.size() << " cameras: " << detection.mode << std::endl;


    // 创建流传输器
    auto streamers = create_streamers(detection);
    if (streamers.empty()) {
        std::cerr << "Failed to create streamers" << std::endl;
        return -1;
    }

    // 创建摄像头捕获器
    auto camera_capture = create_camera_capture(detection);
    if (!camera_capture) {
        std::cerr << "Failed to create camera capture" << std::endl;
        return -1;
    }


    std::atomic<bool> cameras_running{ false };

    // 流传输线程
    std::thread streaming_thread([&]() {
        auto start_time = std::chrono::steady_clock::now();
        const auto frame_interval = std::chrono::milliseconds(1000 / detection.expected_fps);
        auto last_output_time = start_time;
        uint64_t frame_count = 0;

        std::cout << "Starting " << detection.mode << " camera system..." << std::endl;


        camera_capture->start();
        cameras_running = true;

        // 🚀 RTSP直推模式下，主循环只处理非RTSP流
        while (!g_should_exit.load()) {
            if (cameras_running && camera_capture) {
                auto frames = camera_capture->get_sync_yuv420p_frames();

                if (frames.size() == camera_capture->get_camera_count()) {
                    auto current_time = std::chrono::steady_clock::now();

                    if (current_time - last_output_time >= frame_interval) {
                        // 设置PTS
                        for (size_t i = 0; i < frames.size(); ++i) {
                            frames[i]->pts = static_cast<int64_t>(frame_count);
                        }

                        // 发送到流传输器
                        for (size_t i = 0; i < frames.size() && i < streamers.size(); ++i) {
                            streamers[i]->send_frame(frames[i]);
                        }

                        frame_count++;
                        last_output_time = current_time;
                    }

                    // 释放帧
                    for (auto* frame : frames) {
                        camera_capture->release_frame(&frame);
                    }
                }
                else {
                    // 释放任何获取到的帧
                    for (auto* frame : frames) {
                        if (frame) camera_capture->release_frame(&frame);
                    }
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        if (camera_capture && cameras_running.load()) {
            camera_capture->stop();
        }
        });

    // 显示连接信息
    std::cout << "\n=== TCP Stream Info ===" << std::endl;
    std::cout << "Expected FPS: " << detection.expected_fps << std::endl;
    std::cout << "\nTCP Ports:" << std::endl;
    for (size_t i = 0; i < streamers.size(); ++i) {
        std::vector<std::string> labels = { "Left", "Right", "Third", "Fourth" };
        std::cout << "  " << labels[i] << " camera port: " << streamers[i]->get_port() << std::endl;
    }

    // 主循环
    while (!g_should_exit.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    // 清理
    std::cout << "\nStopping..." << std::endl;
    if (streaming_thread.joinable()) {
        streaming_thread.join();
    }

    if (camera_capture) {
        camera_capture->stop();
    }

    for (auto& streamer : streamers) {
        streamer->stop();
    }

    std::cout << detection.mode << " camera system exited." << std::endl;
    return 0;
}
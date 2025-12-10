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

CameraDetectionResult detect_cameras() {
    std::cout << "Detecting USB cameras..." << std::endl;
    auto source = CameraSourceFactory::create_usb_source();
    if (!source) return CameraDetectionResult{};
    return source->detect_cameras();
}

std::unique_ptr<MultiCameraCapture> create_camera_capture(const CameraDetectionResult& detection) {
    try {
        SyncConfig config;
        config.target_fps = detection.expected_fps;

        if (detection.mode == "dual") {
            config.max_queue_size = 15;      // 采集队列
            config.max_sync_queue_size = 10; // 同步队列最多10帧(约330ms)
            config.sync_threshold_us = 500000;
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

    // USB摄像头固定使用640x480分辨率
    int width = 640;
    int height = 480;

    std::vector<std::string> names = { "front", "back", "left", "right" };
    const int base_port = 5010;

    for (size_t i = 0; i < camera_count; ++i) {
        int port = base_port + static_cast<int>(i);
        auto streamer = std::make_unique<TCPStreamer>(names[i], port);

        if (!streamer->init(width, height, detection.expected_fps)) {
            std::cerr << "Failed to initialize streamer " << i << " on port " << port << std::endl;
            return {};
        }

        streamers.push_back(std::move(streamer));
    }

    return streamers;
}

int main() {
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    av_log_set_level(AV_LOG_QUIET);
    avdevice_register_all();

    // 检测USB摄像头
    auto detection = detect_cameras();

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
        uint64_t get_frame_attempts = 0;
        uint64_t successful_sends = 0;

        std::cout << "Starting " << detection.mode << " camera system..." << std::endl;

        camera_capture->start();
        cameras_running = true;

        while (!g_should_exit.load()) {
            if (cameras_running && camera_capture) {
                auto current_time = std::chrono::steady_clock::now();
                
                // 只在需要下一帧时才获取(按30fps节奏)
                if (current_time - last_output_time < frame_interval) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    continue;
                }
                
                get_frame_attempts++;
                auto frames = camera_capture->get_sync_yuv420p_frames();
                size_t expected = camera_capture->get_camera_count();

                if (frames.size() == expected) {
                    // 设置PTS
                    for (size_t i = 0; i < frames.size(); ++i) {
                        frames[i]->pts = static_cast<int64_t>(frame_count);
                    }

                    // 发送到流传输器
                    for (size_t i = 0; i < frames.size() && i < streamers.size(); ++i) {
                        bool sent = streamers[i]->send_frame(frames[i]);
                        if (sent) successful_sends++;
                    }

                    frame_count++;
                    if (frame_count % 300 == 1) {
                        std::cout << "[Streaming] " << frame_count << " synced frames transmitted" << std::endl;
                    }
                    
                    last_output_time = current_time;

                    // 释放帧
                    for (auto* frame : frames) {
                        camera_capture->release_frame(&frame);
                    }
                } else {
                    // 同步队列为空,等待一下
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
            }
        }

        if (camera_capture && cameras_running.load()) {
            camera_capture->stop();
        }
        });

    // 显示连接信息
    std::cout << "\n=== System Ready ===" << std::endl;
    std::cout << "FPS: " << detection.expected_fps << " | Ports: ";
    for (size_t i = 0; i < streamers.size(); ++i) {
        if (i > 0) std::cout << ", ";
        std::cout << streamers[i]->get_port();
    }
    std::cout << std::endl;

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
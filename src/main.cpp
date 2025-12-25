#include "../includes/multi_camera_sync.h"
#include "../includes/tcp_streamer.h"
#include "../includes/config_manager.h"
#include "../includes/camera_input_source.h"
#include <iostream>
#include <chrono>
#include <csignal>
#include <vector>
#include <memory>
#include <future>
#include <thread>

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
            config.max_queue_size = 3;       // 采集队列3帧（保留足够同步空间）
            config.max_sync_queue_size = 2;  // 同步队列2帧（约66ms缓冲）
            config.sync_threshold_us = 500000; // 同步容差500ms（足够两个摄像头对齐）
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

    // 从配置文件获取流配置
    auto& config_mgr = ConfigManager::instance();
    const auto& configs = config_mgr.get_configs();

    for (size_t i = 0; i < camera_count && i < configs.size(); ++i) {
        const auto& config = configs[i];
        auto streamer = std::make_unique<TCPStreamer>(config.name, config.host, config.port);

        if (!streamer->init(width, height, detection.expected_fps)) {
            std::cerr << "Failed to initialize streamer " << i << " (" << config.name 
                      << ") on " << config.host << ":" << config.port << std::endl;
            return {};
        }

        streamers.push_back(std::move(streamer));
    }

    return streamers;
}

int main(int argc, char* argv[]) {
    // 加载配置文件
    std::string config_file = "streamer_config.txt";
    if (argc > 1) {
        config_file = argv[1];
    }
    
    auto& config_mgr = ConfigManager::instance();
    if (!config_mgr.load_config(config_file)) {
        std::cerr << "Failed to load config from " << config_file << std::endl;
        std::cout << "Generating default config..." << std::endl;
        ConfigManager::generate_default_config(config_file);
        std::cerr << "Please update " << config_file << " and run again" << std::endl;
        return -1;
    }

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
                
                get_frame_attempts++;
                
                // 🎯 激进丢帧策略：清空所有旧帧，只保留最新的
                // 不按帧率限制，而是尽可能获取最新帧
                size_t queue_size = camera_capture->get_sync_queue_size();
                if (queue_size > 0) {
                    // 如果队列有多于1个，说明积压了，清空所有旧的
                    while (camera_capture->get_sync_queue_size() > 1) {
                        auto old_frames = camera_capture->get_sync_yuv420p_frames();
                        for (auto* frame : old_frames) {
                            camera_capture->release_frame(&frame);
                        }
                    }
                }
                
                auto frames = camera_capture->get_sync_yuv420p_frames();
                size_t expected = camera_capture->get_camera_count();

                if (frames.size() == expected) {
                    // 统一设置PTS（使用同一个时间基准）
                    int64_t unified_pts = static_cast<int64_t>(frame_count);
                    for (size_t i = 0; i < frames.size(); ++i) {
                        frames[i]->pts = unified_pts;
                    }

                    // 🎯 直接同步发送（send_frame内部有队列，不会长时间阻塞）
                    for (size_t i = 0; i < frames.size() && i < streamers.size(); ++i) {
                        streamers[i]->send_frame(frames[i]);
                    }

                    frame_count++;
                    
                    // 🔍 详细调试信息：每30帧输出一次完整的队列状态
                    if (frame_count % 30 == 0) {
                        auto now_time = std::chrono::steady_clock::now();
                        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now_time - start_time).count();
                        double actual_fps = frame_count * 1000.0 / elapsed;
                        
                        std::cout << "\n=== [DEBUG] Frame " << frame_count 
                                  << " | Elapsed: " << elapsed << "ms"
                                  << " | Actual FPS: " << actual_fps << " ===" << std::endl;
                        std::cout << "SyncQueue size: " << camera_capture->get_sync_queue_size() << std::endl;
                        
                        for (size_t i = 0; i < streamers.size(); ++i) {
                            std::cout << "Streamer[" << i << "] port=" << streamers[i]->get_port()
                                      << " dropped=" << streamers[i]->get_dropped_count() << std::endl;
                        }
                        std::cout << "================================================\n" << std::endl;
                    }
                    
                    last_output_time = current_time;

                    // 🎯 不在这里释放帧，因为异步发送还在使用
                    // send_frame内部会复制数据，所以可以立即释放
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
#include "../includes/camera_input_source.h"
#include <iostream>
#include <set>

extern "C" {
#include <libavformat/avformat.h>
#include <libavdevice/avdevice.h>
}

//==============================================================================
// USBCameraSource 实现
//==============================================================================

CameraDetectionResult USBCameraSource::detect_cameras() {
    CameraDetectionResult result;
    auto auto_detected = scan_usb_cameras();
    result.available_cameras = auto_detected;
    result.input_mode = CameraInputMode::USB_CAMERAS;

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
    }

    return result;
}

bool USBCameraSource::test_camera_connection(const std::string& camera_path) {
    const AVInputFormat* input_format = av_find_input_format("dshow");
    AVFormatContext* test_fmt_ctx = nullptr;
    AVDictionary* options = nullptr;

    av_dict_set(&options, "video_size", "640x480", 0);
    av_dict_set(&options, "framerate", "30", 0);

    int ret = avformat_open_input(&test_fmt_ctx, camera_path.c_str(), input_format, &options);
    av_dict_free(&options);

    if (ret == 0) {
        avformat_close_input(&test_fmt_ctx);
        return true;
    }
    return false;
}

std::vector<std::string> USBCameraSource::scan_usb_cameras() {
    std::vector<std::string> detected_cameras;
    AVDeviceInfoList* device_list = nullptr;
    const AVInputFormat* input_format = av_find_input_format("dshow");

    if (!input_format) return detected_cameras;

    int ret = avdevice_list_input_sources(input_format, nullptr, nullptr, &device_list);
    if (ret < 0 || !device_list) return detected_cameras;

    std::set<std::string> used_names;

    for (int i = 0; i < device_list->nb_devices; ++i) {
        AVDeviceInfo* device = device_list->devices[i];
        if (!device || !device->device_name) continue;

        std::string device_description = device->device_description ? device->device_description : "";

        // 跳过音频设备
        if (device_description.find("audio") != std::string::npos ||
            device_description.find("Microphone") != std::string::npos) {
            continue;
        }

        std::string simple_name = "video=" + device_description;

        if (used_names.find(simple_name) == used_names.end() &&
            test_camera_connection(simple_name)) {
            detected_cameras.push_back(simple_name);
            used_names.insert(simple_name);
            if (detected_cameras.size() >= 4) break;
        }
    }

    avdevice_free_list_devices(&device_list);
    return detected_cameras;
}

//==============================================================================
// RTSPCameraSource 实现
//==============================================================================

RTSPCameraSource::RTSPCameraSource(const std::vector<std::string>& rtsp_urls)
    : rtsp_urls_(rtsp_urls) {
}

CameraDetectionResult RTSPCameraSource::detect_cameras() {
    CameraDetectionResult result;
    result.input_mode = CameraInputMode::RTSP_STREAMS;

    std::vector<std::string> working_cameras;
    for (const auto& url : rtsp_urls_) {
        if (test_camera_connection(url)) {
            working_cameras.push_back(url);
        }
    }

    result.available_cameras = working_cameras;
    size_t camera_count = working_cameras.size();

    if (camera_count >= 4) {
        result.mode = "quad";
        result.expected_fps = 25;
        result.available_cameras.resize(4);
    }
    else if (camera_count >= 3) {
        result.mode = "triple";
        result.expected_fps = 25;
        result.available_cameras.resize(3);
    }
    else if (camera_count >= 2) {
        result.mode = "dual";
        result.expected_fps = 25;
        result.available_cameras.resize(2);
    }
    else {
        result.mode = "none";
        result.expected_fps = 0;
    }

    return result;
}

bool RTSPCameraSource::test_camera_connection(const std::string& rtsp_url) {
    AVFormatContext* test_fmt_ctx = nullptr;
    AVDictionary* options = nullptr;

    av_dict_set(&options, "rtsp_transport", "tcp", 0);
    av_dict_set(&options, "stimeout", "3000000", 0);

    int ret = avformat_open_input(&test_fmt_ctx, rtsp_url.c_str(), nullptr, &options);
    av_dict_free(&options);

    if (ret == 0) {
        avformat_close_input(&test_fmt_ctx);
        return true;
    }
    return false;
}

//==============================================================================
// CameraSourceFactory 实现
//==============================================================================

std::unique_ptr<CameraInputSource> CameraSourceFactory::create_usb_source() {
    return std::make_unique<USBCameraSource>();
}

std::unique_ptr<CameraInputSource> CameraSourceFactory::create_rtsp_source(
    const std::vector<std::string>& rtsp_urls) {
    return std::make_unique<RTSPCameraSource>(rtsp_urls);
}
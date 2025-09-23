#pragma once


#include <vector>
#include <string>
#include <memory>

// 摄像头输入模式枚举
enum class CameraInputMode {
    USB_CAMERAS,
    RTSP_STREAMS,
    MIXED_MODE
};

// 摄像头检测结果结构
struct CameraDetectionResult {
    std::vector<std::string> available_cameras;
    std::string mode;
    int expected_fps;
    CameraInputMode input_mode;
};

// 抽象输入源接口
class CameraInputSource {
public:
    virtual ~CameraInputSource() = default;
    virtual CameraDetectionResult detect_cameras() = 0;
    virtual bool test_camera_connection(const std::string& camera_path) = 0;
    virtual std::string get_mode_name() const = 0;
};

// USB摄像头输入源
class USBCameraSource : public CameraInputSource {
public:
    CameraDetectionResult detect_cameras() override;
    bool test_camera_connection(const std::string& camera_path) override;
    std::string get_mode_name() const override { return "USB"; }

private:
    std::vector<std::string> scan_usb_cameras();
    bool test_camera_simple(const std::string& camera_name);
};

// RTSP流输入源
class RTSPCameraSource : public CameraInputSource {
public:
    RTSPCameraSource(const std::vector<std::string>& rtsp_urls);
    CameraDetectionResult detect_cameras() override;
    bool test_camera_connection(const std::string& rtsp_url) override;
    std::string get_mode_name() const override { return "RTSP"; }

private:
    std::vector<std::string> rtsp_urls_;
    bool test_rtsp_connection(const std::string& rtsp_url);
};

// 输入源工厂
class CameraSourceFactory {
public:
    static std::unique_ptr<CameraInputSource> create_usb_source();
    static std::unique_ptr<CameraInputSource> create_rtsp_source(
        const std::vector<std::string>& rtsp_urls);
    static std::unique_ptr<CameraInputSource> create_source_from_config(
        CameraInputMode mode, const std::vector<std::string>& config_params = {});
};
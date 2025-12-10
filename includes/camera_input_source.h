#pragma once


#include <vector>
#include <string>
#include <memory>

// 摄像头输入模式枚举
enum class CameraInputMode {
    USB_CAMERAS
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

// 输入源工厂
class CameraSourceFactory {
public:
    static std::unique_ptr<CameraInputSource> create_usb_source();
};
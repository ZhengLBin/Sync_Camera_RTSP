// render_display.h
#ifndef RENDER_DISPLAY_H
#define RENDER_DISPLAY_H

#include "sync_camera.h"
#include <atomic>

// 启动 SDL 渲染显示线程，显示两个摄像头图像和时间戳差值
void start_rendering(DualCameraCapture& capture, std::atomic<bool>& should_exit);

#endif // RENDER_DISPLAY_H
	
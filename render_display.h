// render_display.h
#ifndef RENDER_DISPLAY_H
#define RENDER_DISPLAY_H

#include "sync_camera.h"
#include <atomic>

// ���� SDL ��Ⱦ��ʾ�̣߳���ʾ��������ͷͼ���ʱ�����ֵ
void start_rendering(DualCameraCapture& capture, std::atomic<bool>& should_exit);

#endif // RENDER_DISPLAY_H
	
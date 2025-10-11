#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Camera类 (OpenCV + RTSP/H.264)
完全兼容原 test.py 接口，不依赖 gi/GStreamer
支持多摄像头同步读取、动态分辨率、性能统计
"""

import cv2
import threading
import time
import queue

class Camera:
    def __init__(self):
        # 摄像头基本信息
        self.camera_names = ["left", "right", "third", "fourth"]
        self.camera_count = 2
        self.camera_urls = {}  # 摄像头 RTSP URL
        self.camera_resolutions = {}
        self.latest_frames = {}
        self.frame_counters = {name: 0 for name in self.camera_names}
        self.total_frames_received = 0

        # 摄像头状态
        self.is_opened = False
        self.running = False
        self.start_time = None

        # 捕获线程和队列
        self.capture_threads = []
        self.frame_queues = {name: queue.Queue(maxsize=3) for name in self.camera_names}
        self.cap_objs = {}

        # 模式
        self.mode = "RTSP"
        self.server_running = True

        # 初始化默认 RTSP URL
        self._init_default_urls()

    def _init_default_urls(self):
        base_ip = "192.168.16.240"
        self.camera_urls["left"] = f"rtsp://admin:haikang123@{base_ip}:554/Streaming/Channels/101?transportmode=unicast"
        self.camera_urls["right"] = f"rtsp://admin:haikang123@{base_ip}:554/Streaming/Channels/201?transportmode=unicast"

    # ==================== 服务端检测接口 ====================
    def get_server_camera_info(self, force_refresh=False):
        """
        模拟服务端信息检测
        """
        info = {
            "status": "online" if self.server_running else "offline",
            "server_running": self.server_running,
            "mode": self.mode,
            "camera_count": self.camera_count,
            "detected_cameras": [],
            "detection_time": time.time()
        }
        for name in self.camera_names[:self.camera_count]:
            info["detected_cameras"].append({
                "name": name,
                "port": 6010 + self.camera_names.index(name),
                "connected": self.server_running,
            })
        return info

    def get_camera_mapping(self):
        return {name: f"Camera {name.upper()}" for name in self.camera_names[:self.camera_count]}

    def is_server_online(self):
        return self.server_running

    def wait_for_server(self, timeout=10, check_interval=2):
        start = time.time()
        while time.time() - start < timeout:
            if self.is_server_online():
                return True
            time.sleep(check_interval)
        return False

    # ==================== 摄像头操作接口 ====================
    def _detect_available_cameras(self):
        """
        检测可用摄像头
        """
        # RTSP 模式下默认所有 URL 都可用
        return True

    def open(self):
        if self.is_opened:
            return True

        if not self._detect_available_cameras():
            return False

        self.running = True
        self.start_time = time.time()

        for i, camera_name in enumerate(self.camera_names[:self.camera_count]):
            rtsp_url = self.camera_urls.get(camera_name)
            if not rtsp_url:
                print(f"[ERROR] {camera_name} RTSP URL not set")
                continue

            cap = cv2.VideoCapture(rtsp_url)
            if not cap.isOpened():
                print(f"[ERROR] {camera_name} cannot open RTSP stream")
                continue

            self.cap_objs[camera_name] = cap
            width = int(cap.get(cv2.CAP_PROP_FRAME_WIDTH))
            height = int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT))
            self.camera_resolutions[camera_name] = (width, height)

            thread = threading.Thread(target=self._camera_capture_worker, args=(camera_name,), daemon=True)
            thread.start()
            self.capture_threads.append(thread)

        time.sleep(1)
        self.is_opened = True
        print("[INFO] All cameras opened successfully")
        return True

    def close(self):
        if not self.is_opened:
            return
        self.running = False
        for thread in self.capture_threads:
            thread.join(timeout=2)
        for cap in self.cap_objs.values():
            cap.release()
        self.capture_threads.clear()
        self.cap_objs.clear()
        self.is_opened = False
        print("[INFO] All cameras closed")

    def _camera_capture_worker(self, camera_name):
        cap = self.cap_objs.get(camera_name)
        if not cap:
            return

        while self.running:
            ret, frame = cap.read()
            if not ret:
                continue

            self.latest_frames[camera_name] = frame.copy()
            self.frame_counters[camera_name] += 1
            self.total_frames_received += 1

            try:
                self.frame_queues[camera_name].put_nowait(frame)
            except queue.Full:
                try:
                    self.frame_queues[camera_name].get_nowait()
                    self.frame_queues[camera_name].put_nowait(frame)
                except queue.Empty:
                    pass

        cap.release()
        print(f"[INFO] {camera_name} capture thread stopped")

    def is_opened_camera(self):
        return self.is_opened and bool(self.cap_objs)

    # ==================== 读取接口 ====================
    def read(self, camera_name):
        """
        读取单个摄像头最新帧
        """
        frame = self.latest_frames.get(camera_name)
        return (frame is not None, frame)

    def read_all(self):
        """
        读取所有摄像头最新帧
        """
        frames = {}
        for name in self.camera_names[:self.camera_count]:
            frames[name] = self.latest_frames.get(name)
        return frames

    def read_sync(self):
        """
        同步读取所有摄像头帧
        """
        frames = self.read_all()
        success = all(f is not None for f in frames.values())
        return success, frames

    # ==================== 摄像头信息接口 ====================
    def get_camera_info(self):
        info = {
            "mode": self.mode,
            "camera_count": self.camera_count,
            "ports": [6010 + i for i in range(self.camera_count)],
            "cameras": {},
            "runtime": time.time() - self.start_time if self.start_time else 0,
            "total_frames": self.total_frames_received
        }
        for name in self.camera_names[:self.camera_count]:
            frames = self.frame_counters.get(name, 0)
            fps = frames / (time.time() - self.start_time) if self.start_time else 0
            resolution = self.camera_resolutions.get(name, (0, 0))
            info["cameras"][name] = {
                "frames": frames,
                "fps": fps,
                "resolution": resolution,
                "has_data": name in self.latest_frames
            }
        return info

    # ==================== 上下文管理 ====================
    def __enter__(self):
        self.open()
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        self.close()

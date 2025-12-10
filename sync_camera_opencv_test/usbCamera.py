import queue
import socket
import threading
import time
from typing import Tuple, Dict, Any, Optional
import subprocess
import struct

import cv2
import numpy as np


class Camera:
    """
    适配多摄像头同步传输系统的Camera类
    支持自动检测双/三/四摄像头模式，提供统一的接口访问摄像头数据
    """

    def __init__(self, host: str = '127.0.0.1', start_port: int = 5010,
                 width: int = 640, height: int = 480):
        self.host = host
        self.start_port = start_port
        self.width = width
        self.height = height
        self.frame_size = width * height * 3 // 2  # I420格式

        # 摄像头状态
        self.is_opened = False
        self.camera_mode = None  # 'dual', 'triple', 'quad'
        self.camera_count = 0
        self.camera_ports = []
        self.camera_names = ["front", "back", "left", "right", ]  # 支持4个摄像头

        # 服务端检测信息缓存
        self._server_camera_info = None
        self._last_detection_time = 0
        self._detection_cache_duration = 5  # 缓存5秒

        # 数据存储
        self.frame_queues = {}  # {camera_name: queue}
        self.latest_frames = {}  # {camera_name: frame}
        self.frame_counters = {}  # {camera_name: count}

        # 线程管理
        self.capture_threads = []
        self.running = False

        # 统计信息
        self.start_time = None
        self.total_frames_received = 0

    def get_server_camera_info(self, force_refresh: bool = False) -> Dict[str, Any]:
        """
        获取服务端检测到的摄像头信息

        Args:
            force_refresh: 是否强制刷新检测，忽略缓存

        Returns:
            {
                'status': 'online'|'offline'|'partial',
                'mode': 'dual'|'triple'|'quad'|'unknown',
                'camera_count': int,
                'detected_cameras': [
                    {'name': 'left', 'port': 5010, 'connected': True},
                    {'name': 'right', 'port': 5011, 'connected': True},
                    ...
                ],
                'server_running': bool,
                'detection_time': float
            }
        """
        current_time = time.time()

        # 检查缓存
        if (not force_refresh and
                self._server_camera_info and
                current_time - self._last_detection_time < self._detection_cache_duration):
            return self._server_camera_info.copy()

        print("[SERVER_DETECT] Detecting server camera configuration...")

        # 检测服务端状态
        server_info = {
            'status': 'offline',
            'mode': 'unknown',
            'camera_count': 0,
            'detected_cameras': [],
            'server_running': False,
            'detection_time': current_time
        }

        # 测试最多4个端口
        detected_ports = []
        camera_status = []

        for i in range(len(self.camera_names)):
            port = self.start_port + i
            camera_name = self.camera_names[i]

            try:
                test_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                test_sock.settimeout(1.5)  # 1.5秒超时
                result = test_sock.connect_ex((self.host, port))
                test_sock.close()

                if result == 0:
                    detected_ports.append(port)
                    camera_status.append({
                        'name': camera_name,
                        'port': port,
                        'connected': True
                    })
                    print(f"[SERVER_DETECT] ✓ {camera_name.upper()} camera found on port {port}")
                else:
                    camera_status.append({
                        'name': camera_name,
                        'port': port,
                        'connected': False
                    })
                    print(f"[SERVER_DETECT] ✗ {camera_name.upper()} camera not found on port {port}")
                    # 如果端口不连续，停止检测
                    if 0 < i == len(detected_ports):
                        break

            except Exception as e:
                camera_status.append({
                    'name': camera_name,
                    'port': port,
                    'connected': False,
                    'error': str(e)
                })
                print(f"[SERVER_DETECT] ✗ Error testing {camera_name} on port {port}: {e}")
                break

        # 分析检测结果
        camera_count = len(detected_ports)
        server_info['camera_count'] = camera_count
        server_info['detected_cameras'] = camera_status

        if camera_count >= 2:
            server_info['server_running'] = True

            if camera_count >= 4:
                server_info['mode'] = 'quad'
                server_info['status'] = 'online'
            elif camera_count >= 3:
                server_info['mode'] = 'triple'
                server_info['status'] = 'online'
            elif camera_count >= 2:
                server_info['mode'] = 'dual'
                server_info['status'] = 'online'
        else:
            server_info['server_running'] = False
            if camera_count == 1:
                server_info['status'] = 'partial'
            else:
                server_info['status'] = 'offline'

        # 更新缓存
        self._server_camera_info = server_info.copy()
        self._last_detection_time = current_time

        return server_info

    def get_camera_mapping(self) -> Dict[str, str]:
        """
        获取摄像头名称到用途的映射关系

        Returns:
            {'left': '左摄像头', 'right': '右摄像头', 'third': '第三摄像头', 'fourth': '第四摄像头'}
        """
        mapping = {
            "front": '前摄像头',
            "back": '后摄像头',
            'left': '左摄像头',
            'right': '右摄像头',
            'third': '第三摄像头',
            'fourth': '第四摄像头'
        }

        # 根据当前模式返回对应的映射
        server_info = self.get_server_camera_info()
        active_cameras = {}

        for camera in server_info['detected_cameras']:
            if camera['connected']:
                active_cameras[camera['name']] = mapping[camera['name']]

        return active_cameras

    def is_server_online(self) -> bool:
        """
        检查服务端是否在线

        Returns:
            bool: 服务端是否运行并至少有2个摄像头
        """
        server_info = self.get_server_camera_info()
        return server_info['server_running'] and server_info['camera_count'] >= 2

    def wait_for_server(self, timeout: float = 30.0, check_interval: float = 2.0) -> bool:
        """
        等待服务端上线

        Args:
            timeout: 等待超时时间（秒）
            check_interval: 检查间隔（秒）

        Returns:
            bool: 服务端是否成功上线
        """

        start_time = time.time()
        while time.time() - start_time < timeout:
            if self.is_server_online():
                server_info = self.get_server_camera_info(force_refresh=True)
                return True
            time.sleep(check_interval)

        return False

    def open(self) -> bool:
        """打开摄像头连接"""
        if self.is_opened:
            return True

        # 检测可用摄像头
        if not self._detect_available_cameras():
            return False

        # 启动捕获线程
        self.running = True
        self.start_time = time.time()

        for i, port in enumerate(self.camera_ports):
            camera_name = self.camera_names[i]
            thread = threading.Thread(
                target=self._camera_capture_worker,
                args=(port, camera_name),
                daemon=True
            )
            thread.start()
            self.capture_threads.append(thread)

        # 等待至少一个摄像头收到第一帧
        print("Waiting for camera streams to start...")
        max_wait_time = 10  # 最多等待10秒
        start_wait = time.time()
        
        while time.time() - start_wait < max_wait_time:
            # 检查是否至少有一个摄像头收到了数据
            has_data = any(self.latest_frames.get(name) is not None 
                          for name in self.camera_names[:self.camera_count])
            if has_data:
                print(f"First frame received after {time.time() - start_wait:.2f}s")
                break
            time.sleep(0.1)
        
        # 再等待一小段时间确保所有摄像头都启动
        time.sleep(0.5)
        self.is_opened = True
        return True

    def _detect_available_cameras(self) -> bool:
        """检测可用的摄像头流"""

        # 使用服务端检测结果
        server_info = self.get_server_camera_info(force_refresh=True)

        if not server_info['server_running']:
            return False

        # 提取连接的摄像头端口
        available_ports = []
        for camera in server_info['detected_cameras']:
            if camera['connected']:
                available_ports.append(camera['port'])

        self.camera_mode = server_info['mode']
        self.camera_ports = available_ports
        self.camera_count = len(available_ports)

        # 初始化数据结构
        for i, port in enumerate(self.camera_ports):
            camera_name = self.camera_names[i]
            self.frame_queues[camera_name] = queue.Queue(maxsize=5)
            self.latest_frames[camera_name] = None
            self.frame_counters[camera_name] = 0

        return True

    def _camera_capture_worker(self, port: int, camera_name: str):
        """摄像头数据接收线程 - 使用 OpenCV 解码 H.264 流"""
        print(f"[{camera_name.upper()}] Starting capture from port {port}")
        
        # 使用 OpenCV 的 VideoCapture 连接到 TCP 流
        # 格式: tcp://host:port
        stream_url = f"tcp://{self.host}:{port}"
        
        try:
            # 创建 VideoCapture 对象
            cap = cv2.VideoCapture(stream_url, cv2.CAP_FFMPEG)
            
            # 设置缓冲区大小为最小，减少延迟
            cap.set(cv2.CAP_PROP_BUFFERSIZE, 1)
            
            if not cap.isOpened():
                print(f"[{camera_name.upper()}] ERROR: Failed to open stream {stream_url}")
                return
            
            print(f"[{camera_name.upper()}] Successfully connected to {stream_url}")
            
            # 等待接收第一帧（有时需要缓冲）
            first_frame_received = False
            for attempt in range(50):  # 最多尝试5秒
                ret, frame = cap.read()
                if ret and frame is not None:
                    first_frame_received = True
                    # 存储第一帧
                    self.frame_counters[camera_name] += 1
                    self.total_frames_received += 1
                    self.latest_frames[camera_name] = frame.copy()
                    try:
                        self.frame_queues[camera_name].put_nowait(frame)
                    except queue.Full:
                        pass
                    print(f"[{camera_name.upper()}] First frame received")
                    break
                time.sleep(0.1)
            
            if not first_frame_received:
                print(f"[{camera_name.upper()}] ERROR: Timeout waiting for first frame")
                return
            
            frame_error_count = 0
            max_errors = 10  # 连续错误超过此数量则退出
            
            while self.running:
                ret, frame = cap.read()
                
                if not ret or frame is None:
                    frame_error_count += 1
                    if frame_error_count >= max_errors:
                        print(f"[{camera_name.upper()}] Too many errors, stopping capture")
                        break
                    time.sleep(0.01)
                    continue
                
                # 重置错误计数
                frame_error_count = 0
                
                # 更新计数器
                self.frame_counters[camera_name] += 1
                self.total_frames_received += 1
                
                # 存储最新帧
                self.latest_frames[camera_name] = frame.copy()
                
                # 放入队列（如果队列满了，丢弃旧帧）
                try:
                    self.frame_queues[camera_name].put_nowait(frame)
                except queue.Full:
                    try:
                        self.frame_queues[camera_name].get_nowait()
                        self.frame_queues[camera_name].put_nowait(frame)
                    except queue.Empty:
                        pass
        
        except Exception as e:
            print(f"[{camera_name.upper()}] Exception in capture thread: {e}")
        finally:
            if 'cap' in locals():
                cap.release()
            print(f"[{camera_name.upper()}] Capture thread stopped")

    def close(self):
        """关闭摄像头连接"""
        if not self.is_opened:
            return

        print("[INFO] Closing camera connections...")
        self.running = False

        # 等待线程结束
        for thread in self.capture_threads:
            thread.join(timeout=2)

        self.capture_threads.clear()
        self.is_opened = False

        # 显示统计信息
        if self.start_time:
            elapsed = time.time() - self.start_time
            print(f"[STATS] Runtime: {elapsed:.1f}s, Total frames: {self.total_frames_received}")
            for name, count in self.frame_counters.items():
                fps = count / elapsed if elapsed > 0 else 0
                print(f"[STATS] {name.upper()}: {count} frames ({fps:.1f} fps)")

    def read(self, camera_name: str = "left") -> Tuple[bool, Optional[np.ndarray]]:
        """
        读取指定摄像头的最新帧

        Args:
            camera_name: 摄像头名称 ("left", "right", "third", "fourth")

        Returns:
            (success, frame): 成功标志和图像数据
        """
        if not self.is_opened:
            return False, None

        if camera_name not in self.latest_frames:
            return False, None

        frame = self.latest_frames[camera_name]
        if frame is not None:
            return True, frame.copy()
        return False, None

    def read_all(self) -> Dict[str, Optional[np.ndarray]]:
        """
        读取所有摄像头的最新帧

        Returns:
            {camera_name: frame} 字典
        """
        if not self.is_opened:
            return {}

        frames = {}
        for name in self.camera_names[:self.camera_count]:
            success, frame = self.read(name)
            frames[name] = frame if success else None

        return frames

    def read_sync(self) -> Tuple[bool, Dict[str, Optional[np.ndarray]]]:
        """
        尝试读取同步的多摄像头帧

        Returns:
            (success, {camera_name: frame})
        """
        frames = self.read_all()

        # 检查是否所有摄像头都有数据
        valid_frames = {k: v for k, v in frames.items() if v is not None}
        success = len(valid_frames) == self.camera_count

        return success, frames

    def get_camera_info(self) -> Dict[str, Any]:
        """获取摄像头系统信息（客户端视角）"""
        if not self.is_opened:
            return {"status": "closed"}

        elapsed = time.time() - self.start_time if self.start_time else 0

        info = {
            "status": "opened",
            "mode": self.camera_mode,
            "camera_count": self.camera_count,
            "ports": self.camera_ports,
            "runtime": elapsed,
            "total_frames": self.total_frames_received,
            "cameras": {}
        }

        for name, count in self.frame_counters.items():
            fps = count / elapsed if elapsed > 0 else 0
            info["cameras"][name] = {
                "frames": count,
                "fps": fps,
                "has_data": self.latest_frames[name] is not None
            }

        return info

    def is_opened_camera(self) -> bool:
        """检查摄像头是否已打开"""
        return self.is_opened

    def get_frame_size(self) -> Tuple[int, int]:
        """获取图像尺寸"""
        return self.width, self.height

    def __enter__(self):
        """支持 with 语句"""
        self.open()
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        """支持 with 语句"""
        self.close()
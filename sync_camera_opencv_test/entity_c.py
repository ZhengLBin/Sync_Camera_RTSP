import cv2
import numpy as np
import socket
import threading
import time
import queue
from typing import Optional, Tuple, List, Dict, Any

class Camera:
    """
    适配多摄像头同步传输系统的Camera类
    支持自动检测双/三摄像头模式，提供统一的接口访问摄像头数据
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
        self.camera_mode = None  # 'dual' 或 'triple'
        self.camera_count = 0
        self.camera_ports = []
        self.camera_names = ["left", "right", "third"]
        
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

    def open(self) -> bool:  # 添加open方法
        """打开摄像头连接"""
        if self.is_opened:
            print("[INFO] Camera already opened")
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
        
        # 等待连接建立
        time.sleep(1)
        self.is_opened = True
        print(f"[INFO] Camera system opened in {self.camera_mode.upper()} mode")
        return True

    def _detect_available_cameras(self) -> bool:
        """检测可用的摄像头流"""
        print(f"[DETECTION] Detecting cameras from port {self.start_port}...")
        
        available_ports = []
        for i in range(3):  # 最多检测3个摄像头
            port = self.start_port + i
            try:
                test_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                test_sock.settimeout(2)
                result = test_sock.connect_ex((self.host, port))
                test_sock.close()
                
                if result == 0:
                    available_ports.append(port)
                    print(f"[DETECTION] ✓ Found camera on port {port}")
                else:
                    print(f"[DETECTION] ✗ Port {port} not available")
                    break
            except Exception as e:
                print(f"[DETECTION] ✗ Error testing port {port}: {e}")
                break
        
        # 确定模式
        if len(available_ports) >= 3:
            self.camera_mode = 'triple'
            self.camera_ports = available_ports[:3]
        elif len(available_ports) >= 2:
            self.camera_mode = 'dual'
            self.camera_ports = available_ports[:2]
        else:
            print(f"[ERROR] Need at least 2 cameras, found {len(available_ports)}")
            return False
        
        self.camera_count = len(self.camera_ports)
        print(f"[DETECTION] Mode: {self.camera_mode.upper()} ({self.camera_count} cameras)")
        
        # 初始化数据结构
        for i, port in enumerate(self.camera_ports):
            camera_name = self.camera_names[i]
            self.frame_queues[camera_name] = queue.Queue(maxsize=5)
            self.latest_frames[camera_name] = None
            self.frame_counters[camera_name] = 0
        
        return True

    def _i420_to_bgr(self, data: bytes) -> Optional[np.ndarray]:
        """I420格式转BGR"""
        try:
            y_size = self.width * self.height
            uv_size = y_size // 4
            
            # 提取YUV三个平面
            y_plane = np.frombuffer(data[:y_size], dtype=np.uint8).reshape(self.height, self.width)
            u_plane = np.frombuffer(data[y_size:y_size + uv_size], dtype=np.uint8).reshape(self.height//2, self.width//2)
            v_plane = np.frombuffer(data[y_size + uv_size:], dtype=np.uint8).reshape(self.height//2, self.width//2)
            
            # 上采样U和V平面
            u_upsampled = cv2.resize(u_plane, (self.width, self.height), interpolation=cv2.INTER_LINEAR)
            v_upsampled = cv2.resize(v_plane, (self.width, self.height), interpolation=cv2.INTER_LINEAR)
            
            # 合并YUV平面并转BGR
            yuv_img = np.dstack([y_plane, u_upsampled, v_upsampled])
            bgr_img = cv2.cvtColor(yuv_img, cv2.COLOR_YUV2BGR)
            
            return bgr_img
        except Exception as e:
            print(f"[ERROR] I420 conversion failed: {e}")
            return None

    def _camera_capture_worker(self, port: int, camera_name: str):
        """摄像头数据接收线程"""
        sock = None
        try:
            print(f"[{camera_name.upper()}] Connecting to port {port}...")
            sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            sock.settimeout(10)
            sock.connect((self.host, port))
            print(f"[{camera_name.upper()}] ✓ Connected")
            
            while self.running:
                # 接收完整的I420帧数据
                frame_data = b''
                while len(frame_data) < self.frame_size:
                    remaining = self.frame_size - len(frame_data)
                    chunk = sock.recv(min(remaining, 8192))
                    if not chunk:
                        print(f"[{camera_name.upper()}] Connection closed")
                        return
                    frame_data += chunk
                
                # 转换为BGR图像
                bgr_frame = self._i420_to_bgr(frame_data)
                if bgr_frame is not None:
                    # 更新计数器
                    self.frame_counters[camera_name] += 1
                    self.total_frames_received += 1
                    
                    # 存储最新帧
                    self.latest_frames[camera_name] = bgr_frame.copy()
                    
                    # 放入队列（如果队列满了，丢弃旧帧）
                    try:
                        self.frame_queues[camera_name].put_nowait(bgr_frame)
                    except queue.Full:
                        try:
                            self.frame_queues[camera_name].get_nowait()
                            self.frame_queues[camera_name].put_nowait(bgr_frame)
                        except queue.Empty:
                            pass
                            
        except Exception as e:
            print(f"[{camera_name.upper()}] Error: {e}")
        finally:
            if sock:
                sock.close()

    def open(self) -> bool:
        """打开摄像头连接"""
        if self.is_opened:
            print("[INFO] Camera already opened")
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
        
        # 等待连接建立
        time.sleep(1)
        self.is_opened = True
        print(f"[INFO] Camera system opened in {self.camera_mode.upper()} mode")
        return True

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
            camera_name: 摄像头名称 ("left", "right", "third")
        
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
        """获取摄像头系统信息"""
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
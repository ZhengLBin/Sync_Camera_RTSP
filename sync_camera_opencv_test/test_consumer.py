#!/usr/bin/env python3

import cv2
import numpy as np
import socket
import threading
import time
import sys
import queue

class AdaptiveCameraClient:
    def __init__(self, host='127.0.0.1', start_port=5010, width=640, height=480):
        self.host = host
        self.start_port = start_port
        self.width = width
        self.height = height
        self.frame_size = width * height * 3 // 2  # I420
        
        self.running = False
        self.start_time = None
        self.camera_mode = None  # 'dual', 'triple', 'quad'
        
        # 动态摄像头队列和计数器
        self.camera_queues = []
        self.frame_counters = []
        self.camera_names = ["Left", "Right", "Third", "Fourth"]  # 新增第四个摄像头
        self.camera_ports = []
        
        # 显示模式
        self.display_mode = 'combined'  # 'separate' 或 'combined'
        
        # 性能阈值（针对不同模式）
        self.fps_thresholds = {
            'dual': 28,    # 双摄像头30fps
            'triple': 18,  # 三摄像头20fps
            'quad': 16     # 四摄像头18fps
        }
    
    def detect_available_streams(self):
        """检测可用的摄像头流"""
        print(f"[DETECTION] Detecting available camera streams starting from port {self.start_port}...")
        
        available_ports = []
        
        # 检测最多4个端口
        for i in range(4):
            port = self.start_port + i
            try:
                test_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                test_sock.settimeout(3)
                result = test_sock.connect_ex((self.host, port))
                test_sock.close()
                
                if result == 0:
                    available_ports.append(port)
                    print(f"[DETECTION] ✓ Found camera stream on port {port}")
                else:
                    print(f"[DETECTION] ✗ Port {port} not available")
                    break  # 假设端口是连续的
            except:
                print(f"[DETECTION] ✗ Cannot test port {port}")
                break
        
        # 确定模式
        if len(available_ports) >= 4:
            self.camera_mode = 'quad'
            self.camera_ports = available_ports[:4]
            print(f"[DETECTION] Mode: Quad Camera ({len(self.camera_ports)} streams)")
        elif len(available_ports) >= 3:
            self.camera_mode = 'triple'
            self.camera_ports = available_ports[:3]
            print(f"[DETECTION] Mode: Triple Camera ({len(self.camera_ports)} streams)")
        elif len(available_ports) >= 2:
            self.camera_mode = 'dual'
            self.camera_ports = available_ports[:2]
            print(f"[DETECTION] Mode: Dual Camera ({len(self.camera_ports)} streams)")
        else:
            self.camera_mode = 'none'
            print(f"[DETECTION] Error: Need at least 2 camera streams")
            return False
        
        # 初始化队列和计数器
        for i in range(len(self.camera_ports)):
            self.camera_queues.append(queue.Queue(maxsize=3))  # 增加队列大小
            self.frame_counters.append([0])
        
        return True
    
    def i420_to_bgr(self, data):
        """I420转BGR - 完整彩色转换"""
        try:
            y_size = self.width * self.height
            uv_size = y_size // 4
            
            # 提取YUV三个平面
            y_plane = np.frombuffer(data[:y_size], dtype=np.uint8).reshape(self.height, self.width)
            u_plane = np.frombuffer(data[y_size:y_size + uv_size], dtype=np.uint8).reshape(self.height//2, self.width//2)
            v_plane = np.frombuffer(data[y_size + uv_size:], dtype=np.uint8).reshape(self.height//2, self.width//2)
            
            # 上采样U和V平面到原始尺寸
            u_upsampled = cv2.resize(u_plane, (self.width, self.height), interpolation=cv2.INTER_LINEAR)
            v_upsampled = cv2.resize(v_plane, (self.width, self.height), interpolation=cv2.INTER_LINEAR)
            
            # 合并YUV平面
            yuv_img = np.dstack([y_plane, u_upsampled, v_upsampled])
            
            # YUV转BGR
            bgr_img = cv2.cvtColor(yuv_img, cv2.COLOR_YUV2BGR)
            
            return bgr_img
            
        except Exception as e:
            print(f"I420转换错误: {e}")
            return None
    
    def add_frame_info(self, frame, camera_name, port, frame_count, camera_index):
        """在帧上添加信息叠加"""
        if frame is None:
            return None
            
        # 计算FPS
        elapsed = time.time() - self.start_time if self.start_time else 0
        fps = frame_count / elapsed if elapsed > 0 else 0
        
        # 添加半透明背景
        overlay = frame.copy()
        cv2.rectangle(overlay, (5, 5), (300, 180), (0, 0, 0), -1)
        frame = cv2.addWeighted(frame, 0.7, overlay, 0.3, 0)
        
        # 选择颜色（区分摄像头）
        colors = [(0, 255, 0), (0, 255, 255), (255, 0, 255), (255, 128, 0)]  # 绿、黄、品红、橙
        color = colors[camera_index % len(colors)]
        
        # 绘制信息文本
        cv2.putText(frame, f'{camera_name} Camera', (10, 30), 
                   cv2.FONT_HERSHEY_SIMPLEX, 0.7, color, 2)
        cv2.putText(frame, f'FPS: {fps:.1f}', (10, 60), 
                   cv2.FONT_HERSHEY_SIMPLEX, 0.6, color, 2)
        cv2.putText(frame, f'Frames: {frame_count}', (10, 90), 
                   cv2.FONT_HERSHEY_SIMPLEX, 0.6, (255, 255, 0), 2)
        cv2.putText(frame, f'Port: {port}', (10, 120), 
                   cv2.FONT_HERSHEY_SIMPLEX, 0.5, (255, 255, 255), 1)
        cv2.putText(frame, f'Mode: {self.camera_mode.upper()}', (10, 145), 
                   cv2.FONT_HERSHEY_SIMPLEX, 0.5, (200, 200, 200), 1)
        
        # 为四摄像头模式添加特殊标识
        if self.camera_mode == 'quad':
            if camera_index == 2:  # 第三摄像头（插值）
                cv2.putText(frame, 'INTERPOLATED', (10, 170), 
                           cv2.FONT_HERSHEY_SIMPLEX, 0.4, (0, 255, 255), 1)
            cv2.putText(frame, f'Cam {camera_index}', (250, 30), 
                       cv2.FONT_HERSHEY_SIMPLEX, 0.6, color, 2)
        
        return frame
    
    def socket_stream_worker(self, port, camera_name, camera_index):
        """Socket流工作线程"""
        try:
            print(f"[{camera_name}] Connecting to port {port}...")
            sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            sock.settimeout(15)  # 增加超时时间
            sock.connect((self.host, port))
            print(f"[{camera_name}] ✓ Connected to port {port}")
            
            # 为四摄像头模式添加缓冲区优化
            if self.camera_mode == 'quad':
                sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1024*1024)  # 1MB接收缓冲区
            
            while self.running:
                # 读取完整的I420帧
                frame_data = b''
                while len(frame_data) < self.frame_size:
                    remaining = self.frame_size - len(frame_data)
                    chunk_size = min(remaining, 16384)  # 增加块大小
                    chunk = sock.recv(chunk_size)
                    if not chunk:
                        print(f"[{camera_name}] Connection closed")
                        return
                    frame_data += chunk
                
                # 转换为彩色图像
                bgr_frame = self.i420_to_bgr(frame_data)
                if bgr_frame is not None:
                    self.frame_counters[camera_index][0] += 1
                    
                    # 添加信息叠加
                    bgr_frame = self.add_frame_info(
                        bgr_frame, camera_name, port, 
                        self.frame_counters[camera_index][0], camera_index
                    )
                    
                    # 将处理好的图像放入队列
                    try:
                        self.camera_queues[camera_index].put_nowait(bgr_frame)
                    except queue.Full:
                        # 如果队列已满，清空队列并放入新帧
                        try:
                            self.camera_queues[camera_index].get_nowait()
                            self.camera_queues[camera_index].put_nowait(bgr_frame)
                        except queue.Empty:
                            pass
                        
        except Exception as e:
            print(f"[{camera_name}] Error: {e}")
        finally:
            try:
                sock.close()
            except:
                pass
    
    def create_combined_display(self, frames):
        """创建合并显示"""
        if self.camera_mode == 'dual':
            # 双摄像头：左右布局
            canvas_width = self.width * 2
            canvas_height = self.height
            canvas = np.zeros((canvas_height, canvas_width, 3), dtype=np.uint8)
            
            # 左摄像头
            if frames[0] is not None:
                canvas[0:self.height, 0:self.width] = frames[0]
            else:
                cv2.putText(canvas, 'Left Camera\nNo Signal', (50, self.height//2), 
                           cv2.FONT_HERSHEY_SIMPLEX, 1, (0, 0, 255), 2)
            
            # 右摄像头
            if frames[1] is not None:
                canvas[0:self.height, self.width:canvas_width] = frames[1]
            else:
                cv2.putText(canvas, 'Right Camera\nNo Signal', (self.width + 50, self.height//2), 
                           cv2.FONT_HERSHEY_SIMPLEX, 1, (0, 0, 255), 2)
            
            # 添加分割线
            cv2.line(canvas, (self.width, 0), (self.width, self.height), (255, 255, 255), 2)
            
        elif self.camera_mode == 'triple':
            # 三摄像头：2x2布局（左上、右上、底部中央）
            canvas_width = self.width * 2
            canvas_height = self.height * 2
            canvas = np.zeros((canvas_height, canvas_width, 3), dtype=np.uint8)
            
            # 左上：左摄像头
            if frames[0] is not None:
                canvas[0:self.height, 0:self.width] = frames[0]
            
            # 右上：右摄像头
            if frames[1] is not None:
                canvas[0:self.height, self.width:canvas_width] = frames[1]
            
            # 底部中央：第三摄像头
            if frames[2] is not None:
                start_x = self.width // 2
                start_y = self.height
                canvas[start_y:start_y + self.height, start_x:start_x + self.width] = frames[2]
            
            # 添加分割线
            cv2.line(canvas, (self.width, 0), (self.width, self.height), (255, 255, 255), 2)
            cv2.line(canvas, (0, self.height), (canvas_width, self.height), (255, 255, 255), 2)
            
        else:  # quad
            # 四摄像头：2x2标准网格布局
            canvas_width = self.width * 2
            canvas_height = self.height * 2
            canvas = np.zeros((canvas_height, canvas_width, 3), dtype=np.uint8)
            
            # 左上：左摄像头 (Cam 0)
            if frames[0] is not None:
                canvas[0:self.height, 0:self.width] = frames[0]
            else:
                self._draw_no_signal(canvas, "Left Camera\n(Cam 0)", 0, 0)
            
            # 右上：右摄像头 (Cam 1)
            if frames[1] is not None:
                canvas[0:self.height, self.width:canvas_width] = frames[1]
            else:
                self._draw_no_signal(canvas, "Right Camera\n(Cam 1)", self.width, 0)
            
            # 左下：第三摄像头 (Cam 2, 插值)
            if frames[2] is not None:
                canvas[self.height:canvas_height, 0:self.width] = frames[2]
            else:
                self._draw_no_signal(canvas, "Third Camera\n(Cam 2, Interpolated)", 0, self.height)
            
            # 右下：第四摄像头 (Cam 3)
            if frames[3] is not None:
                canvas[self.height:canvas_height, self.width:canvas_width] = frames[3]
            else:
                self._draw_no_signal(canvas, "Fourth Camera\n(Cam 3)", self.width, self.height)
            
            # 添加分割线
            cv2.line(canvas, (self.width, 0), (self.width, canvas_height), (255, 255, 255), 2)
            cv2.line(canvas, (0, self.height), (canvas_width, self.height), (255, 255, 255), 2)
            
            # 为四摄像头模式添加特殊标识
            cv2.putText(canvas, 'QUAD SYNC', (canvas_width - 150, 30), 
                       cv2.FONT_HERSHEY_SIMPLEX, 0.7, (0, 255, 255), 2)
        
        # 添加整体信息
        total_frames = sum(counter[0] for counter in self.frame_counters)
        elapsed = time.time() - self.start_time if self.start_time else 0
        overall_fps = total_frames / (elapsed * len(self.camera_ports)) if elapsed > 0 else 0
        
        info_y = canvas.shape[0] - 30
        mode_display = f'{self.camera_mode.capitalize()} Camera Sync'
        if self.camera_mode == 'quad':
            mode_display += ' (with Interpolation)'
        
        cv2.putText(canvas, f'{mode_display} - Overall FPS: {overall_fps:.1f}', 
                   (10, info_y), cv2.FONT_HERSHEY_SIMPLEX, 0.8, (255, 255, 255), 2)
        
        return canvas
    
    def _draw_no_signal(self, canvas, text, start_x, start_y):
        """绘制无信号提示"""
        text_lines = text.split('\n')
        y_offset = self.height // 2 - len(text_lines) * 15
        for i, line in enumerate(text_lines):
            cv2.putText(canvas, line, (start_x + 50, start_y + y_offset + i * 30), 
                       cv2.FONT_HERSHEY_SIMPLEX, 0.7, (0, 0, 255), 2)
    
    def display_frames_separate(self):
        """分窗口显示摄像头"""
        # 创建窗口
        window_names = []
        for i, name in enumerate(self.camera_names[:len(self.camera_ports)]):
            window_name = f"{name} Camera"
            window_names.append(window_name)
            cv2.namedWindow(window_name, cv2.WINDOW_NORMAL)
            cv2.resizeWindow(window_name, self.width, self.height)
            
            # 设置窗口位置
            if self.camera_mode == 'dual':
                cv2.moveWindow(window_name, 50 + i * (self.width + 20), 50)
            elif self.camera_mode == 'triple':
                positions = [(50, 50), (50 + self.width + 20, 50), (50 + self.width//2, 50 + self.height + 50)]
                cv2.moveWindow(window_name, positions[i][0], positions[i][1])
            else:  # quad
                # 2x2布局
                row = i // 2
                col = i % 2
                x = 50 + col * (self.width + 20)
                y = 50 + row * (self.height + 50)
                cv2.moveWindow(window_name, x, y)
        
        while self.running:
            # 显示每个摄像头
            for i, window_name in enumerate(window_names):
                if not self.camera_queues[i].empty():
                    frame = self.camera_queues[i].get()
                    cv2.imshow(window_name, frame)
            
            # 检查退出和模式切换
            key = cv2.waitKey(1) & 0xFF
            if key == ord('q'):
                self.running = False
                break
            elif key == ord('c'):
                self.display_mode = 'combined'
                cv2.destroyAllWindows()
                return
            
            time.sleep(0.001)
    
    def display_frames_combined(self):
        """合并显示摄像头"""
        window_name = f"{self.camera_mode.capitalize()} Camera View"
        cv2.namedWindow(window_name, cv2.WINDOW_NORMAL)
        
        if self.camera_mode == 'dual':
            cv2.resizeWindow(window_name, self.width * 2, self.height)
        else:  # triple 或 quad
            cv2.resizeWindow(window_name, self.width * 2, self.height * 2)
        
        frames = [None] * len(self.camera_ports)
        
        while self.running:
            # 获取最新帧
            for i in range(len(self.camera_ports)):
                if not self.camera_queues[i].empty():
                    frames[i] = self.camera_queues[i].get()
            
            # 创建合并显示
            combined_frame = self.create_combined_display(frames)
            cv2.imshow(window_name, combined_frame)
            
            # 检查退出和模式切换
            key = cv2.waitKey(1) & 0xFF
            if key == ord('q'):
                self.running = False
                break
            elif key == ord('s'):
                self.display_mode = 'separate'
                cv2.destroyAllWindows()
                return
            
            time.sleep(0.001)
    
    def display_frames(self):
        """主显示循环 - 支持模式切换"""
        while self.running:
            if self.display_mode == 'separate':
                self.display_frames_separate()
            else:
                self.display_frames_combined()
    
    def run_adaptive_stream(self):
        """运行自适应摄像头流"""
        print("="*80)
        print("Adaptive Multi-Camera Sync Client")
        print("Automatically detects dual/triple/quad camera configuration")
        print("="*80)
        
        # 检测可用流
        if not self.detect_available_streams():
            return False
        
        print(f"Mode: {self.camera_mode.upper()} camera sync")
        print(f"Ports: {', '.join(map(str, self.camera_ports))}")
        
        if self.camera_mode == 'quad':
            print("Special Features:")
            print("  - Camera 2 uses frame interpolation")
            print("  - Expected FPS: ~18")
        
        print("Controls:")
        print("  - Combined view: Press 'c'")
        print("  - Separate view: Press 's'") 
        print("  - Exit: Press 'q'")
        print("="*80)
        
        # 启动流
        self.running = True
        self.start_time = time.time()
        
        # 创建工作线程
        threads = []
        for i, port in enumerate(self.camera_ports):
            camera_name = self.camera_names[i]
            thread = threading.Thread(
                target=self.socket_stream_worker,
                args=(port, camera_name, i)
            )
            thread.daemon = True
            thread.start()
            threads.append(thread)
        
        # 在主线程运行显示循环
        try:
            self.display_frames()
        except KeyboardInterrupt:
            print("\nUser interrupted")
            self.running = False
        
        # 等待工作线程结束
        for thread in threads:
            thread.join(timeout=2.0)
            
        cv2.destroyAllWindows()
        
        # 显示最终统计
        if self.start_time:
            elapsed = time.time() - self.start_time
            print(f"\n📊 {self.camera_mode.capitalize()} Camera Sync Statistics:")
            print(f"   Runtime: {elapsed:.1f} seconds")
            
            total_frames = 0
            fps_values = []
            for i, counter in enumerate(self.frame_counters):
                camera_name = self.camera_names[i]
                fps = counter[0] / elapsed if elapsed > 0 else 0
                fps_values.append(fps)
                
                # 为插值摄像头添加标识
                interpolated_mark = " (interpolated)" if (self.camera_mode in ['triple', 'quad'] and i == 2) else ""
                print(f"   {camera_name} Camera{interpolated_mark}: {counter[0]} frames ({fps:.1f} fps)")
                total_frames += counter[0]
            
            # 同步质量分析
            avg_fps = total_frames / (elapsed * len(self.camera_ports)) if elapsed > 0 else 0
            print(f"   Average Sync FPS: {avg_fps:.1f}")
            
            # 帧率一致性分析
            if len(fps_values) > 1:
                fps_std = np.std(fps_values)
                print(f"   Frame Rate Consistency: {fps_std:.2f} (lower is better)")
                
                # 性能评估
                threshold = self.fps_thresholds.get(self.camera_mode, 15)
                if avg_fps > threshold:
                    print(f"   ✓ Excellent {self.camera_mode} camera sync performance!")
                elif avg_fps > threshold * 0.8:
                    print(f"   ✓ Good {self.camera_mode} camera sync performance!")
                else:
                    print(f"   ⚠ {self.camera_mode.capitalize()} sync performance could be improved")
                    print(f"     Expected: >{threshold} fps, Actual: {avg_fps:.1f} fps")
                
                # 四摄像头特殊分析
                if self.camera_mode == 'quad':
                    print("   Quad Camera Analysis:")
                    
                    # 分析主摄像头 vs 插值摄像头性能
                    main_cameras_fps = [fps_values[0], fps_values[1], fps_values[3]]  # 0,1,3是主摄像头
                    interpolated_fps = fps_values[2]  # 2是插值摄像头
                    main_avg = np.mean(main_cameras_fps)
                    
                    print(f"     Main Cameras (0,1,3) Avg FPS: {main_avg:.1f}")
                    print(f"     Interpolated Camera (2) FPS: {interpolated_fps:.1f}")
                    
                    # 插值效果评估
                    interpolation_ratio = interpolated_fps / main_avg if main_avg > 0 else 0
                    if interpolation_ratio > 1.5:
                        print("     ✓ Excellent interpolation performance!")
                    elif interpolation_ratio > 1.0:
                        print("     ✓ Good interpolation performance!")
                    else:
                        print("     ⚠ Interpolation may need optimization")
        
        return True

def main():
    # 解析命令行参数
    if len(sys.argv) > 1:
        start_port = int(sys.argv[1])
    else:
        start_port = 5010
    
    print(f"Starting adaptive multi-camera client from port {start_port}")
    
    client = AdaptiveCameraClient(start_port=start_port)
    success = client.run_adaptive_stream()
    
    if success:
        print("✓ Adaptive multi-camera client exited normally")
    else:
        print("✗ Adaptive multi-camera client exited with error")
        return 1
    
    return 0

if __name__ == "__main__":
    sys.exit(main())
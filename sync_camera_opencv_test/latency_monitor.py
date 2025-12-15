import socket
import struct
import time
import threading
from collections import deque
import statistics

class LatencyMonitor:
    def __init__(self, port, name):
        self.port = port
        self.name = name
        self.frame_times = deque(maxlen=100)
        self.last_frame_time = None
        self.frame_count = 0
        self.running = True
        
    def connect_and_monitor(self):
        """连接到TCP流并监控延迟"""
        try:
            sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            sock.settimeout(5.0)
            sock.connect(('127.0.0.1', self.port))
            print(f"[{self.name}] 已连接到端口 {self.port}")
            
            buffer = b''
            start_time = time.time()
            
            while self.running:
                try:
                    data = sock.recv(8192)
                    if not data:
                        break
                    
                    current_time = time.time()
                    
                    # 记录接收时间
                    if self.last_frame_time is not None:
                        interval = (current_time - self.last_frame_time) * 1000  # ms
                        self.frame_times.append(interval)
                    
                    self.last_frame_time = current_time
                    self.frame_count += 1
                    
                    # 每秒输出一次统计
                    if self.frame_count % 30 == 0:
                        self.print_stats()
                        
                except socket.timeout:
                    continue
                except Exception as e:
                    print(f"[{self.name}] 错误: {e}")
                    break
                    
        except Exception as e:
            print(f"[{self.name}] 连接失败: {e}")
        finally:
            sock.close()
            
    def print_stats(self):
        """打印统计信息"""
        if len(self.frame_times) < 2:
            return
            
        avg_interval = statistics.mean(self.frame_times)
        std_dev = statistics.stdev(self.frame_times)
        min_interval = min(self.frame_times)
        max_interval = max(self.frame_times)
        
        print(f"[{self.name}:{self.port}] 帧数={self.frame_count} "
              f"平均间隔={avg_interval:.1f}ms "
              f"标准差={std_dev:.1f}ms "
              f"范围=[{min_interval:.1f}, {max_interval:.1f}]ms "
              f"队列={len(self.frame_times)}")

def main():
    print("=" * 60)
    print("摄像头延迟监控工具")
    print("=" * 60)
    print("监控端口 5010 和 5011 的帧间隔和延迟")
    print("按 Ctrl+C 停止监控")
    print("=" * 60)
    print()
    
    # 创建两个监控器
    monitor1 = LatencyMonitor(5010, "Camera-0")
    monitor2 = LatencyMonitor(5011, "Camera-1")
    
    # 启动监控线程
    thread1 = threading.Thread(target=monitor1.connect_and_monitor)
    thread2 = threading.Thread(target=monitor2.connect_and_monitor)
    
    thread1.daemon = True
    thread2.daemon = True
    
    thread1.start()
    thread2.start()
    
    try:
        # 主线程每5秒输出对比信息
        while True:
            time.sleep(5)
            print()
            print("=" * 60)
            print(f"对比统计（运行时间: {time.time():.1f}s）")
            print("=" * 60)
            monitor1.print_stats()
            monitor2.print_stats()
            
            # 计算延迟差异
            if monitor1.frame_count > 0 and monitor2.frame_count > 0:
                frame_diff = monitor1.frame_count - monitor2.frame_count
                print(f"\n帧数差异: {frame_diff} 帧")
                if abs(frame_diff) > 5:
                    print(f"⚠️ 警告: 帧数差异较大，可能存在延迟问题！")
            print("=" * 60)
            print()
            
    except KeyboardInterrupt:
        print("\n\n正在停止监控...")
        monitor1.running = False
        monitor2.running = False
        time.sleep(1)
        print("监控已停止")

if __name__ == "__main__":
    main()

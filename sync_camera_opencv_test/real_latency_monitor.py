"""
真正的延迟监控工具 - 通过解析 H264 流中的 PTS 时间戳来测量真实延迟
"""
import socket
import struct
import time
import threading
from collections import deque
import statistics

class H264PTSMonitor:
    """监控 H264 流的 PTS 时间戳和实际延迟"""
    
    def __init__(self, port, name):
        self.port = port
        self.name = name
        self.pts_history = deque(maxlen=100)
        self.receive_times = deque(maxlen=100)
        self.frame_count = 0
        self.running = True
        self.first_pts = None
        self.first_receive_time = None
        
    def connect_and_monitor(self):
        """连接并监控"""
        try:
            sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            sock.settimeout(5.0)
            sock.connect(('127.0.0.1', self.port))
            print(f"[{self.name}] ✅ 已连接到端口 {self.port}")
            
            buffer = b''
            nal_start = b'\x00\x00\x00\x01'
            
            while self.running:
                try:
                    data = sock.recv(65536)
                    if not data:
                        break
                    
                    buffer += data
                    receive_time = time.time()
                    
                    # 简单的帧检测（基于 NAL 起始码）
                    while nal_start in buffer:
                        pos = buffer.find(nal_start)
                        next_pos = buffer.find(nal_start, pos + 4)
                        
                        if next_pos == -1:
                            break
                        
                        # 找到一个完整的 NAL 单元
                        nal_unit = buffer[pos:next_pos]
                        buffer = buffer[next_pos:]
                        
                        # 检查是否是视频帧（NAL type 1, 5）
                        if len(nal_unit) > 4:
                            nal_type = nal_unit[4] & 0x1F
                            if nal_type in [1, 5]:  # P帧或I帧
                                self.frame_count += 1
                                self.receive_times.append(receive_time)
                                
                                # 记录第一帧的接收时间作为基准
                                if self.first_receive_time is None:
                                    self.first_receive_time = receive_time
                                
                                if self.frame_count % 30 == 0:
                                    self.print_stats(receive_time)
                        
                except socket.timeout:
                    continue
                except Exception as e:
                    print(f"[{self.name}] ❌ 错误: {e}")
                    break
                    
        except Exception as e:
            print(f"[{self.name}] ❌ 连接失败: {e}")
        finally:
            sock.close()
            
    def print_stats(self, current_time):
        """打印统计信息"""
        if len(self.receive_times) < 2:
            return
        
        # 计算帧间隔
        intervals = []
        for i in range(1, min(len(self.receive_times), 100)):
            interval = (self.receive_times[i] - self.receive_times[i-1]) * 1000
            intervals.append(interval)
        
        if not intervals:
            return
            
        avg_interval = statistics.mean(intervals)
        std_dev = statistics.stdev(intervals) if len(intervals) > 1 else 0
        
        # 计算从第一帧到现在的总延迟
        elapsed_time = current_time - self.first_receive_time
        expected_frames = elapsed_time * 30  # 假设 30fps
        frame_lag = expected_frames - self.frame_count
        
        print(f"[{self.name}:{self.port}] "
              f"帧数={self.frame_count} "
              f"间隔={avg_interval:.1f}±{std_dev:.1f}ms "
              f"运行={elapsed_time:.1f}s "
              f"落后={frame_lag:.0f}帧")

def main():
    print("=" * 70)
    print("🎯 H264 流真实延迟监控工具")
    print("=" * 70)
    print("监控 5010 和 5011 端口的实际帧接收情况")
    print("通过对比接收时间来检测延迟差异")
    print("按 Ctrl+C 停止监控")
    print("=" * 70)
    print()
    
    # 创建监控器
    monitor1 = H264PTSMonitor(5010, "Cam-0")
    monitor2 = H264PTSMonitor(5011, "Cam-1")
    
    # 启动线程
    thread1 = threading.Thread(target=monitor1.connect_and_monitor)
    thread2 = threading.Thread(target=monitor2.connect_and_monitor)
    
    thread1.daemon = True
    thread2.daemon = True
    
    thread1.start()
    thread2.start()
    
    try:
        last_check_time = time.time()
        
        while True:
            time.sleep(5)
            current_time = time.time()
            
            print()
            print("=" * 70)
            print(f"📊 对比统计（已运行 {current_time - last_check_time:.0f}s）")
            print("=" * 70)
            
            # 计算延迟差异
            if monitor1.frame_count > 30 and monitor2.frame_count > 30:
                frame_diff = abs(monitor1.frame_count - monitor2.frame_count)
                time_diff = frame_diff / 30.0  # 秒
                
                print(f"📹 Cam-0 (5010): {monitor1.frame_count} 帧")
                print(f"📹 Cam-1 (5011): {monitor2.frame_count} 帧")
                print(f"📊 帧数差异: {frame_diff} 帧 ≈ {time_diff:.2f} 秒")
                
                if monitor1.frame_count < monitor2.frame_count:
                    print(f"⚠️  警告: Cam-0 (5010) 落后 {time_diff:.2f} 秒！")
                elif monitor2.frame_count < monitor1.frame_count:
                    print(f"⚠️  警告: Cam-1 (5011) 落后 {time_diff:.2f} 秒！")
                else:
                    print(f"✅ 同步正常！")
                
                # 分析延迟原因
                if frame_diff > 60:  # 超过2秒
                    print()
                    print("🔍 可能的原因:")
                    if monitor1.frame_count < monitor2.frame_count:
                        print("   • Cam-0 的 GStreamer 管道缓冲过多")
                        print("   • Cam-0 的编码器处理速度慢")
                        print("   • Cam-0 的摄像头本身采集慢")
                    else:
                        print("   • Cam-1 的 GStreamer 管道缓冲过多")
                        print("   • Cam-1 的编码器处理速度慢")
                        print("   • Cam-1 的摄像头本身采集慢")
            
            print("=" * 70)
            print()
            
    except KeyboardInterrupt:
        print("\n\n⏸️  正在停止监控...")
        monitor1.running = False
        monitor2.running = False
        time.sleep(1)
        
        # 最终统计
        print()
        print("=" * 70)
        print("📈 最终统计")
        print("=" * 70)
        print(f"Cam-0 (5010): 总共接收 {monitor1.frame_count} 帧")
        print(f"Cam-1 (5011): 总共接收 {monitor2.frame_count} 帧")
        
        if monitor1.frame_count > 0 and monitor2.frame_count > 0:
            frame_diff = abs(monitor1.frame_count - monitor2.frame_count)
            time_diff = frame_diff / 30.0
            print(f"最终差异: {frame_diff} 帧 ≈ {time_diff:.2f} 秒")
        
        print("=" * 70)
        print("监控已停止 ✋")

if __name__ == "__main__":
    main()

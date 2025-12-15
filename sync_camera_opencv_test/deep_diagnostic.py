"""
深度诊断工具 - 检查 GStreamer 管道状态和缓冲情况
"""
import subprocess
import time
import re

def check_gstreamer_pipeline(port):
    """使用 gst-launch 测试管道"""
    print(f"\n{'='*60}")
    print(f"检查端口 {port} 的流")
    print('='*60)
    
    # 使用 gst-launch 测试接收
    cmd = [
        'gst-launch-1.0',
        '-v',
        f'tcpclientsrc', f'host=127.0.0.1', f'port={port}',
        '!', 'h264parse',
        '!', 'avdec_h264',
        '!', 'fpsdisplaysink', 'sync=false'
    ]
    
    try:
        print(f"运行命令: {' '.join(cmd)}")
        proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        
        # 读取15秒的输出
        start_time = time.time()
        while time.time() - start_time < 15:
            line = proc.stderr.readline()
            if line:
                # 提取关键信息
                if 'current' in line.lower() or 'fps' in line.lower():
                    print(f"  {line.strip()}")
                if 'latency' in line.lower() or 'buffer' in line.lower():
                    print(f"  ⚠️  {line.strip()}")
        
        proc.terminate()
        proc.wait(timeout=2)
        
    except Exception as e:
        print(f"错误: {e}")

def analyze_sync_camera_output():
    """分析 sync_camera 的输出"""
    print("\n" + "="*60)
    print("🔍 运行 sync_camera 并分析输出")
    print("="*60)
    
    # 启动 sync_camera
    cmd = ['sync_camera.exe']
    
    try:
        proc = subprocess.Popen(
            cmd, 
            stdout=subprocess.PIPE, 
            stderr=subprocess.PIPE, 
            text=True,
            cwd='D:\\_DesktopMoveHere\\Leji_proj\\sync_camera\\build'
        )
        
        print("等待程序启动...")
        time.sleep(3)
        
        # 读取输出
        start_time = time.time()
        frame_counts = {}
        
        while time.time() - start_time < 20:
            line = proc.stdout.readline()
            if line:
                print(f"  {line.strip()}")
                
                # 提取帧数信息
                match = re.search(r'(\d+) synced frames', line)
                if match:
                    frame_count = int(match.group(1))
                    elapsed = time.time() - start_time
                    fps = frame_count / elapsed
                    print(f"    → 当前 FPS: {fps:.1f}")
        
        proc.terminate()
        proc.wait(timeout=2)
        
    except Exception as e:
        print(f"错误: {e}")

def main():
    print("="*60)
    print("🔬 GStreamer 管道深度诊断工具")
    print("="*60)
    print()
    print("此工具将:")
    print("  1. 检查两个 TCP 流的实际输出")
    print("  2. 测量编码器延迟")
    print("  3. 分析 sync_camera 的运行状态")
    print()
    print("请确保:")
    print("  • sync_camera.exe 已经在运行")
    print("  • GStreamer 工具已安装（gst-launch-1.0）")
    print()
    input("按回车开始诊断...")
    
    # 检查两个端口
    check_gstreamer_pipeline(5010)
    time.sleep(2)
    check_gstreamer_pipeline(5011)
    
    print("\n" + "="*60)
    print("诊断完成")
    print("="*60)

if __name__ == "__main__":
    main()

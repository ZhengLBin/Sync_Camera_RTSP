#!/usr/bin/env python3
"""
Camera类测试脚本
演示如何使用Camera类连接多摄像头同步传输系统
"""

import cv2
import time
from entity_c import Camera

def test_basic_usage():
    """基础使用测试"""
    print("=== 基础使用测试 ===")
    
    # 创建Camera实例
    camera = Camera()
    
    # 打开摄像头
    if not camera.open():
        print("❌ 无法打开摄像头")
        return False
    
    # 获取系统信息
    info = camera.get_camera_info()
    print(f"📷 摄像头模式: {info['mode'].upper()}")
    print(f"📷 摄像头数量: {info['camera_count']}")
    print(f"📷 端口: {info['ports']}")
    
    # 读取几帧数据测试
    print("\n开始读取测试...")
    for i in range(10):
        success, frame = camera.read("left")
        if success:
            print(f"✓ 第{i+1}帧读取成功 - 尺寸: {frame.shape}")
        else:
            print(f"❌ 第{i+1}帧读取失败")
        time.sleep(0.1)
    
    # 关闭摄像头
    camera.close()
    return True

def test_multi_camera_display():
    """多摄像头显示测试"""
    print("\n=== 多摄像头显示测试 ===")
    print("按 'q' 键退出显示")
    
    with Camera() as camera:
        if not camera.is_opened_camera():
            print("❌ 摄像头打开失败")
            return False
        
        info = camera.get_camera_info()
        print(f"📷 {info['mode'].upper()}模式，{info['camera_count']}个摄像头")
        
        # 等待数据稳定
        time.sleep(2)
        
        while True:
            # 读取所有摄像头数据
            frames = camera.read_all()
            
            # 显示每个摄像头
            display_count = 0
            for name, frame in frames.items():
                if frame is not None:
                    window_name = f"{name.upper()} Camera"
                    cv2.imshow(window_name, frame)
                    display_count += 1
            
            if display_count == 0:
                print("⚠️ 暂无图像数据")
            
            # 检查退出
            key = cv2.waitKey(1) & 0xFF
            if key == ord('q'):
                break
        
        cv2.destroyAllWindows()
    
    return True

def test_synchronized_capture():
    """同步捕获测试"""
    print("\n=== 同步捕获测试 ===")
    
    camera = Camera()
    if not camera.open():
        return False
    
    print("测试同步读取...")
    sync_success_count = 0
    
    for i in range(50):
        success, frames = camera.read_sync()
        if success:
            sync_success_count += 1
            valid_cameras = [name for name, frame in frames.items() if frame is not None]
            print(f"✓ 同步帧 {i+1}: {valid_cameras}")
        else:
            print(f"❌ 同步失败 {i+1}")
        
        time.sleep(0.05)
    
    print(f"\n同步成功率: {sync_success_count}/50 ({sync_success_count*2}%)")
    
    # 显示最终统计
    info = camera.get_camera_info()
    print(f"\n📊 最终统计:")
    for name, cam_info in info['cameras'].items():
        print(f"   {name.upper()}: {cam_info['frames']} frames ({cam_info['fps']:.1f} fps)")
    
    camera.close()
    return True

def test_performance_monitoring():
    """性能监控测试"""
    print("\n=== 性能监控测试 ===")
    
    camera = Camera()
    if not camera.open():
        return False
    
    print("运行30秒性能测试...")
    start_time = time.time()
    
    while time.time() - start_time < 30:
        # 每5秒输出一次统计
        if int(time.time() - start_time) % 5 == 0:
            info = camera.get_camera_info()
            print(f"\n⏱️ 运行时间: {info['runtime']:.1f}s")
            print(f"📈 总帧数: {info['total_frames']}")
            
            for name, cam_info in info['cameras'].items():
                status = "🟢" if cam_info['has_data'] else "🔴"
                print(f"   {status} {name.upper()}: {cam_info['fps']:.1f} fps")
        
        time.sleep(1)
    
    camera.close()
    return True

def main():
    """主测试函数"""
    print("🚀 Camera类功能测试")
    print("=" * 50)
    
    # 确保服务端正在运行
    print("⚠️  请确保多摄像头同步传输系统服务端正在运行!")
    print("   启动命令: ./sync_camera")
    input("按回车键继续测试...")
    
    # 运行测试
    tests = [
        ("基础功能", test_basic_usage),
        ("多摄像头显示", test_multi_camera_display),
        ("同步捕获", test_synchronized_capture),
        ("性能监控", test_performance_monitoring)
    ]
    
    results = []
    for test_name, test_func in tests:
        print(f"\n{'='*20} {test_name} {'='*20}")
        try:
            result = test_func()
            results.append((test_name, "✅ 通过" if result else "❌ 失败"))
        except KeyboardInterrupt:
            print(f"\n用户中断了 {test_name} 测试")
            results.append((test_name, "⏹️ 中断"))
            break
        except Exception as e:
            print(f"❌ {test_name} 测试出错: {e}")
            results.append((test_name, f"❌ 错误: {e}"))
    
    # 测试结果汇总
    print(f"\n{'='*50}")
    print("🎯 测试结果汇总:")
    for test_name, result in results:
        print(f"   {test_name}: {result}")
    print("=" * 50)

if __name__ == "__main__":
    main()
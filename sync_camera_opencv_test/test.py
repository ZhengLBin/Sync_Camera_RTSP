#!/usr/bin/env python3 
"""
Camera类测试脚本（支持H.264解码和动态分辨率）
演示如何使用Camera类连接多摄像头同步传输系统
"""

import cv2
import time
from camera import Camera

def test_server_detection():
    """服务端检测测试"""
    print("=== 服务端摄像头检测测试 ===")
    
    camera = Camera()
    
    print("检测服务端摄像头配置...")
    server_info = camera.get_server_camera_info()
    
    print(f"服务端状态: {server_info['status'].upper()}")
    print(f"服务端运行: {'✓' if server_info['server_running'] else '✗'}")
    print(f"摄像头模式: {server_info['mode'].upper()}")
    print(f"摄像头数量: {server_info['camera_count']}")
    print(f"检测时间: {time.strftime('%H:%M:%S', time.localtime(server_info['detection_time']))}")
    
    print("\n摄像头详情:")
    for camera_info in server_info['detected_cameras']:
        status = "✓ 在线" if camera_info['connected'] else "✗ 离线"
        print(f"  {camera_info['name'].upper():>6}: 端口 {camera_info['port']} - {status}")
        if 'error' in camera_info:
            print(f"         错误: {camera_info['error']}")
    
    mapping = camera.get_camera_mapping()
    print("\n摄像头映射:")
    for name, description in mapping.items():
        print(f"  {name}: {description}")
    
    online = camera.is_server_online()
    print(f"\n服务端在线状态: {'✓ 在线' if online else '✗ 离线'}")
    
    return server_info['server_running']

def test_wait_for_server():
    """等待服务端上线测试"""
    print("\n=== 等待服务端测试 ===")
    
    camera = Camera()
    
    if camera.is_server_online():
        print("✓ 服务端已在线")
        return True
    
    print("⏳ 服务端离线，等待上线...")
    print("   请启动服务端程序: ./sync_camera")
    
    success = camera.wait_for_server(timeout=10, check_interval=2)
    
    if success:
        print("✓ 服务端成功上线!")
        server_info = camera.get_server_camera_info()
        print(f"   模式: {server_info['mode'].upper()}")
        print(f"   摄像头: {server_info['camera_count']}个")
    else:
        print("✗ 等待服务端超时")
    
    return success

def test_basic_usage():
    """基础使用测试（支持动态分辨率）"""
    print("\n=== 基础使用测试 ===")
    
    camera = Camera()
    
    if not camera.is_server_online():
        print("✗ 服务端未运行，跳过测试")
        return False
    
    if not camera.open():
        print("✗ 无法打开摄像头")
        return False
    
    info = camera.get_camera_info()
    print(f"✓ 摄像头模式: {info['mode'].upper()}")
    print(f"✓ 摄像头数量: {info['camera_count']}")
    print(f"✓ 端口: {info['ports']}")
    
    # 显示每个摄像头的分辨率
    print("\n摄像头分辨率:")
    for name, cam_info in info['cameras'].items():
        resolution = cam_info.get('resolution', (0, 0))
        print(f"  {name.upper()}: {resolution[0]}x{resolution[1]}")
    
    print("\n开始读取测试...")
    for i in range(10):
        success, frame = camera.read("left")
        if success:
            print(f"✓ 第{i+1}帧读取成功 - 尺寸: {frame.shape}")
        else:
            print(f"✗ 第{i+1}帧读取失败")
        time.sleep(0.1)
    
    camera.close()
    return True

def test_multi_camera_display():
    """多摄像头显示测试（自动适配分辨率）"""
    print("\n=== 多摄像头显示测试 ===")
    print("按 'q' 键退出显示")
    
    camera = Camera()
    
    if not camera.is_server_online():
        print("✗ 服务端未运行，跳过测试")
        return False
    
    with camera:
        if not camera.is_opened_camera():
            print("✗ 摄像头打开失败")
            return False
        
        info = camera.get_camera_info()
        print(f"✓ {info['mode'].upper()}模式，{info['camera_count']}个摄像头")
        
        # 显示分辨率信息
        print("\n分辨率信息:")
        for name, cam_info in info['cameras'].items():
            resolution = cam_info.get('resolution', (0, 0))
            print(f"  {name.upper()}: {resolution[0]}x{resolution[1]}")
        
        time.sleep(1)
        
        frame_count = 0
        fps_counter = {}
        last_fps_time = time.time()
        
        while True:
            frames = camera.read_all()
            
            display_count = 0
            for name, frame in frames.items():
                if frame is not None:
                    # 添加分辨率和FPS信息到图像上
                    h, w = frame.shape[:2]
                    
                    # 计算FPS
                    if name not in fps_counter:
                        fps_counter[name] = 0
                    fps_counter[name] += 1
                    
                    current_time = time.time()
                    if current_time - last_fps_time >= 1.0:
                        fps = fps_counter[name] / (current_time - last_fps_time)
                        # 在图像上显示信息
                        cv2.putText(frame, f"{name.upper()} {w}x{h} @ {fps:.1f}fps", 
                                  (10, 30), cv2.FONT_HERSHEY_SIMPLEX, 0.7, (0, 255, 0), 2)
                    
                    window_name = f"{name.upper()} Camera"
                    cv2.imshow(window_name, frame)
                    display_count += 1
            
            # 重置FPS计数器
            if time.time() - last_fps_time >= 1.0:
                fps_counter = {}
                last_fps_time = time.time()
            
            if display_count == 0:
                print("⏳ 暂无图像数据")
            
            frame_count += 1
            
            key = cv2.waitKey(1) & 0xFF
            if key == ord('q'):
                break
        
        cv2.destroyAllWindows()
        print(f"\n总共显示 {frame_count} 帧")
    
    return True

def test_synchronized_capture():
    """同步捕获测试"""
    print("\n=== 同步捕获测试 ===")
    
    camera = Camera()
    
    if not camera.is_server_online():
        print("✗ 服务端未运行，跳过测试")
        return False
    
    if not camera.open():
        return False
    
    info = camera.get_camera_info()
    print("分辨率信息:")
    for name, cam_info in info['cameras'].items():
        resolution = cam_info.get('resolution', (0, 0))
        print(f"  {name.upper()}: {resolution[0]}x{resolution[1]}")
    
    print("\n测试同步读取...")
    sync_success_count = 0
    
    for i in range(50):
        success, frames = camera.read_sync()
        if success:
            sync_success_count += 1
            valid_cameras = [name for name, frame in frames.items() if frame is not None]
            sizes = [f"{name}({frames[name].shape[1]}x{frames[name].shape[0]})" 
                    for name in valid_cameras]
            print(f"✓ 同步帧 {i+1}: {', '.join(sizes)}")
        else:
            print(f"✗ 同步失败 {i+1}")
        
        time.sleep(0.05)
    
    print(f"\n✓ 同步成功率: {sync_success_count}/50 ({sync_success_count * 2}%)")
    
    info = camera.get_camera_info()
    print("\n最终统计:")
    for name, cam_info in info['cameras'].items():
        resolution = cam_info.get('resolution', (0, 0))
        print(f"  {name.upper()}: {cam_info['frames']} frames ({cam_info['fps']:.1f} fps) @ {resolution[0]}x{resolution[1]}")
    
    camera.close()
    return True

def test_performance_monitoring():
    """性能监控测试"""
    print("\n=== 性能监控测试 ===")
    
    camera = Camera()
    
    if not camera.is_server_online():
        print("✗ 服务端未运行，跳过测试")
        return False
    
    if not camera.open():
        return False
    
    print("运行30秒性能测试...")
    start_time = time.time()
    
    last_print = -1
    while time.time() - start_time < 30:
        elapsed = int(time.time() - start_time)
        if elapsed % 5 == 0 and elapsed != last_print:
            last_print = elapsed
            info = camera.get_camera_info()
            print(f"\n⏱  运行时间: {info['runtime']:.1f}s")
            print(f"📊 总帧数: {info['total_frames']}")
            for name, cam_info in info['cameras'].items():
                status = "✓" if cam_info['has_data'] else "✗"
                resolution = cam_info.get('resolution', (0, 0))
                print(f"   {status} {name.upper()}: {cam_info['fps']:.1f} fps @ {resolution[0]}x{resolution[1]}")
        
        time.sleep(1)
    
    camera.close()
    return True

def test_server_info_refresh():
    """服务端信息缓存和刷新测试"""
    print("\n=== 服务端信息缓存和刷新测试 ===")
    
    camera = Camera()
    
    print("第一次检测...")
    start_time = time.time()
    info1 = camera.get_server_camera_info()
    detect_time1 = time.time() - start_time
    print(f"  检测耗时: {detect_time1:.3f}s")
    print(f"  状态: {info1['status']}, 模式: {info1['mode']}")
    
    print("\n第二次检测（使用缓存）...")
    start_time = time.time()
    info2 = camera.get_server_camera_info()
    detect_time2 = time.time() - start_time
    print(f"  检测耗时: {detect_time2:.3f}s")
    print(f"  状态: {info2['status']}, 模式: {info2['mode']}")
    
    print("\n强制刷新检测...")
    start_time = time.time()
    info3 = camera.get_server_camera_info(force_refresh=True)
    detect_time3 = time.time() - start_time
    print(f"  检测耗时: {detect_time3:.3f}s")
    print(f"  状态: {info3['status']}, 模式: {info3['mode']}")
    
    print("\n性能对比:")
    print(f"  首次检测: {detect_time1:.3f}s")
    speedup = detect_time1/detect_time2 if detect_time2 > 0 else 0
    print(f"  缓存读取: {detect_time2:.3f}s (加速 {speedup:.1f}x)")
    print(f"  强制刷新: {detect_time3:.3f}s")
    
    return True

def main():
    """主测试函数"""
    print("Camera类功能测试（H.264解码 + 动态分辨率支持）")
    print("=" * 60)
    
    tests = [
        ("服务端检测", test_server_detection),
        ("等待服务端", test_wait_for_server),
        ("信息缓存刷新", test_server_info_refresh),
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
            results.append((test_name, "✓ 通过" if result else "✗ 失败"))
        except KeyboardInterrupt:
            print(f"\n⚠ 用户中断了 {test_name} 测试")
            results.append((test_name, "⚠ 中断"))
            break
        except Exception as e:
            print(f"✗ {test_name} 测试出错: {e}")
            results.append((test_name, f"✗ 错误: {e}"))
    
    print(f"\n{'=' * 60}")
    print("测试结果汇总:")
    for test_name, result in results:
        print(f"  {test_name}: {result}")
    print("=" * 60)

if __name__ == "__main__":
    main()
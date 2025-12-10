#!/usr/bin/env python3 
"""
Camera类测试脚本（支持H.264解码和动态分辨率）
演示如何使用Camera类连接多摄像头同步传输系统
"""

import cv2
import time
from usbCamera import Camera

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
    # usbCamera.py 的 get_camera_info 不返回 resolution，这里使用 camera 实例的 width/height
    width, height = camera.width, camera.height
    for name in info['cameras'].keys():
        print(f"  {name.upper()}: {width}x{height}")
    
    print("\n开始读取测试...")
    # 获取第一个可用的摄像头名称
    available_cameras = list(info['cameras'].keys())
    if not available_cameras:
        print("✗ 没有可用的摄像头")
        return False
        
    test_cam = available_cameras[0]
    print(f"测试摄像头: {test_cam}")

    success_count = 0
    for i in range(10):
        success, frame = camera.read(test_cam)
        if success:
            success_count += 1
            print(f"✓ 第{i+1}帧读取成功 - 尺寸: {frame.shape}")
        else:
            print(f"✗ 第{i+1}帧读取失败")
        time.sleep(0.1)
    
    # 显示FPS统计
    print(f"\n读取统计:")
    info = camera.get_camera_info()
    for name, cam_info in info['cameras'].items():
        print(f"  {name.upper()}: {cam_info['frames']} 帧, {cam_info['fps']:.1f} fps")
    
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
        width, height = camera.width, camera.height
        for name in info['cameras'].keys():
            print(f"  {name.upper()}: {width}x{height}")
        
        time.sleep(1)
        
        frame_count = 0
        last_frame_counters = {}  # 记录上一次的帧计数
        fps_values = {}  # 存储最新的FPS值用于持续显示
        last_fps_time = time.time()
        last_fps_print_time = time.time()  # 用于终端输出控制
        no_data_count = 0  # 记录连续无数据次数
        
        # 初始化上一次的帧计数
        for name in info['cameras'].keys():
            last_frame_counters[name] = camera.frame_counters.get(name, 0)
            fps_values[name] = 0.0
        
        while True:
            frames = camera.read_all()
            
            current_time = time.time()
            elapsed = current_time - last_fps_time
            
            display_count = 0
            for name, frame in frames.items():
                if frame is not None:
                    # 添加分辨率和FPS信息到图像上
                    h, w = frame.shape[:2]
                    
                    # 始终显示FPS信息（使用最新值）
                    fps_text = f"{name.upper()} {w}x{h} @ {fps_values[name]:.1f}fps"
                    cv2.putText(frame, fps_text, (10, 30), 
                               cv2.FONT_HERSHEY_SIMPLEX, 0.7, (0, 255, 0), 2)
                    
                    window_name = f"{name.upper()} Camera"
                    cv2.imshow(window_name, frame)
                    display_count += 1
            
            # 每秒计算一次FPS：基于接收线程的实际帧计数
            if elapsed >= 1.0:
                for name in info['cameras'].keys():
                    current_count = camera.frame_counters.get(name, 0)
                    frame_diff = current_count - last_frame_counters[name]
                    fps = frame_diff / elapsed
                    fps_values[name] = fps
                    last_frame_counters[name] = current_count
                last_fps_time = current_time
            
            # 每5秒在终端输出一次FPS统计
            if current_time - last_fps_print_time >= 5.0:
                if fps_values:
                    print("\n[FPS Statistics]")
                    for name, fps in fps_values.items():
                        print(f"  {name.upper()}: {fps:.1f} fps")
                last_fps_print_time = time.time()
            
            if display_count == 0:
                no_data_count += 1
                # 只在刚开始无数据或每30次无数据时输出一次
                if no_data_count <= 3 or no_data_count % 30 == 0:
                    print(f"⏳ 等待图像数据... ({no_data_count})")
            else:
                no_data_count = 0  # 重置计数
            
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
    width, height = camera.width, camera.height
    for name in info['cameras'].keys():
        print(f"  {name.upper()}: {width}x{height}")
    
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
    width, height = camera.width, camera.height
    for name, cam_info in info['cameras'].items():
        print(f"  {name.upper()}: {cam_info['frames']} frames ({cam_info['fps']:.1f} fps) @ {width}x{height}")
    
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
            width, height = camera.width, camera.height
            for name, cam_info in info['cameras'].items():
                status = "✓" if cam_info['has_data'] else "✗"
                print(f"   {status} {name.upper()}: {cam_info['fps']:.1f} fps @ {width}x{height}")
        
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
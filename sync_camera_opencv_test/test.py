#!/usr/bin/env python3 
"""
Camera类测试脚本
演示如何使用Camera类连接多摄像头同步传输系统
包含新增的服务端摄像头信息检测功能
"""

import cv2
import time
from entity_c import Camera

def test_server_detection():
    """服务端检测测试"""
    print("=== 服务端摄像头检测测试 ===")
    
    # 创建Camera实例但不打开连接
    camera = Camera()
    
    # 检测服务端摄像头配置
    print("检测服务端摄像头配置...")
    server_info = camera.get_server_camera_info()
    
    print("   服务端状态: {}".format(server_info['status'].upper()))
    print("   服务端运行: {}".format('' if server_info['server_running'] else ''))
    print("   摄像头模式: {}".format(server_info['mode'].upper()))
    print("   摄像头数量: {}".format(server_info['camera_count']))
    print("   检测时间: {}".format(time.strftime('%H:%M:%S', time.localtime(server_info['detection_time']))))
    
    print("\n 摄像头详情:")
    for camera_info in server_info['detected_cameras']:
        status = " 在线" if camera_info['connected'] else " 离线"
        print("   {:>6}: 端口 {} - {}".format(camera_info['name'].upper(), camera_info['port'], status))
        if 'error' in camera_info:
            print("          错误: {}".format(camera_info['error']))
    
    # 获取摄像头映射
    print("\n 摄像头映射:")
    mapping = camera.get_camera_mapping()
    for name, description in mapping.items():
        print("   {}: {}".format(name, description))
    
    # 检查服务端是否在线
    online = camera.is_server_online()
    print("\n 服务端在线状态: {}".format('在线' if online else '离线'))
    
    return server_info['server_running']

def test_wait_for_server():
    """等待服务端上线测试"""
    print("\n=== 等待服务端测试 ===")
    
    camera = Camera()
    
    if camera.is_server_online():
        print("服务端已在线")
        return True
    
    print(" 服务端离线，等待上线...")
    print("   请启动服务端程序: ./sync_camera")
    
    # 等待服务端上线（10秒超时）
    success = camera.wait_for_server(timeout=10, check_interval=2)
    
    if success:
        print("服务端成功上线!")
        # 显示最新的服务端信息
        server_info = camera.get_server_camera_info()
        print("   模式: {}".format(server_info['mode'].upper()))
        print("   摄像头: {}个".format(server_info['camera_count']))
    else:
        print("等待服务端超时")
    
    return success

def test_basic_usage():
    """基础使用测试"""
    print("\n=== 基础使用测试 ===")
    
    camera = Camera()
    
    if not camera.is_server_online():
        print("服务端未运行，跳过测试")
        return False
    
    if not camera.open():
        print("无法打开摄像头")
        return False
    
    info = camera.get_camera_info()
    print(" 摄像头模式: {}".format(info['mode'].upper()))
    print(" 摄像头数量: {}".format(info['camera_count']))
    print(" 端口: {}".format(info['ports']))
    
    print("\n开始读取测试...")
    for i in range(10):
        success, frame = camera.read("left")
        if success:
            print("✓ 第{}帧读取成功 - 尺寸: {}".format(i+1, frame.shape))
        else:
            print("第{}帧读取失败".format(i+1))
        time.sleep(0.1)
    
    camera.close()
    return True

def test_multi_camera_display():
    """多摄像头显示测试"""
    print("\n=== 多摄像头显示测试 ===")
    print("按 'q' 键退出显示")
    
    camera = Camera()
    
    if not camera.is_server_online():
        print("服务端未运行，跳过测试")
        return False
    
    with camera:
        if not camera.is_opened_camera():
            print("摄像头打开失败")
            return False
        
        info = camera.get_camera_info()
        print(" {}模式，{}个摄像头".format(info['mode'].upper(), info['camera_count']))
        
        time.sleep(2)
        
        while True:
            frames = camera.read_all()
            
            display_count = 0
            for name, frame in frames.items():
                if frame is not None:
                    window_name = "{} Camera".format(name.upper())
                    cv2.imshow(window_name, frame)
                    display_count += 1
            
            if display_count == 0:
                print(" 暂无图像数据")
            
            key = cv2.waitKey(1) & 0xFF
            if key == ord('q'):
                break
        
        cv2.destroyAllWindows()
    
    return True

def test_synchronized_capture():
    """同步捕获测试"""
    print("\n=== 同步捕获测试 ===")
    
    camera = Camera()
    
    if not camera.is_server_online():
        print("服务端未运行，跳过测试")
        return False
    
    if not camera.open():
        return False
    
    print("测试同步读取...")
    sync_success_count = 0
    
    for i in range(50):
        success, frames = camera.read_sync()
        if success:
            sync_success_count += 1
            valid_cameras = [name for name, frame in frames.items() if frame is not None]
            print("✓ 同步帧 {}: {}".format(i+1, valid_cameras))
        else:
            print("同步失败 {}".format(i+1))
        
        time.sleep(0.05)
    
    print("\n同步成功率: {}/50 ({}%)".format(sync_success_count, sync_success_count * 2))
    
    info = camera.get_camera_info()
    print("\n最终统计:")
    for name, cam_info in info['cameras'].items():
        print("   {}: {} frames ({:.1f} fps)".format(name.upper(), cam_info['frames'], cam_info['fps']))
    
    camera.close()
    return True

def test_performance_monitoring():
    """性能监控测试"""
    print("\n=== 性能监控测试 ===")
    
    camera = Camera()
    
    if not camera.is_server_online():
        print("服务端未运行，跳过测试")
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
            print("\n 运行时间: {:.1f}s".format(info['runtime']))
            print(" 总帧数: {}".format(info['total_frames']))
            for name, cam_info in info['cameras'].items():
                status = "" if cam_info['has_data'] else "🔴"
                print("   {} {}: {:.1f} fps".format(status, name.upper(), cam_info['fps']))
        
        time.sleep(1)
    
    camera.close()
    return True

def test_server_info_refresh():
    """服务端信息刷新测试"""
    print("\n=== 服务端信息缓存和刷新测试 ===")
    
    camera = Camera()
    
    print("第一次检测...")
    start_time = time.time()
    info1 = camera.get_server_camera_info()
    detect_time1 = time.time() - start_time
    print("   检测耗时: {:.3f}s".format(detect_time1))
    print("   状态: {}, 模式: {}".format(info1['status'], info1['mode']))
    
    print("\n第二次检测（使用缓存）...")
    start_time = time.time()
    info2 = camera.get_server_camera_info()
    detect_time2 = time.time() - start_time
    print("   检测耗时: {:.3f}s".format(detect_time2))
    print("   状态: {}, 模式: {}".format(info2['status'], info2['mode']))
    
    print("\n强制刷新检测...")
    start_time = time.time()
    info3 = camera.get_server_camera_info(force_refresh=True)
    detect_time3 = time.time() - start_time
    print("   检测耗时: {:.3f}s".format(detect_time3))
    print("   状态: {}, 模式: {}".format(info3['status'], info3['mode']))
    
    print("\n性能对比:")
    print("   首次检测: {:.3f}s".format(detect_time1))
    print("   缓存读取: {:.3f}s (加速 {:.1f}x)".format(detect_time2, detect_time1/detect_time2 if detect_time2 > 0 else 0))
    print("   强制刷新: {:.3f}s".format(detect_time3))
    
    return True

def main():
    """主测试函数"""
    print("Camera类功能测试（含服务端检测）")
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
        print("\n{} {} {}".format('='*20, test_name, '='*20))
        try:
            result = test_func()
            results.append((test_name, "通过" if result else "失败"))
        except KeyboardInterrupt:
            print("\n用户中断了 {} 测试".format(test_name))
            results.append((test_name, "中断"))
            break
        except Exception as e:
            print("{} 测试出错: {}".format(test_name, e))
            results.append((test_name, "错误: {}".format(e)))
    
    print("\n{}".format('=' * 60))
    print("测试结果汇总:")
    for test_name, result in results:
        print("   {}: {}".format(test_name, result))
    print("=" * 60)

if __name__ == "__main__":
    main()

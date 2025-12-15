@echo off
chcp 65001 >nul
echo ========================================
echo 🎬 同时启动两个 ffplay 播放器
echo ========================================
echo.
echo 端口 5010 和 5011 将同时连接
echo 这样可以避免连接时间差导致的延迟
echo.
echo 使用优化参数:
echo   -fflags nobuffer      (无缓冲)
echo   -flags low_delay      (低延迟)
echo   -probesize 32         (最小探测)
echo   -analyzeduration 0    (不分析)
echo   -framedrop            (允许丢帧追赶)
echo.
echo 正在启动...
timeout /t 2

start "Camera-5010" ffplay -i tcp://127.0.0.1:5010 -fflags nobuffer -flags low_delay -probesize 32 -analyzeduration 0 -framedrop -window_title "Camera-5010"
start "Camera-5011" ffplay -i tcp://127.0.0.1:5011 -fflags nobuffer -flags low_delay -probesize 32 -analyzeduration 0 -framedrop -window_title "Camera-5011"

echo.
echo ========================================
echo ✅ 已启动两个播放器
echo ========================================
echo.
echo 测试方法:
echo   在两个摄像头前同时挥手
echo   观察画面延迟应该 ^< 500ms
echo.
echo 如果仍有延迟:
echo   1. 关闭 ffplay
echo   2. 重启 sync_camera.exe
echo   3. 立即运行此脚本 (5秒内)
echo.
echo ========================================

@echo off
chcp 65001 >nul
echo ========================================
echo 超低延迟双摄像头测试
echo ========================================
echo.
echo 启动两个 ffplay 播放器（超低延迟配置）
echo - 左侧: 5010 端口 (前置摄像头)
echo - 右侧: 5011 端口 (后置摄像头)
echo.
echo 按 Ctrl+C 停止所有播放器
echo ========================================
echo.

REM 超低延迟ffplay参数说明：
REM -fflags nobuffer       - 禁用所有缓冲
REM -flags low_delay       - 低延迟模式
REM -probesize 32          - 最小探测大小
REM -analyzeduration 0     - 不分析流
REM -sync ext              - 外部同步
REM -framedrop             - 允许丢帧
REM -infbuf                - 无限缓冲区（避免卡顿但保持低延迟）
REM -fast                  - 快速解码
REM -avioflags direct      - 直接IO

start "Camera-5010" ffplay -fflags nobuffer -flags low_delay -probesize 32 -analyzeduration 0 -sync ext -framedrop -infbuf -fast -avioflags direct -i tcp://127.0.0.1:5010 -x 640 -y 480 -left 50 -top 50

timeout /t 1 /nobreak >nul

start "Camera-5011" ffplay -fflags nobuffer -flags low_delay -probesize 32 -analyzeduration 0 -sync ext -framedrop -infbuf -fast -avioflags direct -i tcp://127.0.0.1:5011 -x 640 -y 480 -left 750 -top 50

echo.
echo 两个播放器已启动
echo 关闭此窗口不会停止播放器，需要手动关闭播放器窗口
pause

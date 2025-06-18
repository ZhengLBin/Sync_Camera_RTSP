@echo off
chcp 65001 > nul
cls

echo ===============================================
echo RTSP 双摄像头测试客户端
echo ===============================================
echo.

echo 使用说明：
echo 1. 先启动 sync_camera.exe
echo 2. 等待看到 "Waiting for client connection..." 提示
echo 3. 运行此脚本连接到RTSP流
echo 4. 观察内存使用情况
echo.

set LEFT_URL=rtsp://192.168.16.247:5004/stream
set RIGHT_URL=rtsp://192.168.16.247:5006/stream

echo 测试连接选项：
echo [1] 只连接左摄像头 (端口 5004)
echo [2] 只连接右摄像头 (端口 5006)  
echo [3] 同时连接双摄像头
echo [4] 使用VLC播放器连接 (需要安装VLC)
echo [5] 测试连接状态
echo [Q] 退出
echo.

:MENU
set /p choice="请选择 [1-5,Q]: "

if /i "%choice%"=="1" goto LEFT_ONLY
if /i "%choice%"=="2" goto RIGHT_ONLY
if /i "%choice%"=="3" goto BOTH_CAMERAS
if /i "%choice%"=="4" goto VLC_PLAYER
if /i "%choice%"=="5" goto TEST_CONNECTION
if /i "%choice%"=="Q" goto EXIT
goto MENU

:LEFT_ONLY
echo.
echo 连接左摄像头流...
echo 使用 Ctrl+C 停止播放
echo.
ffplay -rtsp_transport tcp -fflags nobuffer -framedrop -an %LEFT_URL%
goto MENU

:RIGHT_ONLY
echo.
echo 连接右摄像头流...
echo 使用 Ctrl+C 停止播放
echo.
ffplay -rtsp_transport tcp -fflags nobuffer -framedrop -an %RIGHT_URL%
goto MENU

:BOTH_CAMERAS
echo.
echo 同时连接双摄像头流...
echo 将打开两个播放窗口
echo 使用 Ctrl+C 停止播放
echo.
start "Left Camera" ffplay -rtsp_transport tcp -fflags nobuffer -framedrop -an -x 320 -y 240 %LEFT_URL%
start "Right Camera" ffplay -rtsp_transport tcp -fflags nobuffer -framedrop -an -x 320 -y 240 %RIGHT_URL%
echo 已启动双摄像头播放器
goto MENU

:VLC_PLAYER
echo.
echo 启动VLC播放器...
echo 左摄像头: %LEFT_URL%
echo 右摄像头: %RIGHT_URL%
echo.
start "VLC Left" vlc %LEFT_URL% --intf dummy
start "VLC Right" vlc %RIGHT_URL% --intf dummy
echo 已启动VLC播放器
goto MENU

:TEST_CONNECTION
echo.
echo 测试RTSP连接状态...
echo.

echo 测试左摄像头连接:
curl -I --connect-timeout 3 %LEFT_URL% 2>nul
if %errorlevel%==0 (
    echo ✓ 左摄像头服务可访问
) else (
    echo ✗ 左摄像头服务无法访问
)

echo.
echo 测试右摄像头连接:
curl -I --connect-timeout 3 %RIGHT_URL% 2>nul
if %errorlevel%==0 (
    echo ✓ 右摄像头服务可访问
) else (
    echo ✗ 右摄像头服务无法访问
)

echo.
echo 详细连接测试:
ffprobe -rtsp_transport tcp -i %LEFT_URL% 2>&1 | findstr "Stream\|Duration\|Video"
echo.
goto MENU

:EXIT
echo.
echo 测试完成，再见！
pause
exit /b 0
@echo off
echo ========================================
echo 单播模式客户端 - 接收视频流
echo ========================================
echo.

echo 请输入服务端的IP地址（例如 192.168.1.100）：
set /p SERVER_IP=

if "%SERVER_IP%"=="" (
    echo [错误] 未输入IP地址
    pause
    exit /b 1
)

echo.
echo [配置] 服务端: %SERVER_IP%
echo [提示] 请确保服务端使用 run_sync_camera_unicast.bat 启动
echo.
pause

start "Camera-5010" ffplay -fflags nobuffer -flags low_delay -probesize 32 -analyzeduration 0 -sync ext -framedrop -f mpegts -i udp://%SERVER_IP%:5010 -x 640 -y 480 -left 50 -top 50

timeout /t 1 /nobreak >nul

start "Camera-5011" ffplay -fflags nobuffer -flags low_delay -probesize 32 -analyzeduration 0 -sync ext -framedrop -f mpegts -i udp://%SERVER_IP%:5011 -x 640 -y 480 -left 750 -top 50

echo.
echo 两个播放器已启动（单播模式）
pause

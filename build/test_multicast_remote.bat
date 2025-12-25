@echo off
echo ========================================
echo Remote Multicast Client Test
echo ========================================
echo.

REM 设置服务端的组播地址（需与服务端一致）
set MULTICAST_ADDR=239.255.42.99

echo 提示：请确保服务端已启动并使用组播模式
echo 组播地址: %MULTICAST_ADDR%
echo 端口: 5010, 5011
echo.
echo 按任意键开始播放...
pause

start "Camera-5010" ffplay -fflags nobuffer -flags low_delay -probesize 32 -analyzeduration 0 -sync ext -framedrop -f mpegts -i udp://@%MULTICAST_ADDR%:5010 -x 640 -y 480 -left 50 -top 50

timeout /t 1 /nobreak >nul

start "Camera-5011" ffplay -fflags nobuffer -flags low_delay -probesize 32 -analyzeduration 0 -sync ext -framedrop -f mpegts -i udp://@%MULTICAST_ADDR%:5011 -x 640 -y 480 -left 750 -top 50

echo.
echo 两个播放器已启动（组播模式）
echo 如果无画面，请检查：
echo 1. 服务端是否使用组播模式 (STREAM_TARGET_HOST=239.255.42.99)
echo 2. 防火墙是否放行 UDP 5010/5011
echo 3. 两台主机是否在同一局域网
pause

@echo off
echo Starting UDP video streams (Unicast - Local Test)
echo.

REM 本机单播测试（稳定无丢包）
set LOCAL_HOST=127.0.0.1

start "Camera-5010" ffplay -fflags nobuffer -flags low_delay -probesize 32 -analyzeduration 0 -sync ext -framedrop -f mpegts -i udp://%LOCAL_HOST%:5010 -x 640 -y 480 -left 50 -top 50

timeout /t 1 /nobreak >nul

start "Camera-5011" ffplay -fflags nobuffer -flags low_delay -probesize 32 -analyzeduration 0 -sync ext -framedrop -f mpegts -i udp://%LOCAL_HOST%:5011 -x 640 -y 480 -left 750 -top 50

echo.
echo Two players started in Unicast mode (Local)
echo Use this script for local testing (no packet loss)
pause

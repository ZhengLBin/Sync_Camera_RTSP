@echo off
echo Starting UDP video streams (MPEGTS format)...
echo.

start "Camera-5010" ffplay -fflags nobuffer -flags low_delay -probesize 32 -analyzeduration 0 -sync ext -framedrop -f mpegts -i udp://0.0.0.0:5010 -x 640 -y 480 -left 50 -top 50

timeout /t 1 /nobreak >nul

start "Camera-5011" ffplay -fflags nobuffer -flags low_delay -probesize 32 -analyzeduration 0 -sync ext -framedrop -f mpegts -i udp://0.0.0.0:5011 -x 640 -y 480 -left 750 -top 50

echo.
echo Two players started in UDP mode
echo Port 5010 on left, Port 5011 on right
pause

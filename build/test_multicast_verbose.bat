@echo off
echo ========================================
echo 组播接收测试 - 详细模式
echo ========================================
echo.

set MULTICAST_ADDR=239.255.42.99

echo 配置信息：
echo - 组播地址: %MULTICAST_ADDR%
echo - 端口: 5010, 5011
echo.
echo 即将启动 ffplay（显示所有调试信息）...
echo 如果长时间无输出，说明收不到组播包
echo.
pause

REM 添加详细日志参数
ffplay -v verbose -fflags nobuffer -flags low_delay -probesize 32 -analyzeduration 0 -sync ext -framedrop -f mpegts -i udp://@%MULTICAST_ADDR%:5010?overrun_nonfatal=1

pause

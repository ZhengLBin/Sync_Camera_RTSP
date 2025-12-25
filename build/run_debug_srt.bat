@echo off
chcp 65001 >nul
pushd %~dp0

echo ========================================
echo SRT调试模式 - 详细日志
echo ========================================
echo.

REM 组播配置
set STREAM_TARGET_HOST=239.255.42.99
set STREAM_TARGET_TTL=10
set STREAM_MULTICAST_IFACE=192.168.16.50

echo [配置] 组播地址: %STREAM_TARGET_HOST%
echo [配置] TTL: %STREAM_TARGET_TTL%
echo [配置] 发送网卡: %STREAM_MULTICAST_IFACE%
echo.

set "GST_PLUGIN_PATH=%cd%\gstreamer-1.0"
set "GST_PLUGIN_SYSTEM_PATH=%cd%\gstreamer-1.0"

REM 启用SRT详细调试日志
set "GST_DEBUG=3,srt:5,srtsink:6"
set "GST_DEBUG_NO_COLOR=1"

set "PATH=%cd%;%PATH%"

echo [调试] GST_DEBUG=%GST_DEBUG%
echo.
echo 日志说明:
echo - 查找 "srtsink" 相关的ERROR/WARN信息
echo - 查找端口绑定信息（bind, listen, socket）
echo - 查找SRT连接状态
echo.

sync_camera.exe 2>&1 | findstr /I "srtsink srt bind listen socket error warn port 5010 5011"

pause
popd

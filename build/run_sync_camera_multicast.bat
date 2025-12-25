@echo off
chcp 65001 >nul
pushd %~dp0

echo ========================================
echo 双摄像头同步系统 - 组播模式
echo ========================================
echo 适用场景：多个客户端同时观看（需路由器支持IGMP）
echo.

REM 组播配置
set STREAM_TARGET_HOST=239.255.42.99
set STREAM_TARGET_TTL=10

REM 指定发送网卡（多网卡环境必需）
REM 使用 ipconfig 查看网卡IP，设置为与客户端同一网段的IP
set STREAM_MULTICAST_IFACE=192.168.16.50

echo [配置] 组播地址: %STREAM_TARGET_HOST%
echo [配置] TTL: %STREAM_TARGET_TTL%
echo [配置] 发送网卡: %STREAM_MULTICAST_IFACE%
echo.

set "GST_PLUGIN_PATH=%cd%\gstreamer-1.0"
set "GST_PLUGIN_SYSTEM_PATH=%cd%\gstreamer-1.0"
set "GST_DEBUG=2"
set "PATH=%cd%;%PATH%"

sync_camera.exe
pause
popd

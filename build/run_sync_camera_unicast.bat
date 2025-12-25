@echo off
chcp 65001 >nul
pushd %~dp0

echo ========================================
echo 双摄像头同步系统 - 单播指定客户端
echo ========================================
echo.

echo 请输入客户端的IP地址（例如 192.168.1.50）：
set /p CLIENT_IP=

if "%CLIENT_IP%"=="" (
    echo [错误] 未输入IP地址
    pause
    exit /b 1
)

echo.
echo [配置] 目标客户端: %CLIENT_IP%
set STREAM_TARGET_HOST=%CLIENT_IP%

set "GST_PLUGIN_PATH=%cd%\gstreamer-1.0"
set "GST_PLUGIN_SYSTEM_PATH=%cd%\gstreamer-1.0"
set "GST_DEBUG=2"
set "PATH=%cd%;%PATH%"

echo.
echo [启动] 发送流到 %CLIENT_IP%:5010 和 %CLIENT_IP%:5011
echo ========================================
echo.

sync_camera.exe

pause
popd

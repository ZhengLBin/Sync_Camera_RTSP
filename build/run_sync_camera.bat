@echo off
chcp 65001 >nul
pushd %~dp0

echo ========================================
echo Sync Camera Launcher (Portable Mode)
echo ========================================
echo 当前目录：%cd%
echo.

REM 设置 GStreamer 插件路径（本地优先）
set "GST_PLUGIN_PATH=%cd%\gstreamer-1.0"
set "GST_PLUGIN_SYSTEM_PATH=%cd%\gstreamer-1.0"
set "GST_DEBUG=2"

REM 将当前目录添加到 PATH（确保所有 DLL 可被找到）
set "PATH=%cd%;%PATH%"

echo [检查] 关键文件...
if not exist sync_camera.exe (
    echo [错误] 未找到 sync_camera.exe
    pause
    exit /b 1
)

if not exist x264-164.dll (
    echo [警告] 未找到 x264-164.dll，可能导致 x264enc 插件无法加载
)

if not exist gstreamer-1.0\gstx264.dll (
    echo [警告] 未找到 gstreamer-1.0\gstx264.dll
)

echo [启动] sync_camera.exe
echo ========================================
echo.
sync_camera.exe

echo.
echo ========================================
echo 程序已退出
echo ========================================
pause
popd

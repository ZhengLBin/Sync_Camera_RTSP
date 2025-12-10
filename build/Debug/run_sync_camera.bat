@echo off
pushd %~dp0

set "GST_PLUGIN_PATH=%cd%\gstreamer-1.0"
set "PATH=%PATH%;%cd%"

echo 当前目录：%cd%
echo 尝试启动 sync_camera.exe...
sync_camera.exe
pause
popd

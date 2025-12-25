@echo off
chcp 65001 >nul
echo ========================================
echo GStreamer SRT插件测试
echo ========================================
echo.

set "GST_PLUGIN_PATH=%cd%\gstreamer-1.0"
set "PATH=%cd%;%PATH%"

echo [测试1] 检查SRT插件是否存在...
gst-inspect-1.0 srt
echo.

echo [测试2] 检查srtsink元素...
gst-inspect-1.0 srtsink
echo.

echo [测试3] 尝试创建简单的SRT listener测试管线...
echo 管线: videotestsrc ! srtsink uri=srt://0.0.0.0:9999 mode=listener
echo.
timeout /t 3 /nobreak
echo.

pause

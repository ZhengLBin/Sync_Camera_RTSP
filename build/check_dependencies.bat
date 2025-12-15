@echo off
chcp 65001 >nul
echo ========================================
echo GStreamer 依赖检查工具
echo ========================================
echo.

echo [1] 检查关键 DLL 文件...
echo.

set "MISSING=0"

if exist "sync_camera.exe" (
    echo [✓] sync_camera.exe
) else (
    echo [✗] sync_camera.exe - 缺失！
    set /a MISSING+=1
)

if exist "x264-164.dll" (
    echo [✓] x264-164.dll
) else (
    echo [✗] x264-164.dll - 缺失！（x264enc 需要）
    set /a MISSING+=1
)

if exist "gstreamer-1.0-0.dll" (
    echo [✓] gstreamer-1.0-0.dll
) else (
    echo [✗] gstreamer-1.0-0.dll - 缺失！
    set /a MISSING+=1
)

if exist "glib-2.0-0.dll" (
    echo [✓] glib-2.0-0.dll
) else (
    echo [✗] glib-2.0-0.dll - 缺失！
    set /a MISSING+=1
)

echo.
echo [2] 检查 GStreamer 插件...
echo.

if exist "gstreamer-1.0\gstx264.dll" (
    echo [✓] gstreamer-1.0\gstx264.dll
) else (
    echo [✗] gstreamer-1.0\gstx264.dll - 缺失！
    set /a MISSING+=1
)

if exist "gstreamer-1.0\gstvideoparsersbad.dll" (
    echo [✓] gstreamer-1.0\gstvideoparsersbad.dll (h264parse)
) else (
    echo [✗] gstreamer-1.0\gstvideoparsersbad.dll - 缺失！
    set /a MISSING+=1
)

if exist "gstreamer-1.0\gstcoreelements.dll" (
    echo [✓] gstreamer-1.0\gstcoreelements.dll
) else (
    echo [✗] gstreamer-1.0\gstcoreelements.dll - 缺失！
    set /a MISSING+=1
)

echo.
echo [3] 统计结果...
echo.

if %MISSING%==0 (
    echo [成功] 所有关键依赖都已就位！
    echo.
    echo 如果在其他电脑上仍然报错，请确保：
    echo 1. 将整个 build 文件夹复制到目标电脑
    echo 2. 运行 run_sync_camera.bat 启动程序
    echo 3. 确保目标电脑安装了 Visual C++ Redistributable
) else (
    echo [警告] 缺失 %MISSING% 个关键文件！
    echo.
    echo 请运行以下命令重新构建：
    echo     cmake --build . --config Debug
)

echo.
echo ========================================
pause

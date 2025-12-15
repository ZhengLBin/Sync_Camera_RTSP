@echo off
chcp 65001 >nul
echo ========================================
echo GStreamer 完整依赖复制工具
echo ========================================
echo.
echo 此脚本将复制 GStreamer 的所有 DLL 和插件到 build 目录
echo 这是最保险的方法，确保所有依赖都被包含
echo.
echo 按任意键开始复制...
pause >nul
echo.

set "GSTREAMER_ROOT=C:\Program Files\gstreamer\1.0\msvc_x86_64"
set "BUILD_DIR=%~dp0"

echo [1/3] 复制 GStreamer bin 目录下的所有 DLL...
echo.
xcopy "%GSTREAMER_ROOT%\bin\*.dll" "%BUILD_DIR%" /Y /Q /I
echo 完成！
echo.

echo [2/3] 创建 gstreamer-1.0 插件目录...
if not exist "%BUILD_DIR%gstreamer-1.0\" mkdir "%BUILD_DIR%gstreamer-1.0\"
echo 完成！
echo.

echo [3/3] 复制所有 GStreamer 插件...
echo.
xcopy "%GSTREAMER_ROOT%\lib\gstreamer-1.0\*.dll" "%BUILD_DIR%gstreamer-1.0\" /Y /Q /I
echo 完成！
echo.

echo ========================================
echo 复制完成！
echo ========================================
echo.
echo 统计信息：
dir /b "%BUILD_DIR%*.dll" 2>nul | find /c /v "" > temp_count.txt
set /p DLL_COUNT=<temp_count.txt
del temp_count.txt
echo - 根目录 DLL 数量: %DLL_COUNT%

dir /b "%BUILD_DIR%gstreamer-1.0\*.dll" 2>nul | find /c /v "" > temp_count.txt
set /p PLUGIN_COUNT=<temp_count.txt
del temp_count.txt
echo - 插件数量: %PLUGIN_COUNT%
echo.

echo 现在可以将整个 build 文件夹复制到其他电脑测试了！
echo.
echo ========================================
pause

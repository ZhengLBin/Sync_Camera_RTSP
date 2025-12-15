@echo off
chcp 65001 >nul
cls
echo ========================================
echo 🔧 快速重新编译和测试
echo ========================================
echo.

cd D:\_DesktopMoveHere\Leji_proj\sync_camera

echo [1/3] 编译项目...
cmake --build build --config Debug 2>&1 | findstr /C:"error" /C:"warning" /C:"成功" /C:"失败"

if %ERRORLEVEL% NEQ 0 (
    echo.
    echo ❌ 编译失败！查看完整错误:
    cmake --build build --config Debug
    pause
    exit /b 1
)

echo ✅ 编译成功！
echo.

echo [2/3] 检查可执行文件...
if exist "build\Debug\sync_camera.exe" (
    echo ✅ build\Debug\sync_camera.exe 存在
) else if exist "build\sync_camera.exe" (
    echo ✅ build\sync_camera.exe 存在
) else (
    echo ❌ 找不到可执行文件
    pause
    exit /b 1
)
echo.

echo [3/3] 准备测试...
echo.
echo ========================================
echo 测试步骤:
echo ========================================
echo.
echo 1️⃣  在此窗口运行:
echo     cd build
echo     run_sync_camera.bat
echo.
echo 2️⃣  在新窗口运行监控:
echo     cd sync_camera_opencv_test
echo     python real_latency_monitor.py
echo.
echo 3️⃣  观察两个摄像头的帧数差异
echo     • 如果差异 ^< 10 帧 → ✅ 正常
echo     • 如果差异 ^> 60 帧 → ❌ 有 2 秒延迟
echo.
echo ========================================
echo.
echo 🔍 关键修复内容:
echo   ✅ 使用统一的 PTS 时间戳（不再使用独立 frame_count）
echo   ✅ 添加了调试日志（每60帧输出PTS和队列信息）
echo   ✅ 并行发送帧
echo   ✅ 优化编码器参数
echo.
echo ========================================
pause

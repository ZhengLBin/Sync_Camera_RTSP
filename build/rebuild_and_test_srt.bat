@echo off
chcp 65001 >nul
echo ========================================
echo 重新编译并运行（含SRT优化）
echo ========================================
echo.

echo [步骤1] 进入build目录...
cd /d "%~dp0"
echo.

echo [步骤2] 编译项目...
cmake --build . --config Debug
if %ERRORLEVEL% NEQ 0 (
    echo [错误] 编译失败
    pause
    exit /b 1
)
echo.

echo [步骤3] 设置防火墙规则（需要管理员权限）...
echo 请在新窗口中以管理员身份运行 setup_firewall_srt.bat
echo 或者手动执行：
echo   右键 setup_firewall_srt.bat -^> 以管理员身份运行
echo.
pause
echo.

echo [步骤4] 运行SRT诊断...
call diagnose_srt_connection.bat
echo.

echo [步骤5] 启动服务端（使用组播配置）...
echo 服务端将在 SRT Listener 模式下监听:
echo   - 前置摄像头: srt://192.168.16.50:5010
echo   - 后置摄像头: srt://192.168.16.50:5011
echo.
echo 客户端连接命令（Linux）:
echo   ffplay -fflags nobuffer -flags low_delay -framedrop "srt://192.168.16.50:5010?mode=caller&latency=200"
echo   ffplay -fflags nobuffer -flags low_delay -framedrop "srt://192.168.16.50:5011?mode=caller&latency=200"
echo.
pause

call run_sync_camera_multicast.bat

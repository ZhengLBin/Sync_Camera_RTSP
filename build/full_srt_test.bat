@echo off
chcp 65001 >nul
pushd %~dp0

echo ========================================
echo 完整SRT测试流程
echo ========================================
echo.

echo [步骤1/5] 编译项目...
cmake --build . --config Debug
if %ERRORLEVEL% NEQ 0 (
    echo [错误] 编译失败
    pause
    exit /b 1
)
echo [完成] 编译成功
echo.

echo [步骤2/5] 启动服务端（后台模式）...
start "SRT Camera Server" /MIN cmd /c run_sync_camera_multicast.bat
echo 等待5秒让服务端初始化...
timeout /t 5 /nobreak >nul
echo.

echo [步骤3/5] 检查端口监听状态...
echo 查找sync_camera.exe进程:
tasklist | findstr sync_camera.exe
echo.
echo 检查UDP端口5010/5011:
netstat -ano | findstr ":5010"
netstat -ano | findstr ":5011"
echo.

echo [步骤4/5] 端口分析...
set PORT_5010_FOUND=0
set PORT_5011_FOUND=0

for /f %%a in ('netstat -ano ^| findstr ":5010"') do set PORT_5010_FOUND=1
for /f %%a in ('netstat -ano ^| findstr ":5011"') do set PORT_5011_FOUND=1

if %PORT_5010_FOUND%==1 (
    echo [✓] 端口5010正在监听
) else (
    echo [✗] 端口5010未监听 - SRT Listener可能未启动
)

if %PORT_5011_FOUND%==1 (
    echo [✓] 端口5011正在监听
) else (
    echo [✗] 端口5011未监听 - SRT Listener可能未启动
)
echo.

echo [步骤5/5] Linux客户端连接命令...
echo.
echo 在Linux主机上执行以下命令测试连接:
echo.
echo   前置摄像头:
echo   ffplay -fflags nobuffer -flags low_delay -framedrop ^
echo     "srt://192.168.16.50:5010?mode=caller&latency=200"
echo.
echo   后置摄像头:
echo   ffplay -fflags nobuffer -flags low_delay -framedrop ^
echo     "srt://192.168.16.50:5011?mode=caller&latency=200"
echo.

echo ========================================
echo 测试准备完成
echo ========================================
echo.
echo 提示: 
echo 1. 如果端口未监听，查看服务端窗口的错误信息
echo 2. 确保防火墙已配置（运行 setup_firewall_srt.bat）
echo 3. 客户端连接失败时，检查服务端日志
echo.
pause
popd

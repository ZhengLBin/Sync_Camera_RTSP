@echo off
chcp 65001 >nul
echo ========================================
echo SRT端口监听状态检查
echo ========================================
echo.

echo [检查1] 查找sync_camera.exe进程...
tasklist | findstr sync_camera.exe
echo.

echo [检查2] 检查端口5010和5011的监听状态...
echo 期望看到: UDP 0.0.0.0:5010 或 UDP [::]:5010
echo.
netstat -ano | findstr ":5010"
netstat -ano | findstr ":5011"
echo.

echo [检查3] 检查所有UDP监听端口（50xx范围）...
netstat -ano -p UDP | findstr "50"
echo.

echo [检查4] 检查GStreamer进程的网络连接...
for /f "tokens=5" %%a in ('netstat -ano ^| findstr ":5010"') do (
    echo.
    echo === 端口5010被进程 %%a 占用 ===
    tasklist /FI "PID eq %%a" /FO TABLE
    echo.
)
for /f "tokens=5" %%a in ('netstat -ano ^| findstr ":5011"') do (
    echo.
    echo === 端口5011被进程 %%a 占用 ===
    tasklist /FI "PID eq %%a" /FO TABLE
    echo.
)

echo ========================================
echo 分析说明：
echo - 如果没有显示任何端口，说明SRT listener未启动
echo - 如果看到TCP而非UDP，说明协议配置错误
echo - 正常应该显示：UDP 0.0.0.0:5010 或 UDP *:5010
echo ========================================
pause

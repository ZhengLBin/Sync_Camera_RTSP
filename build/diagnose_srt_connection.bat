@echo off
chcp 65001 >nul
echo ========================================
echo SRT连接诊断工具
echo ========================================
echo.

echo [检查1] 检查SRT端口监听状态...
echo.
netstat -ano | findstr ":5010"
netstat -ano | findstr ":5011"
echo.

echo [检查2] 检查防火墙规则...
echo.
netsh advfirewall firewall show rule name=all | findstr "5010"
netsh advfirewall firewall show rule name=all | findstr "5011"
echo.

echo [检查3] 检查网络连通性...
echo 目标IP: 192.168.16.50
ping -n 2 192.168.16.50
echo.

echo [检查4] 检查进程占用端口...
echo.
for /f "tokens=5" %%a in ('netstat -ano ^| findstr ":5010"') do (
    echo Port 5010 被进程 %%a 占用
    tasklist /FI "PID eq %%a" /FO TABLE
)
for /f "tokens=5" %%a in ('netstat -ano ^| findstr ":5011"') do (
    echo Port 5011 被进程 %%a 占用
    tasklist /FI "PID eq %%a" /FO TABLE
)
echo.

echo ========================================
echo 诊断完成
echo ========================================
pause

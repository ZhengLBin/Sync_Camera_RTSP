@echo off
echo ========================================
echo 网络诊断工具
echo ========================================
echo.

echo 请输入服务端的IP地址（例如 192.168.1.100）：
set /p SERVER_IP=

echo.
echo [测试1] Ping 服务端...
ping -n 4 %SERVER_IP%

echo.
echo [测试2] 检查本机网络接口...
ipconfig | findstr /C:"IPv4"

echo.
echo [测试3] 检查路由表（组播路由）...
route print | findstr /C:"224.0.0.0"

echo.
echo [测试4] 尝试接收组播流（5秒测试）...
echo 提示：如果看到数据包大小变化，说明能收到组播流
echo.
timeout /t 2 /nobreak >nul

REM 使用 ffprobe 测试能否接收到流
ffprobe -v quiet -print_format json -show_streams -timeout 5000000 udp://@239.255.42.99:5010 2>nul

if %ERRORLEVEL% EQU 0 (
    echo [成功] 能接收到组播流！
) else (
    echo [失败] 无法接收组播流
    echo.
    echo 可能原因：
    echo 1. 服务端未启动或未使用组播模式
    echo 2. 防火墙阻止了 UDP 5010/5011
    echo 3. 路由器/交换机不支持组播或未启用 IGMP
    echo 4. 不在同一子网（组播 TTL=1）
)

echo.
echo ========================================
pause

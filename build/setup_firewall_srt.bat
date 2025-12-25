@echo off
chcp 65001 >nul
echo ========================================
echo SRT端口防火墙配置（管理员权限）
echo ========================================
echo.

REM 检查管理员权限
net session >nul 2>&1
if %ERRORLEVEL% NEQ 0 (
    echo [错误] 需要管理员权限
    echo 请右键点击此脚本，选择"以管理员身份运行"
    pause
    exit /b 1
)

echo [配置] 添加 SRT (UDP) 5010/5011 防火墙规则...
echo.

REM 删除可能存在的旧规则
netsh advfirewall firewall delete rule name="SRT-CameraStream-5010-IN" >nul 2>&1
netsh advfirewall firewall delete rule name="SRT-CameraStream-5011-IN" >nul 2>&1
netsh advfirewall firewall delete rule name="SRT-CameraStream-5010-OUT" >nul 2>&1
netsh advfirewall firewall delete rule name="SRT-CameraStream-5011-OUT" >nul 2>&1
netsh advfirewall firewall delete rule name="SRT-CameraStream-EXE" >nul 2>&1

REM 添加UDP规则（SRT使用UDP协议）
netsh advfirewall firewall add rule name="SRT-CameraStream-5010-IN" dir=in action=allow protocol=UDP localport=5010 profile=any
netsh advfirewall firewall add rule name="SRT-CameraStream-5011-IN" dir=in action=allow protocol=UDP localport=5011 profile=any
netsh advfirewall firewall add rule name="SRT-CameraStream-5010-OUT" dir=out action=allow protocol=UDP localport=5010 profile=any
netsh advfirewall firewall add rule name="SRT-CameraStream-5011-OUT" dir=out action=allow protocol=UDP localport=5011 profile=any

REM 添加程序规则（允许 sync_camera.exe 的所有网络活动）
netsh advfirewall firewall add rule name="SRT-CameraStream-EXE" dir=in action=allow program="%~dp0sync_camera.exe" enable=yes profile=any
netsh advfirewall firewall add rule name="SRT-CameraStream-EXE-OUT" dir=out action=allow program="%~dp0sync_camera.exe" enable=yes profile=any

echo.
echo [完成] 防火墙规则已添加
echo.
echo 已添加的规则：
netsh advfirewall firewall show rule name=all | findstr "SRT-CameraStream"
echo.

echo [提示] 如果仍然无法连接，请检查：
echo 1. 网络连通性（ping 192.168.16.50）
echo 2. 客户端和服务端在同一子网
echo 3. 路由器或交换机没有阻止流量
echo.
pause

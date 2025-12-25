@echo off
echo ========================================
echo 防火墙快速配置（管理员权限）
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

echo [配置] 添加 UDP 5010/5011 防火墙规则...
echo.

REM 删除可能存在的旧规则
netsh advfirewall firewall delete rule name="CameraStream-5010-IN" >nul 2>&1
netsh advfirewall firewall delete rule name="CameraStream-5011-IN" >nul 2>&1
netsh advfirewall firewall delete rule name="CameraStream-5010-OUT" >nul 2>&1
netsh advfirewall firewall delete rule name="CameraStream-5011-OUT" >nul 2>&1

REM 添加新规则（入站+出站，所有配置文件）
netsh advfirewall firewall add rule name="CameraStream-5010-IN" dir=in action=allow protocol=UDP localport=5010 profile=any
netsh advfirewall firewall add rule name="CameraStream-5011-IN" dir=in action=allow protocol=UDP localport=5011 profile=any
netsh advfirewall firewall add rule name="CameraStream-5010-OUT" dir=out action=allow protocol=UDP localport=5010 profile=any
netsh advfirewall firewall add rule name="CameraStream-5011-OUT" dir=out action=allow protocol=UDP localport=5011 profile=any

echo.
echo [完成] 防火墙规则已添加
echo.
echo 已添加的规则：
netsh advfirewall firewall show rule name=all | findstr "CameraStream"
echo.
pause

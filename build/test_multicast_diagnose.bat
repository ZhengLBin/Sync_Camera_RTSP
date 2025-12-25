@echo off
echo ========================================
echo 组播网络测试 - 步骤诊断
echo ========================================
echo.

set MULTICAST_ADDR=239.255.42.99
set TEST_PORT=5010

echo [步骤1] 检查本机网络接口
echo ========================================
ipconfig | findstr /C:"IPv4" /C:"以太网适配器"
echo.

echo [步骤2] 检查防火墙状态
echo ========================================
netsh advfirewall show allprofiles state
echo.

echo [步骤3] 测试能否监听 UDP 端口
echo ========================================
echo 正在尝试绑定 UDP %TEST_PORT%...
netstat -an | findstr ":%TEST_PORT%"
if %ERRORLEVEL% EQU 0 (
    echo [警告] 端口 %TEST_PORT% 已被占用
) else (
    echo [正常] 端口 %TEST_PORT% 可用
)
echo.

echo [步骤4] 检查组播路由表
echo ========================================
route print | findstr "224.0.0.0"
if %ERRORLEVEL% EQU 0 (
    echo [正常] 存在组播路由
) else (
    echo [警告] 未找到组播路由，可能不支持组播
)
echo.

echo [步骤5] 使用 ffplay 监听（30秒超时）
echo ========================================
echo 提示：
echo - 如果看到 "Connection timed out"，说明收不到组播包
echo - 如果有数据输出，说明网络正常
echo.
echo 按任意键开始监听 udp://@%MULTICAST_ADDR%:%TEST_PORT%
pause >nul

timeout /t 3 /nobreak >nul

ffplay -v info -timeout 30000000 -f mpegts -i udp://@%MULTICAST_ADDR%:%TEST_PORT%

echo.
echo ========================================
echo 测试结束
echo.
echo 如果上面显示 "Connection timed out" 或无任何输出：
echo 1. 检查服务端是否正在运行
echo 2. 检查路由器是否支持 IGMP（组播协议）
echo 3. 尝试临时完全关闭防火墙测试
echo 4. 检查是否有多个网卡（禁用不用的）
pause

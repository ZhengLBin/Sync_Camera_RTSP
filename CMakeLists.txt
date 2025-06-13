cmake_minimum_required(VERSION 3.8)
if (POLICY CMP0141)
    cmake_policy(SET CMP0141 NEW)
    set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<IF:$<AND:$<C_COMPILER_ID:MSVC>,$<CXX_COMPILER_ID:MSVC>>,$<$<CONFIG:Debug,RelWithDebInfo>:EditAndContinue>,$<$<CONFIG:Debug,RelWithDebInfo>:ProgramDatabase>>")
endif()

project(sync_camera)

# 设置 C++ 标准
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 包含vcpkg的头文件和库目录
# include_directories("F:/vcpkg/installed/x64-windows/include")
# link_directories("F:/vcpkg/installed/x64-windows/lib")

# FFmpeg路径
include_directories("F:/ffmpeg-7.0.2-full_build-shared/include")
link_directories("F:/ffmpeg-7.0.2-full_build-shared/lib")


set(VLD_ROOT "F:/Visual_Leak_Detector")
include_directories(${VLD_ROOT}/include)



# GStreamer路径
set(GSTREAMER_ROOT "C:/Program Files/gstreamer/1.0/msvc_x86_64")
include_directories("${GSTREAMER_ROOT}/include/gstreamer-1.0")
include_directories("${GSTREAMER_ROOT}/include/glib-2.0")
include_directories("${GSTREAMER_ROOT}/lib/glib-2.0/include")
link_directories("${GSTREAMER_ROOT}/lib")

# 可执行文件
add_executable(sync_camera 
    "sync_camera.cpp" 
    "sync_camera.h" 
    "main.cpp" 
    "video_streamer.cpp" 
    "video_streamer.h"
)


if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    # 链接 VLD 库
    target_link_libraries(sync_camera PRIVATE "${VLD_ROOT}/lib/Win64/vld.lib")
    
    # 复制 DLL 到输出目录
    add_custom_command(TARGET sync_camera POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${VLD_ROOT}/bin/Win64/vld_x64.dll"
        $<TARGET_FILE_DIR:sync_camera>
        COMMENT "Copying VLD DLL to output directory"
    )
endif()


# OpenCV配置
set(OpenCV_DIR "F:/opencv/build")
find_package(OpenCV REQUIRED)
target_include_directories(sync_camera PRIVATE ${OpenCV_INCLUDE_DIRS})
target_link_libraries(sync_camera PRIVATE ${OpenCV_LIBS})

# FFmpeg库链接
target_link_libraries(sync_camera PRIVATE 
    F:/ffmpeg-7.0.2-full_build-shared/lib/avcodec.lib
    F:/ffmpeg-7.0.2-full_build-shared/lib/avformat.lib
    F:/ffmpeg-7.0.2-full_build-shared/lib/avutil.lib
    F:/ffmpeg-7.0.2-full_build-shared/lib/swscale.lib
    F:/ffmpeg-7.0.2-full_build-shared/lib/avdevice.lib
)

# GStreamer库链接（增强版本）
target_link_libraries(sync_camera PRIVATE
    gstreamer-1.0.lib
    gstrtspserver-1.0.lib
    gstapp-1.0.lib
    gstvideo-1.0.lib        # 新增：视频处理库
    gstbase-1.0.lib         # 新增：基础库
    gobject-2.0.lib
    glib-2.0.lib
    gmodule-2.0.lib
    gthread-2.0.lib
    gio-2.0.lib
    intl.lib                # 新增：国际化库
)

# SDL2库链接
# target_link_libraries(sync_camera PRIVATE SDL2.lib)

# 系统库（增强版本）
target_link_libraries(sync_camera PRIVATE
    Secur32.lib
    Bcrypt.lib
    Mfuuid.lib
    strmiids.lib
    winmm.lib
    ws2_32.lib
    ole32.lib
    uuid.lib
    user32.lib              # 新增：用户界面库
    kernel32.lib            # 新增：内核库
    advapi32.lib            # 新增：高级API库
)

# 拷贝DLL和插件（改进版本）

# 1) 拷贝 ffmpeg DLL
add_custom_command(TARGET sync_camera POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    "F:/ffmpeg-7.0.2-full_build-shared/bin"
    $<TARGET_FILE_DIR:sync_camera>
    COMMENT "拷贝 FFmpeg DLLs"
)

# 2) 拷贝 vcpkg DLL
# add_custom_command(TARGET sync_camera POST_BUILD
    # COMMAND ${CMAKE_COMMAND} -E copy_directory
    # "F:/vcpkg/installed/x64-windows/bin"
    # $<TARGET_FILE_DIR:sync_camera>
    # COMMENT "拷贝 vcpkg DLLs"
# )

# 3) 拷贝 GStreamer DLL
add_custom_command(TARGET sync_camera POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    "${GSTREAMER_ROOT}/bin"
    $<TARGET_FILE_DIR:sync_camera>
    COMMENT "拷贝 GStreamer DLLs"
)

# 4) 创建并拷贝完整的 GStreamer 插件目录
add_custom_command(TARGET sync_camera POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E remove_directory
    $<TARGET_FILE_DIR:sync_camera>/gstreamer-1.0
    COMMENT "清理旧的插件目录"
)

add_custom_command(TARGET sync_camera POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
    $<TARGET_FILE_DIR:sync_camera>/gstreamer-1.0
    COMMENT "创建 GStreamer 插件目录"
)

add_custom_command(TARGET sync_camera POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    "${GSTREAMER_ROOT}/lib/gstreamer-1.0"
    $<TARGET_FILE_DIR:sync_camera>/gstreamer-1.0
    COMMENT "拷贝 GStreamer 插件"
)


# 5) 创建增强的启动脚本
file(WRITE ${CMAKE_BINARY_DIR}/run_sync_camera.bat
"@echo off
set GST_PLUGIN_PATH=%~dp0gstreamer-1.0
set GST_PLUGIN_SYSTEM_PATH=%~dp0gstreamer-1.0
set GST_DEBUG=1
echo Starting sync_camera with GStreamer plugins...
echo Plugin path: %GST_PLUGIN_PATH%
%~dp0sync_camera.exe
pause
")

# 6) 创建调试启动脚本
file(WRITE ${CMAKE_BINARY_DIR}/run_sync_camera_debug.bat
"@echo off
set GST_PLUGIN_PATH=%~dp0gstreamer-1.0
set GST_PLUGIN_SYSTEM_PATH=%~dp0gstreamer-1.0
set GST_DEBUG=3
set GST_DEBUG_DUMP_DOT_DIR=%~dp0
echo Starting sync_camera with DEBUG mode...
echo Plugin path: %GST_PLUGIN_PATH%
echo Debug level: %GST_DEBUG%
echo Dot files will be saved to: %GST_DEBUG_DUMP_DOT_DIR%
%~dp0sync_camera.exe
pause
")

# 7) 创建 GStreamer 诊断脚本
file(WRITE ${CMAKE_BINARY_DIR}/gstreamer_diagnostics.bat
"@echo off
chcp 65001 > nul
cls

echo ============================================
echo GStreamer 插件诊断脚本
echo ============================================

set \"GSTREAMER_ROOT=C:\\Program Files\\gstreamer\\1.0\\msvc_x86_64\"
set \"PROJECT_DIR=%~dp0\"
set \"PLUGIN_DIR=%PROJECT_DIR%gstreamer-1.0\"

echo.
echo 1. 检查 GStreamer 安装路径
if exist \"%GSTREAMER_ROOT%\" (
    echo ✓ GStreamer 根目录存在: %GSTREAMER_ROOT%
) else (
    echo ✗ GStreamer 根目录不存在: %GSTREAMER_ROOT%
    pause
    exit /b 1
)

echo.
echo 2. 检查关键插件文件
for %%f in (
    \"gstcoreelements.dll\"
    \"gstvideoconvert.dll\" 
    \"gstvideoconvertscale.dll\"
    \"gstx264.dll\"
    \"gstapp.dll\"
    \"gstrtp.dll\"
    \"gstrtpmanager.dll\"
) do (
    if exist \"%PLUGIN_DIR%\\%%f\" (
        echo ✓ %%f
    ) else (
        echo ✗ %%f ^(缺失^)
    )
)

echo.
echo 3. 测试 GStreamer 基本功能
set GST_PLUGIN_PATH=%PLUGIN_DIR%
\"%GSTREAMER_ROOT%\\bin\\gst-inspect-1.0.exe\" --version 2>nul
if %errorlevel%==0 (
    echo ✓ gst-inspect-1.0 可用
) else (
    echo ✗ gst-inspect-1.0 不可用
)

echo.
echo ============================================
echo 诊断完成
echo ============================================
pause
")

# 拷贝所有脚本到输出目录
add_custom_command(TARGET sync_camera POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
    ${CMAKE_BINARY_DIR}/run_sync_camera.bat
    $<TARGET_FILE_DIR:sync_camera>/run_sync_camera.bat
    COMMENT "拷贝启动脚本"
)

add_custom_command(TARGET sync_camera POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
    ${CMAKE_BINARY_DIR}/run_sync_camera_debug.bat
    $<TARGET_FILE_DIR:sync_camera>/run_sync_camera_debug.bat
    COMMENT "拷贝调试启动脚本"
)

add_custom_command(TARGET sync_camera POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
    ${CMAKE_BINARY_DIR}/gstreamer_diagnostics.bat
    $<TARGET_FILE_DIR:sync_camera>/gstreamer_diagnostics.bat
    COMMENT "拷贝诊断脚本"
)

# 检查并报告配置
if(NOT EXISTS "${GSTREAMER_ROOT}")
    message(WARNING "GStreamer 未找到: ${GSTREAMER_ROOT}")
    message(WARNING "请安装 GStreamer Development Libraries")
    message(WARNING "下载地址: https://gstreamer.freedesktop.org/download/")
else()
    message(STATUS "GStreamer 路径: ${GSTREAMER_ROOT}")
endif()

if(NOT EXISTS "${GSTREAMER_ROOT}/lib/gstreamer-1.0")
    message(WARNING "GStreamer 插件未找到: ${GSTREAMER_ROOT}/lib/gstreamer-1.0")
    message(WARNING "请确保安装了完整的 GStreamer 开发包")
else()
    message(STATUS "GStreamer 插件路径: ${GSTREAMER_ROOT}/lib/gstreamer-1.0")
endif()

# 打印配置信息
message(STATUS "=== 构建配置 ===")
message(STATUS "VideoStreamer: GStreamer RTSP (增强版本)")
message(STATUS "GStreamer 根目录: ${GSTREAMER_ROOT}")
message(STATUS "插件拷贝路径: ./gstreamer-1.0")
message(STATUS "启动脚本: run_sync_camera.bat")
message(STATUS "调试脚本: run_sync_camera_debug.bat")
message(STATUS "诊断脚本: gstreamer_diagnostics.bat")
message(STATUS "==================")

# 构建后提示
add_custom_command(TARGET sync_camera POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E echo "====================================="
    COMMAND ${CMAKE_COMMAND} -E echo "构建完成!"
    COMMAND ${CMAKE_COMMAND} -E echo "可用脚本:"
    COMMAND ${CMAKE_COMMAND} -E echo "  run_sync_camera.bat        - 正常启动"
    COMMAND ${CMAKE_COMMAND} -E echo "  run_sync_camera_debug.bat  - 调试模式"
    COMMAND ${CMAKE_COMMAND} -E echo "  gstreamer_diagnostics.bat  - 插件诊断"
    COMMAND ${CMAKE_COMMAND} -E echo "====================================="
)
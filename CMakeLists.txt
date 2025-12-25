# ==============================================================================
# 1. 基础配置
# ==============================================================================

cmake_minimum_required(VERSION 3.8)

if (POLICY CMP0141)
    cmake_policy(SET CMP0141 NEW)
    if (CMAKE_BUILD_TYPE MATCHES "Debug|RelWithDebInfo")
        set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "ProgramDatabase")
    endif()
endif()

project(sync_camera LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ==============================================================================
# 2. 外部库路径配置
# ==============================================================================

set(FFMPEG_ROOT "F:/ffmpeg-7.0.2-full_build-shared")
set(GSTREAMER_ROOT "C:/Program Files/gstreamer/1.0/msvc_x86_64")

# ==============================================================================
# 3. 包含与链接目录
# ==============================================================================

include_directories(
    "${FFMPEG_ROOT}/include"
    "${GSTREAMER_ROOT}/include/gstreamer-1.0"
    "${GSTREAMER_ROOT}/include/glib-2.0"
    "${GSTREAMER_ROOT}/lib/glib-2.0/include"
)

link_directories(
    "${FFMPEG_ROOT}/lib"
    "${GSTREAMER_ROOT}/lib"
)

# ==============================================================================
# 4. 可执行文件定义
# ==============================================================================

add_executable(sync_camera
    src/main.cpp
    src/multi_camera_sync.cpp
    src/tcp_streamer.cpp
    src/config_manager.cpp
    includes/multi_camera_sync.h
    includes/tcp_streamer.h
    includes/config_manager.h
    src/camera_input_source.cpp
    includes/camera_input_source.h
)

# ==============================================================================
# 5. 链接库
# ==============================================================================

# FFmpeg
target_link_libraries(sync_camera PRIVATE
    "${FFMPEG_ROOT}/lib/avcodec.lib"
    "${FFMPEG_ROOT}/lib/avformat.lib"
    "${FFMPEG_ROOT}/lib/avutil.lib"
    "${FFMPEG_ROOT}/lib/swscale.lib"
    "${FFMPEG_ROOT}/lib/avdevice.lib"
)

# GStreamer + GLib
target_link_libraries(sync_camera PRIVATE
    gstreamer-1.0
    gstapp-1.0
    gstvideo-1.0
    gstrtspserver-1.0
    gobject-2.0
    glib-2.0
)

# Windows 系统库
target_link_libraries(sync_camera PRIVATE
    Secur32
    Bcrypt
    ws2_32
    ole32
    user32
)

# ==============================================================================
# 6. DLL 自动复制（精确模式）
# ==============================================================================

add_custom_command(TARGET sync_camera POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:sync_camera>"
)

# --- FFmpeg DLL ---
message(STATUS "Detecting FFmpeg DLLs...")
file(GLOB FFMPEG_DLLS "${FFMPEG_ROOT}/bin/*.dll")

foreach(_dll IN LISTS FFMPEG_DLLS)
    get_filename_component(_dll_name "${_dll}" NAME)
    add_custom_command(TARGET sync_camera POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_dll}"
        "$<TARGET_FILE_DIR:sync_camera>/${_dll_name}"
        COMMENT "Copying FFmpeg DLL: ${_dll_name}"
    )
endforeach()

# --- GStreamer 所有 DLL（全量复制策略）---
message(STATUS "Copying ALL GStreamer DLLs from bin directory...")

# 获取 GStreamer bin 目录下的所有 DLL
file(GLOB GSTREAMER_ALL_DLLS "${GSTREAMER_ROOT}/bin/*.dll")

foreach(_dll_path IN LISTS GSTREAMER_ALL_DLLS)
    get_filename_component(_dll_name "${_dll_path}" NAME)
    add_custom_command(TARGET sync_camera POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_dll_path}"
        "$<TARGET_FILE_DIR:sync_camera>/${_dll_name}"
        COMMENT "Copying GStreamer DLL: ${_dll_name}"
    )
endforeach()

message(STATUS "Will copy ${CMAKE_MATCH_COUNT} GStreamer DLLs")

# --- GStreamer 插件（全量复制）---
add_custom_command(TARGET sync_camera POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:sync_camera>/gstreamer-1.0"
)

message(STATUS "Copying ALL GStreamer plugins...")
file(GLOB GSTREAMER_ALL_PLUGINS "${GSTREAMER_ROOT}/lib/gstreamer-1.0/*.dll")

foreach(_plugin_path IN LISTS GSTREAMER_ALL_PLUGINS)
    get_filename_component(_plugin_name "${_plugin_path}" NAME)
    add_custom_command(TARGET sync_camera POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${_plugin_path}"
        "$<TARGET_FILE_DIR:sync_camera>/gstreamer-1.0/${_plugin_name}"
        COMMENT "Copying plugin: ${_plugin_name}"
    )
endforeach()

# ======================================
# ✅ 更安全的方式生成 run_sync_camera.bat
# ======================================
set(RUN_SCRIPT_PATH "${CMAKE_CURRENT_BINARY_DIR}/run_sync_camera.bat")

file(WRITE "${RUN_SCRIPT_PATH}" "@echo off\n")
file(APPEND "${RUN_SCRIPT_PATH}" "chcp 65001 >nul\n")
file(APPEND "${RUN_SCRIPT_PATH}" "pushd %~dp0\n\n")
file(APPEND "${RUN_SCRIPT_PATH}" "echo ========================================\n")
file(APPEND "${RUN_SCRIPT_PATH}" "echo Sync Camera Launcher (Portable Mode)\n")
file(APPEND "${RUN_SCRIPT_PATH}" "echo ========================================\n")
file(APPEND "${RUN_SCRIPT_PATH}" "echo 当前目录：%cd%\n")
file(APPEND "${RUN_SCRIPT_PATH}" "echo.\n\n")
file(APPEND "${RUN_SCRIPT_PATH}" "REM 设置 GStreamer 插件路径（本地优先）\n")
file(APPEND "${RUN_SCRIPT_PATH}" "set \"GST_PLUGIN_PATH=%cd%\\gstreamer-1.0\"\n")
file(APPEND "${RUN_SCRIPT_PATH}" "set \"GST_PLUGIN_SYSTEM_PATH=%cd%\\gstreamer-1.0\"\n")
file(APPEND "${RUN_SCRIPT_PATH}" "set \"GST_DEBUG=2\"\n\n")
file(APPEND "${RUN_SCRIPT_PATH}" "REM 将当前目录添加到 PATH（确保所有 DLL 可被找到）\n")
file(APPEND "${RUN_SCRIPT_PATH}" "set \"PATH=%cd%;%PATH%\"\n\n")
file(APPEND "${RUN_SCRIPT_PATH}" "echo [检查] 关键文件...\n")
file(APPEND "${RUN_SCRIPT_PATH}" "if not exist sync_camera.exe (\n")
file(APPEND "${RUN_SCRIPT_PATH}" "    echo [错误] 未找到 sync_camera.exe\n")
file(APPEND "${RUN_SCRIPT_PATH}" "    pause\n")
file(APPEND "${RUN_SCRIPT_PATH}" "    exit /b 1\n")
file(APPEND "${RUN_SCRIPT_PATH}" ")\n\n")
file(APPEND "${RUN_SCRIPT_PATH}" "if not exist x264-164.dll (\n")
file(APPEND "${RUN_SCRIPT_PATH}" "    echo [警告] 未找到 x264-164.dll，可能导致 x264enc 插件无法加载\n")
file(APPEND "${RUN_SCRIPT_PATH}" ")\n\n")
file(APPEND "${RUN_SCRIPT_PATH}" "if not exist gstreamer-1.0\\gstx264.dll (\n")
file(APPEND "${RUN_SCRIPT_PATH}" "    echo [警告] 未找到 gstreamer-1.0\\gstx264.dll\n")
file(APPEND "${RUN_SCRIPT_PATH}" ")\n\n")
file(APPEND "${RUN_SCRIPT_PATH}" "echo [启动] sync_camera.exe\n")
file(APPEND "${RUN_SCRIPT_PATH}" "echo ========================================\n")
file(APPEND "${RUN_SCRIPT_PATH}" "echo.\n")
file(APPEND "${RUN_SCRIPT_PATH}" "sync_camera.exe\n\n")
file(APPEND "${RUN_SCRIPT_PATH}" "echo.\n")
file(APPEND "${RUN_SCRIPT_PATH}" "echo ========================================\n")
file(APPEND "${RUN_SCRIPT_PATH}" "echo 程序已退出\n")
file(APPEND "${RUN_SCRIPT_PATH}" "echo ========================================\n")
file(APPEND "${RUN_SCRIPT_PATH}" "pause\n")
file(APPEND "${RUN_SCRIPT_PATH}" "popd\n")

add_custom_command(TARGET sync_camera POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${RUN_SCRIPT_PATH}"
        "$<TARGET_FILE_DIR:sync_camera>/run_sync_camera.bat"
    COMMENT "✅ Generated stable launcher script (run_sync_camera.bat)"
)


# ==============================================================================
# 8. 打包配置 (CPack)
# ==============================================================================

set(CPACK_PACKAGE_VERSION "1.0.0")
set(CPACK_PACKAGE_NAME "SyncCameraTool")
set(CPACK_GENERATOR "ZIP")
set(CPACK_PACKAGE_DESCRIPTION "Multi-camera RTSP sync tool with FFmpeg/GStreamer")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_SOURCE_DIR}/readme.md")

install(TARGETS sync_camera RUNTIME DESTINATION bin)
install(FILES ${FFMPEG_DLLS} DESTINATION bin)
install(DIRECTORY "$<TARGET_FILE_DIR:sync_camera>/gstreamer-1.0/" DESTINATION bin/gstreamer-1.0)

include(CPack)

# ==============================================================================
# 9. 构建完成提示
# ==============================================================================

add_custom_command(TARGET sync_camera POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E echo "=========================================="
    COMMAND ${CMAKE_COMMAND} -E echo "Build completed successfully!"
    COMMAND ${CMAKE_COMMAND} -E echo "Use 'run_sync_camera.bat' to start on any PC."
    COMMAND ${CMAKE_COMMAND} -E echo "=========================================="
)

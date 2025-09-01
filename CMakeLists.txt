cmake_minimum_required(VERSION 3.8)
if (POLICY CMP0141)
    cmake_policy(SET CMP0141 NEW)
    set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<IF:$<AND:$<C_COMPILER_ID:MSVC>,$<CXX_COMPILER_ID:MSVC>>,$<$<CONFIG:Debug,RelWithDebInfo>:EditAndContinue>,$<$<CONFIG:Debug,RelWithDebInfo>:ProgramDatabase>>")
endif()

project(sync_camera LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Paths
set(FFMPEG_ROOT "F:/ffmpeg-7.0.2-full_build-shared")
set(GSTREAMER_ROOT "C:/Program Files/gstreamer/1.0/msvc_x86_64")

# Include directories
include_directories(
    "${FFMPEG_ROOT}/include"
    "${GSTREAMER_ROOT}/include/gstreamer-1.0"
    "${GSTREAMER_ROOT}/include/glib-2.0"
    "${GSTREAMER_ROOT}/lib/glib-2.0/include"
)

# Link directories
link_directories(
    "${FFMPEG_ROOT}/lib"
    "${GSTREAMER_ROOT}/lib"
)

add_executable(sync_camera
  src/main.cpp
  "src/multi_camera_sync.cpp"
  
  "src/tcp_streamer.cpp"
  
  "includes/multi_camera_sync.h"
  
 "includes/tcp_streamer.h"
)

# Link FFmpeg
target_link_libraries(sync_camera PRIVATE
    "${FFMPEG_ROOT}/lib/avcodec.lib"
    "${FFMPEG_ROOT}/lib/avformat.lib"
    "${FFMPEG_ROOT}/lib/avutil.lib"
    "${FFMPEG_ROOT}/lib/swscale.lib"
    "${FFMPEG_ROOT}/lib/avdevice.lib"
)

# Link GStreamer and GLib
target_link_libraries(sync_camera PRIVATE
    gstreamer-1.0
    gstapp-1.0
    gstvideo-1.0
    gstrtspserver-1.0
    gobject-2.0
    glib-2.0
)

# System libraries
target_link_libraries(sync_camera PRIVATE
    Secur32
    Bcrypt
    ws2_32
    ole32
    user32
)

# Post-build: ensure output folder
add_custom_command(TARGET sync_camera POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:sync_camera>"
)

# Copy FFmpeg and GStreamer core DLLs
set(POST_BUILD_DLLS
    # FFmpeg (已有的保持不变)
    "${FFMPEG_ROOT}/bin/avcodec-61.dll"
    "${FFMPEG_ROOT}/bin/avformat-61.dll"
    "${FFMPEG_ROOT}/bin/avutil-59.dll"
    "${FFMPEG_ROOT}/bin/swscale-8.dll"
    "${FFMPEG_ROOT}/bin/avdevice-61.dll"
    
    # GStreamer 核心库
    "${GSTREAMER_ROOT}/bin/gstreamer-1.0-0.dll"
    "${GSTREAMER_ROOT}/bin/gstapp-1.0-0.dll"
    "${GSTREAMER_ROOT}/bin/gstvideo-1.0-0.dll"
    "${GSTREAMER_ROOT}/bin/gstrtspserver-1.0-0.dll"
    "${GSTREAMER_ROOT}/bin/glib-2.0-0.dll"
    "${GSTREAMER_ROOT}/bin/gobject-2.0-0.dll"
    "${GSTREAMER_ROOT}/bin/intl-8.dll"
    
    # 添加这些重要的 GStreamer 库
    "${GSTREAMER_ROOT}/bin/gstbase-1.0-0.dll"
    "${GSTREAMER_ROOT}/bin/gstcontroller-1.0-0.dll"
    "${GSTREAMER_ROOT}/bin/gstnet-1.0-0.dll"
    "${GSTREAMER_ROOT}/bin/gio-2.0-0.dll"
    "${GSTREAMER_ROOT}/bin/gmodule-2.0-0.dll"
    "${GSTREAMER_ROOT}/bin/gthread-2.0-0.dll"
)
foreach(_dll IN LISTS POST_BUILD_DLLS)
    add_custom_command(TARGET sync_camera POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${_dll}"
        "$<TARGET_FILE_DIR:sync_camera>/"
    )
endforeach()

# Copy only required GStreamer plugins
# 按需复制插件，替换下面列表为你项目中实际使用的插件名
set(REQUIRED_PLUGINS
    # 核心插件（必需）
    gstcoreelements.dll     # 包含 queue, identity, tee 等基础元素
    gstapp.dll              # 包含 appsrc, appsink
    gsttcp.dll              # 包含 tcpserversink, tcpclientsrc
    
    # 视频相关插件
    gstvideoconvertscale.dll # 视频转换和缩放
    gstvideoparsersbad.dll   # 视频解析器
    
    # 编码插件
    gstx264.dll             # x264 编码器
    
    # RTP 相关插件
    gstrtp.dll              # RTP 支持
    gstrtpmanager.dll       # RTP 管理
    
    # 其他可能需要的插件
    gstplayback.dll         # 播放相关
    gsttypefindfunctions.dll # 类型检测
)

# 创建插件目录
add_custom_command(TARGET sync_camera POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:sync_camera>/gstreamer-1.0"
)

# 逐个复制
foreach(_plugin IN LISTS REQUIRED_PLUGINS)
    add_custom_command(TARGET sync_camera POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${GSTREAMER_ROOT}/lib/gstreamer-1.0/${_plugin}"
        "$<TARGET_FILE_DIR:sync_camera>/gstreamer-1.0/${_plugin}"
        COMMENT "Copy plugin ${_plugin}"
    )
endforeach()

# Verify GStreamer path
if(NOT EXISTS "${GSTREAMER_ROOT}")
    message(WARNING "GStreamer not found at ${GSTREAMER_ROOT}. Please install GStreamer development libraries.")
endif()

# ========== CPack 基础打包配置开始 ==========
# 版本号（需你根据实际情况修改，如 1.0.0 等）
set(CPACK_PACKAGE_VERSION "1.0.0")
set(CPACK_PACKAGE_VERSION_MAJOR "1")
set(CPACK_PACKAGE_VERSION_MINOR "0")
set(CPACK_PACKAGE_VERSION_PATCH "0")

# 项目名称（会体现在安装包等相关标识中，可自定义，也可用项目名）
set(CPACK_PACKAGE_NAME "SyncCameraTool")

# 安装包生成器，这里选 ZIP 生成 zip 压缩包（基础打包常用）
set(CPACK_GENERATOR "ZIP")  

# 安装包描述（在安装相关信息里显示，简要说明项目作用）
set(CPACK_PACKAGE_DESCRIPTION "Data acquisition and synchronization tool for multi-camera systems")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Multi-camera sync tool with FFmpeg/GStreamer support")

# 授权协议相关（如果有单独的 LICENSE 文件，替换下面路径为实际 LICENSE 文件路径；
# 若没有，用 readme.md 也可，或者你补充真实的授权说明文件路径）
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_SOURCE_DIR}/readme.md")  

# ========== 关键：指定要打包的目标和文件 ==========
# 假设你的可执行文件目标名称是 sync_camera（需你确认实际目标名，
# 也就是 add_executable 里设置的目标，比如 add_executable(sync_camera ...)，
# 这里就填 sync_camera ）
set(EXECUTABLE_TARGET_NAME "sync_camera") 

# ========== 替换原有的 install(DIRECTORY ...) 部分 ==========
# 1. 安装可执行文件到 bin 目录
install(TARGETS ${EXECUTABLE_TARGET_NAME}
        RUNTIME DESTINATION bin
)

# 2. 手动安装所有已配置的 DLL（复用你已定义的 POST_BUILD_DLLS 列表）
foreach(_dll IN LISTS POST_BUILD_DLLS)
    install(FILES "${_dll}"
            DESTINATION bin
            CONFIGURATIONS Release  # 只在 Release 模式打包
    )
endforeach()

# 3. 安装 GStreamer 插件目录（包含所有复制的插件）
install(DIRECTORY "$<TARGET_FILE_DIR:sync_camera>/gstreamer-1.0/"
        DESTINATION bin/gstreamer-1.0
        CONFIGURATIONS Release
)

# ========== 启用 CPack 完成打包配置 ==========
include(CPack)
# ========== CPack 基础打包配置结束 ==========

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

# Source files
add_executable(sync_camera
    sync_camera.cpp
    sync_camera.h
    main.cpp
    video_streamer.cpp
    video_streamer.h
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
    # FFmpeg
    "${FFMPEG_ROOT}/bin/avcodec-61.dll"
    "${FFMPEG_ROOT}/bin/avformat-61.dll"
    "${FFMPEG_ROOT}/bin/avutil-59.dll"
    "${FFMPEG_ROOT}/bin/swscale-8.dll"
    "${FFMPEG_ROOT}/bin/avdevice-61.dll"
    # GStreamer core
    "${GSTREAMER_ROOT}/bin/gstreamer-1.0-0.dll"
    "${GSTREAMER_ROOT}/bin/gstapp-1.0-0.dll"
    "${GSTREAMER_ROOT}/bin/gstvideo-1.0-0.dll"
    "${GSTREAMER_ROOT}/bin/gstrtspserver-1.0-0.dll"
    "${GSTREAMER_ROOT}/bin/glib-2.0-0.dll"
    "${GSTREAMER_ROOT}/bin/gobject-2.0-0.dll"
    "${GSTREAMER_ROOT}/bin/intl-8.dll"
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
    gstapp.dll
    gstvideoconvertscale.dll         # 或者 gstvideo.dll，根据实际目录情况
    gstx264.dll           # x264 插件名，可能为 libx264-1.0.dll
    gstrtp.dll            # RTP 支持
    gstvideoparsersbad.dll
    gstrtpmanager.dll
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

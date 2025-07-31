@echo off
echo ========================================
echo    Simple Camera Test Compiler v1.0
echo ========================================

set FFMPEG_ROOT=F:/ffmpeg-7.0.2-full_build-shared

echo Compiling simple_camera_test.cpp...

g++ -std=c++11 ^
    -O2 ^
    -I"%FFMPEG_ROOT%/include" ^
    -L"%FFMPEG_ROOT%/lib" ^
    -o simple_camera_test.exe ^
    simple_camera_test.cpp ^
    -lavcodec ^
    -lavformat ^
    -lavutil ^
    -lavdevice ^
    -lswscale ^
    -lws2_32 ^
    -lSecur32 ^
    -lBcrypt

if %ERRORLEVEL% equ 0 (
    echo.
    echo [SUCCESS] Compilation successful!
    echo.
    echo Copying required DLL files...
    
    rem Copy FFmpeg DLL files
    copy "%FFMPEG_ROOT%\bin\avcodec-61.dll" . >nul 2>&1
    copy "%FFMPEG_ROOT%\bin\avformat-61.dll" . >nul 2>&1
    copy "%FFMPEG_ROOT%\bin\avutil-59.dll" . >nul 2>&1
    copy "%FFMPEG_ROOT%\bin\swscale-8.dll" . >nul 2>&1
    copy "%FFMPEG_ROOT%\bin\avdevice-61.dll" . >nul 2>&1
    
    echo [SUCCESS] DLL files copied successfully!
    echo.
    echo ========================================
    echo            READY TO TEST
    echo ========================================
    echo.
    echo Run the simple test program:
    echo     simple_camera_test.exe
    echo.
    echo This version is:
    echo   - Compatible with older compilers
    echo   - No complex threading or async
    echo   - Reliable timeout handling
    echo   - Clear test results
    echo.
    echo ========================================
    echo.
) else (
    echo.
    echo [ERROR] Compilation failed!
    echo.
    echo Try these fixes:
    echo   1. Use C++11: -std=c++11
    echo   2. Remove std parameter completely
    echo   3. Check MinGW installation
    echo.
    echo Simple test command without std:
    echo g++ -O2 -I"%FFMPEG_ROOT%/include" -L"%FFMPEG_ROOT%/lib" -o simple_camera_test.exe simple_camera_test.cpp -lavcodec -lavformat -lavutil -lavdevice -lswscale -lws2_32 -lSecur32 -lBcrypt
    echo.
)

pause
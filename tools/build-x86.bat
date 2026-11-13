@echo off
REM 32-bit (Win32) configure + build.
REM 32 位和 64 位是两套 OpenCV，这里显式指定 32 位那一套。
REM 用 Ninja 生成器时目标架构由进来的 vcvars 决定，所以必须先 vcvars32。
call "D:\Develop\VS2019\Enterprise\VC\Auxiliary\Build\vcvars32.bat" >nul
set OPENCV_ROOT=D:\Develop\OpenCV455_32
cd /d "%~dp0.."
if not exist build_x86 mkdir build_x86
cd build_x86
if not exist build.ninja (
    cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -DOPENCV_ROOT="%OPENCV_ROOT%" .. || exit /b 1
)
cmake --build . --config Release

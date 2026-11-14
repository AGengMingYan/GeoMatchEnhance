@echo off
REM 32-bit (Win32) configure + build.
REM 32/64 use different OpenCV builds, so the 32-bit one is selected explicitly.
REM Ninja takes the target arch from the active vcvars, so vcvars32 must run first.
REM Paths are for this machine's VS2019 (D:\develop2\vs2019\IDE\VC) and the
REM 32-bit OpenCV copy at <repo>\OpenCV455_MSVC32 (flat layout).
set "VCVARS32=D:\develop2\vs2019\IDE\VC\Auxiliary\Build\vcvars32.bat"
if not exist "%VCVARS32%" (
    echo vcvars32.bat not found: %VCVARS32%
    echo edit VCVARS32 at the top of this file to point at your VS2019 install
    exit /b 1
)
call "%VCVARS32%" >nul
set OPENCV_ROOT=%~dp0..\OpenCV455_MSVC32
cd /d "%~dp0.."
if not exist build_x86 mkdir build_x86
cd build_x86
if not exist build.ninja (
    cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -DOPENCV_ROOT="%OPENCV_ROOT%" .. || exit /b 1
)
cmake --build . --config Release


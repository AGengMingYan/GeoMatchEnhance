@echo off
REM 32-bit (Win32) configure + build.
REM 32/64 use different OpenCV builds, so the 32-bit one is selected explicitly.
REM Ninja takes the target arch from the active vcvars, so vcvars32 must run first.
REM Paths are for this machine's VS2019 (D:\develop2\vs2019\IDE\VC) and the
REM 32-bit OpenCV copy at <repo>\OpenCV455_MSVC32 (flat layout).
REM --- locate vcvars32.bat -----------------------------------------------------
REM Checked in order; add your own VS install path at the top if it differs.
REM (Deliberately no "for /f vswhere": the "(x86)" inside that path breaks the
REM  parenthesis counting of a surrounding if( ... ) block.)
set "VCVARS32="
if not defined VCVARS32 if exist "D:\Develop\VS2019\Enterprise\VC\Auxiliary\Build\vcvars32.bat" set "VCVARS32=D:\Develop\VS2019\Enterprise\VC\Auxiliary\Build\vcvars32.bat"
if not defined VCVARS32 if exist "D:\develop2\vs2019\IDE\VC\Auxiliary\Build\vcvars32.bat" set "VCVARS32=D:\develop2\vs2019\IDE\VC\Auxiliary\Build\vcvars32.bat"
if not defined VCVARS32 if exist "%ProgramFiles%\Microsoft Visual Studio\2019\Enterprise\VC\Auxiliary\Build\vcvars32.bat" set "VCVARS32=%ProgramFiles%\Microsoft Visual Studio\2019\Enterprise\VC\Auxiliary\Build\vcvars32.bat"
if not defined VCVARS32 if exist "%ProgramFiles%\Microsoft Visual Studio\2019\Professional\VC\Auxiliary\Build\vcvars32.bat" set "VCVARS32=%ProgramFiles%\Microsoft Visual Studio\2019\Professional\VC\Auxiliary\Build\vcvars32.bat"
if not defined VCVARS32 if exist "%ProgramFiles%\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvars32.bat" set "VCVARS32=%ProgramFiles%\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvars32.bat"
if not defined VCVARS32 if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\2019\Enterprise\VC\Auxiliary\Build\vcvars32.bat" set "VCVARS32=%ProgramFiles(x86)%\Microsoft Visual Studio\2019\Enterprise\VC\Auxiliary\Build\vcvars32.bat"
if not defined VCVARS32 if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\2019\Professional\VC\Auxiliary\Build\vcvars32.bat" set "VCVARS32=%ProgramFiles(x86)%\Microsoft Visual Studio\2019\Professional\VC\Auxiliary\Build\vcvars32.bat"
if not defined VCVARS32 if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvars32.bat" set "VCVARS32=%ProgramFiles(x86)%\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvars32.bat"
if not defined VCVARS32 (
    echo [build-x86] vcvars32.bat not found
    echo [build-x86] edit tools\build-x86.bat and add your VS install path
    exit /b 1
)
echo [build-x86] using %VCVARS32%
call "%VCVARS32%" >nul

REM The repo ships a flat 32-bit OpenCV at <repo>\OpenCV455_MSVC32; when it is
REM absent CMakeLists.txt falls back to D:/Develop/OpenCV455_32 etc. Only pass
REM -DOPENCV_ROOT when the shipped copy actually exists (a bogus path is fatal).
set "OCV_ARG="
if exist "%~dp0..\OpenCV455_MSVC32\include\opencv2" set OCV_ARG=-DOPENCV_ROOT="%~dp0..\OpenCV455_MSVC32"

cd /d "%~dp0.."
if not exist build_x86 mkdir build_x86
cd build_x86
REM A cache left behind by an x64 run would make this script silently produce
REM 64-bit exes inside build_x86\ - detect it and reconfigure from scratch.
if exist build.ninja (
    findstr /C:"x86/cl.exe" CMakeCache.txt >nul 2>&1 || (
        echo [build-x86] build_x86\ cache is not x86 - reconfiguring
        del /q build.ninja rules.ninja CMakeCache.txt >nul 2>&1
        rmdir /s /q CMakeFiles >nul 2>&1
    )
)
if not exist build.ninja (
    cmake -G Ninja -DCMAKE_BUILD_TYPE=Release %OCV_ARG% .. || exit /b 1
)
cmake --build . --config Release
if errorlevel 1 exit /b 1
echo.
echo x86 build done -^> build_x86\GeoMatchDemo.exe  build_x86\GeoGUIDemo.exe


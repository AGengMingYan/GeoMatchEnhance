@echo off
REM ---------------------------------------------------------------------------
REM 64-bit (x64) configure + build  ->  build\*.exe
REM
REM The architecture comes from the active vcvars: the Ninja generator has no
REM -A/PLATFORM switch, so vcvars64.bat must run first (it also puts cl.exe,
REM rc.exe and the Windows SDK headers on PATH).
REM
REM Extra cmake options can be appended, e.g.
REM     tools\build-x64.bat -DGEOMATCH_SIMD=OFF
REM ---------------------------------------------------------------------------
set "VCVARS64=D:\develop2\vs2019\IDE\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS64%" (
    echo vcvars64.bat not found: %VCVARS64%
    echo edit VCVARS64 at the top of this file to point at your VS2019 install
    exit /b 1
)
call "%VCVARS64%" >nul
cd /d "%~dp0.."
if not exist build mkdir build
cd build
if not exist build.ninja (
    cmake -G Ninja -DCMAKE_BUILD_TYPE=Release %* .. || exit /b 1
)
cmake --build . --config Release %*
if errorlevel 1 exit /b 1
echo.
echo x64 build done -^> build\GeoMatchDemo.exe  build\GeoGUIDemo.exe

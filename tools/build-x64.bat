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
REM --- locate vcvars64.bat -----------------------------------------------------
REM Checked in order; add your own VS install path at the top if it differs.
REM (Deliberately no "for /f vswhere": the "(x86)" inside that path breaks the
REM  parenthesis counting of a surrounding if( ... ) block.)
set "VCVARS64="
if not defined VCVARS64 if exist "D:\Develop\VS2019\Enterprise\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS64=D:\Develop\VS2019\Enterprise\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCVARS64 if exist "D:\develop2\vs2019\IDE\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS64=D:\develop2\vs2019\IDE\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCVARS64 if exist "%ProgramFiles%\Microsoft Visual Studio\2019\Enterprise\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS64=%ProgramFiles%\Microsoft Visual Studio\2019\Enterprise\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCVARS64 if exist "%ProgramFiles%\Microsoft Visual Studio\2019\Professional\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS64=%ProgramFiles%\Microsoft Visual Studio\2019\Professional\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCVARS64 if exist "%ProgramFiles%\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS64=%ProgramFiles%\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCVARS64 if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\2019\Enterprise\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS64=%ProgramFiles(x86)%\Microsoft Visual Studio\2019\Enterprise\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCVARS64 if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\2019\Professional\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS64=%ProgramFiles(x86)%\Microsoft Visual Studio\2019\Professional\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCVARS64 if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS64=%ProgramFiles(x86)%\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCVARS64 (
    echo [build-x64] vcvars64.bat not found
    echo [build-x64] edit tools\build-x64.bat and add your VS install path
    exit /b 1
)
echo [build-x64] using %VCVARS64%
call "%VCVARS64%" >nul
cd /d "%~dp0.."
if not exist build mkdir build
cd build
REM A cache left behind by an x86 run would make this script silently produce
REM 32-bit exes inside build\ - detect it and reconfigure from scratch.
if exist build.ninja (
    findstr /C:"x64/cl.exe" CMakeCache.txt >nul 2>&1 || (
        echo [build-x64] build\ cache is not x64 - reconfiguring
        del /q build.ninja rules.ninja CMakeCache.txt >nul 2>&1
        rmdir /s /q CMakeFiles >nul 2>&1
    )
)
if not exist build.ninja (
    cmake -G Ninja -DCMAKE_BUILD_TYPE=Release %* .. || exit /b 1
)
cmake --build . --config Release %*
if errorlevel 1 exit /b 1
echo.
echo x64 build done -^> build\GeoMatchDemo.exe  build\GeoGUIDemo.exe

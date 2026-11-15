@rem ========================================================================
@rem build-halcon-ncc-levels.bat - build the "how many NCC levels does Halcon
@rem really build" probe (32-bit, HalconDemo/Halcon17/x86sse2-win32).
@rem Output: HalconDemo/halcon_ncc_levels.exe
@rem ========================================================================
@echo off
setlocal

set "REPO_ROOT=%~dp0..\"
set "HALCON_ROOT=%REPO_ROOT%HalconDemo\Halcon17"
set "HALCON_LIB=%HALCON_ROOT%\x86sse2-win32"
set "HALCON_BIN=%HALCON_ROOT%\bin"

if not exist "%HALCON_LIB%\halconcpp.lib" (
    echo [error] Halcon 32-bit lib not found: %HALCON_LIB%\halconcpp.lib
    exit /b 1
)

set "VSVARS=D:\develop2\vs2019\IDE\VC\Auxiliary\Build\vcvars32.bat"
if not exist "%VSVARS%" (
    echo [error] vcvars32.bat not found: %VSVARS%
    echo        edit VSVARS in this script to point at your VS install
    exit /b 1
)
call "%VSVARS%" >nul

set "PATH=%HALCON_BIN%;%PATH%"
set "HALCONARCH=x86sse2-win32"

cl.exe /nologo /EHsc /O2 /utf-8 /W3 /I"%HALCON_ROOT%\include" ^
    "%REPO_ROOT%tools\halcon_ncc_levels.cpp" ^
    /link "%HALCON_LIB%\halconcpp.lib" ^
    /OUT:"%REPO_ROOT%HalconDemo\halcon_ncc_levels.exe"
if errorlevel 1 (
    echo [error] compile failed
    exit /b 1
)

echo.
echo [info] build OK: HalconDemo\halcon_ncc_levels.exe
endlocal

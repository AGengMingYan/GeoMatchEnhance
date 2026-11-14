@rem ========================================================================
@rem build-halcon-ncc-compare.bat - build the Halcon 17 NCC compare demo
@rem
@rem 32-bit only (HalconDemo/Halcon17/x86sse2-win32). Links halconcpp.lib.
@rem Output: HalconDemo/halcon_ncc_compare.exe (Win32).
@rem ========================================================================
@echo off
setlocal EnableDelayedExpansion

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
echo [info] calling %VSVARS%
call "%VSVARS%"

set "PATH=%HALCON_BIN%;%PATH%"
set "HALCONARCH=x86sse2-win32"

set "OUT_EXE=%REPO_ROOT%HalconDemo\halcon_ncc_compare.exe"
set "SRC_CPP=%REPO_ROOT%HalconDemo\halcon_ncc_compare.cpp"

echo [info] source: %SRC_CPP%
echo [info] output: %OUT_EXE%
echo.

cl.exe /EHsc /O2 /utf-8 /W3 /I"%HALCON_ROOT%\include" "%SRC_CPP%" /link "%HALCON_LIB%\halconcpp.lib" /OUT:"%OUT_EXE%"
if %ERRORLEVEL% neq 0 (
    echo [error] compile failed
    exit /b %ERRORLEVEL%
)

echo.
echo [info] build OK: %OUT_EXE%
endlocal
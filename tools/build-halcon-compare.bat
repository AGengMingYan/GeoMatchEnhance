@rem ========================================================================
@rem build-halcon-compare.bat - build the Halcon 17 shape-matching compare demo
@rem
@rem The local Halcon 17 is 32-bit only (HalconDemo/Halcon17/x86sse2-win32),
@rem so this must be built with the x86 MSVC toolchain (vcvars32.bat) and
@rem linked against the 32-bit halconcpp.lib.
@rem
@rem Output halcon_shape_compare.exe (Win32) is run separately from the x64
@rem GeoMatchDemo, then scores / poses / timings are compared by hand.
@rem ========================================================================
@echo off
setlocal EnableDelayedExpansion

@rem ---- script dir (tools/) and repo root ----
set "SCRIPT_DIR=%~dp0"
set "REPO_ROOT=%~dp0..\"

@rem ---- Halcon 17 SDK (32-bit, shipped in repo) ----
set "HALCON_ROOT=%REPO_ROOT%HalconDemo\Halcon17"
set "HALCON_LIB=%HALCON_ROOT%\x86sse2-win32"
set "HALCON_BIN=%HALCON_ROOT%\bin"

@rem ---- check the Halcon lib exists ----
if not exist "%HALCON_LIB%\halconcpp.lib" (
    echo [error] Halcon 32-bit lib not found: %HALCON_LIB%\halconcpp.lib
    echo        expected: HalconDemo/Halcon17/x86sse2-win32/halconcpp.lib
    exit /b 1
)

@rem ---- locate vcvars32.bat ---------------------------------------------------
@rem Checked in order; add your own VS install path at the top if it differs.
@rem (No "for /f vswhere": the "(x86)" inside that path breaks the parenthesis
@rem  counting of a surrounding if( ... ) block - see .clinerules.)
set "VSVARS="
if not defined VSVARS if exist "D:\Develop\VS2019\Enterprise\VC\Auxiliary\Build\vcvars32.bat" set "VSVARS=D:\Develop\VS2019\Enterprise\VC\Auxiliary\Build\vcvars32.bat"
if not defined VSVARS if exist "D:\develop2\vs2019\IDE\VC\Auxiliary\Build\vcvars32.bat" set "VSVARS=D:\develop2\vs2019\IDE\VC\Auxiliary\Build\vcvars32.bat"
if not defined VSVARS if exist "%ProgramFiles%\Microsoft Visual Studio\2019\Enterprise\VC\Auxiliary\Build\vcvars32.bat" set "VSVARS=%ProgramFiles%\Microsoft Visual Studio\2019\Enterprise\VC\Auxiliary\Build\vcvars32.bat"
if not defined VSVARS if exist "%ProgramFiles%\Microsoft Visual Studio\2019\Professional\VC\Auxiliary\Build\vcvars32.bat" set "VSVARS=%ProgramFiles%\Microsoft Visual Studio\2019\Professional\VC\Auxiliary\Build\vcvars32.bat"
if not defined VSVARS if exist "%ProgramFiles%\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvars32.bat" set "VSVARS=%ProgramFiles%\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvars32.bat"
if not defined VSVARS if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\2019\Enterprise\VC\Auxiliary\Build\vcvars32.bat" set "VSVARS=%ProgramFiles(x86)%\Microsoft Visual Studio\2019\Enterprise\VC\Auxiliary\Build\vcvars32.bat"
if not defined VSVARS if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\2019\Professional\VC\Auxiliary\Build\vcvars32.bat" set "VSVARS=%ProgramFiles(x86)%\Microsoft Visual Studio\2019\Professional\VC\Auxiliary\Build\vcvars32.bat"
if not defined VSVARS if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvars32.bat" set "VSVARS=%ProgramFiles(x86)%\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvars32.bat"
if not defined VSVARS (
    echo [error] vcvars32.bat not found
    echo        edit the VSVARS candidates in this script to point at your VS install
    exit /b 1
)
echo [info] calling %VSVARS%
call "%VSVARS%"

@rem ---- Halcon runtime env (DLL path + arch) ----
set "PATH=%HALCON_BIN%;%PATH%"
set "HALCONARCH=x86sse2-win32"

@rem ---- output exe sits next to the source (HalconDemo/) ----
set "OUT_EXE=%REPO_ROOT%HalconDemo\halcon_shape_compare.exe"
set "SRC_CPP=%REPO_ROOT%HalconDemo\halcon_shape_compare.cpp"

echo [info] source: %SRC_CPP%
echo [info] output: %OUT_EXE%
echo [info] Halcon lib: %HALCON_LIB%\halconcpp.lib
echo [info] Halcon inc: %HALCON_ROOT%\include
echo.

cl.exe /EHsc /O2 /utf-8 /W3 /I"%HALCON_ROOT%\include" "%SRC_CPP%" /link "%HALCON_LIB%\halconcpp.lib" /OUT:"%OUT_EXE%"
if %ERRORLEVEL% neq 0 (
    echo [error] compile failed
    exit /b %ERRORLEVEL%
)

echo.
echo [info] build OK: %OUT_EXE%
echo [info] run: %OUT_EXE% ..\case2\template_a.jpg ..\case2\search_a_03.jpg -minscore 0.3 -max 1
echo [info] make sure %HALCON_BIN% is on PATH at runtime (set by this script, current window only)
endlocal
@rem ========================================================================
@rem build-halcon-scaled-compare.bat - build the Halcon 17 SCALED shape-model demo
@rem
@rem Difference from build-halcon-compare.bat:
@rem   * source is HalconDemo/HalconCompare.cpp (create_scaled_shape_model)
@rem   * it includes <HalconCpp.h> directly, so BOTH include dirs are needed:
@rem         Halcon17\include          (HalconCpp.h)
@rem         Halcon17\include\halconcpp (the C++ API headers it pulls in)
@rem
@rem Local Halcon 17 is 32-bit only, so this must be an x86 build (vcvars32).
@rem
@rem Usage:  tools\build-halcon-scaled-compare.bat
@rem   then:  HalconDemo\HalconCompare.exe <template> <image_dir> [angle] [extent]
@rem                                        [scaleMin] [scaleMax]
@rem   env needed at runtime (this script sets them for the current window):
@rem         PATH += HalconDemo\Halcon17\bin ; HALCONARCH=x86sse2-win32
@rem
@rem Known behaviour: an out-of-range ScaleMin makes Halcon ABORT (0xC0000409)
@rem instead of throwing, so the tool calls setvbuf() to keep stdout visible.
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

if not exist "%HALCON_LIB%\halconcpp.lib" (
    echo [error] Halcon 32-bit lib not found: %HALCON_LIB%\halconcpp.lib
    exit /b 1
)

@rem ---- load the x86 MSVC environment ----
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
set "HALCONROOT=%HALCON_ROOT%"

set "OUT_EXE=%REPO_ROOT%HalconDemo\HalconCompare.exe"
set "SRC_CPP=%REPO_ROOT%HalconDemo\HalconCompare.cpp"

echo [info] source: %SRC_CPP%
echo [info] output: %OUT_EXE%
echo.

@rem ---- build in a temp obj dir so no .obj litters the repo root ----
pushd "%REPO_ROOT%_ai_tmp"
cl.exe /nologo /EHsc /O2 /utf-8 /W3 ^
    /I"%HALCON_ROOT%\include" /I"%HALCON_ROOT%\include\halconcpp" ^
    "%SRC_CPP%" /Fo:halcon_compare.obj /link "%HALCON_LIB%\halconcpp.lib" /OUT:"%OUT_EXE%"
set "RC=%ERRORLEVEL%"
popd

if %RC% neq 0 (
    echo [error] compile failed
    exit /b %RC%
)

echo.
echo [info] build OK: %OUT_EXE%
echo [info] run example:
echo        HalconDemo\HalconCompare.exe dataset\image\m4.bmp dataset\image 0.5 5 0.9 1.1
echo [info] scale bounds on this machine (measured): ScaleMin must keep the
echo        scaled template bigger than ~4 px, ScaleMax has no practical limit
endlocal

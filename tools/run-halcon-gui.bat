@rem ========================================================================
@rem run-halcon-gui.bat - 启动 HalconDemo/bin/HalconGUIDemo.exe
@rem
@rem exe 与运行期 DLL（halcon*.dll / opencv_world455.dll）都收拢在
@rem HalconDemo/bin 下，直接双击 exe 即可运行；本脚本只是带参数启动，
@rem 并把工作目录固定到仓库根（便于相对路径读取 case1 等目录）。
@rem
@rem 用法：tools\run-halcon-gui.bat [case1|图片目录] [--ncc]
@rem ========================================================================
@echo off
setlocal

set "SCRIPT_DIR=%~dp0"
set "REPO_ROOT=%~dp0.."
set "BIN_DIR=%REPO_ROOT%\HalconDemo\bin"
set "EXE=%BIN_DIR%\HalconGUIDemo.exe"

if not exist "%EXE%" (
    echo [error] 找不到 %EXE%
    echo         先执行 tools\build-halcon-gui.bat
    pause
    exit /b 1
)

set "HALCONARCH=x86sse2-win32"

start "HalconGUIDemo" /D "%REPO_ROOT%" "%EXE%" %*
endlocal

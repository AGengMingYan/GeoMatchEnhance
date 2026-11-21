@rem ========================================================================
@rem build-halcon-gui.bat - build HalconGUIDemo.exe (ImGui + Halcon 17, 32-bit)
@rem
@rem 仓库内的 Halcon 17 只有 32 位库（HalconDemo/Halcon17/x86sse2-win32），
@rem 所以这里必须用 x86 MSVC 工具链（vcvars32.bat）+ 32 位 OpenCV。
@rem
@rem 依赖：
@rem   HalconDemo/Halcon17/{include, x86sse2-win32, bin}
@rem   OpenCV 32 位（扁平布局：include/ + opencv_world455.lib/.dll）
@rem     候选：仓库根\OpenCV455_MSVC32、D:\Develop\OpenCV455_32、D:\develop2\OpenCV455_32
@rem   imgui/imgui/imgui-master/imgui-master
@rem
@rem 输出：HalconDemo/bin/HalconGUIDemo.exe（Win32，可直接双击运行）
@rem       构建成功后会把 halcon*.dll 和 opencv_world455.dll 拷到同目录。
@rem ========================================================================
@echo off
setlocal EnableDelayedExpansion

@rem 本文件是 UTF-8(无 BOM) 且含中文注释。默认 GBK 控制台下 cmd 会按 GBK
@rem 解码，累积错位后会把某些行首的 "@rem" 吃掉、把注释当命令执行
@rem （实测输出 "系统找不到指定的文件" / "不是内部或外部命令"）。
@rem 因此先把代码页切到 65001 再让 cmd 继续逐行解析，结束时还原调用者的代码页。
set "GM_SAVED_CP="
for /f "tokens=* delims=" %%L in ('chcp') do set "GM_CP_LINE=%%L"
@rem chcp 的输出随语言变化（"Active code page: 936" / "活动代码页: 936"），
@rem 只有最后一个 token 恒为数字，别用 tokens=2 取（会拿到 "code"）。
for %%t in (!GM_CP_LINE!) do set "GM_SAVED_CP=%%t"
chcp 65001 >nul

@rem ---- 脚本目录(tools/) 与仓库根 ----
set "SCRIPT_DIR=%~dp0"
set "REPO_ROOT=%~dp0..\"

@rem ---- Halcon 17 SDK (32 位) ----
set "HALCON_ROOT=%REPO_ROOT%HalconDemo\Halcon17"
set "HALCON_LIB=%HALCON_ROOT%\x86sse2-win32"
set "HALCON_BIN=%HALCON_ROOT%\bin"

@rem ---- 32 位 OpenCV（扁平布局：include/ + opencv_world455.lib） ----
@rem 仓库自带的 OpenCV455_MSVC32 在部分机器上不存在，按顺序回退到本机安装的
@rem 32 位 OpenCV。候选顺序与 tools\build-x86.bat / CMakeLists.txt 一致。
@rem （不写 for /f：路径里的 "(x86)" 会撕裂 if( ... ) 块的括号计数，见 .clinerules）
set "OPENCV32="
if not defined OPENCV32 if exist "%REPO_ROOT%OpenCV455_MSVC32\opencv_world455.lib" set "OPENCV32=%REPO_ROOT%OpenCV455_MSVC32"
if not defined OPENCV32 if exist "D:\Develop\OpenCV455_32\opencv_world455.lib" set "OPENCV32=D:\Develop\OpenCV455_32"
if not defined OPENCV32 if exist "D:\develop2\OpenCV455_32\opencv_world455.lib" set "OPENCV32=D:\develop2\OpenCV455_32"

@rem ---- ImGui ----
set "IMGUI_ROOT=%REPO_ROOT%imgui\imgui\imgui-master\imgui-master"

@rem ---- 检查依赖 ----
if not exist "%HALCON_LIB%\halconcpp.lib" (
    echo [error] 找不到 Halcon 32 位库: %HALCON_LIB%\halconcpp.lib
    exit /b 1
)
if not defined OPENCV32 (
    echo [error] 找不到 32 位 OpenCV（需要 opencv_world455.lib + include\opencv2）
    echo         尝试过的位置:
    echo           %REPO_ROOT%OpenCV455_MSVC32
    echo           D:\Develop\OpenCV455_32
    echo           D:\develop2\OpenCV455_32
    echo         请把本机 32 位 OpenCV 加到上面的候选列表里
    exit /b 1
)
if not exist "%IMGUI_ROOT%\imgui.cpp" (
    echo [error] 找不到 ImGui: %IMGUI_ROOT%\imgui.cpp
    exit /b 1
)

@rem ---- x86 MSVC 环境 ----
set "VSVARS=D:\develop2\vs2019\IDE\VC\Auxiliary\Build\vcvars32.bat"
if not exist "%VSVARS%" if exist "D:\Develop\VS2019\Enterprise\VC\Auxiliary\Build\vcvars32.bat" (
    set "VSVARS=D:\Develop\VS2019\Enterprise\VC\Auxiliary\Build\vcvars32.bat"
)
if not exist "%VSVARS%" (
    echo [error] 找不到 vcvars32.bat: %VSVARS%
    echo         请修改本脚本里的 VSVARS
    exit /b 1
)
echo [info] calling %VSVARS%
call "%VSVARS%"

@rem ---- 运行期依赖（当前窗口有效） ----
set "PATH=%HALCON_BIN%;%OPENCV32%;%PATH%"
set "HALCONARCH=x86sse2-win32"

@rem ---- 编译输出目录 ----
set "OBJ_DIR=%REPO_ROOT%HalconDemo\obj"
if not exist "%OBJ_DIR%" mkdir "%OBJ_DIR%"
set "OUT_DIR=%REPO_ROOT%HalconDemo\bin"
if not exist "%OUT_DIR%" mkdir "%OUT_DIR%"
set "OUT_EXE=%OUT_DIR%\HalconGUIDemo.exe"
set "SRC_CPP=%REPO_ROOT%HalconDemo\HalconGUIDemo.cpp"

echo [info] source : %SRC_CPP%
echo [info] output : %OUT_EXE%
echo [info] halcon : %HALCON_LIB%\halconcpp.lib
echo [info] opencv : %OPENCV32%\opencv_world455.lib
echo.

pushd "%OBJ_DIR%"
cl.exe /nologo /EHsc /O2 /utf-8 /W3 /MD ^
    /I"%HALCON_ROOT%\include" ^
    /I"%IMGUI_ROOT%" /I"%IMGUI_ROOT%\backends" ^
    /I"%OPENCV32%\include" ^
    "%SRC_CPP%" ^
    "%IMGUI_ROOT%\imgui.cpp" ^
    "%IMGUI_ROOT%\imgui_draw.cpp" ^
    "%IMGUI_ROOT%\imgui_tables.cpp" ^
    "%IMGUI_ROOT%\imgui_widgets.cpp" ^
    "%IMGUI_ROOT%\backends\imgui_impl_win32.cpp" ^
    "%IMGUI_ROOT%\backends\imgui_impl_dx11.cpp" ^
    /link /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup ^
    "%HALCON_LIB%\halconcpp.lib" "%OPENCV32%\opencv_world455.lib" ^
    d3d11.lib dxgi.lib d3dcompiler.lib comdlg32.lib ole32.lib uuid.lib shell32.lib ^
    /OUT:"%OUT_EXE%"
set "RC=%ERRORLEVEL%"
popd

if not "%RC%"=="0" (
    echo [error] 编译失败，退出码 %RC%
    exit /b %RC%
)

@rem ---- 把运行期 DLL 收拢到 exe 同目录，之后无需设置 PATH ----
copy /Y "%HALCON_BIN%\halcon.dll"      "%OUT_DIR%" >nul
copy /Y "%HALCON_BIN%\halconc.dll"     "%OUT_DIR%" >nul
copy /Y "%HALCON_BIN%\halconcpp.dll"   "%OUT_DIR%" >nul
copy /Y "%HALCON_BIN%\halconcppxl.dll" "%OUT_DIR%" >nul
copy /Y "%OPENCV32%\opencv_world455.dll" "%OUT_DIR%" >nul
if not exist "%OUT_DIR%\opencv_world455.dll" (
    echo [error] 运行期 DLL 拷贝失败，请检查 %OUT_DIR% 的写权限
    exit /b 1
)

echo.
echo [info] build OK: %OUT_EXE%
echo [info] run: "%OUT_EXE%" case1   ^(DLL 已在同目录，可直接双击^)
if defined GM_SAVED_CP chcp %GM_SAVED_CP% >nul
endlocal

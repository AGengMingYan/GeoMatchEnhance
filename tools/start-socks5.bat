@echo off
setlocal
chcp 65001 >nul
set "ELECTRON_RUN_AS_NODE=1"
set "DOUBAO_EXE=D:\Application\Doubao\app\Doubao.exe"

if not exist "%DOUBAO_EXE%" (
  echo [ERROR] not found: %DOUBAO_EXE%
  pause
  exit /b 1
)

REM Usage: start-socks5.bat [--port 1080] [--host 127.0.0.1] [--upstream http://host:port] [-v]
"%DOUBAO_EXE%" "%~dp0socks5-proxy.js" %*

echo.
echo proxy exited
pause

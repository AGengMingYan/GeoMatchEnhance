@echo off
REM Build the Halcon-vs-NccMatch ground truth tool (x64).
REM Requires the local Halcon 17 install at D:\Develop\Halcon17 (x64-win64 libs).
call "D:\Develop\VS2019\Enterprise\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0.."
set HALCONROOT=D:\Develop\Halcon17
set HALCONARCH=x64-win64
cl /nologo /std:c++17 /utf-8 /O2 /EHsc /MD ^
   /I"D:\Develop\OpenCV455\opencv\build\include" ^
   /I"D:\Develop\Halcon17\include\halconcpp" ^
   /I"D:\Develop\Halcon17\include" -I. ^
   /Fe:build_x64\halcon_ncc.exe tools\halcon_ncc.cpp ^
   /Fo:build_x64\halcon_ncc.obj ^
   build_x64\GeoMatch.lib ^
   /link /LIBPATH:"D:\Develop\OpenCV455\opencv\build\x64\vc15\lib" opencv_world455.lib ^
         /LIBPATH:"D:\Develop\Halcon17\lib\x64-win64" halconcpp.lib

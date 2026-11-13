cmake_minimum_required(VERSION 3.10.0)
project(GeoMatchDemo VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()



# OpenCV build root; override on the command line with -DOPENCV_ROOT=<path>
if(NOT OPENCV_ROOT)
    if(EXISTS "D:/Develop/OpenCV455/opencv/build")
        set(OPENCV_ROOT "D:/Develop/OpenCV455/opencv/build")
    else()
        set(OPENCV_ROOT "D:/develop2/OpenCV455/opencv/build")
    endif()
endif()
message(STATUS "OpenCV root: ${OPENCV_ROOT}")

set(OpenCV_INCLUDE_DIRS "${OPENCV_ROOT}/include")
if(EXISTS "${OPENCV_ROOT}/x64/vc15/lib/opencv_world455.lib")
    set(OpenCV_LIBS "${OPENCV_ROOT}/x64/vc15/lib/opencv_world455.lib")
elseif(EXISTS "${OPENCV_ROOT}/x64/vc14/lib/opencv_world455.lib")
    set(OpenCV_LIBS "${OPENCV_ROOT}/x64/vc14/lib/opencv_world455.lib")
else()
    set(OpenCV_LIBS "${OPENCV_ROOT}/opencv_world455.lib")
endif()


# set(OpenCV_INCLUDE_DIRS "C:/Develop/OpenCV455_32/include")
# set(OpenCV_LIBS "C:/Develop/OpenCV455_32/opencv_world455.lib")


# set(OPENCV_ROOT "D:/develop2/OpenCV455/opencv/build" CACHE PATH "OpenCV build/install root")

# if(EXISTS "${OPENCV_ROOT}/OpenCVConfig.cmake")
#     set(OpenCV_DIR "${OPENCV_ROOT}")
#     find_package(OpenCV QUIET)
# endif()

# if(NOT OpenCV_FOUND)
#     message(STATUS "OpenCVConfig.cmake not usable -> falling back to explicit paths")
#     set(OPENCV_INCLUDE_DIR "${OPENCV_ROOT}/include")
#     if(EXISTS "${OPENCV_ROOT}/x64/vc15/lib")
#         set(OPENCV_LIB_DIR "${OPENCV_ROOT}/x64/vc15/lib")
#     else()
#         set(OPENCV_LIB_DIR "${OPENCV_ROOT}/x64/vc14/lib")
#     endif()
#     set(OpenCV_INCLUDE_DIRS "${OPENCV_INCLUDE_DIR}")
#     set(OpenCV_LIBS
#         optimized "${OPENCV_LIB_DIR}/opencv_world455.lib"
#         debug     "${OPENCV_LIB_DIR}/opencv_world455d.lib")
# endif()





include_directories(${OpenCV_INCLUDE_DIRS})
message("${OpenCV_INCLUDE_DIRS}")
#-----------------------------------------------------------------------
#  GeoMatch library: shape matcher core + high level facade.
#  GEOMATCH_BUILD_SHARED=ON  -> shared library (dll / so)
#  GEOMATCH_BUILD_SHARED=OFF -> static library (default, no dll hell)
#-----------------------------------------------------------------------
option(GEOMATCH_BUILD_SHARED "build GeoMatch as a shared library" OFF)

set(GEOMATCH_SOURCES
    ShapeMatch/ShapeMatch.cpp
    ShapeMatch/ShapeMatch.h
    NccMatch/NccMatch.cpp
    NccMatch/NccMatch.h
    GeoMatch/GeoMatch.cpp
    GeoMatch/GeoMatch.h
)

if(GEOMATCH_BUILD_SHARED)
    add_library(GeoMatch SHARED ${GEOMATCH_SOURCES})
    # PRIVATE: we are building it   PUBLIC: consumers import from the dll
    target_compile_definitions(GeoMatch PRIVATE GEOMATCH_EXPORTS PUBLIC GEOMATCH_SHARED)
else()
    add_library(GeoMatch STATIC ${GEOMATCH_SOURCES})
    # static build: the symbols are baked into the consumer, no decoration
    target_compile_definitions(GeoMatch PUBLIC GEOMATCH_STATIC)
endif()

target_include_directories(GeoMatch PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
if(OpenCV_LIBS)
    target_link_libraries(GeoMatch PUBLIC ${OpenCV_LIBS})
else()
    target_link_libraries(GeoMatch PUBLIC ${OpenCV_LIBS} ${OpenCV_LIBRARIES})
endif()
target_compile_definitions(GeoMatch PRIVATE _USE_MATH_DEFINES)
# the static lib is meant to be consumed with the same runtime as the app
if(MSVC AND NOT GEOMATCH_BUILD_SHARED)
    set_property(TARGET GeoMatch PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()

#-----------------------------------------------------------------------
#  command line front end - now only argument parsing and printing
#-----------------------------------------------------------------------
add_executable(GeoMatchDemo main.cpp)
add_executable(Demo1 main1.cpp)
add_executable(GeoGUIDemo GeoGUIDemo.cpp)
# NCC 命令行前端(对标 Halcon create_ncc_model / find_ncc_model)
add_executable(NccDemo NccDemo.cpp)
target_include_directories(NccDemo PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
if(OpenCV_LIBS)
    target_link_libraries(NccDemo PRIVATE GeoMatch ${OpenCV_LIBS})
else()
    target_link_libraries(NccDemo PRIVATE GeoMatch ${OpenCV_LIBS} ${OpenCV_LIBRARIES})
endif()
if(MSVC)
    target_compile_options(NccDemo PRIVATE /utf-8 $<$<CONFIG:Release>:/O2 /arch:AVX2>)
endif()

# 命令行分析工具: 拿已有模型跑一批真实图片，打印每张的分数/位姿/耗时
add_executable(AnalyzeCase AnalyzeCase.cpp)
target_include_directories(AnalyzeCase PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
if(OpenCV_LIBS)
    target_link_libraries(AnalyzeCase PRIVATE GeoMatch ${OpenCV_LIBS})
else()
    target_link_libraries(AnalyzeCase PRIVATE GeoMatch ${OpenCV_LIBS} ${OpenCV_LIBRARIES})
endif()
if(MSVC)
    target_compile_options(AnalyzeCase PRIVATE /utf-8)
endif()

target_include_directories(GeoMatchDemo PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(GeoMatchDemo PRIVATE GeoMatch)
target_link_libraries(Demo1 PRIVATE GeoMatch)

#-----------------------------------------------------------------------
#  Dear ImGui - Win32 + DirectX11 backends (used by GeoGUIDemo)
#  the sources live in imgui/imgui/imgui-master/imgui-master
#-----------------------------------------------------------------------
set(IMGUI_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/imgui/imgui/imgui-master/imgui-master")

add_library(imgui STATIC
    ${IMGUI_ROOT}/imgui.cpp
    ${IMGUI_ROOT}/imgui_draw.cpp
    ${IMGUI_ROOT}/imgui_tables.cpp
    ${IMGUI_ROOT}/imgui_widgets.cpp
    ${IMGUI_ROOT}/backends/imgui_impl_win32.cpp
    ${IMGUI_ROOT}/backends/imgui_impl_dx11.cpp
)
target_include_directories(imgui PUBLIC ${IMGUI_ROOT} ${IMGUI_ROOT}/backends)
target_link_libraries(imgui PUBLIC d3d11 dxgi d3dcompiler)

#-----------------------------------------------------------------------
#  GeoGUIDemo - imgui front end for quick template matching checks
#-----------------------------------------------------------------------
target_include_directories(GeoGUIDemo PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${IMGUI_ROOT} ${IMGUI_ROOT}/backends)
target_compile_definitions(GeoGUIDemo PRIVATE _USE_MATH_DEFINES)
if(OpenCV_LIBS)
    target_link_libraries(GeoGUIDemo PRIVATE GeoMatch imgui ${OpenCV_LIBS} comdlg32 ole32 uuid)
else()
    target_link_libraries(GeoGUIDemo PRIVATE GeoMatch imgui ${OpenCV_LIBS} ${OpenCV_LIBRARIES} comdlg32)
endif()
if(MSVC)
    # /utf-8: the source contains chinese labels, MSVC must not read it as cp936
    target_compile_options(GeoGUIDemo PRIVATE /utf-8 $<$<CONFIG:Release>:/O2 /arch:AVX2>)
endif()

# release build uses fast math + SIMD; MSVC also gets /arch:AVX2 when available
if(MSVC)
    target_compile_options(GeoMatch  PRIVATE $<$<CONFIG:Release>:/O2 /arch:AVX2>)
    target_compile_options(GeoMatchDemo PRIVATE $<$<CONFIG:Release>:/O2 /arch:AVX2>)
    target_compile_options(Demo1 PRIVATE $<$<CONFIG:Release>:/O2 /arch:AVX2>)
endif()

target_compile_definitions(GeoMatchDemo PRIVATE _USE_MATH_DEFINES)

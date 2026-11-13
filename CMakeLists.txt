cmake_minimum_required(VERSION 3.10.0)
project(GeoMatchDemo VERSION 1.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# 说明：Windows 下 32/64 位是两套 OpenCV，要靠 OPENCV_ROOT 区分。
#   x64: -DCMAKE_GENERATOR_PLATFORM=x64 -DOPENCV_ROOT=D:/Develop/OpenCV455/opencv/build
#   x86: -DCMAKE_GENERATOR_PLATFORM=Win32 -DOPENCV_ROOT=D:/Develop/OpenCV455_32
# Ninja 生成器没有 PLATFORM，改用 vcvars64/vcvars32 决定目标架构。



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

# BUILD_INTERFACE: 构建时用源码树；INSTALL_INTERFACE: 装好之后用 <sdk>/include。
# 只写 ${CMAKE_CURRENT_SOURCE_DIR} 会让 CMake 拒绝导出（"prefixed in the source directory"）。
target_include_directories(GeoMatch PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<INSTALL_INTERFACE:include>)
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
# 必须按 GUI 子系统链接（/SUBSYSTEM:WINDOWS）：
# 默认的 add_executable 是控制台子系统，双击/被拉起时会额外开一个黑窗口，
# 而且那个控制台常常盖在 GUI 上面，鼠标点不到界面。
# 源码用的是 main()，所以显式把入口指到 mainCRTStartup，否则 MSVC 会找 WinMain
# 并报 "unresolved external symbol WinMain"。
set_target_properties(GeoGUIDemo PROPERTIES WIN32_EXECUTABLE TRUE)
if(MSVC)
    target_link_options(GeoGUIDemo PRIVATE /ENTRY:mainCRTStartup)
endif()

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

#-----------------------------------------------------------------------
#  最小调用示例（构建出来保证 SDK 文档里的代码是能编译的）
#  GEOMATCH_BUILD_EXAMPLES=OFF 可以不编译它们
#-----------------------------------------------------------------------
option(GEOMATCH_BUILD_EXAMPLES "build the SDK examples" ON)
if(GEOMATCH_BUILD_EXAMPLES AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/examples")
    add_executable(example_shape_match examples/example_shape_match.cpp)
    target_link_libraries(example_shape_match PRIVATE GeoMatch)
    add_executable(example_ncc_match examples/example_ncc_match.cpp)
    target_link_libraries(example_ncc_match PRIVATE GeoMatch)
    if(MSVC)
        target_compile_options(example_shape_match PRIVATE /utf-8)
        target_compile_options(example_ncc_match PRIVATE /utf-8)
    endif()
endif()

#-----------------------------------------------------------------------
#  安装规则：gen 目录就是 SDK 的 layout
#      include/GeoMatch/GeoMatch.h          (facade)
#      include/ShapeMatch/ShapeMatch.h
#      include/NccMatch/NccMatch.h
#      lib/GeoMatch.lib  |  bin/GeoMatch.dll
#      lib/cmake/GeoMatch/GeoMatchConfig.cmake
#
#  cmake --install . --prefix <sdk> 即可得到可交付目录。
#-----------------------------------------------------------------------
include(GNUInstallDirs)
set(GEOMATCH_INSTALL_CMAKEDIR "${CMAKE_INSTALL_LIBDIR}/cmake/GeoMatch")

install(TARGETS GeoMatch
        EXPORT  GeoMatchTargets
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})

# 头文件按原目录结构安装（头里用的是 "ShapeMatch/ShapeMatch.h" 这种带子目录的写法），
# 消费方只需要 -I<sdk>/include。
install(FILES GeoMatch/GeoMatch.h     DESTINATION include/GeoMatch    COMPONENT headers)
install(FILES ShapeMatch/ShapeMatch.h DESTINATION include/ShapeMatch  COMPONENT headers)
install(FILES NccMatch/NccMatch.h     DESTINATION include/NccMatch    COMPONENT headers)

install(EXPORT GeoMatchTargets
        FILE GeoMatchTargets.cmake
        NAMESPACE GeoMatch::
        DESTINATION ${GEOMATCH_INSTALL_CMAKEDIR})

# 版本/目标文件，供 find_package(GeoMatch) 使用
include(CMakePackageConfigHelpers)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/GeoMatchConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)

file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/GeoMatchConfig.cmake.in"
"@PACKAGE_INIT@\n\ninclude(\"\${CMAKE_CURRENT_LIST_DIR}/GeoMatchTargets.cmake\")\n\n# 导出库的 include 目录，方便消费方直接 include_directories()\nget_target_property(_gm_inc GeoMatch::GeoMatch INTERFACE_INCLUDE_DIRECTORIES)\nset(GeoMatch_INCLUDE_DIRS \"\${_gm_inc}\")\nset(GeoMatch_LIBRARIES GeoMatch::GeoMatch)\nset(GeoMatch_VERSION \"@PROJECT_VERSION@\")\ncheck_required_components(GeoMatch)\n")
configure_package_config_file(
    "${CMAKE_CURRENT_BINARY_DIR}/GeoMatchConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/GeoMatchConfig.cmake"
    INSTALL_DESTINATION ${GEOMATCH_INSTALL_CMAKEDIR})

install(FILES
        "${CMAKE_CURRENT_BINARY_DIR}/GeoMatchConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/GeoMatchConfigVersion.cmake"
        DESTINATION ${GEOMATCH_INSTALL_CMAKEDIR})

# SDK 文档一起装进包（存在才装）
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/docs/SDK.md")
    install(FILES docs/SDK.md DESTINATION . COMPONENT docs)
endif()


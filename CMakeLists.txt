cmake_minimum_required(VERSION 3.15)
project(GeoMatchDemo VERSION 1.1.3 LANGUAGES C CXX)

#===============================================================================
#  手动编译（推荐，一条命令）
#-------------------------------------------------------------------------------
#  x64 :  tools\build-x64.bat          -> build\*.exe
#  x86 :  tools\build-x86.bat          -> build_x86\*.exe
#
#  等价的手工命令（Ninja 没有 PLATFORM，架构由 vcvars 决定，必须先 call）：
#      call "<你的 VS2019>\VC\Auxiliary\Build\vcvars64.bat"    (32 位用 vcvars32.bat)
#      cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -S . -B build
#      cmake --build build
#
#  想用 Visual Studio 生成器（不用先 call vcvars，单步调试更方便）：
#      cmake -G "Visual Studio 16 2019" -A x64 -S . -B build_vs
#      cmake --build build_vs --config Release
#
#  常见 -D 选项：
#      -DOPENCV_ROOT=<path>           指定 OpenCV（不指定就按 32/64 位自动找）
#      -DGEOMATCH_SIMD=OFF            不编 /arch:AVX2（给不支持 AVX2 的 CPU）
#      -DGEOMATCH_BUILD_GUI=OFF       不编 imgui 界面（只要库 + 命令行）
#      -DGEOMATCH_BUILD_SHARED=ON     把 GeoMatch 编成 dll
#      -DGEOMATCH_BUILD_EXAMPLES=OFF  不编 examples/
#
#  产物：
#      GeoMatch.lib      库（ShapeMatch 形状匹配 + NccMatch 灰度相关 + GeoMatch 门面）
#      GeoMatchDemo.exe  命令行（-t1 建模 / -t2 搜索 / 位置参数 = 图片目录）
#      NccDemo.exe       NCC 命令行（对标 create_ncc_model / find_ncc_model）
#      GeoGUIDemo.exe    imgui 界面（结果图 / 参数面板 / 日志）
#      AnalyzeCase.exe   批量评估工具
#      Demo1.exe         最小调用示例
#
#===============================================================================

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Release / Debug" FORCE)
endif()

#===============================================================================
#  OpenCV：32/64 位是两套，靠 OPENCV_ROOT 区分；不指定就按目标架构自动找。
#  （CMAKE_SIZEOF_VOID_P = 8 -> x64，4 -> x86）
#===============================================================================
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(GM_ARCH "x64")
    set(GM_OCV_SUBDIRS "x64/vc15" "x64/vc14")
    set(GM_OCV_CANDIDATES
        "D:/develop2/OpenCV455/opencv/build"
        "D:/Develop/OpenCV455/opencv/build"
        "C:/opencv/build")
else()
    set(GM_ARCH "x86")
    set(GM_OCV_SUBDIRS "x86/vc15" "x86/vc14")
    # 仓库里自带一套 32 位 OpenCV（include/ + opencv_world455.lib 平铺）
    set(GM_OCV_CANDIDATES
        "${CMAKE_CURRENT_SOURCE_DIR}/OpenCV455_MSVC32"
        "D:/Develop/OpenCV455_32"
        "D:/develop2/OpenCV455_32")
endif()

# ---- 1) OpenCV 根目录（可被 -DOPENCV_ROOT 覆盖） ----------------------------
set(OPENCV_ROOT "" CACHE PATH "OpenCV 构建根目录（含 include/opencv2，以及 <arch>/vcNN/lib 或平铺的 *.lib）")
if(NOT OPENCV_ROOT)
    foreach(cand ${GM_OCV_CANDIDATES})
        if(EXISTS "${cand}/include/opencv2")
            set(OPENCV_ROOT "${cand}")
            break()
        endif()
    endforeach()
endif()
if(NOT OPENCV_ROOT OR NOT EXISTS "${OPENCV_ROOT}/include/opencv2")
    message(FATAL_ERROR
        "找不到 OpenCV（${GM_ARCH}）。请用 -DOPENCV_ROOT=<path> 指定，"
        "该目录下要有 include/opencv2。尝试过的位置：${GM_OCV_CANDIDATES}")
endif()

set(OpenCV_INCLUDE_DIRS "${OPENCV_ROOT}/include")

# ---- 2) 库目录：<arch>/vcNN/lib 优先，其次平铺 ------------------------------
set(OpenCV_LIB_DIR "")
foreach(sub ${GM_OCV_SUBDIRS})
    if(EXISTS "${OPENCV_ROOT}/${sub}/lib")
        set(OpenCV_LIB_DIR "${OPENCV_ROOT}/${sub}/lib")
        break()
    endif()
endforeach()
if(NOT OpenCV_LIB_DIR)
    file(GLOB _flat "${OPENCV_ROOT}/opencv_world*.lib")
    if(_flat)
        set(OpenCV_LIB_DIR "${OPENCV_ROOT}")
    endif()
endif()
if(NOT OpenCV_LIB_DIR)
    message(FATAL_ERROR "在 ${OPENCV_ROOT} 下找不到 opencv_world*.lib（${GM_ARCH}）")
endif()

file(GLOB OpenCV_REL_LIBS "${OpenCV_LIB_DIR}/opencv_world*.lib")
list(FILTER OpenCV_REL_LIBS EXCLUDE REGEX "d\\.lib$")
file(GLOB OpenCV_DBG_LIBS "${OpenCV_LIB_DIR}/opencv_world*d.lib")
list(GET OpenCV_REL_LIBS 0 OpenCV_LIB_RELEASE)
# Debug 版同时给出：VS 生成器切到 Debug 时不会撞 LNK2038（RuntimeLibrary 不匹配）。
# 注意 IMPORTED 目标的 INTERFACE_LINK_LIBRARIES 不接受 optimized/debug 关键字，
# 必须用生成器表达式按配置选。
set(OpenCV_LIBS "$<$<NOT:$<CONFIG:Debug>>:${OpenCV_LIB_RELEASE}>")
if(OpenCV_DBG_LIBS)
    list(GET OpenCV_DBG_LIBS 0 OpenCV_LIB_DEBUG)
    list(APPEND OpenCV_LIBS "$<$<CONFIG:Debug>:${OpenCV_LIB_DEBUG}>")
endif()

# ---- 3) 运行时 dll：编完自动拷到 exe 旁边（以前要手工拷） -------------------
file(GLOB OpenCV_DLLS
    "${OPENCV_ROOT}/x64/vc15/bin/opencv_world*.dll"
    "${OPENCV_ROOT}/x86/vc15/bin/opencv_world*.dll"
    "${OPENCV_ROOT}/bin/opencv_world*.dll"
    "${OPENCV_ROOT}/opencv_world*.dll")

function(geomatch_copy_opencv_dll target)
    if(OpenCV_DLLS)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${OpenCV_DLLS}
                    "$<TARGET_FILE_DIR:${target}>"
            COMMENT "Copied OpenCV runtime dll next to ${target}")
    endif()
endfunction()


#===============================================================================
#  编译选项
#-------------------------------------------------------------------------------
# 源码是 UTF-8（无 BOM）且含中文注释/字面量；不加 /utf-8 时 MSVC 按 cp936 读，
# 会刷一屏 C4819，中文输出也是乱码。
#===============================================================================
option(GEOMATCH_SIMD "Release 编 /arch:AVX2（运行需要 AVX2 CPU；代码里会自动判断）" ON)
if(MSVC)
    add_compile_options(/utf-8 /W3)
    if(GEOMATCH_SIMD)
        add_compile_options(/arch:AVX2)
    endif()
    add_compile_definitions(_USE_MATH_DEFINES)
endif()

#-------------------------------------------------------------------------------
#  GeoMatch 库：ShapeMatch（形状/梯度）+ NccMatch（灰度相关）+ GeoMatch（门面）
#    GEOMATCH_BUILD_SHARED=ON  -> dll；OFF（默认）-> 静态库，免 dll hell
#-------------------------------------------------------------------------------
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
    # PRIVATE: 自己在编；PUBLIC: 消费方要定义 GEOMATCH_SHARED 才会 import dll
    target_compile_definitions(GeoMatch PRIVATE GEOMATCH_EXPORTS PUBLIC GEOMATCH_SHARED)
else()
    add_library(GeoMatch STATIC ${GEOMATCH_SOURCES})
    target_compile_definitions(GeoMatch PUBLIC GEOMATCH_STATIC)
endif()

# BUILD_INTERFACE: 构建时用源码树；INSTALL_INTERFACE: 装好之后用 <sdk>/include。
# 只写 ${CMAKE_CURRENT_SOURCE_DIR} 会让 install(EXPORT) 拒绝导出
# （"prefixed in the source directory"）。
#
# OpenCV 的头/库也包在 BUILD_INTERFACE 里：我们自己编 exe 时要传递（PUBLIC），
# 但**不写进导出的 GeoMatchTargets.cmake** —— 否则 SDK 到别的机器上会引用
# 这台机器的绝对路径而链接失败。SDK 消费方按 docs/SDK.md 自己链 OpenCV。
target_include_directories(GeoMatch PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<BUILD_INTERFACE:${OpenCV_INCLUDE_DIRS}>
    $<INSTALL_INTERFACE:include>)
target_link_libraries(GeoMatch PUBLIC
    "$<BUILD_INTERFACE:$<$<NOT:$<CONFIG:Debug>>:${OpenCV_LIB_RELEASE}>>")
if(OpenCV_LIB_DEBUG)
    target_link_libraries(GeoMatch PUBLIC
        "$<BUILD_INTERFACE:$<$<CONFIG:Debug>:${OpenCV_LIB_DEBUG}>>")
endif()

#-------------------------------------------------------------------------------
#  命令行前端（只做参数解析和打印）
#-------------------------------------------------------------------------------
add_executable(GeoMatchDemo main.cpp)
target_include_directories(GeoMatchDemo PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(GeoMatchDemo PRIVATE GeoMatch)

add_executable(Demo1 main1.cpp)
target_include_directories(Demo1 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(Demo1 PRIVATE GeoMatch)

# NCC 命令行前端（对标 Halcon create_ncc_model / find_ncc_model）
add_executable(NccDemo NccDemo.cpp)
target_include_directories(NccDemo PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(NccDemo PRIVATE GeoMatch)

# 批量评估：拿已有模型跑一批真实图片，打印每张的分数/位姿/耗时
add_executable(AnalyzeCase AnalyzeCase.cpp)
target_include_directories(AnalyzeCase PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(AnalyzeCase PRIVATE GeoMatch)

foreach(t GeoMatchDemo Demo1 NccDemo AnalyzeCase)
    geomatch_copy_opencv_dll(${t})
endforeach()

#===============================================================================
#  Dear ImGui - Win32 + DirectX11 backends（GeoGUIDemo 用）
#    GEOMATCH_BUILD_GUI=OFF 可以不编它
#===============================================================================
option(GEOMATCH_BUILD_GUI "build the imgui front end (GeoGUIDemo)" ON)
set(IMGUI_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/imgui/imgui/imgui-master/imgui-master")

if(GEOMATCH_BUILD_GUI AND EXISTS "${IMGUI_ROOT}/imgui.cpp")
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

    add_executable(GeoGUIDemo GeoGUIDemo.cpp)
    # 必须按 GUI 子系统链接（/SUBSYSTEM:WINDOWS）：
    # 默认的 add_executable 是控制台子系统，双击/被脚本拉起时会额外开一个黑窗口，
    # 那个控制台常常盖在 GUI 上面，鼠标点不到界面。
    # 源码用的是 main()，所以显式把入口指到 mainCRTStartup，否则 MSVC 会找 WinMain
    # 并报 "unresolved external symbol WinMain"。
    set_target_properties(GeoGUIDemo PROPERTIES WIN32_EXECUTABLE TRUE)
    if(MSVC)
        target_link_options(GeoGUIDemo PRIVATE /ENTRY:mainCRTStartup)
    endif()
    target_include_directories(GeoGUIDemo PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR} ${IMGUI_ROOT} ${IMGUI_ROOT}/backends)
    target_link_libraries(GeoGUIDemo PRIVATE GeoMatch imgui comdlg32 ole32 uuid)
    geomatch_copy_opencv_dll(GeoGUIDemo)
elseif(GEOMATCH_BUILD_GUI)
    message(STATUS "imgui 源码不存在，跳过 GeoGUIDemo：${IMGUI_ROOT}")
endif()

#===============================================================================
#  最小调用示例（编出来保证 SDK 文档里的代码是能编译的）
#===============================================================================
option(GEOMATCH_BUILD_EXAMPLES "build the SDK examples" ON)
if(GEOMATCH_BUILD_EXAMPLES AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/examples")
    add_executable(example_shape_match examples/example_shape_match.cpp)
    target_link_libraries(example_shape_match PRIVATE GeoMatch)
    add_executable(example_ncc_match examples/example_ncc_match.cpp)
    target_link_libraries(example_ncc_match PRIVATE GeoMatch)
    geomatch_copy_opencv_dll(example_shape_match)
    geomatch_copy_opencv_dll(example_ncc_match)
endif()

#===============================================================================
#  安装规则：装出来的目录就是 SDK 的 layout
#      include/GeoMatch/GeoMatch.h          (facade)
#      include/ShapeMatch/ShapeMatch.h
#      include/NccMatch/NccMatch.h
#      lib/GeoMatch.lib  |  bin/GeoMatch.dll
#      lib/cmake/GeoMatch/GeoMatchConfig.cmake
#
#    cmake --install . --prefix <sdk>
#===============================================================================
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

# 版本/配置文件，供 find_package(GeoMatch) 使用
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

#===============================================================================
#  配置完成摘要
#===============================================================================
message(STATUS "")
message(STATUS "================ GeoMatch 配置完成 ================")
message(STATUS "  架构        : ${GM_ARCH}")
message(STATUS "  配置        : ${CMAKE_BUILD_TYPE}${CMAKE_CONFIGURATION_TYPES}")
message(STATUS "  生成器      : ${CMAKE_GENERATOR}")
message(STATUS "  OpenCV      : ${OPENCV_ROOT}")
message(STATUS "  OpenCV lib  : ${OpenCV_LIB_RELEASE}")
message(STATUS "  AVX2        : ${GEOMATCH_SIMD}")
message(STATUS "  GUI 界面    : ${GEOMATCH_BUILD_GUI}")
message(STATUS "  输出目录    : ${CMAKE_BINARY_DIR}")
message(STATUS "  下一步      : cmake --build ${CMAKE_BINARY_DIR}")
message(STATUS "==================================================")


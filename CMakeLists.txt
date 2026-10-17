cmake_minimum_required(VERSION 3.10.0)
project(GeoMatchDemo VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(OPENCV_ROOT "D:/Develop/OpenCV455/opencv/build" CACHE PATH "OpenCV build/install root")

if(EXISTS "${OPENCV_ROOT}/OpenCVConfig.cmake")
    set(OpenCV_DIR "${OPENCV_ROOT}")
    find_package(OpenCV QUIET)
endif()

if(NOT OpenCV_FOUND)
    message(STATUS "OpenCVConfig.cmake not usable -> falling back to explicit paths")
    set(OPENCV_INCLUDE_DIR "${OPENCV_ROOT}/include")
    if(EXISTS "${OPENCV_ROOT}/x64/vc15/lib")
        set(OPENCV_LIB_DIR "${OPENCV_ROOT}/x64/vc15/lib")
    else()
        set(OPENCV_LIB_DIR "${OPENCV_ROOT}/x64/vc14/lib")
    endif()
    set(OpenCV_INCLUDE_DIRS "${OPENCV_INCLUDE_DIR}")
    set(OpenCV_LIBS
        optimized "${OPENCV_LIB_DIR}/opencv_world455.lib"
        debug     "${OPENCV_LIB_DIR}/opencv_world455d.lib")
endif()

include_directories(${OpenCV_INCLUDE_DIRS})

#-----------------------------------------------------------------------
#  GeoMatch library: shape matcher core + high level facade.
#  GEOMATCH_BUILD_SHARED=ON  -> shared library (dll / so)
#  GEOMATCH_BUILD_SHARED=OFF -> static library (default, no dll hell)
#-----------------------------------------------------------------------
option(GEOMATCH_BUILD_SHARED "build GeoMatch as a shared library" OFF)

set(GEOMATCH_SOURCES
    ShapeMatch/ShapeMatch.cpp
    ShapeMatch/ShapeMatch.h
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

target_include_directories(GeoMatchDemo PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(GeoMatchDemo PRIVATE GeoMatch)

# release build uses fast math + SIMD; MSVC also gets /arch:AVX2 when available
if(MSVC)
    target_compile_options(GeoMatch  PRIVATE $<$<CONFIG:Release>:/O2 /arch:AVX2>)
    target_compile_options(GeoMatchDemo PRIVATE $<$<CONFIG:Release>:/O2 /arch:AVX2>)
endif()

target_compile_definitions(GeoMatchDemo PRIVATE _USE_MATH_DEFINES)

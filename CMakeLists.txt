cmake_minimum_required(VERSION 3.16)
project(ScreenMirror)

set(CMAKE_CXX_STANDARD 17)

set(TURBOJPEG_DIR "${CMAKE_SOURCE_DIR}/turbojpeg")
include_directories(${TURBOJPEG_DIR})

set(SOURCES
    src/main.cpp
    src/mirror_net.cpp
    src/mirror_receiver.cpp
    src/hl_common.cpp
    src/hud_overlay.cpp
    src/perf_logger.cpp
    src/d3d11_renderer.cpp
)

if(WIN32)
    add_executable(ScreenMirror WIN32 ${SOURCES})
    target_link_libraries(ScreenMirror
        ws2_32
        d3d11
        dxgi
        d3dcompiler
        winmm
        ${TURBOJPEG_DIR}/turbojpeg-static.lib
    )
else()
    message(FATAL_ERROR "Only Windows is supported")
endif()
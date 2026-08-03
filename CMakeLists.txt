cmake_minimum_required(VERSION 3.16)
project(ScreenMirror)

set(CMAKE_CXX_STANDARD 17)

# 🔴 关键修复：强制使用 winsock2，禁止 winsock.h
add_compile_definitions(WIN32_LEAN_AND_MEAN)
add_compile_definitions(NOMINMAX)
add_compile_definitions(_WINSOCKAPI_)

set(TURBOJPEG_DIR "${CMAKE_SOURCE_DIR}/turbojpeg")
include_directories(${TURBOJPEG_DIR}/include)

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
        ${TURBOJPEG_DIR}/lib/turbojpeg-static.lib
    )
else()
    message(FATAL_ERROR "Only Windows is supported")
endif()

# =============================================================================
# 屏幕局部采集与 UDP 推流工具 - CMake 配置
# 技术栈: C++17, ImGui(DX11), WGC, TurboJPEG, Winsock2
# 目标: WIN32 子系统，隐藏控制台
# 依赖: vcpkg 安装 imgui[dx11-binding,win32-binding] 与 libjpeg-turbo
# =============================================================================

cmake_minimum_required(VERSION 3.16)
project(NetStreamer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)

# -----------------------------------------------------------------------------
# WIN32 应用程序：不显示控制台窗口
# -----------------------------------------------------------------------------
add_executable(${PROJECT_NAME} WIN32
    main.cpp
    ui.cpp
    streamer.cpp
)

# -----------------------------------------------------------------------------
# ImGui：vcpkg 提供（需安装 imgui[dx11-binding,win32-binding]）
# -----------------------------------------------------------------------------
find_package(imgui CONFIG REQUIRED)
target_link_libraries(${PROJECT_NAME} PRIVATE imgui::imgui)

# -----------------------------------------------------------------------------
# TurboJPEG：vcpkg 提供 libjpeg-turbo
# -----------------------------------------------------------------------------
find_package(libjpeg-turbo CONFIG REQUIRED)
target_link_libraries(${PROJECT_NAME} PRIVATE libjpeg-turbo::turbojpeg)

# -----------------------------------------------------------------------------
# Windows SDK：D3D11, DXGI, Win32, Winsock2, C++/WinRT (WGC)
# -----------------------------------------------------------------------------
target_compile_definitions(${PROJECT_NAME} PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    d3d11
    dxgi
    dxguid
    dwmapi
    ws2_32
)

# C++/WinRT：用于 Windows.Graphics.Capture（仅需 Windows SDK 头与运行时）
target_compile_options(${PROJECT_NAME} PRIVATE /permissive-)

# -----------------------------------------------------------------------------
# 包含目录（当前工程）
# -----------------------------------------------------------------------------
target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

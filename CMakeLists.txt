cmake_minimum_required(VERSION 3.24)
project(ScreenSender LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "本项目只能在 Windows 上构建。")
endif()

if(NOT CMAKE_CONFIGURATION_TYPES)
    if(NOT CMAKE_BUILD_TYPE)
        set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    endif()
    if(NOT CMAKE_BUILD_TYPE STREQUAL "Release")
        message(FATAL_ERROR "必须使用 Release 模式构建。")
    endif()
else()
    set(CMAKE_CONFIGURATION_TYPES "Release" CACHE STRING "" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_definitions(-D_WIN32_WINNT=0x0A00)
add_definitions(-DUNICODE -D_UNICODE)
add_definitions(-DWINRT_LEAN_AND_MEAN)

set(CMAKE_EXE_LINKER_FLAGS_RELEASE "${CMAKE_EXE_LINKER_FLAGS_RELEASE} /DEBUG:NONE /INCREMENTAL:NO")
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /DNDEBUG /O2 /Gy /GL")

if(NOT DEFINED VCPKG_TARGET_TRIPLET)
    set(VCPKG_TARGET_TRIPLET "x64-windows" CACHE STRING "vcpkg triplet")
endif()

set(SCREENSENDER_SOURCES
    src/auth_keyauth.cpp
    src/capture_wgc.cpp
    src/encoder_jpeg.cpp
    src/udp_sender.cpp
    src/app_core.cpp
    src/ui.cpp
    src/main.cpp
)

add_executable(ScreenSender ${SCREENSENDER_SOURCES})

set_target_properties(ScreenSender PROPERTIES
    WIN32_EXECUTABLE OFF
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/out/Release"
)

target_include_directories(ScreenSender PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

find_package(cppwinrt CONFIG REQUIRED)
find_package(libjpeg-turbo CONFIG REQUIRED)
find_package(imgui CONFIG REQUIRED)

if(TARGET libjpeg-turbo::turbojpeg)
    set(JPEG_TURBO_TARGET libjpeg-turbo::turbojpeg)
elseif(TARGET libjpeg-turbo::turbojpeg-static)
    set(JPEG_TURBO_TARGET libjpeg-turbo::turbojpeg-static)
else()
    message(FATAL_ERROR "找到了 libjpeg-turbo，但没有找到 turbojpeg 可链接目标。")
endif()

target_link_libraries(ScreenSender PRIVATE
    ${JPEG_TURBO_TARGET}
    imgui::imgui
    Microsoft::CppWinRT
    WindowsApp
    d3d11
    dxgi
    d3dcompiler
    imm32
    ws2_32
    user32
    winhttp
)

if(MSVC)
    target_compile_options(ScreenSender PRIVATE
        /W4
        /permissive-
        /Zc:__cplusplus
        /utf-8
    )
    set_property(TARGET ScreenSender PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
endif()

target_compile_definitions(ScreenSender PRIVATE WIN32_LEAN_AND_MEAN)
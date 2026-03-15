# NetStreamer / DiagTool

屏幕局部采集与 UDP 推流工具。无边框高端电竞风格 ImGui 界面，WGC 采集屏幕中心 640×640，TurboJPEG 压缩后通过 UDP 发送。

## 环境要求

- Windows 10 1803 及以上（WGC 需要）
- Visual Studio 2019/2022 或 CMake + MSVC
- Windows SDK（含 C++/WinRT 与 `windows.graphics.capture.interop.h`）
- vcpkg（推荐，用于 libjpeg-turbo；ImGui 由 CMake FetchContent 拉取）

## 依赖安装（vcpkg）

```bash
vcpkg install libjpeg-turbo:x64-windows
```

ImGui 由 CMake 通过 FetchContent 自动拉取。

## 编译

```bash
cd "d:\Desktop\PC shuangji Ai game\UDP\Text"
mkdir build
cd build
cmake .. -DCMAKE_TOOLCHAIN_FILE=[vcpkg]/scripts/buildsystems/vcpkg.cmake
cmake --build . --config Release
```

运行后为无控制台 GUI，主窗口约 550×400，支持顶部区域拖拽移动。

## 功能说明

- **数据链路**：目标 IP/端口、JPEG 画质(30–100)、帧率(30–144)、启动/断开推流。
- **安全防护**：WDA 界面隐身（防截图）、擦除 WGC 黄框、重置窗口标题。
- **运行状态**：实时帧数、字节数、实际 FPS。

推流在后台线程执行：WGC 抓取屏幕中心 640×640 → TurboJPEG 压缩 → UDP 发送，并按设定 FPS 休眠。

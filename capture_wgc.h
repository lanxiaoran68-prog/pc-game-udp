#pragma once

#include <vector>
#include <cstdint>
#include <string>
#include <windows.h>
#include <wrl/client.h>

// C++/WinRT
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

// D3D11 / DXGI
#include <d3d11.h>
#include <dxgi1_2.h>

// 只在本模块使用 WRL，不抛异常
using Microsoft::WRL::ComPtr;

// 简单日志接口（在其它模块里实现或直接映射到 printf/OutputDebugString）
// 这里仅声明，避免循环依赖。
void LogInfo(const char* fmt, ...);
void LogError(const char* fmt, ...);
struct GlobalLogLine
{
    bool isError = false;
    std::string text;
};
void ConsumeGlobalLogs(std::vector<GlobalLogLine>& outLines);

// WGC 捕获封装：只在构造函数中完成一次性初始化（敏感 API 不在主循环频繁调用）
class ScreenCaptureWGC
{
public:
    ScreenCaptureWGC();
    ~ScreenCaptureWGC();

    // 初始化一次：创建 D3D11 设备、WGC FramePool、Session 等
    // - 返回 true 表示初始化成功，可以开始抓帧
    // - 返回 false 表示初始化失败，外层可选择退出或重试
    bool Initialize();

    // 是否初始化成功（供外部检查）
    bool IsInitialized() const noexcept { return m_initialized; }

    // 抓一帧并拷贝到 CPU：输出为当前裁剪区域 BGRA 缓冲区
    //
    // - 输出：
    //   outBuffer:  以 BGRA 排列的原始像素数据（大小 = outStride * outHeight）
    //   outWidth:   实际宽度（由当前 SetOutputSize / cropMode 决定）
    //   outHeight:  实际高度（由当前 SetOutputSize / cropMode 决定）
    //   outStride:  每行字节数（= outWidth * 4）
    //
    // - 返回：
    //   true  表示成功抓到一帧并拷贝到 CPU
    //   false 表示失败（静默处理，已写日志），调用方可选择略过本帧
    //
    // **注意**：
    // - 不抛出任何异常，所有失败路径通过返回 false 表达
    bool CaptureFrame(std::vector<uint8_t>& outBuffer,
                      int& outWidth,
                      int& outHeight,
                      int& outStride);

    // 控制 WGC 边缘标记（部分系统/版本可用；不可用时静默忽略）
    void SetHideBorder(bool hide) noexcept;
    void SetOutputSize(int width, int height) noexcept;
    void SetCenterCropMode() noexcept;
    void SetCustomCropMode(int x, int y, int w, int h) noexcept;

    struct HideBorderStatus
    {
        bool probed = false;       // 是否探测过 API 支持性
        bool supported = false;    // 系统是否支持
        bool desiredHide = false;  // 当前期望配置值
        bool applied = false;      // 是否已成功应用到 session（仅在 supported 时有意义）
    };

    HideBorderStatus GetHideBorderStatus() const noexcept { return m_hideBorderStatus; }
    RECT GetCurrentCropRect() const noexcept { return m_cropRect; }
    RECT GetFullCaptureRect() const noexcept { return m_fullMonitorRect; }

private:
    // 内部辅助：创建 D3D11 设备（硬件优先）
    bool CreateD3DDevice();

    // 内部辅助：找到主显示器（只调用一次 EnumDisplayMonitors）
    bool GetPrimaryMonitor(HMONITOR& outMonitor);

    // 内部辅助：根据主显示器创建 GraphicsCaptureItem，只调用一次
    bool CreateCaptureItemForPrimaryMonitor();

    // 初始化 WGC FramePool & Session，只调用一次
    bool InitializeFramePoolAndSession();

    // 根据当前显示分辨率计算屏幕中心的裁剪区域
    // 输入：全屏矩形（full monitor rect）
    RECT ComputeCenterCropRect(const RECT& fullRect) const;
    RECT ComputeCustomCropRect(const RECT& fullRect) const;

    // 从 WGC 抓到的 GPU 纹理中，拷贝裁剪区域到 CPU 内存（BGRA）
    bool CopyFrameToCpuBgra(ComPtr<ID3D11Texture2D> srcTexture,
                            const RECT& cropRect,
                            std::vector<uint8_t>& outBuffer,
                            int& outWidth,
                            int& outHeight,
                            int& outStride);

private:
    bool m_initialized = false;
    bool m_hideBorder = false;
    bool m_borderApiKnown = false;
    bool m_borderApiSupported = false;
    HideBorderStatus m_hideBorderStatus{};

    // D3D11 设备/上下文
    ComPtr<ID3D11Device>        m_d3dDevice;
    ComPtr<ID3D11DeviceContext> m_d3dContext;

    // WGC / Direct3D11
    winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice m_winrtDevice{ nullptr };
    winrt::Windows::Graphics::Capture::GraphicsCaptureItem         m_captureItem{ nullptr };
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool  m_framePool{ nullptr };
    winrt::Windows::Graphics::Capture::GraphicsCaptureSession      m_session{ nullptr };

    // 裁剪输出参数（由 m_targetWidth/m_targetHeight 决定，支持动态输出尺寸）
    int m_targetWidth  = 640;
    int m_targetHeight = 640;
    bool m_useCustomCrop = false;
    RECT m_customCropRect{};

    // 全屏矩形 + 裁剪矩形（根据主屏幕分辨率计算）
    RECT m_fullMonitorRect{};
    RECT m_cropRect{};
};


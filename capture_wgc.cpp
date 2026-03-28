#include "capture_wgc.h"

#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <vector>
#include <deque>
#include <mutex>
#include <windows.h>

#include <roapi.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <windows.graphics.capture.h>
#include <windows.graphics.capture.interop.h>

// C++/WinRT 命名空间
using namespace winrt;
using namespace winrt::Windows::Graphics;
using namespace winrt::Windows::Graphics::Capture;
using namespace winrt::Windows::Graphics::DirectX;
using namespace winrt::Windows::Graphics::DirectX::Direct3D11;

// --------------------- 简单日志实现 ---------------------
static std::mutex g_logMutex;
static std::deque<GlobalLogLine> g_logQueue;
static constexpr size_t kMaxGlobalLogs = 1000;

void LogV(const char* level, const char* fmt, va_list args)
{
    char buf[1024] = {};
    std::vsnprintf(buf, sizeof(buf), fmt, args);

    std::printf("[%s] ", level);
    std::printf("%s", buf);
    std::printf("\n");

    {
        std::lock_guard<std::mutex> lk(g_logMutex);
        GlobalLogLine line;
        line.isError = (std::strcmp(level, "ERROR") == 0);
        line.text = std::string("[") + level + "] " + buf;
        g_logQueue.push_back(std::move(line));
        while (g_logQueue.size() > kMaxGlobalLogs)
            g_logQueue.pop_front();
    }
}

void LogInfo(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    LogV("INFO", fmt, args);
    va_end(args);
}

void LogError(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    LogV("ERROR", fmt, args);
    va_end(args);
}

void ConsumeGlobalLogs(std::vector<GlobalLogLine>& outLines)
{
    outLines.clear();
    std::lock_guard<std::mutex> lk(g_logMutex);
    outLines.insert(outLines.end(), g_logQueue.begin(), g_logQueue.end());
    g_logQueue.clear();
}

// --------------------- ScreenCaptureWGC 实现 ---------------------

ScreenCaptureWGC::ScreenCaptureWGC()
{
    // 初始化 C++/WinRT apartment，不依赖异常
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
}

ScreenCaptureWGC::~ScreenCaptureWGC()
{
    if (m_session)
    {
        m_session.Close();
        m_session = nullptr;
    }

    if (m_framePool)
    {
        m_framePool.Close();
        m_framePool = nullptr;
    }

    m_captureItem = nullptr;
    m_winrtDevice = nullptr;
}

bool ScreenCaptureWGC::Initialize()
{
    if (m_initialized)
    {
        return true;
    }

    if (!CreateD3DDevice())
    {
        LogError("CreateD3DDevice failed");
        return false;
    }

    HMONITOR hPrimary = nullptr;
    if (!GetPrimaryMonitor(hPrimary))
    {
        LogError("GetPrimaryMonitor failed");
        return false;
    }

    if (!CreateCaptureItemForPrimaryMonitor())
    {
        LogError("CreateCaptureItemForPrimaryMonitor failed");
        return false;
    }

    if (!InitializeFramePoolAndSession())
    {
        LogError("InitializeFramePoolAndSession failed");
        return false;
    }

    m_cropRect = m_useCustomCrop ? ComputeCustomCropRect(m_fullMonitorRect)
                                 : ComputeCenterCropRect(m_fullMonitorRect);

    LogInfo("ScreenCaptureWGC initialized. FullRect=(%ld,%ld,%ld,%ld), CropRect=(%ld,%ld,%ld,%ld)",
            m_fullMonitorRect.left,
            m_fullMonitorRect.top,
            m_fullMonitorRect.right,
            m_fullMonitorRect.bottom,
            m_cropRect.left,
            m_cropRect.top,
            m_cropRect.right,
            m_cropRect.bottom);

    m_initialized = true;
    return true;
}

bool ScreenCaptureWGC::CreateD3DDevice()
{
    UINT creationFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;

    D3D_FEATURE_LEVEL featureLevels[] =
    {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
    };

    D3D_FEATURE_LEVEL featureLevel{};
    HRESULT hr = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        creationFlags,
        featureLevels,
        ARRAYSIZE(featureLevels),
        D3D11_SDK_VERSION,
        m_d3dDevice.GetAddressOf(),
        &featureLevel,
        m_d3dContext.GetAddressOf());

    if (FAILED(hr))
    {
        LogError("D3D11CreateDevice failed. hr=0x%08X", hr);
        return false;
    }

    ComPtr<IDXGIDevice> dxgiDevice;
    hr = m_d3dDevice.As(&dxgiDevice);
    if (FAILED(hr))
    {
        LogError("ID3D11Device::As(IDXGIDevice) failed. hr=0x%08X", hr);
        return false;
    }

    winrt::com_ptr<IInspectable> inspectableDevice;
    hr = CreateDirect3D11DeviceFromDXGIDevice(
        dxgiDevice.Get(),
        inspectableDevice.put());
    if (FAILED(hr) || !inspectableDevice)
    {
        LogError("CreateDirect3D11DeviceFromDXGIDevice failed. hr=0x%08X", hr);
        return false;
    }

    m_winrtDevice = inspectableDevice.as<IDirect3DDevice>();
    if (!m_winrtDevice)
    {
        LogError("Failed to cast IInspectable to IDirect3DDevice.");
        return false;
    }

    return true;
}

bool ScreenCaptureWGC::GetPrimaryMonitor(HMONITOR& outMonitor)
{
    outMonitor = nullptr;

    struct EnumData
    {
        HMONITOR hPrimary = nullptr;
        RECT     rect{};
    } data;

    auto MonitorEnumProc = [](HMONITOR hMon, HDC, LPRECT lprc, LPARAM lp) -> BOOL
    {
        (void)lprc;
        EnumData* p = reinterpret_cast<EnumData*>(lp);
        MONITORINFOEXA info{};
        info.cbSize = sizeof(info);
        if (!GetMonitorInfoA(hMon, &info))
        {
            return TRUE;
        }

        if (info.dwFlags & MONITORINFOF_PRIMARY)
        {
            p->hPrimary = hMon;
            p->rect = info.rcMonitor;
            return FALSE;
        }
        return TRUE;
    };

    // 重置错误码，方便后续判断真实失败
    SetLastError(0);
    BOOL enumResult = EnumDisplayMonitors(nullptr, nullptr, MonitorEnumProc, reinterpret_cast<LPARAM>(&data));

    // 无论 enumResult 是 TRUE 还是 FALSE，只要找到了主显示器就视为成功
    if (data.hPrimary)
    {
        outMonitor = data.hPrimary;
        m_fullMonitorRect = data.rect;
        return true;
    }

    // 没有找到主显示器，才认为失败，此时再查看返回值和错误码
    DWORD err = GetLastError();
    LogError("Primary monitor not found. EnumDisplayMonitors result=%d, GetLastError=%lu",
             static_cast<int>(enumResult), err);
    return false;
}

bool ScreenCaptureWGC::CreateCaptureItemForPrimaryMonitor()
{
    HMONITOR hMon = MonitorFromRect(&m_fullMonitorRect, MONITOR_DEFAULTTOPRIMARY);
    if (!hMon)
    {
        LogError("MonitorFromRect failed. GetLastError=%lu", GetLastError());
        return false;
    }

    // 使用 RoGetActivationFactory 获取 IGraphicsCaptureItemInterop（Win32 桌面互操作）
    winrt::hstring className = L"Windows.Graphics.Capture.GraphicsCaptureItem";
    winrt::com_ptr<::IGraphicsCaptureItemInterop> interop;
    HRESULT hr = RoGetActivationFactory(
        reinterpret_cast<HSTRING>(winrt::get_abi(className)),
        __uuidof(::IGraphicsCaptureItemInterop),
        interop.put_void());

    if (FAILED(hr) || interop.get() == nullptr)
    {
        LogError("Failed to get IGraphicsCaptureItemInterop. hr=0x%08X", hr);
        return false;
    }

    GraphicsCaptureItem item{ nullptr };
    hr = interop->CreateForMonitor(
        hMon,
        __uuidof(ABI::Windows::Graphics::Capture::IGraphicsCaptureItem),
        reinterpret_cast<void**>(winrt::put_abi(item)));

    if (FAILED(hr) || !item)
    {
        LogError("IGraphicsCaptureItemInterop::CreateForMonitor failed. hr=0x%08X", hr);
        return false;
    }

    m_captureItem = item;
    return true;
}

bool ScreenCaptureWGC::InitializeFramePoolAndSession()
{
    if (!m_captureItem || !m_winrtDevice)
    {
        LogError("InitializeFramePoolAndSession: capture item or device is null.");
        return false;
    }

    auto size = m_captureItem.Size();
    int width = static_cast<int>(size.Width);
    int height = static_cast<int>(size.Height);
    if (width <= 0 || height <= 0)
    {
        LogError("Capture item size invalid: %d x %d", width, height);
        return false;
    }

    constexpr int frameCount = 3;
    m_framePool = Direct3D11CaptureFramePool::Create(
        m_winrtDevice,
        DirectXPixelFormat::B8G8R8A8UIntNormalized,
        frameCount,
        size);

    if (!m_framePool)
    {
        LogError("Direct3D11CaptureFramePool::Create failed.");
        return false;
    }

    m_session = m_framePool.CreateCaptureSession(m_captureItem);
    if (!m_session)
    {
        LogError("GraphicsCaptureSession::CreateCaptureSession failed.");
        return false;
    }

    // 应用隐藏边缘标记设置（如果系统支持该属性）
    SetHideBorder(m_hideBorder);

    m_session.StartCapture();
    return true;
}

void ScreenCaptureWGC::SetHideBorder(bool hide) noexcept
{
    m_hideBorder = hide;
    m_hideBorderStatus.desiredHide = hide;
    if (!m_session)
        return;

    // 某些 Windows 版本/SDK 支持 IsBorderRequired；
    // 为避免在 WorkerLoop 中反复触发异常，这里只探测一次，后续走快速分支。
    if (m_borderApiKnown && !m_borderApiSupported)
    {
        m_hideBorderStatus.probed = true;
        m_hideBorderStatus.supported = false;
        m_hideBorderStatus.applied = false;
        return;
    }

    try
    {
        // hideBorder=true => 不需要边框提示
        m_session.IsBorderRequired(!hide);
        m_borderApiKnown = true;
        m_borderApiSupported = true;
        m_hideBorderStatus.probed = true;
        m_hideBorderStatus.supported = true;
        m_hideBorderStatus.applied = true;
    }
    catch (...)
    {
        m_borderApiKnown = true;
        m_borderApiSupported = false;
        m_hideBorderStatus.probed = true;
        m_hideBorderStatus.supported = false;
        m_hideBorderStatus.applied = false;
        // 静默忽略：平台不支持时保持兼容
    }
}

void ScreenCaptureWGC::SetOutputSize(int width, int height) noexcept
{
    if (width <= 0 || height <= 0)
        return;
    m_targetWidth = width;
    m_targetHeight = height;
    m_cropRect = m_useCustomCrop ? ComputeCustomCropRect(m_fullMonitorRect)
                                 : ComputeCenterCropRect(m_fullMonitorRect);
}

void ScreenCaptureWGC::SetCenterCropMode() noexcept
{
    m_useCustomCrop = false;
    m_cropRect = ComputeCenterCropRect(m_fullMonitorRect);
}

void ScreenCaptureWGC::SetCustomCropMode(int x, int y, int w, int h) noexcept
{
    m_useCustomCrop = true;
    m_customCropRect.left = x;
    m_customCropRect.top = y;
    m_customCropRect.right = x + w;
    m_customCropRect.bottom = y + h;
    m_cropRect = ComputeCustomCropRect(m_fullMonitorRect);
}

RECT ScreenCaptureWGC::ComputeCenterCropRect(const RECT& fullRect) const
{
    int fullWidth = fullRect.right - fullRect.left;
    int fullHeight = fullRect.bottom - fullRect.top;

    int cropWidth = m_targetWidth;
    int cropHeight = m_targetHeight;

    if (cropWidth > fullWidth)  cropWidth = fullWidth;
    if (cropHeight > fullHeight) cropHeight = fullHeight;

    int left = fullRect.left + (fullWidth - cropWidth) / 2;
    int top = fullRect.top + (fullHeight - cropHeight) / 2;

    RECT r{};
    r.left = left;
    r.top = top;
    r.right = left + cropWidth;
    r.bottom = top + cropHeight;
    return r;
}

RECT ScreenCaptureWGC::ComputeCustomCropRect(const RECT& fullRect) const
{
    RECT r = m_customCropRect;
    const int fw = fullRect.right - fullRect.left;
    const int fh = fullRect.bottom - fullRect.top;
    if (fw <= 0 || fh <= 0)
        return ComputeCenterCropRect(fullRect);

    if (r.left < fullRect.left) r.left = fullRect.left;
    if (r.top < fullRect.top) r.top = fullRect.top;
    if (r.right > fullRect.right) r.right = fullRect.right;
    if (r.bottom > fullRect.bottom) r.bottom = fullRect.bottom;

    if (r.right <= r.left || r.bottom <= r.top)
        return ComputeCenterCropRect(fullRect);
    return r;
}

bool ScreenCaptureWGC::CopyFrameToCpuBgra(ComPtr<ID3D11Texture2D> srcTexture,
                                          const RECT& cropRect,
                                          std::vector<uint8_t>& outBuffer,
                                          int& outWidth,
                                          int& outHeight,
                                          int& outStride)
{
    if (!srcTexture)
    {
        LogError("CopyFrameToCpuBgra: srcTexture is null.");
        return false;
    }

    D3D11_TEXTURE2D_DESC desc{};
    srcTexture->GetDesc(&desc);

    D3D11_TEXTURE2D_DESC stagingDesc = desc;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDesc.MiscFlags = 0;

    ComPtr<ID3D11Texture2D> stagingTexture;
    HRESULT hr = m_d3dDevice->CreateTexture2D(&stagingDesc, nullptr, stagingTexture.GetAddressOf());
    if (FAILED(hr))
    {
        LogError("CreateTexture2D (staging) failed. hr=0x%08X", hr);
        return false;
    }

    m_d3dContext->CopyResource(stagingTexture.Get(), srcTexture.Get());

    D3D11_MAPPED_SUBRESOURCE mapped{};
    hr = m_d3dContext->Map(stagingTexture.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr))
    {
        LogError("ID3D11DeviceContext::Map failed. hr=0x%08X", hr);
        return false;
    }

    int fullWidth = static_cast<int>(desc.Width);
    int fullHeight = static_cast<int>(desc.Height);

    RECT r = cropRect;
    if (r.left < 0) r.left = 0;
    if (r.top < 0) r.top = 0;
    if (r.right > fullWidth) r.right = fullWidth;
    if (r.bottom > fullHeight) r.bottom = fullHeight;

    int cropWidth = r.right - r.left;
    int cropHeight = r.bottom - r.top;

    if (cropWidth <= 0 || cropHeight <= 0)
    {
        m_d3dContext->Unmap(stagingTexture.Get(), 0);
        LogError("CopyFrameToCpuBgra: invalid crop rect.");
        return false;
    }

    outWidth = cropWidth;
    outHeight = cropHeight;
    outStride = cropWidth * 4;

    outBuffer.resize(static_cast<size_t>(outStride) * outHeight);

    uint8_t* dst = outBuffer.data();
    const uint8_t* srcBase = static_cast<const uint8_t*>(mapped.pData);

    for (int y = 0; y < cropHeight; ++y)
    {
        const uint8_t* srcRow = srcBase +
                                static_cast<size_t>(y + r.top) * mapped.RowPitch +
                                static_cast<size_t>(r.left) * 4;
        uint8_t* dstRow = dst + static_cast<size_t>(y) * outStride;

        std::memcpy(dstRow, srcRow, static_cast<size_t>(outStride));
    }

    m_d3dContext->Unmap(stagingTexture.Get(), 0);

    LogInfo("Captured %dx%d frame, %zu bytes", cropWidth, cropHeight, outBuffer.size());
    return true;
}

bool ScreenCaptureWGC::CaptureFrame(std::vector<uint8_t>& outBuffer,
                                    int& outWidth,
                                    int& outHeight,
                                    int& outStride)
{
    outBuffer.clear();
    outWidth = outHeight = outStride = 0;

    if (!m_initialized)
    {
        LogError("CaptureFrame called before Initialize.");
        return false;
    }

    if (!m_framePool || !m_session)
    {
        LogError("CaptureFrame: frame pool or session is null.");
        return false;
    }

    Direct3D11CaptureFrame frame = m_framePool.TryGetNextFrame();
    if (!frame)
    {
        // 无新帧，静默返回 false
        return false;
    }

    auto surface = frame.Surface();
    if (!surface)
    {
        LogError("CaptureFrame: surface is null.");
        return false;
    }

    // 使用 IUnknown* + QueryInterface 获取 IDirect3DDxgiInterfaceAccess
    IUnknown* surfaceUnknown = reinterpret_cast<IUnknown*>(winrt::get_abi(surface));

    winrt::com_ptr<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess> dxgiInterfaceAccess;
    HRESULT hr = surfaceUnknown->QueryInterface(
        __uuidof(::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess),
        dxgiInterfaceAccess.put_void());
    if (FAILED(hr) || dxgiInterfaceAccess.get() == nullptr)
    {
        LogError("Failed to get IDirect3DDxgiInterfaceAccess from surface. hr=0x%08X", hr);
        return false;
    }

    winrt::com_ptr<ID3D11Texture2D> frameTexture;
    hr = dxgiInterfaceAccess->GetInterface(__uuidof(ID3D11Texture2D), frameTexture.put_void());
    if (FAILED(hr) || frameTexture.get() == nullptr)
    {
        LogError("GetInterface(ID3D11Texture2D) failed. hr=0x%08X", hr);
        return false;
    }

    bool ok = CopyFrameToCpuBgra(frameTexture.get(), m_cropRect, outBuffer,
                                 outWidth, outHeight, outStride);
    if (!ok)
    {
        LogError("CopyFrameToCpuBgra failed.");
        return false;
    }

    LogInfo("CaptureFrame: %dx%d stride=%d size=%zu bytes",
            outWidth, outHeight, outStride, outBuffer.size());

    return true;
}


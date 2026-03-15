// =============================================================================
// streamer.cpp - WGC capture screen center 640x640 + TurboJPEG compress + UDP
// Background thread loop with FPS control
// =============================================================================

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <Windows.Graphics.Capture.Interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <unknwn.h>

#include "streamer.h"
#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>
#include <vector>
#include <algorithm>
#include <random>
#include <sstream>

#include <winsock2.h>
#include <ws2tcpip.h>
#include <Windows.h>
#include <d3d11.h>
#include <dxgi.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "ws2_32.lib")

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/base.h>

#include <turbojpeg.h>

using namespace winrt::Windows::Graphics::Capture;
using namespace winrt::Windows::Graphics::DirectX;
using namespace winrt::Windows::Graphics::DirectX::Direct3D11;

// Manual COM interface definition to avoid SDK/header version issues.
// IDirect3DDxgiInterfaceAccess is a COM interface, not a WinRT type.
struct __declspec(uuid("3628E81B-3CAC-4C60-B7F4-23CE0E0C3356")) IDirect3DDxgiInterfaceAccess : ::IUnknown
{
    virtual HRESULT __stdcall GetInterface(GUID const& id, void** object) = 0;
};

namespace NetStreamer {

namespace {

const int kCaptureWidth = 640;
const int kCaptureHeight = 640;

// Engine singleton
struct Engine {
    std::mutex configMutex;
    StreamConfig config;
    StreamStats stats;
    std::atomic<bool> running{ false };
    std::thread worker;
    winrt::com_ptr<ID3D11Device> d3dDevice;
    winrt::com_ptr<ID3D11DeviceContext> d3dContext;
    GraphicsCaptureItem captureItem{ nullptr };
    Direct3D11CaptureFramePool framePool{ nullptr };
    GraphicsCaptureSession session{ nullptr };
    winrt::com_ptr<ID3D11Texture2D> stagingTexture;
    SOCKET udpSocket = INVALID_SOCKET;
    sockaddr_in destAddr = {};
    bool destAddrValid = false;
} g_engine;

StreamerEngine* GetStreamerEngineImpl() { return reinterpret_cast<StreamerEngine*>(&g_engine); }

// Get IGraphicsCaptureItemInterop factory (from Windows.Graphics.Capture.Interop.h)
winrt::com_ptr<IGraphicsCaptureItemInterop> GetCaptureItemInterop()
{
    static winrt::com_ptr<IGraphicsCaptureItemInterop> s_factory = [] {
        auto f = winrt::get_activation_factory<GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
        return f.as<IGraphicsCaptureItemInterop>();
    }();
    return s_factory;
}

// Get DXGI/D3D interface from WinRT IInspectable via our global COM IDirect3DDxgiInterfaceAccess.
// Uses IUnknown -> QueryInterface -> GetInterface; no WinRT .as<> on the interop interface.
template <typename T>
auto GetDxgiFromInspectable(winrt::Windows::Foundation::IInspectable const& inspectable) {
    winrt::com_ptr<T> result;
    auto item_cp = inspectable.as<::IUnknown>();
    winrt::com_ptr<::IDirect3DDxgiInterfaceAccess> interop;
    winrt::check_hresult(item_cp->QueryInterface(__uuidof(::IDirect3DDxgiInterfaceAccess), interop.put_void()));
    winrt::check_hresult(interop->GetInterface(__uuidof(T), result.put_void()));
    return result;
}

// Create WinRT Direct3D device for WGC
IDirect3DDevice CreateD3DDeviceFromDxgi(IDXGIDevice* dxgiDevice)
{
    winrt::com_ptr<::IInspectable> inspectable;
    winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice, inspectable.put()));
    return inspectable.as<IDirect3DDevice>();
}

// Primary monitor handle
HMONITOR GetPrimaryMonitor()
{
    HMONITOR primary = nullptr;
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR mon, HDC, LPRECT, LPARAM lp) -> BOOL {
        MONITORINFOEXW mi = {};
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfoW(mon, &mi) && (mi.dwFlags & MONITORINFOF_PRIMARY))
        {
            *reinterpret_cast<HMONITOR*>(lp) = mon;
            return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&primary));
    return primary;
}

// Init WGC: primary monitor, optional hide border
bool EnsureD3DAndWgc(const StreamConfig& config)
{
    if (g_engine.d3dDevice) return true;

    D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0 };
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
        levels, 1, D3D11_SDK_VERSION,
        g_engine.d3dDevice.put(), nullptr, g_engine.d3dContext.put()
    );
    if (FAILED(hr)) return false;

    winrt::com_ptr<IDXGIDevice> dxgiDevice;
    g_engine.d3dDevice->QueryInterface(IID_PPV_ARGS(dxgiDevice.put()));
    IDirect3DDevice winrtDevice = CreateD3DDeviceFromDxgi(dxgiDevice.get());
    if (!winrtDevice) return false;

    HMONITOR hMon = GetPrimaryMonitor();
    if (!hMon) return false;
    auto interop = GetCaptureItemInterop();
    GraphicsCaptureItem item{ nullptr };
    hr = interop->CreateForMonitor(hMon, winrt::guid_of<GraphicsCaptureItem>(), winrt::put_abi(item));
    if (FAILED(hr) || !item) return false;

    g_engine.captureItem = item;
    winrt::Windows::Graphics::SizeInt32 sizeInt32 = item.Size();
    if (sizeInt32.Width < kCaptureWidth || sizeInt32.Height < kCaptureHeight)
        return false;
    g_engine.framePool = Direct3D11CaptureFramePool::CreateFreeThreaded(
        winrtDevice,
        DirectXPixelFormat::B8G8R8A8UIntNormalized,
        3,
        winrt::Windows::Graphics::SizeInt32{ 640, 640 }
    );
    g_engine.session = g_engine.framePool.CreateCaptureSession(item);
    g_engine.session.IsBorderRequired(config.hideWgcBorder ? false : true);
    g_engine.session.IsCursorCaptureEnabled(false);
    g_engine.session.StartCapture();
    return true;
}

void DestroyWgc()
{
    if (g_engine.session) { g_engine.session.Close(); g_engine.session = nullptr; }
    if (g_engine.framePool) { g_engine.framePool.Close(); g_engine.framePool = nullptr; }
    g_engine.captureItem = nullptr;
    g_engine.stagingTexture = nullptr;
}

bool CreateStagingTexture(int width, int height)
{
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    HRESULT hr = g_engine.d3dDevice->CreateTexture2D(&desc, nullptr, g_engine.stagingTexture.put());
    return SUCCEEDED(hr);
}

// Copy center 640x640 BGRA from current frame into buffer (row-packed).
bool CaptureCenter640x640(std::vector<uint8_t>& outBgra)
{
    Direct3D11CaptureFrame frame{ nullptr };
    while (auto f = g_engine.framePool.TryGetNextFrame())
        frame = f;
    if (!frame) return false;

    winrt::Windows::Graphics::SizeInt32 contentSize = frame.ContentSize();
    int screenW = static_cast<int>(contentSize.Width);
    int screenH = static_cast<int>(contentSize.Height);
    if (screenW < kCaptureWidth || screenH < kCaptureHeight)
        return false;

    int left = (screenW - kCaptureWidth) / 2;
    int top = (screenH - kCaptureHeight) / 2;

    auto surface = frame.Surface();
    winrt::com_ptr<ID3D11Texture2D> frameTex = GetDxgiFromInspectable<ID3D11Texture2D>(surface);
    if (!frameTex) return false;

    if (!g_engine.stagingTexture)
    {
        if (!CreateStagingTexture(kCaptureWidth, kCaptureHeight))
            return false;
    }

    D3D11_BOX box = {};
    box.left = left;
    box.top = top;
    box.front = 0;
    box.right = left + kCaptureWidth;
    box.bottom = top + kCaptureHeight;
    box.back = 1;
    g_engine.d3dContext->CopySubresourceRegion(
        g_engine.stagingTexture.get(), 0, 0, 0, 0,
        frameTex.get(), 0, &box
    );

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    HRESULT hr = g_engine.d3dContext->Map(g_engine.stagingTexture.get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) return false;

    outBgra.resize(kCaptureWidth * kCaptureHeight * 4);
    const uint8_t* src = static_cast<const uint8_t*>(mapped.pData);
    for (int y = 0; y < kCaptureHeight; ++y)
        memcpy(outBgra.data() + y * kCaptureWidth * 4, src + y * mapped.RowPitch, kCaptureWidth * 4);
    g_engine.d3dContext->Unmap(g_engine.stagingTexture.get(), 0);
    return true;
}

// Compress to JPEG; caller must tjFree the returned buffer
unsigned char* CompressToJpeg(const uint8_t* bgra, int width, int height, int quality, unsigned long* outSize)
{
    tjhandle handle = tjInitCompress();
    if (!handle) return nullptr;
    unsigned char* jpegBuf = nullptr;
    unsigned long jpegSize = 0;
    int pitch = width * 4;
    int result = tjCompress2(handle, bgra, width, pitch, height, TJPF_BGRA,
        &jpegBuf, &jpegSize, TJSAMP_420, quality, TJFLAG_FASTDCT);
    tjDestroy(handle);
    if (result != 0 || !jpegBuf) return nullptr;
    *outSize = jpegSize;
    return jpegBuf;
}

bool SendUdp(const void* data, int len)
{
    if (g_engine.udpSocket == INVALID_SOCKET || !g_engine.destAddrValid)
        return false;
    int sent = sendto(g_engine.udpSocket, static_cast<const char*>(data), len, 0,
        reinterpret_cast<const sockaddr*>(&g_engine.destAddr), sizeof(g_engine.destAddr));
    return sent == len;
}

void WorkerThread()
{
    std::vector<uint8_t> bgra;
    bgra.reserve(kCaptureWidth * kCaptureHeight * 4);

    auto lastTime = std::chrono::steady_clock::now();
    uint64_t frameCount = 0;
    uint64_t byteCount = 0;
    double smoothFps = 0.0;

    while (g_engine.running)
    {
        StreamConfig cfg;
        {
            std::lock_guard<std::mutex> lock(g_engine.configMutex);
            cfg = g_engine.config;
        }

        if (!CaptureCenter640x640(bgra))
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        unsigned long jpegSize = 0;
        unsigned char* jpegBuf = CompressToJpeg(bgra.data(), kCaptureWidth, kCaptureHeight, cfg.jpegQuality, &jpegSize);
        if (!jpegBuf)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        bool sent = SendUdp(jpegBuf, (int)jpegSize);
        tjFree(jpegBuf);
        if (sent)
        {
            frameCount++;
            byteCount += jpegSize;
        }

        int targetFps = std::clamp(cfg.targetFps, 30, 144);
        auto frameDuration = std::chrono::microseconds(1000000 / targetFps);
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(now - lastTime);
        if (elapsed < frameDuration)
            std::this_thread::sleep_for(frameDuration - elapsed);
        lastTime = std::chrono::steady_clock::now();

        {
            double instantFps = 1000000.0 / (double)std::max<int64_t>(elapsed.count(), 1);
            smoothFps = smoothFps * 0.9 + instantFps * 0.1;
        }
        g_engine.stats.framesSent = frameCount;
        g_engine.stats.bytesSent = byteCount;
        g_engine.stats.actualFps = smoothFps;
    }

    g_engine.stats.isRunning = false;
}

} // anonymous

StreamerEngine* GetStreamerEngine() { return GetStreamerEngineImpl(); }

bool StartStreaming(const StreamConfig& config)
{
    if (g_engine.running) return true;

    WSADATA wsa = {};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
    {
        g_engine.stats.lastError = "WSAStartup failed";
        return false;
    }

    g_engine.udpSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_engine.udpSocket == INVALID_SOCKET)
    {
        g_engine.stats.lastError = "socket failed";
        return false;
    }

    memset(&g_engine.destAddr, 0, sizeof(g_engine.destAddr));
    g_engine.destAddr.sin_family = AF_INET;
    g_engine.destAddr.sin_port = htons(config.port);
    if (inet_pton(AF_INET, config.targetIp.c_str(), &g_engine.destAddr.sin_addr) != 1)
    {
        closesocket(g_engine.udpSocket);
        g_engine.udpSocket = INVALID_SOCKET;
        g_engine.stats.lastError = "invalid target IP";
        return false;
    }
    g_engine.destAddrValid = true;

    {
        std::lock_guard<std::mutex> lock(g_engine.configMutex);
        g_engine.config = config;
    }
    g_engine.stats = StreamStats{};
    g_engine.stats.isRunning = true;
    g_engine.stats.lastError.clear();

    if (!EnsureD3DAndWgc(config))
    {
        g_engine.stats.lastError = "WGC init failed";
        g_engine.stats.isRunning = false;
        closesocket(g_engine.udpSocket);
        g_engine.udpSocket = INVALID_SOCKET;
        g_engine.destAddrValid = false;
        return false;
    }

    g_engine.running = true;
    g_engine.worker = std::thread(WorkerThread);
    return true;
}

void StopStreaming()
{
    g_engine.running = false;
    if (g_engine.worker.joinable())
        g_engine.worker.join();
    DestroyWgc();
    if (g_engine.udpSocket != INVALID_SOCKET)
    {
        closesocket(g_engine.udpSocket);
        g_engine.udpSocket = INVALID_SOCKET;
    }
    g_engine.destAddrValid = false;
    g_engine.d3dContext = nullptr;
    g_engine.d3dDevice = nullptr;
}

bool IsStreaming() { return g_engine.running; }

StreamStats GetStreamStats()
{
    StreamStats s;
    s.framesSent = g_engine.stats.framesSent;
    s.bytesSent = g_engine.stats.bytesSent;
    s.actualFps = g_engine.stats.actualFps;
    s.isRunning = g_engine.stats.isRunning;
    s.lastError = g_engine.stats.lastError;
    return s;
}

void SetWdaExcludeFromCapture(void* hwnd, bool exclude)
{
#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif
    if (hwnd)
        SetWindowDisplayAffinity((HWND)hwnd, exclude ? WDA_EXCLUDEFROMCAPTURE : WDA_NONE);
}

void ResetWindowTitle(void* hwnd)
{
    if (!hwnd) return;
    static std::mt19937 rng((unsigned)std::chrono::steady_clock::now().time_since_epoch().count());
    std::string base = "DiagTool";
    std::string suffix;
    for (int i = 0; i < 6; ++i)
        suffix += (char)('a' + (rng() % 26));
    std::string title = base + "_" + suffix;
    SetWindowTextA((HWND)hwnd, title.c_str());
}

} // namespace NetStreamer

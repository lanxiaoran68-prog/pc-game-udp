// =============================================================================
// main.cpp - 程序入口、无边框窗口、DX11/ImGui 初始化、WDA 防护与风格注入
// 使用 WIN32 子系统，无控制台窗口
// =============================================================================

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _WINSOCKAPI_

#include <Windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "ws2_32.lib")

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

#include "ui.h"
#include "streamer.h"

#include <string>
#include <chrono>
#include <thread>

// -----------------------------------------------------------------------------
// 全局：DX11 与窗口
// -----------------------------------------------------------------------------
static ID3D11Device*            g_pd3dDevice = nullptr;
static ID3D11DeviceContext*     g_pd3dDeviceContext = nullptr;
static IDXGISwapChain*          g_pSwapChain = nullptr;
static ID3D11RenderTargetView*  g_mainRenderTargetView = nullptr;
static HWND                     g_hWnd = nullptr;
static const int                kWindowWidth = 550;
static const int                kWindowHeight = 400;

// UI 状态（与 streamer 解耦，由 UI 绘制读写）
static bool g_wdaEnabled = false;
static bool g_hideWgcBorder = true;
static bool g_streamToggle = false;
static std::string g_windowTitle = "DiagTool";

// 前向声明：ImGui Win32 消息处理
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// -----------------------------------------------------------------------------
// 创建 D3D11 设备与交换链
// -----------------------------------------------------------------------------
static bool CreateDeviceD3D()
{
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = g_hWnd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT createDeviceFlags = 0;
    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        createDeviceFlags,
        featureLevelArray,
        2,
        D3D11_SDK_VERSION,
        &sd,
        &g_pSwapChain,
        &g_pd3dDevice,
        &featureLevel,
        &g_pd3dDeviceContext
    );
    if (hr != S_OK)
        return false;

    ID3D11Texture2D* pBackBuffer = nullptr;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    if (!pBackBuffer)
        return false;
    g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
    pBackBuffer->Release();
    return true;
}

static void CleanupDeviceD3D()
{
    if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = nullptr; }
    if (g_pSwapChain) { g_pSwapChain->Release(); g_pSwapChain = nullptr; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = nullptr; }
    if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = nullptr; }
}

// -----------------------------------------------------------------------------
// 注入高端电竞暗黑风格（深灰背景 + 紫色强调 + 圆角）
// -----------------------------------------------------------------------------
static void InjectImGuiStyle()
{
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 8.0f;
    style.FrameRounding = 6.0f;
    style.GrabRounding = 6.0f;
    style.ScrollbarRounding = 4.0f;
    style.TabRounding = 4.0f;

    // 整体背景深灰/纯黑
    ImVec4* colors = style.Colors;
    colors[ImGuiCol_WindowBg] = ImVec4(0.08f, 0.08f, 0.08f, 1.00f);
    colors[ImGuiCol_ChildBg] = ImVec4(0.06f, 0.06f, 0.06f, 1.00f);
    colors[ImGuiCol_PopupBg] = ImVec4(0.08f, 0.08f, 0.08f, 0.98f);

    // 强调色：电竞紫（按钮、滑块、勾选）
    const ImVec4 accent(0.56f, 0.27f, 0.68f, 1.00f);
    colors[ImGuiCol_Button] = ImVec4(0.25f, 0.25f, 0.28f, 1.00f);
    colors[ImGuiCol_ButtonHovered] = ImVec4(accent.x * 0.7f, accent.y * 0.7f, accent.z * 0.7f, 0.8f);
    colors[ImGuiCol_ButtonActive] = accent;
    colors[ImGuiCol_Header] = ImVec4(accent.x * 0.5f, accent.y * 0.5f, accent.z * 0.5f, 0.6f);
    colors[ImGuiCol_HeaderHovered] = ImVec4(accent.x * 0.7f, accent.y * 0.7f, accent.z * 0.7f, 0.8f);
    colors[ImGuiCol_HeaderActive] = accent;
    colors[ImGuiCol_CheckMark] = accent;
    colors[ImGuiCol_SliderGrab] = accent;
    colors[ImGuiCol_SliderGrabActive] = ImVec4(accent.x * 1.2f, accent.y * 1.2f, accent.z * 1.2f, 1.00f);
    colors[ImGuiCol_FrameBg] = ImVec4(0.14f, 0.14f, 0.16f, 1.00f);
    colors[ImGuiCol_FrameBgHovered] = ImVec4(0.20f, 0.20f, 0.22f, 1.00f);
    colors[ImGuiCol_FrameBgActive] = ImVec4(accent.x * 0.3f, accent.y * 0.3f, accent.z * 0.3f, 0.5f);
    colors[ImGuiCol_TitleBg] = ImVec4(0.06f, 0.06f, 0.06f, 1.00f);
    colors[ImGuiCol_TitleBgActive] = ImVec4(0.08f, 0.08f, 0.08f, 1.00f);
    colors[ImGuiCol_Text] = ImVec4(0.92f, 0.92f, 0.94f, 1.00f);
    colors[ImGuiCol_TextDisabled] = ImVec4(0.45f, 0.45f, 0.48f, 1.00f);
    colors[ImGuiCol_Border] = ImVec4(0.22f, 0.22f, 0.24f, 1.00f);
}

// -----------------------------------------------------------------------------
// 无边框窗口拖拽：在标题区域返回 HTCAPTION
// -----------------------------------------------------------------------------
static LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg)
    {
    case WM_NCHITTEST:
    {
        // 无边框窗口：客户区顶部作为标题栏，可拖拽
        POINT pt = { LOWORD(lParam), HIWORD(lParam) };
        ::ScreenToClient(hWnd, &pt);
        if (pt.y >= 0 && pt.y < 32)
            return HTCAPTION;
        return HTCLIENT;
    }
    case WM_SIZE:
        if (g_pd3dDevice != nullptr && wParam != SIZE_MINIMIZED)
        {
            if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = nullptr; }
            g_pSwapChain->ResizeBuffers(0, (UINT)LOWORD(lParam), (UINT)HIWORD(lParam), DXGI_FORMAT_UNKNOWN, 0);
            ID3D11Texture2D* pBackBuffer;
            g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
            g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
            pBackBuffer->Release();
        }
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU)
            return 0;
        break;
    case WM_DESTROY:
        ::PostQuitMessage(0);
        return 0;
    }
    return ::DefWindowProcW(hWnd, msg, wParam, lParam);
}

// -----------------------------------------------------------------------------
// 应用 WDA 界面隐身（防截图）
// -----------------------------------------------------------------------------
#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

static void ApplyWda(HWND hwnd, bool exclude)
{
    if (!hwnd) return;
    SetWindowDisplayAffinity(hwnd, exclude ? WDA_EXCLUDEFROMCAPTURE : WDA_NONE);
}

// -----------------------------------------------------------------------------
// 程序入口（无控制台）
// -----------------------------------------------------------------------------
int APIENTRY wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE, _In_ LPWSTR, _In_ int)
{
    // Winsock 初始化（UDP 推流用）
    WSADATA wsa = {};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
        return 1;

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"NetStreamerClass";
    if (!RegisterClassExW(&wc))
        return 1;

    // 无边框窗口，居中
    int x = (GetSystemMetrics(SM_CXSCREEN) - kWindowWidth) / 2;
    int y = (GetSystemMetrics(SM_CYSCREEN) - kWindowHeight) / 2;
    g_hWnd = CreateWindowExW(
        0,
        wc.lpszClassName,
        L"DiagTool",
        WS_POPUP,
        x, y, kWindowWidth, kWindowHeight,
        nullptr, nullptr, hInstance, nullptr
    );
    if (!g_hWnd)
        return 1;

    if (!CreateDeviceD3D())
    {
        CleanupDeviceD3D();
        return 1;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ImGui::StyleColorsDark();
    InjectImGuiStyle();

    ImGui_ImplWin32_Init(g_hWnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    DiagTool::InitUIState();

    ShowWindow(g_hWnd, SW_SHOWDEFAULT);

    bool done = false;

    while (!done)
    {
        MSG msg;
        while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT)
                done = true;
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
        }
        if (done)
            break;

        // 根据 UI 勾选应用 WDA
        ApplyWda(g_hWnd, g_wdaEnabled);

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        NetStreamer::StreamConfig config;
        DiagTool::GetStreamConfigFromUI(&config);
        NetStreamer::StreamStats stats = NetStreamer::GetStreamStats();

        DiagTool::RenderMainUI(config, stats, &g_wdaEnabled, &g_hideWgcBorder, &g_streamToggle, g_hWnd);

        // 推流开关：从 UI 同步到引擎
        if (g_streamToggle && !NetStreamer::IsStreaming())
        {
            config.hideWgcBorder = g_hideWgcBorder;
            NetStreamer::StartStreaming(config);
        }
        else if (!g_streamToggle && NetStreamer::IsStreaming())
            NetStreamer::StopStreaming();

        ImGui::Render();
        const float clearColor[4] = { 0.08f, 0.08f, 0.08f, 1.0f };
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clearColor);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_pSwapChain->Present(1, 0);
    }

    NetStreamer::StopStreaming();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupDeviceD3D();
    DestroyWindow(g_hWnd);
    WSACleanup();
    return 0;
}

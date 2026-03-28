#pragma once

#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <string>
#include <cstdint>

#include "auth_keyauth.h"

class AppCore;

// ImGui + DX11 + Win32 UI，屏幕采集推流工具
class UI
{
public:
    UI();
    ~UI();

    // 进入 UI 消息循环
    int Run(AppCore& app);

private:
    bool CreateDeviceD3D(HWND hWnd);
    void CleanupDeviceD3D();
    void CreateRenderTarget();
    void CleanupRenderTarget();

    bool EnsurePreviewTexture(int width, int height);
    void UpdatePreviewTexture(const void* bgra, int width, int height, int stride);

    void DrawSidebar(int& selectedNav);

    // 页面分区：中间区域（Setting Row Card 风格）
    void DrawSystemMid(AppCore& app);
    void DrawParamsMid(AppCore& app);
    void DrawCaptureMid(AppCore& app);
    void DrawAdvancedMid(AppCore& app);

    // 页面分区：右侧工作区（卡片化）
    void DrawSystemRight(AppCore& app, bool& running);

    void DrawPreviewPanel(AppCore& app);
    void DrawBottomBar(AppCore& app, bool& running);
    void DrawAuthGate();
    void DrawLoginPage();
    void DrawRegisterPage();
    void DrawActivatePage();
    void EnsureAuthBootstrap();

    static LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

private:
    HWND m_hwnd = nullptr;

    Microsoft::WRL::ComPtr<ID3D11Device>            m_device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext>     m_context;
    Microsoft::WRL::ComPtr<IDXGISwapChain>          m_swapChain;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView>  m_rtv;

    Microsoft::WRL::ComPtr<ID3D11Texture2D>         m_previewTex;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_previewSrv;
    int m_previewW = 0;
    int m_previewH = 0;

    // UI state
    int  m_selectedNav = 0;

    AuthState m_authState = AuthState::Login;
    KeyAuthService m_authService;
    bool m_authBootstrapped = false;
    std::string m_authHint;
    uint64_t m_nextAuthRefreshTickMs = 0;

    char m_loginUser[64] = {};
    char m_loginPass[64] = {};
    char m_regUser[64] = {};
    char m_regPass[64] = {};
    char m_regCode[128] = {};
    char m_activateCode[128] = {};
};

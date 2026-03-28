#include <windows.h>
#include <string>
#include <random>
#include <chrono>
#include <cstring>

#include "app_core.h"
#include "ui.h"

// 日志接口（由其它模块实现）
void LogInfo(const char* fmt, ...);
void LogError(const char* fmt, ...);

// 控制台显示开关：
// - true: 本地调试时显示控制台，便于观察采集/认证/推流日志
// - false: 发布给用户前关闭，启动后自动隐藏控制台黑窗
constexpr bool kShowConsole = false;

// 生成随机字母数字字符串
static std::string GenerateRandomString(size_t len)
{
    const char charset[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz"
        "0123456789";
    const size_t charsetSize = sizeof(charset) - 1;

    uint64_t seed = static_cast<uint64_t>(
        std::chrono::high_resolution_clock::now().time_since_epoch().count());
    seed ^= static_cast<uint64_t>(GetCurrentProcessId()) << 32;
    seed ^= reinterpret_cast<uintptr_t>(&len);

    std::mt19937 rng(static_cast<uint32_t>(seed));
    std::uniform_int_distribution<size_t> dist(0, charsetSize - 1);

    std::string s;
    s.reserve(len);
    for (size_t i = 0; i < len; ++i)
    {
        s.push_back(charset[dist(rng)]);
    }
    return s;
}

// 注册一个随机窗口类并创建隐藏窗口，用于扰动内存指纹
static HWND CreateHiddenRandomWindow(const std::string& className, const std::string& title)
{
    WNDCLASSEXA wc{};
    wc.cbSize = sizeof(WNDCLASSEXA);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.hIcon = nullptr;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.lpszClassName = className.c_str();

    if (!RegisterClassExA(&wc))
    {
        DWORD err = GetLastError();
        LogError("RegisterClassExA failed. err=%lu", err);
        return nullptr;
    }

    HWND hwnd = CreateWindowExA(
        0,
        className.c_str(),
        title.c_str(),
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        200, 200,
        nullptr,
        nullptr,
        wc.hInstance,
        nullptr);

    if (!hwnd)
    {
        DWORD err = GetLastError();
        LogError("CreateWindowExA failed. err=%lu", err);
        return nullptr;
    }

    // 不展示这个窗口，保持隐藏状态
    ShowWindow(hwnd, SW_HIDE);
    UpdateWindow(hwnd);

    LogInfo("Hidden window created. class=%s title=%s", className.c_str(), title.c_str());
    return hwnd;
}

// 对指定窗口应用 WDA_EXCLUDEFROMCAPTURE
static void ApplyDisplayAffinity(HWND hwnd, const char* name)
{
    if (!hwnd)
    {
        LogError("ApplyDisplayAffinity: %s hwnd is null.", name);
        return;
    }

    if (SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE))
    {
        LogInfo("SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE) applied to %s window.", name);
    }
    else
    {
        DWORD err = GetLastError();
        // 控制台窗口失败通常不影响主功能：避免用 ERROR 级别污染 UI 日志
        if (name && std::strcmp(name, "console") == 0)
            LogInfo("SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE) skipped for console window. err=%lu", err);
        else
            LogError("SetWindowDisplayAffinity failed for %s. err=%lu", name, err);
    }
}

int main()
{
    // 随机生成窗口类名和标题（每次启动不同）
    std::string randomSuffix = GenerateRandomString(12);
    std::string ts = std::to_string(static_cast<unsigned long long>(GetTickCount64()));
    std::string className = "ScreenSenderClass_" + randomSuffix + "_" + ts;
    std::string windowTitle = "ScreenSender_" + randomSuffix + "_" + ts;

    // 创建一个隐藏窗口，使用随机类名和标题，增加内存特征扰动
    HWND hHidden = CreateHiddenRandomWindow(className, windowTitle);

    // 获取控制台窗口句柄并应用 WDA_EXCLUDEFROMCAPTURE
    HWND hConsole = GetConsoleWindow();
    if (hConsole)
    {
        ApplyDisplayAffinity(hConsole, "console");
        if (!kShowConsole)
        {
            // 发布模式：隐藏控制台黑窗（不影响日志机制）
            ShowWindow(hConsole, SW_HIDE);
        }
        else
        {
            // 调试模式：保留控制台，方便直接观察输出
            ShowWindow(hConsole, SW_SHOW);
        }
    }
    else
    {
        LogError("GetConsoleWindow returned null.");
    }

    // 对隐藏窗口也应用反捕获（如存在）
    if (hHidden)
    {
        ApplyDisplayAffinity(hHidden, "hidden");
    }

    // 运行核心逻辑
    AppCore app;
    if (!app.Initialize())
    {
        LogError("AppCore initialization failed. Exiting.");
        return 1;
    }

    UI ui;
    int ret = ui.Run(app);

    LogInfo("知觉Ai (推流端) stopped.");
    return ret;
}


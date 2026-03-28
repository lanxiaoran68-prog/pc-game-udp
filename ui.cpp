#include "ui.h"

#include "app_core.h"

#include <vector>
#include <string>
#include <deque>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <climits>
#include <cstring>

#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx11.h>

extern void LogInfo(const char* fmt, ...);
extern void LogError(const char* fmt, ...);

static const char* AuthStateName(AuthState s)
{
    switch (s)
    {
    case AuthState::CheckingSession: return "CheckingSession";
    case AuthState::Login: return "Login";
    case AuthState::Register: return "Register";
    case AuthState::Activate: return "Activate";
    case AuthState::Expired: return "Expired";
    case AuthState::Authenticated: return "Authenticated";
    default: return "?";
    }
}

static void AuthUiLogTransition(const KeyAuthService& authSvc, AuthState from, AuthState to,
                                const std::string& hint, const char* reason)
{
    if (!kAuthDebugLog)
        return;
    const AuthUserInfo& u = authSvc.GetUserInfo();
    LogInfo("[AuthUI] state %s -> %s | %s | hint=%s | user=%s | expired=%d expiryKnown=%d expiryUnix=%lld expiryText=%s",
            AuthStateName(from), AuthStateName(to), reason ? reason : "", hint.c_str(),
            u.username.empty() ? "(empty)" : u.username.c_str(),
            u.expired ? 1 : 0, u.expiryKnown ? 1 : 0, static_cast<long long>(u.expiryUnix),
            u.expiryText.empty() ? "(empty)" : u.expiryText.c_str());
}

// imgui_impl_win32.h 建议手动声明
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

static const char* kAppTitle = "知觉Ai (推流端)";
static const wchar_t* kAppTitleW = L"知觉Ai (推流端)";
static const char* kConfigFile = "zj_streamer.ini";
static AppCore::Config g_savedConfig = AppCore::DefaultConfig();
static bool g_savedConfigKnown = false;

// Header 命中测试参数（WndProc 使用）
static int g_headerDragHeightPx = 70;          // header 可拖动高度（像素）
static int g_headerNoDragRightPx = 130;        // 右上角按钮区宽度（像素），从拖动区排除

// 标题字体（Run() 中创建）
static ImFont* g_fontTitle = nullptr;
static ImFont* g_fontSub = nullptr;

// 主题语义层级色（由 ApplyTheme 写入，供局部面板/侧栏使用）
static ImVec4 g_appBg   = ImVec4(0.96f, 0.97f, 0.98f, 1.00f); // app background
static ImVec4 g_panelBg = ImVec4(0.98f, 0.99f, 1.00f, 1.00f); // panel background
static ImVec4 g_cardBg  = ImVec4(1.00f, 1.00f, 1.00f, 1.00f); // card background

struct UiLog
{
    struct Line { std::string s; ImU32 col; };
    std::deque<Line> lines;
    bool autoScroll = true;
    size_t maxLines = 400;

    void Add(ImU32 col, const char* fmt, ...)
    {
        char buf[1024] = {};
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);

        lines.push_back(Line{ buf, col });
        while (lines.size() > maxLines) lines.pop_front();
    }
};

static void ApplyTheme()
{
    ImGuiStyle& st = ImGui::GetStyle();
    st.WindowRounding = 14.0f;
    st.ChildRounding = 14.0f;
    st.FrameRounding = 10.0f;
    st.PopupRounding = 10.0f;
    st.ScrollbarRounding = 12.0f;
    st.GrabRounding = 10.0f;
    st.WindowPadding = ImVec2(14, 14);
    st.FramePadding = ImVec2(10, 8);
    st.ItemSpacing = ImVec2(10, 12);
    st.ItemInnerSpacing = ImVec2(8, 6);
    st.TabRounding = 10.0f;
    st.TabBorderSize = 0.0f;
    st.WindowBorderSize = 0.0f;
    st.ChildBorderSize = 0.0f;
    st.FrameBorderSize = 0.0f;

    // 固定浅色主题：先做稳、做像（不再支持深色模式）
    const ImVec4 ACCENT       = ImVec4(0.00f, 0.71f, 0.68f, 1.00f);
    const ImVec4 ACCENT_HOVER = ImVec4(0.02f, 0.78f, 0.74f, 1.00f);
    const ImVec4 ACCENT_ACT   = ImVec4(0.00f, 0.62f, 0.60f, 1.00f);

    ImVec4 windowBg, panelBg, cardBg, frameBg, frameHover, frameAct;
    ImVec4 border, sep;
    ImVec4 text, textMuted;
    ImVec4 header, headerHover, headerAct;
    ImVec4 tab, tabHover, tabSel;
    ImVec4 scrollbarGrab, scrollbarGrabHover, scrollbarGrabAct;

    windowBg   = ImVec4(0.96f, 0.97f, 0.98f, 1.00f); // app background
    panelBg    = ImVec4(0.98f, 0.99f, 1.00f, 1.00f); // panel background
    cardBg     = ImVec4(1.00f, 1.00f, 1.00f, 1.00f); // card background
    frameBg    = ImVec4(0.95f, 0.96f, 0.97f, 1.00f);
    frameHover = ImVec4(0.93f, 0.95f, 0.96f, 1.00f);
    frameAct   = ImVec4(0.92f, 0.94f, 0.95f, 1.00f);
    border     = ImVec4(0.89f, 0.91f, 0.93f, 1.00f);
    sep        = ImVec4(0.89f, 0.91f, 0.93f, 1.00f);
    text       = ImVec4(0.17f, 0.20f, 0.25f, 1.00f);
    textMuted  = ImVec4(0.47f, 0.52f, 0.60f, 1.00f);
    header     = ImVec4(0.93f, 0.96f, 0.97f, 1.00f);
    headerHover= ImVec4(0.91f, 0.95f, 0.96f, 1.00f);
    headerAct  = ImVec4(0.89f, 0.94f, 0.95f, 1.00f);
    tab        = ImVec4(0.93f, 0.96f, 0.97f, 1.00f);
    tabHover   = ImVec4(0.90f, 0.95f, 0.95f, 1.00f);
    tabSel     = ImVec4(0.88f, 0.95f, 0.94f, 1.00f);
    scrollbarGrab      = ImVec4(0.78f, 0.82f, 0.86f, 0.70f);
    scrollbarGrabHover = ImVec4(0.72f, 0.78f, 0.84f, 0.80f);
    scrollbarGrabAct   = ImVec4(0.68f, 0.75f, 0.82f, 0.90f);

    // 覆盖关键组件的颜色（统一产品设计语言）
    st.Colors[ImGuiCol_Text] = text;
    st.Colors[ImGuiCol_TextDisabled] = textMuted;
    st.Colors[ImGuiCol_WindowBg] = windowBg; // app background
    st.Colors[ImGuiCol_PopupBg]  = cardBg;
    st.Colors[ImGuiCol_ChildBg]  = cardBg;   // child default as card; panel uses local PushStyleColor

    st.Colors[ImGuiCol_Border]   = border;
    st.Colors[ImGuiCol_Separator]= sep;
    st.Colors[ImGuiCol_SeparatorHovered] = ImVec4(sep.x, sep.y, sep.z, 1.0f);
    st.Colors[ImGuiCol_SeparatorActive]  = ACCENT;

    st.Colors[ImGuiCol_FrameBg]        = frameBg;
    st.Colors[ImGuiCol_FrameBgHovered] = frameHover;
    st.Colors[ImGuiCol_FrameBgActive]  = frameAct;

    st.Colors[ImGuiCol_Header]        = header;
    st.Colors[ImGuiCol_HeaderHovered] = headerHover;
    st.Colors[ImGuiCol_HeaderActive]  = headerAct;

    st.Colors[ImGuiCol_Button]        = ACCENT;
    st.Colors[ImGuiCol_ButtonHovered] = ACCENT_HOVER;
    st.Colors[ImGuiCol_ButtonActive]  = ACCENT_ACT;

    st.Colors[ImGuiCol_CheckMark]        = ACCENT;
    st.Colors[ImGuiCol_SliderGrab]       = ImVec4(ACCENT.x, ACCENT.y, ACCENT.z, 0.90f);
    st.Colors[ImGuiCol_SliderGrabActive] = ACCENT;

    st.Colors[ImGuiCol_Tab]               = tab;
    st.Colors[ImGuiCol_TabHovered]        = tabHover;
    st.Colors[ImGuiCol_TabSelected]       = tabSel;
    st.Colors[ImGuiCol_TabSelectedOverline] = ACCENT;

    st.Colors[ImGuiCol_ScrollbarBg]         = ImVec4(0, 0, 0, 0);
    st.Colors[ImGuiCol_ScrollbarGrab]       = scrollbarGrab;
    st.Colors[ImGuiCol_ScrollbarGrabHovered]= scrollbarGrabHover;
    st.Colors[ImGuiCol_ScrollbarGrabActive] = scrollbarGrabAct;

    // 语义三层背景写回：app/panel/card
    g_appBg   = windowBg;
    g_panelBg = panelBg;
    g_cardBg  = cardBg;
}

static ImU32 AccentU32() { return IM_COL32(0, 181, 173, 255); }
static ImU32 TextDarkU32()
{
    // 让深色/浅色都走语义文本色（保持同一套设计语言）
    ImVec4 t = ImGui::GetStyle().Colors[ImGuiCol_Text];
    return ImGui::ColorConvertFloat4ToU32(t);
}
static ImU32 TextMuteU32()
{
    ImVec4 t = ImGui::GetStyle().Colors[ImGuiCol_TextDisabled];
    return ImGui::ColorConvertFloat4ToU32(t);
}

// 前向声明：配置保存函数在文件后面定义，按钮区需要调用
static void SaveConfigFile(const AppCore::Config& cfg, UiLog& log);

// 顶部状态摘要条（右侧）
static void DrawStatusSummaryBar(AppCore& app)
{
    AppCore::Stats st = app.GetStats();
    AppCore::Config cfg = app.GetConfig();
    const bool running = app.IsRunning();
    AppCore::Health health = app.GetHealth();
    AppCore::StartupCheckResult check = app.RunStartupCheck();
    AppCore::CriticalFailure cf = app.GetLastCriticalFailure();
    AppCore::RuntimeCaptureInfo rt = app.GetRuntimeCaptureInfo();

    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 14.0f);
    ImGui::BeginChild("right_status", ImVec2(0, 104), true, ImGuiWindowFlags_NoScrollbar);

    // 第一段：运行状态（运行/自检/模块）
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(TextMuteU32()), "运行状态");
    {
        std::string selfCheck;
        if (!check.canStart)
            selfCheck = "不可启动";
        else
            selfCheck = check.hasWarning ? "警告" : "可启动";

        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(TextDarkU32()),
                           "运行：%s  |  自检：%s",
                           running ? "运行中" : "已停止",
                           selfCheck.c_str());

        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(TextMuteU32()), "模块：");
        ImGui::SameLine();
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(health.captureOk ? AccentU32() : IM_COL32(200, 70, 70, 255)), "捕获");
        ImGui::SameLine();
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(health.encoderOk ? AccentU32() : IM_COL32(200, 70, 70, 255)), "编码");
        ImGui::SameLine();
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(health.senderReady ? AccentU32() : IM_COL32(200, 70, 70, 255)), "网络");
    }

    ImGui::Spacing();
    ImGui::Separator();

    // 第二段：当前输出摘要（真实输出/裁剪/目标）
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(TextMuteU32()), "输出摘要");
    {
        const char* cropMode = rt.usingCustomCrop ? "自定义" : "中心";
        if (rt.outputW > 0 && rt.outputH > 0)
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(TextDarkU32()),
                               "输出：%dx%d  |  裁剪：%s",
                               rt.outputW, rt.outputH, cropMode);
        else
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(TextDarkU32()), "输出：等待采集画面…");

        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(TextDarkU32()), "目标：%s:%u",
                           cfg.ip.c_str(), (unsigned)cfg.port);
    }

    ImGui::Spacing();
    ImGui::Separator();

    // 第三段：最近失败（聚焦诊断，而非翻日志）
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(TextMuteU32()), "最近失败");
    if (cf.seq == 0 || cf.message.empty())
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(AccentU32()), "无关键错误");
    else
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(IM_COL32(200, 70, 70, 255)), "%s：%s", cf.module.c_str(), cf.message.c_str());

    // 保留少量运行数值（不拥挤）：仅作为透明度较低的附带信息
    ImGui::Spacing();
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(TextMuteU32()), "FPS %.1f  |  吞吐 %.1f KB/s",
                        st.fps, st.sendBytesPerSec / 1024.0f);

    ImGui::EndChild();
    ImGui::PopStyleVar();
}

static void DrawHeaderBar(HWND hwnd, AppCore& app)
{
    // 自定义 header：左侧主标题 + 副标题，右侧窗口按钮（不放运行信息）
    (void)app;

    // 同步 WndProc 的拖动高度（与 header 实际高度一致）
    g_headerDragHeightPx = 58;

    // 轻量顶栏：不再绘制整条“白色整块卡片背景”
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0.0f);
    ImGui::BeginChild("header_bar", ImVec2(0, 46), false,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBackground);

    // 左：主标题（更像软件主标题）+ 同行轻副标题（更弱，不抢层级）
    const float leftPad = 12.0f;
    const float topPad  = 6.0f;
    ImGui::SetCursorPos(ImVec2(leftPad, topPad));

    if (g_fontTitle) ImGui::PushFont(g_fontTitle);
    ImGui::TextUnformatted("知觉Ai (推流端)");
    if (g_fontTitle) ImGui::PopFont();

    // 同行副标题：更小字号、更淡
    ImGui::SameLine(0.0f, 10.0f);
    ImVec4 mute = ImGui::GetStyle().Colors[ImGuiCol_TextDisabled];
    mute.w = 0.55f;
    if (g_fontSub) ImGui::PushFont(g_fontSub);
    ImGui::TextUnformatted("设置中心");
    if (g_fontSub) ImGui::PopFont();

    // 最右：窗口按钮（真正可用 + 更像桌面客户端）
    // 使用矢量图标自绘：统一尺寸/圆角/居中，关闭 hover 偏红
    const float btnW = 36.0f;
    const float btnH = 30.0f;
    const float gap  = 4.0f;
    const float groupW = btnW * 3.0f + gap * 2.0f;
    float rightButtonsW = groupW + 6.0f;

    // 更新 WndProc 的“禁止拖动”区域宽度
    g_headerNoDragRightPx = (int)(rightButtonsW + 10.0f);

    // 垂直居中按钮组
    ImGui::SetCursorPosX(ImGui::GetWindowWidth() - rightButtonsW);
    ImGui::SetCursorPosY((46.0f - btnH) * 0.5f);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 colText = TextDarkU32();
    // 更接近原生标题栏按钮：hover/pressed 更“轻”，圆角更小
    ImVec4 hovV = ImVec4(0.10f, 0.12f, 0.14f, 0.06f);
    ImVec4 prsV = ImVec4(0.10f, 0.12f, 0.14f, 0.10f);
    const ImU32 colHover = ImGui::ColorConvertFloat4ToU32(hovV);
    const ImU32 colPress = ImGui::ColorConvertFloat4ToU32(prsV);
    // 关闭按钮红更克制，避免廉价感
    const ImU32 colHoverClose = IM_COL32(220, 90, 90, 55);
    const ImU32 colPressClose = IM_COL32(200, 75, 75, 95);
    const ImU32 colOutline = IM_COL32(0, 0, 0, 18);

    auto DrawWinBtn = [&](const char* id, int kind /*0 min,1 max,2 close*/, bool closeBtn) -> bool
    {
        ImVec2 p0 = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton(id, ImVec2(btnW, btnH));
        bool hovered = ImGui::IsItemHovered();
        bool pressed = ImGui::IsItemActive();
        bool clicked = ImGui::IsItemClicked();

        ImU32 bg = 0;
        if (hovered)
            bg = closeBtn ? (pressed ? colPressClose : colHoverClose) : (pressed ? colPress : colHover);
        if (bg != 0)
            dl->AddRectFilled(p0, ImVec2(p0.x + btnW, p0.y + btnH), bg, 5.0f);
        // 轻描边：更像原生按钮“触感”，但保持克制
        if (hovered || pressed)
            dl->AddRect(p0, ImVec2(p0.x + btnW, p0.y + btnH), colOutline, 5.0f, 0, 1.0f);

        // 图标改为矢量绘制：尺寸统一、对齐统一
        const ImU32 fg = (closeBtn && hovered) ? IM_COL32(255, 255, 255, 235) : colText;
        ImVec2 c(p0.x + btnW * 0.5f, p0.y + btnH * 0.5f);
        const float s = 9.5f;
        const float thick = 2.0f;
        if (kind == 0)
        {
            // minimize：略上移，让视觉中心更像原生
            dl->AddLine(ImVec2(c.x - s, c.y + 6.0f), ImVec2(c.x + s, c.y + 6.0f), fg, thick);
        }
        else if (kind == 1)
        {
            // maximize：略上移/收紧，修正视觉中心
            const float yoff = -1.2f;
            dl->AddRect(ImVec2(c.x - s, c.y - s + yoff), ImVec2(c.x + s, c.y + s + yoff), fg, 2.0f, 0, thick);
        }
        else
        {
            // close：略上移，保持和 maximize 的视觉中心一致
            const float yoff = -1.2f;
            dl->AddLine(ImVec2(c.x - s, c.y - s + yoff), ImVec2(c.x + s, c.y + s + yoff), fg, thick);
            dl->AddLine(ImVec2(c.x + s, c.y - s + yoff), ImVec2(c.x - s, c.y + s + yoff), fg, thick);
        }

        return clicked;
    };

    // 最小化
    if (DrawWinBtn("##win_min", 0, false) && hwnd)
        ShowWindow(hwnd, SW_MINIMIZE);
    ImGui::SameLine(0.0f, gap);

    // 最大化/还原
    bool maximized = hwnd ? (IsZoomed(hwnd) != FALSE) : false;
    if (DrawWinBtn("##win_max", 1, false) && hwnd)
        ShowWindow(hwnd, maximized ? SW_RESTORE : SW_MAXIMIZE);
    ImGui::SameLine(0.0f, gap);

    // 关闭
    if (DrawWinBtn("##win_close", 2, true) && hwnd)
        PostMessageW(hwnd, WM_CLOSE, 0, 0);

    // 轻量顶栏分隔线（不再是整块白色卡片）
    {
        ImDrawList* dlSep = ImGui::GetWindowDrawList();
        ImVec2 p0 = ImGui::GetWindowPos();
        ImVec2 sz = ImGui::GetWindowSize();
        ImU32 sep = ImGui::ColorConvertFloat4ToU32(ImGui::GetStyle().Colors[ImGuiCol_Separator]);
        dlSep->AddLine(ImVec2(p0.x + 14.0f, p0.y + sz.y - 0.5f),
                       ImVec2(p0.x + sz.x - 14.0f, p0.y + sz.y - 0.5f),
                       sep, 1.0f);
    }

    ImGui::EndChild();
    ImGui::PopStyleVar();
}

static std::string Trim(std::string s)
{
    auto notSpace = [](unsigned char c) { return c != ' ' && c != '\t' && c != '\r' && c != '\n'; };
    while (!s.empty() && !notSpace((unsigned char)s.front())) s.erase(s.begin());
    while (!s.empty() && !notSpace((unsigned char)s.back())) s.pop_back();
    return s;
}

static void LoadConfigFile(AppCore& app, UiLog& log)
{
    std::ifstream f(kConfigFile, std::ios::in);
    if (!f.is_open())
    {
        log.Add(IM_COL32(120, 120, 120, 255), "未找到配置文件：%s（将使用默认参数）", kConfigFile);
        g_savedConfig = app.GetConfig();
        g_savedConfigKnown = true;
        return;
    }

    AppCore::Config cfg = app.GetConfig();
    std::string line;
    while (std::getline(f, line))
    {
        line = Trim(line);
        if (line.empty()) continue;
        if (line[0] == '#' || line[0] == ';') continue;
        auto pos = line.find('=');
        if (pos == std::string::npos) continue;
        std::string k = Trim(line.substr(0, pos));
        std::string v = Trim(line.substr(pos + 1));

        if (k == "ip") cfg.ip = v;
        else if (k == "port") { int p = std::atoi(v.c_str()); if (p > 0 && p < 65536) cfg.port = (uint16_t)p; }
        else if (k == "width") { int x = std::atoi(v.c_str()); if (x > 0) cfg.width = x; }
        else if (k == "height") { int x = std::atoi(v.c_str()); if (x > 0) cfg.height = x; }
        else if (k == "jpegQuality") { int x = std::atoi(v.c_str()); if (x >= 1 && x <= 100) cfg.jpegQuality = x; }
        else if (k == "targetFps") { int x = std::atoi(v.c_str()); if (x >= 1 && x <= 240) cfg.targetFps = x; }
        else if (k == "hideBorder") { cfg.hideBorder = (v == "1" || v == "true" || v == "TRUE"); }
        else if (k == "cropMode") { cfg.cropMode = (v == "custom") ? AppCore::Config::CropMode::Custom : AppCore::Config::CropMode::Center; }
        else if (k == "customCropX") cfg.customCropX = std::atoi(v.c_str());
        else if (k == "customCropY") cfg.customCropY = std::atoi(v.c_str());
        else if (k == "customCropW") cfg.customCropW = (std::max)(1, std::atoi(v.c_str()));
        else if (k == "customCropH") cfg.customCropH = (std::max)(1, std::atoi(v.c_str()));
    }
    app.SetConfig(cfg);
    g_savedConfig = cfg;
    g_savedConfigKnown = true;
    log.Add(IM_COL32(0, 140, 130, 255), "已加载配置：%s", kConfigFile);
}

static void SaveConfigFile(const AppCore::Config& cfg, UiLog& log)
{
    std::ofstream f(kConfigFile, std::ios::out | std::ios::trunc);
    if (!f.is_open())
    {
        log.Add(IM_COL32(220, 60, 60, 255), "保存失败：无法写入 %s", kConfigFile);
        return;
    }
    f << "ip=" << cfg.ip << "\n";
    f << "port=" << cfg.port << "\n";
    f << "width=" << cfg.width << "\n";
    f << "height=" << cfg.height << "\n";
    f << "jpegQuality=" << cfg.jpegQuality << "\n";
    f << "targetFps=" << cfg.targetFps << "\n";
    f << "hideBorder=" << (cfg.hideBorder ? 1 : 0) << "\n";
    f << "cropMode=" << (cfg.cropMode == AppCore::Config::CropMode::Custom ? "custom" : "center") << "\n";
    f << "customCropX=" << cfg.customCropX << "\n";
    f << "customCropY=" << cfg.customCropY << "\n";
    f << "customCropW=" << cfg.customCropW << "\n";
    f << "customCropH=" << cfg.customCropH << "\n";
    g_savedConfig = cfg;
    g_savedConfigKnown = true;
    log.Add(IM_COL32(0, 140, 130, 255), "已保存配置：%s", kConfigFile);
}

UI::UI() = default;

UI::~UI()
{
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    CleanupRenderTarget();
    CleanupDeviceD3D();
}

bool UI::CreateDeviceD3D(HWND hWnd)
{
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT createFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    D3D_FEATURE_LEVEL featureLevel{};
    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        createFlags,
        levels,
        ARRAYSIZE(levels),
        D3D11_SDK_VERSION,
        &sd,
        m_swapChain.GetAddressOf(),
        m_device.GetAddressOf(),
        &featureLevel,
        m_context.GetAddressOf());

    if (FAILED(hr))
    {
        LogError("D3D11CreateDeviceAndSwapChain failed. hr=0x%08X", hr);
        return false;
    }

    CreateRenderTarget();
    return true;
}

void UI::CleanupDeviceD3D()
{
    m_swapChain.Reset();
    m_context.Reset();
    m_device.Reset();
}

void UI::CreateRenderTarget()
{
    Microsoft::WRL::ComPtr<ID3D11Texture2D> backBuffer;
    if (FAILED(m_swapChain->GetBuffer(0, IID_PPV_ARGS(backBuffer.GetAddressOf()))))
        return;
    m_device->CreateRenderTargetView(backBuffer.Get(), nullptr, m_rtv.GetAddressOf());
}

void UI::CleanupRenderTarget()
{
    m_rtv.Reset();
}

bool UI::EnsurePreviewTexture(int width, int height)
{
    if (width <= 0 || height <= 0)
        return false;

    if (m_previewTex && width == m_previewW && height == m_previewH)
        return true;

    m_previewSrv.Reset();
    m_previewTex.Reset();
    m_previewW = width;
    m_previewH = height;

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = (UINT)width;
    desc.Height = (UINT)height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    HRESULT hr = m_device->CreateTexture2D(&desc, nullptr, m_previewTex.GetAddressOf());
    if (FAILED(hr))
    {
        LogError("CreateTexture2D(preview) failed. hr=0x%08X", hr);
        return false;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = desc.Format;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;
    hr = m_device->CreateShaderResourceView(m_previewTex.Get(), &srvDesc, m_previewSrv.GetAddressOf());
    if (FAILED(hr))
    {
        LogError("CreateShaderResourceView(preview) failed. hr=0x%08X", hr);
        return false;
    }

    return true;
}

void UI::UpdatePreviewTexture(const void* bgra, int width, int height, int stride)
{
    if (!bgra || !EnsurePreviewTexture(width, height))
        return;

    m_context->UpdateSubresource(m_previewTex.Get(), 0, nullptr, bgra, (UINT)stride, 0);
}

LRESULT WINAPI UI::WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    // 无边框窗口支持：先处理非客户区计算与命中测试，避免被 ImGui handler 吞掉
    switch (msg)
    {
    case WM_NCCALCSIZE:
        if (wParam)
            return 0; // 移除系统非客户区（标题栏/边框）
        break;
    case WM_GETMINMAXINFO:
    {
        // 无边框 + WS_THICKFRAME 最大化兼容：避免覆盖任务栏/多显示器错位
        MINMAXINFO* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
        if (!mmi)
            break;

        HMONITOR hMon = MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        if (hMon && GetMonitorInfoW(hMon, &mi))
        {
            RECT rcWork = mi.rcWork;
            RECT rcMon = mi.rcMonitor;
            mmi->ptMaxPosition.x = rcWork.left - rcMon.left;
            mmi->ptMaxPosition.y = rcWork.top - rcMon.top;
            mmi->ptMaxSize.x = rcWork.right - rcWork.left;
            mmi->ptMaxSize.y = rcWork.bottom - rcWork.top;
        }
        return 0;
    }
    case WM_NCHITTEST:
    {
        // 无边框窗口：基于 GetWindowRect 的屏幕坐标命中，手感更稳定
        const bool zoomed = (IsZoomed(hWnd) != FALSE);
        POINT ptScreen{ (LONG)(short)LOWORD(lParam), (LONG)(short)HIWORD(lParam) };
        RECT wr{};
        GetWindowRect(hWnd, &wr);

        const int w = wr.right - wr.left;
        const int h = wr.bottom - wr.top;
        const int x = ptScreen.x - wr.left;
        const int y = ptScreen.y - wr.top;

        if (x < 0 || y < 0 || x >= w || y >= h)
            return HTNOWHERE;

        // 缩放热区：基于系统边框厚度 + 额外余量，提升无边框手感
        const int frameX = GetSystemMetrics(SM_CXSIZEFRAME);
        const int frameY = GetSystemMetrics(SM_CYSIZEFRAME);
        const int pad = GetSystemMetrics(SM_CXPADDEDBORDER);

        const int edgeX = (std::max)(12, frameX + pad + 6);
        const int edgeY = (std::max)(12, frameY + pad + 6);
        const int cornerX = edgeX + 10;
        const int cornerY = edgeY + 10;

        const bool onLeft = (x < edgeX);
        const bool onRight = (x >= w - edgeX);
        const bool onTop = (y < edgeY);
        const bool onBottom = (y >= h - edgeY);

        const bool inLeftCorner = (x < cornerX);
        const bool inRightCorner = (x >= w - cornerX);
        const bool inTopCorner = (y < cornerY);
        const bool inBottomCorner = (y >= h - cornerY);

        // 角命中优先级高于边（尤其右下角）
        if (inBottomCorner && inRightCorner) return HTBOTTOMRIGHT;
        if (inBottomCorner && inLeftCorner)  return HTBOTTOMLEFT;
        if (inTopCorner && inRightCorner)    return HTTOPRIGHT;
        if (inTopCorner && inLeftCorner)     return HTTOPLEFT;

        if (onRight)  return HTRIGHT;
        if (onBottom) return HTBOTTOM;
        if (onLeft)   return HTLEFT;
        if (onTop)    return HTTOP;

        // 边/角都不是，再看 header 拖动（顶边缩放已优先返回 HTTOP）
        if (y < g_headerDragHeightPx)
        {
            if (x >= w - g_headerNoDragRightPx)
                return HTCLIENT; // 右上角按钮区
            return HTCAPTION;    // 顶部空白区域拖动窗口
        }

        // 最大化状态：不要返回缩放命中（保持正常桌面行为）
        if (zoomed)
            return HTCLIENT;

        return HTCLIENT;
    }
    }

    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg)
    {
    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED)
        {
            UI* ui = reinterpret_cast<UI*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));
            if (ui && ui->m_swapChain)
            {
                ui->CleanupRenderTarget();
                ui->m_swapChain->ResizeBuffers(
                    0, (UINT)LOWORD(lParam), (UINT)HIWORD(lParam),
                    DXGI_FORMAT_UNKNOWN, 0);
                ui->CreateRenderTarget();
            }
        }
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

void UI::DrawSidebar(int& selectedNav)
{
    // 独立侧边栏：品牌区 + 主导航区 + 底部全局项区
    const ImGuiStyle& st = ImGui::GetStyle();
    const ImVec4 sidebarBg = g_panelBg;

    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 16.0f);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, sidebarBg);
    ImGui::BeginChild("sidebar", ImVec2(150, 0), true, ImGuiWindowFlags_NoScrollbar);

    // 顶部品牌区
    ImGui::BeginChild("sb_brand", ImVec2(0, 92), false, ImGuiWindowFlags_NoScrollbar);
    ImGui::Spacing();
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(TextDarkU32()), "知觉Ai");
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(TextMuteU32()), "推流端 / Screen Streamer");
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::EndChild();

    // 主导航区（占满剩余空间）
    ImGui::BeginChild("sb_nav", ImVec2(0, 0), false, ImGuiWindowFlags_NoScrollbar);

    struct NavItem { const char* icon; const char* label; };
    static const NavItem items[] = {
        { "■", "系统" },
        { "■", "参数" },
        { "■", "采集设置" },
        { "■", "隐藏选项" }
    };

    const float itemH = 40.0f;
    const float itemR = 12.0f;
    const float padX  = 10.0f;
    const float barW  = 3.0f;
    const float gapY  = 6.0f;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (int i = 0; i < (int)IM_ARRAYSIZE(items); ++i)
    {
        ImGui::PushID(i);
        const bool selected = (selectedNav == i);

        ImVec2 sz = ImVec2(ImGui::GetContentRegionAvail().x, itemH);
        if (sz.x < 1.0f) sz.x = 1.0f;

        // 使用正常布局流：不要用 SetCursorPos/SetCursorScreenPos 推进
        ImGui::InvisibleButton("nav_btn", sz);
        const bool hovered = ImGui::IsItemHovered();
        if (ImGui::IsItemClicked())
            selectedNav = i;

        ImVec2 p0 = ImGui::GetItemRectMin();
        ImVec2 p1 = ImGui::GetItemRectMax();

        ImU32 bgCol = 0;
        if (selected)
        {
            ImVec4 c = st.Colors[ImGuiCol_HeaderActive];
            // 让选中态更像“卡片高亮”，不至于过浓
            c.w = (std::min)(0.95f, c.w + 0.15f);
            bgCol = ImGui::ColorConvertFloat4ToU32(c);
        }
        else if (hovered)
        {
            ImVec4 c = st.Colors[ImGuiCol_HeaderHovered];
            c.w = (std::min)(0.75f, c.w + 0.10f);
            bgCol = ImGui::ColorConvertFloat4ToU32(c);
        }

        if (bgCol != 0)
            dl->AddRectFilled(p0, p1, bgCol, itemR);

        if (selected)
        {
            dl->AddRectFilled(
                ImVec2(p0.x, p0.y + 6.0f),
                ImVec2(p0.x + barW, p1.y - 6.0f),
                AccentU32(), 2.0f);
        }

        const ImU32 textCol = selected ? AccentU32() : TextDarkU32();
        const ImU32 iconCol = selected ? AccentU32() : TextMuteU32();

        float x = p0.x + padX + (selected ? 2.0f : 0.0f);
        float y = p0.y + (itemH - ImGui::GetFontSize()) * 0.5f;
        dl->AddText(ImVec2(x + 2.0f, y), iconCol, items[i].icon);
        dl->AddText(ImVec2(x + 18.0f, y), textCol, items[i].label);

        // 用 Dummy 产生垂直间距（不会越界）
        ImGui::Dummy(ImVec2(0.0f, gapY));
        ImGui::PopID();
    }

    ImGui::EndChild(); // sb_nav

    ImGui::EndChild(); // sidebar
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
}

static bool ConfigEquals(const AppCore::Config& a, const AppCore::Config& b)
{
    return a.ip == b.ip &&
           a.port == b.port &&
           a.width == b.width &&
           a.height == b.height &&
           a.jpegQuality == b.jpegQuality &&
           a.targetFps == b.targetFps &&
           a.hideBorder == b.hideBorder &&
           a.cropMode == b.cropMode &&
           a.customCropX == b.customCropX &&
           a.customCropY == b.customCropY &&
           a.customCropW == b.customCropW &&
           a.customCropH == b.customCropH;
}

// ===== 中间设置区：Section + Setting Row Card（第二阶段） =====
static void DrawSectionHeader(const char* title)
{
    ImGui::Spacing();
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(TextMuteU32()), "%s", title);
    ImGui::Spacing();
}

static bool BeginSettingRowCard(const char* id, const char* iconText, const char* title, const char* desc, float rowH = 62.0f)
{
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 14.0f);
    bool ok = ImGui::BeginChild(id, ImVec2(0, rowH), true, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    // 统一行内布局：左图标占位 + 中间两行文字 + 右控件区
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 10.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10, 8));

    // 左侧图标占位（更像设置中心的图标栏）
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 6.0f);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 6.0f);
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(AccentU32()), "%s", iconText);

    // 中间标题/描述
    ImGui::SameLine();
    float textStartX = ImGui::GetCursorPosX() + 6.0f;
    ImGui::SetCursorPosX(textStartX);
    const float textBaseY = ImGui::GetCursorPosY();
    ImGui::SetCursorPosY(textBaseY - 2.0f);
    ImGui::BeginGroup();
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(TextDarkU32()), "%s", title);
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(TextMuteU32()), "%s", desc);
    ImGui::EndGroup();

    // 右侧控件区：留给调用者绘制
    // 恢复 Y 基线，避免前面对文本的微调（-2px）把右侧控件顶到 Child 的上裁剪区。
    ImGui::SetCursorPosY(textBaseY);
    ImGui::SameLine();
    return ok;
}

static void EndSettingRowCard()
{
    ImGui::PopStyleVar(2);
    ImGui::EndChild();
    ImGui::PopStyleVar();
}

static void AlignRightControl(float width)
{
    float avail = ImGui::GetContentRegionAvail().x;
    if (avail <= 0.0f)
        return;

    width = (std::min)(width, avail);
    float rightEdge = ImGui::GetContentRegionMax().x;
    float x = rightEdge - width;
    if (x < ImGui::GetCursorPosX()) x = ImGui::GetCursorPosX();
    ImGui::SetCursorPosX(x);
}

// ===== 中间设置区：Section + Setting Row Card（复用） =====
struct ConfigUiState
{
    bool initialized = false;
    char ipBuf[64] = {};
    char ipStageBuf[64] = {};
    int portBuf = 0;
    char portStageBuf[16] = {};
    int presetIndex = 0; // 0自定义 1低延迟 2均衡 3高画质
    int resIndex = 0;
    int resWidth = 640;
    int resHeight = 640;
    int qualBuf = 85;
    int fpsBuf = 60;
    bool hideBorderBuf = false;
    int cropModeIndex = 0; // 0 center, 1 custom
    int customCropX = 0;
    int customCropY = 0;
    int customCropW = 640;
    int customCropH = 640;
    char cropXStageBuf[24] = {};
    char cropYStageBuf[24] = {};
    char cropWStageBuf[24] = {};
    char cropHStageBuf[24] = {};
};

static const char* kPresetItems[] = { "自定义", "低延迟", "均衡", "高画质" };

struct ResolutionOption
{
    int w = 0;
    int h = 0;
    std::string label;
};

static const std::vector<ResolutionOption>& GetBaseResolutionOptions()
{
    static const std::vector<ResolutionOption> kBase = {
        {640, 640, "640 x 640"},
        {800, 800, "800 x 800"},
        {1080, 1080, "1080 x 1080"},
        {2560, 1440, "2560 x 1440"},
        {1920, 1440, "1920 x 1440"},
        {1920, 1200, "1920 x 1200"},
        {1920, 1080, "1920 x 1080"},
        {1680, 1050, "1680 x 1050"},
        {1600, 1200, "1600 x 1200"},
        {1600, 1024, "1600 x 1024"},
        {1600, 900, "1600 x 900"},
        {1440, 1080, "1440 x 1080"},
        {1440, 900, "1440 x 900"},
        {1366, 768, "1366 x 768"},
        {1360, 768, "1360 x 768"},
        {1280, 1024, "1280 x 1024"},
    };
    return kBase;
}

static int FindResolutionIndex(const std::vector<ResolutionOption>& items, int w, int h)
{
    for (size_t i = 0; i < items.size(); ++i)
    {
        if (items[i].w == w && items[i].h == h)
            return static_cast<int>(i);
    }
    return -1;
}

static std::vector<ResolutionOption> BuildResolutionOptions(const AppCore::RuntimeCaptureInfo& rt, int currentW, int currentH)
{
    std::vector<ResolutionOption> items = GetBaseResolutionOptions();
    const int fullW = rt.fullSourceRect.right - rt.fullSourceRect.left;
    const int fullH = rt.fullSourceRect.bottom - rt.fullSourceRect.top;

    if (fullW > 0 && fullH > 0)
    {
        if (FindResolutionIndex(items, fullW, fullH) < 0)
        {
            ResolutionOption rec{};
            rec.w = fullW;
            rec.h = fullH;
            rec.label = std::to_string(fullW) + " x " + std::to_string(fullH) + "（推荐）";
            items.insert(items.begin(), rec);
        }
        else
        {
            // 已存在同分辨率，仅附加“推荐”提示，避免重复项
            const int idx = FindResolutionIndex(items, fullW, fullH);
            if (idx >= 0)
                items[idx].label += "（推荐）";
        }
    }

    // 兼容历史配置：当前值不在列表中时，补一个“当前”项，防止下拉选中错乱。
    if (currentW > 0 && currentH > 0 && FindResolutionIndex(items, currentW, currentH) < 0)
    {
        ResolutionOption cur{};
        cur.w = currentW;
        cur.h = currentH;
        cur.label = std::to_string(currentW) + " x " + std::to_string(currentH) + "（当前）";
        items.push_back(cur);
    }
    return items;
}

static void ApplyPresetToUi(ConfigUiState& s, int presetIndex)
{
    // 仅联动关键参数：分辨率/JPEG质量/FPS
    if (presetIndex == 1)      { s.resWidth = 640;  s.resHeight = 640;  s.qualBuf = 70; s.fpsBuf = 120; } // 低延迟
    else if (presetIndex == 2) { s.resWidth = 800;  s.resHeight = 800;  s.qualBuf = 85; s.fpsBuf = 60; }  // 均衡
    else if (presetIndex == 3) { s.resWidth = 1080; s.resHeight = 1080; s.qualBuf = 95; s.fpsBuf = 45; }  // 高画质
}

static int DetectPresetFromUi(const ConfigUiState& s)
{
    if (s.resWidth == 640 && s.resHeight == 640 && s.qualBuf == 70 && s.fpsBuf == 120) return 1;
    if (s.resWidth == 800 && s.resHeight == 800 && s.qualBuf == 85 && s.fpsBuf == 60)  return 2;
    if (s.resWidth == 1080 && s.resHeight == 1080 && s.qualBuf == 95 && s.fpsBuf == 45)  return 3;
    return 0; // 自定义
}

static void SyncStageBuffersFromTypedValues(ConfigUiState& s)
{
    std::snprintf(s.ipStageBuf, sizeof(s.ipStageBuf), "%s", s.ipBuf);
    std::snprintf(s.portStageBuf, sizeof(s.portStageBuf), "%d", s.portBuf);
    std::snprintf(s.cropXStageBuf, sizeof(s.cropXStageBuf), "%d", s.customCropX);
    std::snprintf(s.cropYStageBuf, sizeof(s.cropYStageBuf), "%d", s.customCropY);
    std::snprintf(s.cropWStageBuf, sizeof(s.cropWStageBuf), "%d", s.customCropW);
    std::snprintf(s.cropHStageBuf, sizeof(s.cropHStageBuf), "%d", s.customCropH);
}

static bool TryParseInt(const char* text, int& outValue)
{
    if (!text || !text[0])
        return false;
    char* end = nullptr;
    long v = std::strtol(text, &end, 10);
    if (!end || *end != '\0')
        return false;
    if (v < INT_MIN || v > INT_MAX)
        return false;
    outValue = static_cast<int>(v);
    return true;
}

static bool HasUnsavedChanges(const AppCore::Config& current)
{
    if (!g_savedConfigKnown)
        return false;
    return !ConfigEquals(current, g_savedConfig);
}

static ConfigUiState& GetCfgState(AppCore& app)
{
    static ConfigUiState s;
    AppCore::Config cfg = app.GetConfig();
    if (!s.initialized)
    {
        std::snprintf(s.ipBuf, sizeof(s.ipBuf), "%s", cfg.ip.c_str());
        s.portBuf = (int)cfg.port;
        s.resWidth = cfg.width;
        s.resHeight = cfg.height;
        s.resIndex = 0;
        s.qualBuf = cfg.jpegQuality;
        s.fpsBuf = cfg.targetFps;
        s.hideBorderBuf = cfg.hideBorder;
        s.cropModeIndex = (cfg.cropMode == AppCore::Config::CropMode::Custom) ? 1 : 0;
        s.customCropX = cfg.customCropX;
        s.customCropY = cfg.customCropY;
        s.customCropW = cfg.customCropW;
        s.customCropH = cfg.customCropH;
        s.presetIndex = DetectPresetFromUi(s);
        SyncStageBuffersFromTypedValues(s);
        s.initialized = true;
    }
    return s;
}

struct CommitInputState
{
    bool ipEditing = false;
    bool portEditing = false;
    bool cropEditing = false;
};

static void CommitCfgIfChanged(AppCore& app, const ConfigUiState& s, const CommitInputState& inputState)
{
    AppCore::Config cfg = app.GetConfig();
    AppCore::Config newCfg = cfg;
    if (!inputState.ipEditing)
        newCfg.ip = s.ipBuf;
    if (!inputState.portEditing && s.portBuf > 0 && s.portBuf < 65536)
        newCfg.port = (uint16_t)s.portBuf;
    newCfg.width = (std::max)(1, s.resWidth);
    newCfg.height = (std::max)(1, s.resHeight);
    newCfg.jpegQuality = std::clamp(s.qualBuf, 1, 100);
    newCfg.targetFps = std::clamp(s.fpsBuf, 1, 240);
    newCfg.hideBorder = s.hideBorderBuf;
    newCfg.cropMode = (s.cropModeIndex == 1) ? AppCore::Config::CropMode::Custom : AppCore::Config::CropMode::Center;
    if (!inputState.cropEditing)
    {
        newCfg.customCropX = s.customCropX;
        newCfg.customCropY = s.customCropY;
        newCfg.customCropW = (std::max)(1, s.customCropW);
        newCfg.customCropH = (std::max)(1, s.customCropH);
    }
    if (!ConfigEquals(cfg, newCfg))
        app.SetConfig(newCfg);
}

static void ClampCustomCropInputsToSourceSpace(const AppCore::RuntimeCaptureInfo& rt, ConfigUiState& s)
{
    const int fullL = rt.fullSourceRect.left;
    const int fullT = rt.fullSourceRect.top;
    const int fullR = rt.fullSourceRect.right;
    const int fullB = rt.fullSourceRect.bottom;
    if (fullR <= fullL || fullB <= fullT)
    {
        // 源空间不可用时：至少保证 W/H 不会落到明显非法值，避免后续产生溢出/崩溃。
        s.customCropX = std::clamp(s.customCropX, -1000000, 1000000);
        s.customCropY = std::clamp(s.customCropY, -1000000, 1000000);
        s.customCropW = std::clamp(s.customCropW, 1, 1000000);
        s.customCropH = std::clamp(s.customCropH, 1, 1000000);
        return;
    }

    s.customCropX = std::clamp(s.customCropX, fullL, fullR - 1);
    s.customCropY = std::clamp(s.customCropY, fullT, fullB - 1);

    const int maxW = (fullR - s.customCropX);
    const int maxH = (fullB - s.customCropY);
    s.customCropW = std::clamp(s.customCropW, 1, (std::max)(1, maxW));
    s.customCropH = std::clamp(s.customCropH, 1, (std::max)(1, maxH));
}

static void LoadUiFromConfig(ConfigUiState& s, const AppCore::Config& cfg)
{
    std::snprintf(s.ipBuf, sizeof(s.ipBuf), "%s", cfg.ip.c_str());
    s.portBuf = (int)cfg.port;
    s.resWidth = cfg.width;
    s.resHeight = cfg.height;
    s.resIndex = 0;
    s.qualBuf = cfg.jpegQuality;
    s.fpsBuf = cfg.targetFps;
    s.hideBorderBuf = cfg.hideBorder;
    s.cropModeIndex = (cfg.cropMode == AppCore::Config::CropMode::Custom) ? 1 : 0;
    s.customCropX = cfg.customCropX;
    s.customCropY = cfg.customCropY;
    s.customCropW = cfg.customCropW;
    s.customCropH = cfg.customCropH;
    s.presetIndex = DetectPresetFromUi(s);
    SyncStageBuffersFromTypedValues(s);
}

void UI::DrawSystemMid(AppCore& app)
{
    AppCore::Config cfg = app.GetConfig();
    AppCore::Stats st = app.GetStats();
    AppCore::Health h = app.GetHealth();
    ImGui::BeginChild("cards", ImVec2(0, 0), false, ImGuiWindowFlags_NoScrollbar);
    DrawSectionHeader("系统");
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(TextDarkU32()), "提示");
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(TextMuteU32()),
        "系统页主要用于：预览、日志、启动/停止、保存。参数/采集/隐藏选项请在左侧菜单中切换。");

    ImGui::Spacing();
    if (BeginSettingRowCard("row_sys_diag", "●", "运行诊断", "基于当前模块真实状态的摘要"))
    {
        AlignRightControl(260.0f);
        ImGui::BeginGroup();
        ImGui::TextDisabled("目标  %s:%u", cfg.ip.c_str(), cfg.port);
        ImGui::TextDisabled("发送  %s (%d 包)", st.lastSendOk ? "OK" : "ERR", st.lastSendParts);
        ImGui::TextDisabled("模块  捕获:%s 编码:%s 网络:%s",
            h.captureOk ? "OK" : "ERR",
            h.encoderOk ? "OK" : "ERR",
            h.senderReady ? "OK" : "ERR");
        ImGui::EndGroup();
    }
    EndSettingRowCard();

    ImGui::EndChild();
}

void UI::DrawParamsMid(AppCore& app)
{
    ConfigUiState& s = GetCfgState(app);
    AppCore::Stats st = app.GetStats();
    AppCore::Health h = app.GetHealth();
    CommitInputState inputState{};
    ImGui::BeginChild("cards", ImVec2(0, 0), false, ImGuiWindowFlags_NoScrollbar);
    DrawSectionHeader("参数");
    if (HasUnsavedChanges(app.GetConfig()))
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(IM_COL32(220, 120, 40, 255)), "有未保存修改");
    else
        ImGui::TextDisabled("配置已保存");
    ImGui::Spacing();

    if (BeginSettingRowCard("row_preset", "●", "预设模式", "自定义 / 低延迟 / 均衡 / 高画质"))
    {
        float ctrlW = ImGui::GetContentRegionAvail().x;
        AlignRightControl(ctrlW);
        ImGui::SetNextItemWidth(ctrlW);
        int before = s.presetIndex;
        if (ImGui::Combo("##preset_mode", &s.presetIndex, kPresetItems, IM_ARRAYSIZE(kPresetItems)))
        {
            if (s.presetIndex != 0)
                ApplyPresetToUi(s, s.presetIndex);
            else if (before != 0)
                s.presetIndex = DetectPresetFromUi(s);
        }
    }
    EndSettingRowCard();
    ImGui::Spacing();

    if (BeginSettingRowCard("row_reset_default", "●", "一键恢复默认", "恢复到工程默认配置并同步到 UI/运行配置"))
    {
        float ctrlW = ImGui::GetContentRegionAvail().x;
        float btnW = (std::min)(120.0f, ctrlW);
        AlignRightControl(btnW);
        if (ImGui::Button("恢复默认", ImVec2(btnW, 34)))
        {
            AppCore::Config dft = AppCore::DefaultConfig();
            app.SetConfig(dft);
            LoadUiFromConfig(s, dft);
        }
    }
    EndSettingRowCard();
    ImGui::Spacing();

    if (BeginSettingRowCard("row_ip", "●", "目标 IP", "推流接收端 IPv4 地址（实时生效）"))
    {
        float ctrlW = ImGui::GetContentRegionAvail().x;
        AlignRightControl(ctrlW);
        ImGui::SetNextItemWidth(ctrlW);
        ImGui::InputText("##ip", s.ipStageBuf, sizeof(s.ipStageBuf), ImGuiInputTextFlags_EnterReturnsTrue);
        inputState.ipEditing = inputState.ipEditing || ImGui::IsItemActive();
        if (ImGui::IsItemDeactivatedAfterEdit())
        {
            std::snprintf(s.ipBuf, sizeof(s.ipBuf), "%s", s.ipStageBuf);
        }
    }
    EndSettingRowCard();
    ImGui::Spacing();

    if (BeginSettingRowCard("row_port", "●", "目标端口", "推流接收端端口（1-65535，实时生效）"))
    {
        float ctrlW = ImGui::GetContentRegionAvail().x;
        AlignRightControl(ctrlW);
        ImGui::SetNextItemWidth(ctrlW);
        ImGui::InputText("##port", s.portStageBuf, sizeof(s.portStageBuf),
                         ImGuiInputTextFlags_CharsDecimal | ImGuiInputTextFlags_EnterReturnsTrue);
        inputState.portEditing = inputState.portEditing || ImGui::IsItemActive();
        if (ImGui::IsItemDeactivatedAfterEdit())
        {
            int parsed = 0;
            if (TryParseInt(s.portStageBuf, parsed) && parsed > 0 && parsed < 65536)
            {
                s.portBuf = parsed;
                std::snprintf(s.portStageBuf, sizeof(s.portStageBuf), "%d", s.portBuf);
            }
            else
            {
                std::snprintf(s.portStageBuf, sizeof(s.portStageBuf), "%d", s.portBuf);
            }
        }
    }
    EndSettingRowCard();
    ImGui::Spacing();

    if (BeginSettingRowCard("row_quality", "●", "JPEG 质量", "画质更好但带宽更高（实时生效）"))
    {
        float ctrlW = ImGui::GetContentRegionAvail().x;
        AlignRightControl(ctrlW);
        ImGui::SetNextItemWidth(ctrlW);
        ImGui::SliderInt("##quality", &s.qualBuf, 1, 100);
    }
    EndSettingRowCard();
    ImGui::Spacing();

    if (BeginSettingRowCard("row_fps", "●", "目标 FPS", "采集/编码/发送目标帧率（实时生效）"))
    {
        float ctrlW = ImGui::GetContentRegionAvail().x;
        AlignRightControl(ctrlW);
        ImGui::SetNextItemWidth(ctrlW);
        ImGui::SliderInt("##fps", &s.fpsBuf, 10, 120);
    }
    EndSettingRowCard();

    // 手动改动后若不匹配预设，自动回到“自定义”
    s.presetIndex = DetectPresetFromUi(s);

    ImGui::Spacing();
    if (BeginSettingRowCard("row_net_summary", "●", "网络摘要", "当前发送状态/分包/吞吐（来自真实发送统计）"))
    {
        float ctrlW = ImGui::GetContentRegionAvail().x;
        AlignRightControl(ctrlW);
        ImGui::BeginGroup();
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ctrlW);
        ImGui::TextDisabled("发送：%s  分包：%d", st.lastSendOk ? "OK" : "ERR", st.lastSendParts);
        ImGui::TextDisabled("吞吐：%.1f KB/s  平均帧：%.0f B",
            st.sendBytesPerSec / 1024.0f, st.avgFrameBytes);
        ImGui::TextDisabled("网络模块：%s", h.senderReady ? "OK" : "ERR");
        ImGui::PopTextWrapPos();
        ImGui::EndGroup();
    }
    EndSettingRowCard();

    CommitCfgIfChanged(app, s, inputState);
    ImGui::EndChild();
}

void UI::DrawCaptureMid(AppCore& app)
{
    ConfigUiState& s = GetCfgState(app);
    AppCore::Config cfg = app.GetConfig();
    AppCore::Health h = app.GetHealth();
    auto hb = app.GetHideBorderStatus();
    auto rt = app.GetRuntimeCaptureInfo();
    CommitInputState inputState{};
    ImGui::BeginChild("cards", ImVec2(0, 0), false, ImGuiWindowFlags_NoScrollbar);
    DrawSectionHeader("采集设置");

    if (BeginSettingRowCard("row_res", "●", "分辨率", "采集输出分辨率（实时生效）", 66.0f))
    {
        std::vector<ResolutionOption> resItems = BuildResolutionOptions(rt, s.resWidth, s.resHeight);
        int selected = FindResolutionIndex(resItems, s.resWidth, s.resHeight);
        if (selected < 0) selected = 0;

        std::vector<const char*> labels;
        labels.reserve(resItems.size());
        for (auto& it : resItems) labels.push_back(it.label.c_str());

        float ctrlW = ImGui::GetContentRegionAvail().x;
        AlignRightControl(ctrlW);
        ImGui::SetNextItemWidth(ctrlW);
        if (ImGui::Combo("##res", &selected, labels.data(), static_cast<int>(labels.size())))
        {
            if (selected >= 0 && selected < static_cast<int>(resItems.size()))
            {
                s.resWidth = resItems[selected].w;
                s.resHeight = resItems[selected].h;
                s.resIndex = selected;
            }
        }
        else
        {
            s.resIndex = selected;
        }
    }
    EndSettingRowCard();
    ImGui::Spacing();

    const char* cropModeItems[] = { "中心裁剪", "自定义裁剪" };
    if (BeginSettingRowCard("row_crop_mode", "●", "裁剪模式", "为后续可视化编辑预留接入（当前先做输入）", 66.0f))
    {
        float ctrlW = ImGui::GetContentRegionAvail().x;
        AlignRightControl(ctrlW);
        ImGui::SetNextItemWidth(ctrlW);
        ImGui::Combo("##crop_mode", &s.cropModeIndex, cropModeItems, IM_ARRAYSIZE(cropModeItems));
    }
    EndSettingRowCard();
    ImGui::Spacing();

    if (s.cropModeIndex == 1)
    {
        if (BeginSettingRowCard("row_crop_rect", "●", "自定义裁剪区域", "X / Y / W / H（像素）", 90.0f))
        {
            float ctrlW = ImGui::GetContentRegionAvail().x;

            if (ctrlW > 0.0f)
            {
                const ImGuiInputTextFlags inputFlags = ImGuiInputTextFlags_CharsDecimal | ImGuiInputTextFlags_EnterReturnsTrue;

                const float spacingX = ImGui::GetStyle().ItemSpacing.x;
                const float minCellW = 56.0f; // 仅用于决定一行/两行退让
                const float maxCellW = minCellW + 14.0f; // 让大窗口下也保持“紧凑块”外观

                bool deactX = false, deactY = false, deactW = false, deactH = false;

                auto DrawCropIntField = [&](const char* visLabel,
                                            const char* hiddenIdLabel,
                                            char* stageBuf,
                                            size_t stageBufSize,
                                            float fieldW) -> bool
                {
                    ImGui::BeginGroup();
                    ImGui::TextDisabled("%s", visLabel);
                    ImGui::SetNextItemWidth(fieldW);
                    ImGui::InputText(hiddenIdLabel, stageBuf, stageBufSize, inputFlags);
                    inputState.cropEditing = inputState.cropEditing || ImGui::IsItemActive();
                    bool deact = ImGui::IsItemDeactivatedAfterEdit();
                    // 固定列宽：确保 SameLine 时各列起点由 fieldW 控制，而不是由标签宽度/内容宽度波动
                    ImGui::Dummy(ImVec2(fieldW, 0.0f));
                    ImGui::EndGroup();
                    return deact;
                };

                const float rawOneRowCellW = (ctrlW - 3.0f * spacingX) / 4.0f;
                const bool oneRow = (rawOneRowCellW >= minCellW);

                // 计算整组控件块宽度 groupW：先右对齐整块，再在块内排版
                float groupW = 0.0f;
                if (oneRow)
                {
                    const float cellW = (std::min)(rawOneRowCellW, maxCellW);
                    groupW = cellW * 4.0f + 3.0f * spacingX;
                }
                else
                {
                    const float colW = (ctrlW - spacingX) / 2.0f;
                    groupW = colW * 2.0f + spacingX;
                }

                AlignRightControl(groupW);

                if (oneRow)
                {
                    const float cellW = (std::min)(rawOneRowCellW, maxCellW);
                    deactX = DrawCropIntField("X", "##cx", s.cropXStageBuf, sizeof(s.cropXStageBuf), cellW);
                    ImGui::SameLine(0.0f, spacingX);

                    deactY = DrawCropIntField("Y", "##cy", s.cropYStageBuf, sizeof(s.cropYStageBuf), cellW);
                    ImGui::SameLine(0.0f, spacingX);

                    deactW = DrawCropIntField("W", "##cw", s.cropWStageBuf, sizeof(s.cropWStageBuf), cellW);
                    ImGui::SameLine(0.0f, spacingX);

                    deactH = DrawCropIntField("H", "##ch", s.cropHStageBuf, sizeof(s.cropHStageBuf), cellW);
                }
                else
                {
                    const float colW = (ctrlW - spacingX) / 2.0f;

                    // 第1行：X / Y
                    deactX = DrawCropIntField("X", "##cx", s.cropXStageBuf, sizeof(s.cropXStageBuf), colW);
                    ImGui::SameLine(0.0f, spacingX);

                    deactY = DrawCropIntField("Y", "##cy", s.cropYStageBuf, sizeof(s.cropYStageBuf), colW);

                    ImGui::NewLine();

                    // 第2行：W / H
                    deactW = DrawCropIntField("W", "##cw", s.cropWStageBuf, sizeof(s.cropWStageBuf), colW);
                    ImGui::SameLine(0.0f, spacingX);

                    deactH = DrawCropIntField("H", "##ch", s.cropHStageBuf, sizeof(s.cropHStageBuf), colW);
                }

                // 用户完成编辑（回车/失焦通常会触发 deactivatedAfterEdit）后做一次无声钳制
                if (deactX || deactY || deactW || deactH)
                {
                    int parsed = 0;
                    if (TryParseInt(s.cropXStageBuf, parsed)) s.customCropX = parsed;
                    if (TryParseInt(s.cropYStageBuf, parsed)) s.customCropY = parsed;
                    if (TryParseInt(s.cropWStageBuf, parsed)) s.customCropW = parsed;
                    if (TryParseInt(s.cropHStageBuf, parsed)) s.customCropH = parsed;
                    ClampCustomCropInputsToSourceSpace(rt, s);
                    // 钳制后回写 staging 文本，保持 UI 与最终有效值一致
                    std::snprintf(s.cropXStageBuf, sizeof(s.cropXStageBuf), "%d", s.customCropX);
                    std::snprintf(s.cropYStageBuf, sizeof(s.cropYStageBuf), "%d", s.customCropY);
                    std::snprintf(s.cropWStageBuf, sizeof(s.cropWStageBuf), "%d", s.customCropW);
                    std::snprintf(s.cropHStageBuf, sizeof(s.cropHStageBuf), "%d", s.customCropH);
                }
            }
        }
        EndSettingRowCard();
        ImGui::Spacing();
    }

    if (BeginSettingRowCard("row_preview_tip", "●", "预览窗口", "预览/启动/日志在“系统”页集中显示"))
    {
        AlignRightControl(140.0f);
        ImGui::TextDisabled("去系统页查看");
    }
    EndSettingRowCard();

    ImGui::Spacing();
    if (BeginSettingRowCard("row_cap_status", "●", "捕获状态", "当前输出与会话状态（真实数据）"))
    {
        AlignRightControl(260.0f);
        ImGui::BeginGroup();
        const int fullW = rt.fullSourceRect.right - rt.fullSourceRect.left;
        const int fullH = rt.fullSourceRect.bottom - rt.fullSourceRect.top;
        ImGui::TextDisabled("源空间（真实）：%d x %d", fullW, fullH);
        ImGui::TextDisabled("输出（真实）：%d x %d", rt.outputW, rt.outputH);
        ImGui::TextDisabled("输出（配置）：%d x %d", cfg.width, cfg.height);
        ImGui::TextDisabled("裁剪模式：%s", rt.usingCustomCrop ? "自定义" : "中心");
        ImGui::TextDisabled("裁剪区域（真实）：%ld,%ld %ldx%ld",
            rt.cropRect.left, rt.cropRect.top,
            rt.cropRect.right - rt.cropRect.left,
            rt.cropRect.bottom - rt.cropRect.top);
        ImGui::TextDisabled("捕获模块：%s", h.captureOk ? "OK" : "ERR");
        ImGui::TextDisabled("hideBorder：%s / %s",
            hb.probed ? (hb.supported ? "支持" : "不支持") : "未探测",
            (hb.probed && hb.supported) ? (hb.applied ? "已应用" : "未应用") : "不可用");
        ImGui::EndGroup();
    }
    EndSettingRowCard();

    // 分辨率属于预设关键参数，手动改动后同步预设归类
    s.presetIndex = DetectPresetFromUi(s);

    CommitCfgIfChanged(app, s, inputState);
    ImGui::EndChild();
}

void UI::DrawAdvancedMid(AppCore& app)
{
    ConfigUiState& s = GetCfgState(app);
    ImGui::BeginChild("cards", ImVec2(0, 0), false, ImGuiWindowFlags_NoScrollbar);
    DrawSectionHeader("隐藏选项");

    if (BeginSettingRowCard("row_hideborder", "●", "隐藏边缘标记", "隐藏采集边缘提示标记（如存在）"))
    {
        AlignRightControl(90.0f);
        ImGui::Checkbox("##hideborder", &s.hideBorderBuf);
    }
    EndSettingRowCard();
    // 可验证状态说明（不改布局，只在现有区域补充清晰状态）
    {
        auto st = app.GetHideBorderStatus();
        ImGui::Spacing();
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(TextMuteU32()), "状态");
        ImGui::Separator();
        ImGui::Text("配置：hideBorder = %s", st.desiredHide ? "true" : "false");
        ImGui::Text("支持性探测：%s", st.probed ? "已探测" : "未探测");
        ImGui::Text("系统支持：%s", st.probed ? (st.supported ? "支持" : "不支持") : "未知");
        if (st.probed && st.supported)
            ImGui::Text("应用到会话：%s", st.applied ? "已应用" : "未应用");
        else
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(TextMuteU32()), "应用到会话：不可用");
    }
    ImGui::Spacing();

    if (BeginSettingRowCard("row_future", "●", "预留扩展", "后续可加入更多隐藏参数/反检测细项"))
    {
        AlignRightControl(120.0f);
        ImGui::TextDisabled("敬请期待");
    }
    EndSettingRowCard();

    CommitInputState inputState{};
    CommitCfgIfChanged(app, s, inputState);
    ImGui::EndChild();
}

void UI::DrawSystemRight(AppCore& app, bool& running)
{
    // 轻量会员状态区（不改现有右侧主结构）
    const AuthUserInfo& u = m_authService.GetUserInfo();
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 10.0f);
    ImGui::BeginChild("member_bar", ImVec2(0, 72), true, ImGuiWindowFlags_NoScrollbar);
    ImGui::TextDisabled("会员状态");
    ImGui::Text("用户：%s", u.username.empty() ? "未登录" : u.username.c_str());
    ImGui::SameLine();
    const char* expiryDisplay = u.expiryKnown ? (u.expiryText.empty() ? "未知" : u.expiryText.c_str()) : "待同步";
    ImGui::TextDisabled("到期：%s", expiryDisplay);
    ImGui::SameLine();
    const bool showExpired = u.expiryKnown && u.expired;
    ImGui::TextColored(
        ImGui::ColorConvertU32ToFloat4(showExpired ? IM_COL32(220, 70, 70, 255) : AccentU32()),
        "状态：%s", showExpired ? "已过期" : (u.expiryKnown ? "有效" : "待同步"));
    ImGui::SameLine();
    ImGui::TextDisabled("设备绑定保护：已启用");
    if (ImGui::SmallButton("激活/续费"))
    {
        const AuthState prev = m_authState;
        m_authState = AuthState::Activate;
        m_authHint.clear();
        AuthUiLogTransition(m_authService, prev, m_authState, m_authHint, "sidebar: open Activate page");
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("退出登录"))
    {
        const AuthState prev = m_authState;
        m_authService.Logout();
        m_authState = AuthState::Login;
        m_authHint = "已退出登录";
        AuthUiLogTransition(m_authService, prev, m_authState, m_authHint, "sidebar: Logout");
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::Spacing();

    DrawStatusSummaryBar(app);
    ImGui::Spacing();

    float rightAvail = ImGui::GetContentRegionAvail().y;
    const float kButtonsH = 80.0f;
    const float kLogH     = 180.0f;
    const float kSpacing  = 10.0f;
    float previewH = rightAvail - kButtonsH - kLogH - 2.0f * kSpacing;
    if (previewH < 120.0f) previewH = 120.0f;

    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 14.0f);
    ImGui::BeginChild("preview_card", ImVec2(0, previewH), true, ImGuiWindowFlags_NoScrollbar);
    DrawPreviewPanel(app);
    ImGui::EndChild();
    ImGui::PopStyleVar();

    ImGui::Spacing();
    DrawBottomBar(app, running);
}

// 右侧固定工作区：不再保留历史的“右侧分页面”实现

void UI::DrawPreviewPanel(AppCore& app)
{
    std::vector<uint8_t> bgra;
    int w = 0, h = 0, stride = 0;
    if (app.GetLatestBgra(bgra, w, h, stride))
        UpdatePreviewTexture(bgra.data(), w, h, stride);

    AppCore::Stats st = app.GetStats();
    AppCore::RuntimeCaptureInfo rt = app.GetRuntimeCaptureInfo();

    // 预览卡片内容（调用方负责创建固定高度的 Child）
    // 标题行：左标题 + 右侧指标（测宽右对齐，避免固定定位）
    ImGui::TextUnformatted("采集窗口");
    // 使用 snprintf 生成稳定格式
    char rightBuf[128]{};
    std::snprintf(rightBuf, sizeof(rightBuf), "FPS %.1f | 平均帧 %.0f B | %.1f KB/s",
                  st.fps, st.avgFrameBytes, st.sendBytesPerSec / 1024.0f);
    const float rightW = ImGui::CalcTextSize(rightBuf).x;
    const float rightX = (ImGui::GetContentRegionMax().x - rightW);
    ImGui::SameLine();
    if (rightX > ImGui::GetCursorPosX())
        ImGui::SetCursorPosX(rightX);
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(TextMuteU32()), "%s", rightBuf);
    ImGui::Separator();

    ImVec2 avail = ImGui::GetContentRegionAvail();
    if (m_previewSrv && w > 0 && h > 0)
    {
        float aspect = (float)w / (float)h;
        float drawW = avail.x;
        float drawH = drawW / aspect;
        if (drawH > avail.y)
        {
            drawH = avail.y;
            drawW = drawH * aspect;
        }
        ImVec2 size(drawW, drawH);
        ImVec2 cursor = ImGui::GetCursorPos();
        cursor.x += (avail.x - size.x) * 0.5f;
        cursor.y += (avail.y - size.y) * 0.5f;
        ImGui::SetCursorPos(cursor);
        ImVec2 imgScreenTL = ImGui::GetCursorScreenPos();
        ImGui::Image((ImTextureID)m_previewSrv.Get(), size);

        // 只读叠加：根据 previewSpaceMode 进行明确语义绘制
        // - FullSource：使用 fullSourceRect/cropRect 映射到当前显示区域
        // - CroppedOutput：预览缓冲本身就是 crop 输出，因此裁剪框应覆盖整幅预览边界
        if (rt.outputW > 0 && rt.outputH > 0 &&
            (rt.cropRect.right > rt.cropRect.left) && (rt.cropRect.bottom > rt.cropRect.top))
        {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImU32 col = rt.usingCustomCrop ? IM_COL32(0, 210, 255, 200) : IM_COL32(0, 181, 173, 200);
            const float thick = rt.usingCustomCrop ? 2.0f : 1.5f;

            const ImVec2 imgScreenBR(imgScreenTL.x + size.x, imgScreenTL.y + size.y);

            if (rt.previewSpaceMode == AppCore::RuntimeCaptureInfo::PreviewSpaceMode::CroppedOutput)
            {
                // 预览缓冲代表裁剪输出：cropRect 在预览空间就是“整幅”
                dl->AddRect(imgScreenTL, imgScreenBR, col, 0.0f, 0, thick);
            }
            else
            {
                // 预览缓冲代表完整源空间：将 cropRect 从 fullSourceRect 坐标系映射到显示区域
                const int fullW = rt.fullSourceRect.right - rt.fullSourceRect.left;
                const int fullH = rt.fullSourceRect.bottom - rt.fullSourceRect.top;
                if (fullW > 0 && fullH > 0)
                {
                    const float scaleX = size.x / (float)fullW;
                    const float scaleY = size.y / (float)fullH;
                    const float x0 = (rt.cropRect.left - rt.fullSourceRect.left) * scaleX;
                    const float y0 = (rt.cropRect.top - rt.fullSourceRect.top) * scaleY;
                    const float x1 = (rt.cropRect.right - rt.fullSourceRect.left) * scaleX;
                    const float y1 = (rt.cropRect.bottom - rt.fullSourceRect.top) * scaleY;

                    const float rx0 = (std::max)(0.0f, (std::min)(x0, size.x));
                    const float ry0 = (std::max)(0.0f, (std::min)(y0, size.y));
                    const float rx1 = (std::max)(0.0f, (std::min)(x1, size.x));
                    const float ry1 = (std::max)(0.0f, (std::min)(y1, size.y));

                    ImVec2 rectTL(imgScreenTL.x + rx0, imgScreenTL.y + ry0);
                    ImVec2 rectBR(imgScreenTL.x + rx1, imgScreenTL.y + ry1);
                    dl->AddRect(rectTL, rectBR, col, 0.0f, 0, thick);
                }
            }
        }
    }
    else
    {
        ImGui::Spacing();
        ImVec2 textSize = ImGui::CalcTextSize("等待采集画面...");
        ImVec2 cursor = ImGui::GetCursorPos();
        cursor.x += (avail.x - textSize.x) * 0.5f;
        cursor.y += (avail.y - textSize.y) * 0.5f;
        ImGui::SetCursorPos(cursor);
        ImGui::TextDisabled("等待采集画面...");
    }
}

void UI::DrawBottomBar(AppCore& app, bool& running)
{
    static UiLog log;
    static bool logInit = false;
    if (!logInit)
    {
        log.Add(IM_COL32(0, 140, 130, 255), "%s 已启动（UI 已就绪）", kAppTitle);
        LoadConfigFile(app, log);
        logInit = true;
    }

    // 接入底层真实日志（capture/encoder/sender/app_core 统一入口）
    {
        std::vector<GlobalLogLine> lines;
        ConsumeGlobalLogs(lines);
        for (const auto& l : lines)
            log.lines.push_back(UiLog::Line{ l.text, l.isError ? IM_COL32(220, 70, 70, 255) : IM_COL32(120, 130, 140, 255) });
        while (log.lines.size() > log.maxLines) log.lines.pop_front();
    }

    ImGui::BeginChild("bottom", ImVec2(0, 220), true);

    // 底部按钮行
    ImGui::BeginChild("bottom_buttons", ImVec2(0, 64), false);

    running = app.IsRunning();
    const bool dirty = HasUnsavedChanges(app.GetConfig());
    ImVec2 btnSize(160, 44);

    if (!running)
    {
        if (ImGui::Button("启动推流", btnSize))
        {
            auto check = app.RunStartupCheck();
            if (!check.canStart)
            {
                log.Add(IM_COL32(230, 80, 80, 255), "启动前自检失败：%s", check.primaryReason.c_str());
            }
            else if (app.Start())
            {
                log.Add(IM_COL32(0, 140, 130, 255), "推流已启动");
                if (check.hasWarning)
                    log.Add(IM_COL32(220, 140, 60, 255), "启动警告：%s", check.primaryReason.c_str());
            }
            else
            {
                log.Add(IM_COL32(230, 80, 80, 255), "推流启动失败（请检查初始化/日志）");
            }
        }
    }
    else
    {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.80f, 0.20f, 0.25f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.86f, 0.25f, 0.30f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.74f, 0.18f, 0.22f, 1.0f));
        if (ImGui::Button("停止推流", btnSize))
        {
            app.Stop();
            log.Add(IM_COL32(230, 80, 80, 255), "推流已停止");
        }
        ImGui::PopStyleColor(3);
    }

    ImGui::SameLine();
    if (ImGui::Button("保存", ImVec2(120, 44)))
        SaveConfigFile(app.GetConfig(), log);

    ImGui::SameLine();
    if (dirty)
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(IM_COL32(220, 120, 40, 255)), "有未保存修改");
    else
        ImGui::TextDisabled("已保存");

    ImGui::SameLine();
    ImGui::TextDisabled("提示：IP/端口/参数修改会实时生效");

    ImGui::EndChild();

    // 日志区
    ImGui::Separator();
    ImGui::TextUnformatted("推流日志");
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetWindowWidth() - 90.0f);
    if (ImGui::SmallButton("清空日志"))
        log.lines.clear();
    ImGui::BeginChild("log", ImVec2(0, 0), true, ImGuiWindowFlags_HorizontalScrollbar);
    for (const auto& l : log.lines)
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(l.col), "%s", l.s.c_str());
    if (log.autoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
        ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();

    ImGui::EndChild();
}

void UI::EnsureAuthBootstrap()
{
    if (m_authBootstrapped)
        return;
    m_authBootstrapped = true;
    const AuthState prev = m_authState;
    m_authState = AuthState::Login;
    std::string err;
    m_authService.StartupCheckSession(err);
    const std::string& remembered = m_authService.GetRememberedUsername();
    if (!remembered.empty())
        std::snprintf(m_loginUser, sizeof(m_loginUser), "%s", remembered.c_str());
    m_authHint = "请登录";
    AuthUiLogTransition(m_authService, prev, m_authState, m_authHint,
                        "EnsureAuthBootstrap: always Login (no token auto-login; remembered username only)");
}

void UI::DrawLoginPage()
{
    ImGui::TextUnformatted("账户登录");
    ImGui::Separator();
    ImGui::Spacing();
    const float formW = 360.0f;
    ImGui::SetNextItemWidth(formW);
    ImGui::InputText("用户名", m_loginUser, sizeof(m_loginUser));
    ImGui::SetNextItemWidth(formW);
    ImGui::InputText("密码", m_loginPass, sizeof(m_loginPass), ImGuiInputTextFlags_Password);
    ImGui::Spacing();
    if (ImGui::Button("登录", ImVec2(140, 36)))
    {
        if (kAuthDebugLog)
            LogInfo("[AuthUI] login_button_clicked user=%s", m_loginUser);
        std::string err;
        const AuthState prev = m_authState;
        AuthFlowResult r = m_authService.Login(m_loginUser, m_loginPass, err);
        if (r == AuthFlowResult::Success || r == AuthFlowResult::Expired)
        {
            if (r == AuthFlowResult::Expired)
            {
                m_authState = AuthState::Expired;
                m_authHint.clear();
                AuthUiLogTransition(m_authService, prev, m_authState, m_authHint,
                                    "Login -> Expired (membership inactive)");
            }
            else
            {
                m_authState = AuthState::Authenticated;
                m_nextAuthRefreshTickMs = GetTickCount64() + 60 * 1000ULL;
                m_authHint = "登录成功";
                AuthUiLogTransition(m_authService, prev, m_authState, m_authHint,
                                    "Login -> Authenticated");
            }
            m_loginPass[0] = '\0';
        }
        else
        {
            m_authHint = err.empty() ? "登录失败" : err;
            AuthUiLogTransition(m_authService, prev, m_authState, m_authHint, "Login -> Failed (stay on Login page)");
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("去注册", ImVec2(120, 36)))
    {
        const AuthState prev = m_authState;
        m_authState = AuthState::Register;
        AuthUiLogTransition(m_authService, prev, m_authState, m_authHint, "Login -> Register navigation");
    }
}

void UI::DrawRegisterPage()
{
    ImGui::TextUnformatted("注册账户");
    ImGui::Separator();
    ImGui::Spacing();
    const float formW = 360.0f;
    ImGui::SetNextItemWidth(formW);
    ImGui::InputText("用户名", m_regUser, sizeof(m_regUser));
    ImGui::SetNextItemWidth(formW);
    ImGui::InputText("密码", m_regPass, sizeof(m_regPass), ImGuiInputTextFlags_Password);
    ImGui::SetNextItemWidth(formW);
    ImGui::InputText("注册码", m_regCode, sizeof(m_regCode));
    ImGui::Spacing();
    if (ImGui::Button("注册", ImVec2(140, 36)))
    {
        std::string err;
        if (m_authService.Register(m_regUser, m_regPass, m_regCode, err))
        {
            const AuthState prev = m_authState;
            m_authHint = "注册成功，请登录";
            m_authState = AuthState::Login;
            m_authService.RememberLoginUsername(m_regUser);
            AuthUiLogTransition(m_authService, prev, m_authState, m_authHint, "Register success -> Login");
            std::snprintf(m_loginUser, sizeof(m_loginUser), "%s", m_regUser);
            m_regPass[0] = '\0';
            m_regCode[0] = '\0';
        }
        else
        {
            m_authHint = err.empty() ? "注册失败" : err;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("返回登录", ImVec2(120, 36)))
    {
        const AuthState prev = m_authState;
        m_authState = AuthState::Login;
        AuthUiLogTransition(m_authService, prev, m_authState, m_authHint, "Register -> back Login");
    }
}

void UI::DrawActivatePage()
{
    ImGui::TextUnformatted("会员激活 / 续费");
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::SetNextItemWidth(360.0f);
    ImGui::InputText("注册码", m_activateCode, sizeof(m_activateCode));
    ImGui::Spacing();
    if (ImGui::Button("激活/续费", ImVec2(140, 36)))
    {
        if (kAuthDebugLog)
            LogInfo("[AuthUI] Activate_page: submit renew keyLen=%zu", std::strlen(m_activateCode));
        std::string err;
        AuthFlowResult ar = m_authService.Activate(m_activateCode, err);
        if (ar == AuthFlowResult::Success || ar == AuthFlowResult::Expired)
        {
            const AuthState prev = m_authState;
            m_authHint = "续费成功，请重新登录以同步最新会员状态";
            m_activateCode[0] = '\0';
            if (kAuthDebugLog)
                LogInfo("[AuthUI] Activate_page: renew success -> Logout -> Login");
            m_authService.Logout();
            m_authState = AuthState::Login;
            m_nextAuthRefreshTickMs = 0;
            AuthUiLogTransition(m_authService, prev, m_authState, m_authHint,
                                "Activate page: renew OK -> Logout -> Login");
        }
        else
        {
            m_authHint = err.empty() ? "激活失败" : err;
        }
    }
    ImGui::SameLine();
    if (!m_authService.IsIdentityVerified())
    {
        if (ImGui::Button("返回登录", ImVec2(120, 36)))
        {
            const AuthState prev = m_authState;
            m_authState = AuthState::Login;
            AuthUiLogTransition(m_authService, prev, m_authState, m_authHint, "Activate -> Login");
        }
    }
    else
    {
        if (ImGui::Button("返回主界面", ImVec2(120, 36)))
        {
            const AuthState prev = m_authState;
            m_authState = AuthState::Authenticated;
            AuthUiLogTransition(m_authService, prev, m_authState, m_authHint, "Activate -> Authenticated");
        }
    }
}

static void DrawExpiredPageCommon(const AuthUserInfo& u)
{
    ImGui::TextUnformatted("会员状态：已过期");
    ImGui::Separator();
    ImGui::Text("账号：%s", u.username.empty() ? "未知" : u.username.c_str());
    ImGui::Text("到期时间：%s", u.expiryText.empty() ? "未知" : u.expiryText.c_str());
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(IM_COL32(210, 78, 78, 255)),
        "账号已过期，暂不能进入主界面，请续费后继续使用");
    ImGui::Spacing();
}

void UI::DrawAuthGate()
{
    ImGui::BeginChild("auth_gate", ImVec2(0, 0), false, ImGuiWindowFlags_NoScrollbar);
    ImGui::Spacing();
    ImGui::TextUnformatted("知觉Ai (推流端) - 账户认证");
    ImGui::TextDisabled("通过认证后进入主界面");
    ImGui::TextDisabled("安全策略：账号默认绑定机器（管理员可在后台重置设备绑定）");
    if (!m_authHint.empty())
    {
        ImGui::Spacing();
        // Expired 页面会显示固定说明，避免全局提示重复。
        const bool suppressHintInExpired = (m_authState == AuthState::Expired) && (m_authHint.find("账号已过期") != std::string::npos);
        if (!suppressHintInExpired)
        {
            const bool ok = (m_authHint.find("成功") != std::string::npos) || (m_authHint.find("已自动登录") != std::string::npos);
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(ok ? AccentU32() : IM_COL32(210, 78, 78, 255)), "%s", m_authHint.c_str());
            ImGui::Spacing();
        }
    }

    switch (m_authState)
    {
    case AuthState::CheckingSession:
        ImGui::TextUnformatted("正在检查本地会话...");
        break;
    case AuthState::Login:
        DrawLoginPage();
        break;
    case AuthState::Register:
        DrawRegisterPage();
        break;
    case AuthState::Activate:
        DrawActivatePage();
        break;
    case AuthState::Expired:
    {
        const AuthUserInfo& u = m_authService.GetUserInfo();
        DrawExpiredPageCommon(u);
        ImGui::SetNextItemWidth(360.0f);
        ImGui::InputText("注册码", m_activateCode, sizeof(m_activateCode));
        ImGui::Spacing();
        if (ImGui::Button("立即续费", ImVec2(140, 36)))
        {
            if (kAuthDebugLog)
                LogInfo("[AuthUI] Expired_page: 立即续费 clicked keyLen=%zu", std::strlen(m_activateCode));
            std::string err;
            AuthFlowResult ar = m_authService.Activate(m_activateCode, err);
            if (ar == AuthFlowResult::Success || ar == AuthFlowResult::Expired)
            {
                const AuthState prev = m_authState;
                m_authHint = "续费成功，请重新登录以同步最新会员状态";
                m_activateCode[0] = '\0';
                if (kAuthDebugLog)
                    LogInfo("[AuthUI] Expired_page: renew success -> Logout -> Login (re-login expected)");
                m_authService.Logout();
                m_authState = AuthState::Login;
                m_nextAuthRefreshTickMs = 0;
                AuthUiLogTransition(m_authService, prev, m_authState, m_authHint,
                                    "Expired: renew OK -> Logout -> Login");
            }
            else
            {
                m_authHint = err.empty() ? "续费失败" : err;
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("退出登录", ImVec2(120, 36)))
        {
            const AuthState prev = m_authState;
            m_authService.Logout();
            m_authState = AuthState::Login;
            m_authHint = "已退出登录";
            AuthUiLogTransition(m_authService, prev, m_authState, m_authHint, "Expired: Logout");
        }
        break;
    }
    case AuthState::Authenticated:
    default:
        ImGui::TextUnformatted("认证通过");
        break;
    }
    ImGui::EndChild();
}

int UI::Run(AppCore& app)
{
    WNDCLASSEXW wc{ sizeof(WNDCLASSEXW), CS_CLASSDC, UI::WndProc, 0L, 0L,
                    GetModuleHandleW(nullptr), nullptr, nullptr, nullptr, nullptr,
                    L"ScreenSenderUI", nullptr };
    if (!RegisterClassExW(&wc))
        return 1;

    m_hwnd = CreateWindowW(
        wc.lpszClassName,
        kAppTitleW,
        WS_POPUP | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU,
        100, 100, 1180, 720,
        nullptr, nullptr, wc.hInstance, nullptr);
    SetWindowLongPtrW(m_hwnd, GWLP_USERDATA, (LONG_PTR)this);

    if (!CreateDeviceD3D(m_hwnd))
    {
        CleanupDeviceD3D();
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }

    ShowWindow(m_hwnd, SW_SHOWDEFAULT);
    UpdateWindow(m_hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();

    // 字体：优先微软雅黑，其次黑体；使用中文 GlyphRanges
    ImFontConfig font_cfg;
    font_cfg.SizePixels = 16.0f;
    const ImWchar* ranges = io.Fonts->GetGlyphRangesChineseFull();
    bool fontLoaded = false;
    if (GetFileAttributesW(L"C:\\Windows\\Fonts\\msyh.ttc") != INVALID_FILE_ATTRIBUTES)
        fontLoaded = (io.Fonts->AddFontFromFileTTF("C:/Windows/Fonts/msyh.ttc", 16.0f, &font_cfg, ranges) != nullptr);
    if (!fontLoaded && GetFileAttributesW(L"C:\\Windows\\Fonts\\simhei.ttf") != INVALID_FILE_ATTRIBUTES)
        fontLoaded = (io.Fonts->AddFontFromFileTTF("C:/Windows/Fonts/simhei.ttf", 16.0f, &font_cfg, ranges) != nullptr);
    if (!fontLoaded)
        io.Fonts->AddFontDefault();

    // 标题字体：不使用 SetWindowFontScale，使用独立字号（更稳、更像正式客户端）
    g_fontTitle = nullptr;
    g_fontSub = nullptr;
    ImFontConfig title_cfg;
    // 主标题再强化一档
    title_cfg.SizePixels = 24.0f;
    if (GetFileAttributesW(L"C:\\Windows\\Fonts\\msyh.ttc") != INVALID_FILE_ATTRIBUTES)
        g_fontTitle = io.Fonts->AddFontFromFileTTF("C:/Windows/Fonts/msyh.ttc", 24.0f, &title_cfg, ranges);
    if (!g_fontTitle && GetFileAttributesW(L"C:\\Windows\\Fonts\\simhei.ttf") != INVALID_FILE_ATTRIBUTES)
        g_fontTitle = io.Fonts->AddFontFromFileTTF("C:/Windows/Fonts/simhei.ttf", 24.0f, &title_cfg, ranges);

    // 副标题更小字号（更弱）
    ImFontConfig sub_cfg;
    sub_cfg.SizePixels = 13.5f;
    if (GetFileAttributesW(L"C:\\Windows\\Fonts\\msyh.ttc") != INVALID_FILE_ATTRIBUTES)
        g_fontSub = io.Fonts->AddFontFromFileTTF("C:/Windows/Fonts/msyh.ttc", 13.5f, &sub_cfg, ranges);
    if (!g_fontSub && GetFileAttributesW(L"C:\\Windows\\Fonts\\simhei.ttf") != INVALID_FILE_ATTRIBUTES)
        g_fontSub = io.Fonts->AddFontFromFileTTF("C:/Windows/Fonts/simhei.ttf", 13.5f, &sub_cfg, ranges);

    ApplyTheme();

    ImGui_ImplWin32_Init(m_hwnd);
    ImGui_ImplDX11_Init(m_device.Get(), m_context.Get());

    bool running = false;
    EnsureAuthBootstrap();
    m_nextAuthRefreshTickMs = (m_authState == AuthState::Authenticated) ? (GetTickCount64() + 60 * 1000ULL) : 0;

    MSG msg{};
    while (msg.message != WM_QUIT)
    {
        while (PeekMessageW(&msg, nullptr, 0U, 0U, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        ImGui::Begin(kAppTitle, nullptr,
            ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoTitleBar |
            ImGuiWindowFlags_NoBringToFrontOnFocus);

        // 自定义顶部 Header（替代系统标题栏的视觉存在感）
        DrawHeaderBar(m_hwnd, app);
        ImGui::Spacing();

        if (m_authState != AuthState::Authenticated)
        {
            DrawAuthGate();
        }
        else
        {
            // 运行时会员过期守卫：定期刷新当前会员状态，过期立即切换到 Expired 并停止推流。
            const uint64_t nowTick = GetTickCount64();
            if (m_nextAuthRefreshTickMs != 0 && nowTick >= m_nextAuthRefreshTickMs)
            {
                const AuthState prev = m_authState;
                std::string err;
                if (kAuthDebugLog)
                    LogInfo("[AuthUI] RefreshSession guard tick fired");
                AuthFlowResult rr = m_authService.RefreshSession(err);
                if (rr == AuthFlowResult::Expired)
                {
                    m_authState = AuthState::Expired;
                    m_authHint.clear();
                    AuthUiLogTransition(m_authService, prev, m_authState, m_authHint,
                                        "RefreshSession -> Expired");
                    if (app.IsRunning())
                        app.Stop();
                }
                else if (rr == AuthFlowResult::Failed)
                {
                    m_authState = AuthState::Login;
                    m_authHint = err.empty() ? "会话校验失败，请重新登录" : err;
                    AuthUiLogTransition(m_authService, prev, m_authState, m_authHint,
                                        "RefreshSession -> Failed -> Login");
                    if (app.IsRunning())
                        app.Stop();
                }
                else if (kAuthDebugLog)
                    AuthUiLogTransition(m_authService, prev, m_authState, m_authHint,
                                        "RefreshSession -> Success (stay Authenticated)");
                m_nextAuthRefreshTickMs = nowTick + 60 * 1000ULL;
            }

            // 主内容：左侧导航 + 中间卡片 + 右侧预览+按钮+日志
            ImGui::PushStyleColor(ImGuiCol_ChildBg, g_panelBg);
            ImGui::BeginChild("main_row", ImVec2(0, 0), false, ImGuiWindowFlags_NoScrollbar);
            ImGui::PopStyleColor();
            DrawSidebar(m_selectedNav);
            ImGui::SameLine();

            // 中间：卡片设置（左侧菜单为唯一导航）
            ImGui::PushStyleColor(ImGuiCol_ChildBg, g_panelBg);
            ImGui::BeginChild("mid", ImVec2(430, 0), false, ImGuiWindowFlags_NoScrollbar);
            ImGui::PopStyleColor();
            switch (m_selectedNav)
            {
            case 0: DrawSystemMid(app); break;
            case 1: DrawParamsMid(app); break;
            case 2: DrawCaptureMid(app); break;
            case 3: DrawAdvancedMid(app); break;
            default: DrawSystemMid(app); break;
            }
            ImGui::EndChild();

            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_ChildBg, g_panelBg);
            ImGui::BeginChild("right", ImVec2(0, 0), false, ImGuiWindowFlags_NoScrollbar);
            ImGui::PopStyleColor();
            DrawSystemRight(app, running);
            ImGui::EndChild();
            ImGui::EndChild(); // main_row
        }

        ImGui::End();

        ImGui::Render();
        const float clearColor[4] = { g_appBg.x, g_appBg.y, g_appBg.z, g_appBg.w };
        m_context->OMSetRenderTargets(1, m_rtv.GetAddressOf(), nullptr);
        m_context->ClearRenderTargetView(m_rtv.Get(), clearColor);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        m_swapChain->Present(1, 0);
    }

    app.Stop();

    CleanupRenderTarget();
    CleanupDeviceD3D();
    DestroyWindow(m_hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return 0;
}

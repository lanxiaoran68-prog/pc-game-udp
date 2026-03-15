// =============================================================================
// ui.cpp - 高级无边框 ImGui 界面实现（左右分栏 + 三面板 + 标题区拖拽）
// =============================================================================

#include "ui.h"
#include "imgui.h"
#include <Windows.h>
#include <string>
#include <cstring>

// 与 main.cpp 中窗口尺寸一致
static const int kWindowWidth = 550;
static const int kWindowHeight = 400;

namespace DiagTool {

namespace {

enum class Panel { DataLink, Security, Status };
static Panel s_currentPanel = Panel::DataLink;

// 数据链路面板使用的临时配置（与 streamer 解耦，仅 UI 状态）
static char s_targetIp[64] = "127.0.0.1";
static char s_targetPort[16] = "9999";
static int  s_jpegQuality = 80;
static int  s_targetFps = 60;

// 标题区高度（与 main 中 WM_NCHITTEST 的 32 像素一致，仅用于布局）
const float kTitleBarHeight = 28.0f;

} // anonymous

void InitUIState()
{
    s_currentPanel = Panel::DataLink;
    strncpy_s(s_targetIp, "127.0.0.1", sizeof(s_targetIp) - 1);
    strncpy_s(s_targetPort, "9999", sizeof(s_targetPort) - 1);
    s_jpegQuality = 80;
    s_targetFps = 60;
}

void GetStreamConfigFromUI(NetStreamer::StreamConfig* outConfig)
{
    if (!outConfig) return;
    outConfig->targetIp = s_targetIp;
    outConfig->port = (uint16_t)atoi(s_targetPort);
    outConfig->jpegQuality = s_jpegQuality;
    outConfig->targetFps = s_targetFps;
    outConfig->hideWgcBorder = true; // 由调用方根据 g_hideWgcBorder 覆盖
}

// 左侧导航按钮：统一风格
static bool NavButton(const char* label, bool selected)
{
    if (selected)
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.56f, 0.27f, 0.68f, 0.6f));
    bool clicked = ImGui::Button(label, ImVec2(-1, 36));
    if (selected)
        ImGui::PopStyleColor();
    return clicked;
}

void RenderMainUI(
    const NetStreamer::StreamConfig& /* streamConfig */,
    NetStreamer::StreamStats& streamStats,
    bool* pWdaEnabled,
    bool* pHideWgcBorder,
    bool* pStreamToggle,
    void* mainWindowHwnd)
{
    HWND hwnd = (HWND)mainWindowHwnd;

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2((float)kWindowWidth, (float)kWindowHeight));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("MainWindow", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoCollapse);

    const float sideBarWidth = 120.0f;
    const float rightWidth = ImGui::GetContentRegionAvail().x - sideBarWidth;

    // ---- 标题区（用于无边框拖拽）----
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.15f, 0.15f, 0.17f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.2f, 0.2f, 0.22f, 1.0f));
    ImGui::Button("##TitleBar", ImVec2(ImGui::GetContentRegionAvail().x, kTitleBarHeight));
    ImGui::PopStyleColor(3);
    ImGui::SameLine(0, 0);
    ImGui::SetCursorPosX(12.0f);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - kTitleBarHeight + 6.0f);
    ImGui::TextUnformatted("DiagTool");

    ImGui::SetCursorPosY(kTitleBarHeight);
    ImGui::Separator();

    // ---- 左侧边栏 ----
    ImGui::BeginChild("SideBar", ImVec2(sideBarWidth, -1), true, ImGuiWindowFlags_NoScrollbar);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 8.0f);
    if (NavButton("数据链路", s_currentPanel == Panel::DataLink))
        s_currentPanel = Panel::DataLink;
    if (NavButton("安全防护", s_currentPanel == Panel::Security))
        s_currentPanel = Panel::Security;
    if (NavButton("运行状态", s_currentPanel == Panel::Status))
        s_currentPanel = Panel::Status;
    ImGui::EndChild();

    ImGui::SameLine(0, 0);
    ImGui::BeginChild("RightPanel", ImVec2(rightWidth, -1), true);

    switch (s_currentPanel)
    {
    case Panel::DataLink:
    {
        // 同步外部 config 到本地编辑（仅用于显示；实际发送用 GetStreamConfigFromUI）
        if (s_targetIp[0] == '\0') strncpy_s(s_targetIp, "127.0.0.1", sizeof(s_targetIp) - 1);
        ImGui::Text("目标地址");
        ImGui::SetNextItemWidth(-1);
        ImGui::InputText("##IP", s_targetIp, sizeof(s_targetIp), ImGuiInputTextFlags_CharsDecimal);
        ImGui::Text("端口");
        ImGui::SetNextItemWidth(-1);
        ImGui::InputText("##Port", s_targetPort, sizeof(s_targetPort), ImGuiInputTextFlags_CharsDecimal);
        ImGui::Spacing();
        ImGui::Text("画质 (JPEG Quality)");
        ImGui::SliderInt("##Quality", &s_jpegQuality, 30, 100, "%d");
        ImGui::Text("帧率限制 (FPS)");
        ImGui::SliderInt("##FPS", &s_targetFps, 30, 144, "%d");
        ImGui::Spacing();
        ImGui::Spacing();
        bool streaming = streamStats.isRunning;
        if (streaming)
        {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.6f, 0.2f, 0.2f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.75f, 0.25f, 0.25f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.5f, 0.15f, 0.15f, 1.0f));
        }
        if (ImGui::Button(streaming ? "断开 推流" : "启动 推流", ImVec2(-1, 44)))
            *pStreamToggle = !(*pStreamToggle);
        if (streaming)
            ImGui::PopStyleColor(3);
        break;
    }
    case Panel::Security:
    {
        ImGui::Checkbox("启用 WDA 界面隐身 (防截图)", pWdaEnabled);
        ImGui::Spacing();
        ImGui::Checkbox("擦除 WGC 捕获黄框", pHideWgcBorder);
        ImGui::Spacing();
        if (ImGui::Button("重置窗口特征", ImVec2(-1, 32)))
            NetStreamer::ResetWindowTitle(hwnd);
        break;
    }
    case Panel::Status:
    {
        ImGui::Text("推流状态: %s", streamStats.isRunning ? "运行中" : "已停止");
        ImGui::Text("已发送帧数: %llu", (unsigned long long)streamStats.framesSent);
        ImGui::Text("已发送字节: %llu", (unsigned long long)streamStats.bytesSent);
        ImGui::Text("实际 FPS: %.1f", streamStats.actualFps);
        if (!streamStats.lastError.empty())
            ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "错误: %s", streamStats.lastError.c_str());
        break;
    }
    }

    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::End();
}

} // namespace DiagTool

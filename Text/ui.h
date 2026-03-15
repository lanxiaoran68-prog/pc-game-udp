// =============================================================================
// ui.h - 高级无边框 ImGui 界面（左右分栏 + 三面板）
// 风格：深色电竞风、圆角、强调紫
// =============================================================================

#pragma once

#include "streamer.h"

namespace DiagTool {

// 初始化 UI 状态（在 ImGui 首次帧之前调用一次）
void InitUIState();

// 主界面绘制：无边框窗口内左右分栏，根据选中项切换右侧面板
// 拖拽移动在 UI 内部通过标题区 + SetWindowPos 实现
void RenderMainUI(
    const NetStreamer::StreamConfig& streamConfig,
    NetStreamer::StreamStats& streamStats,
    bool* pWdaEnabled,
    bool* pHideWgcBorder,
    bool* pStreamToggle,
    void* mainWindowHwnd  // 用于无边框拖拽 SetWindowPos 与 WDA/标题重置
);

// 从 UI 读取当前配置（供 main 传给 Streamer）
void GetStreamConfigFromUI(NetStreamer::StreamConfig* outConfig);

} // namespace DiagTool

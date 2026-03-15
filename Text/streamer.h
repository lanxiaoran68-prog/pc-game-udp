// =============================================================================
// streamer.h - 屏幕采集与 UDP 推流核心模块（WGC + TurboJPEG + Winsock2）
// 命名采用中性词：NetStreamer / DiagTool，避免敏感词汇
// =============================================================================

#pragma once

#include <cstdint>
#include <string>
#include <atomic>
#include <functional>
#include <memory>

namespace NetStreamer {

// 推流配置（由 UI 传入）
struct StreamConfig {
    std::string targetIp;
    uint16_t    port{ 0 };
    int         jpegQuality{ 80 };   // 30-100
    int         targetFps{ 60 };     // 30-144
    bool        hideWgcBorder{ true }; // 擦除 WGC 捕获黄框 -> IsBorderRequired = false
};

// 运行状态回调（供 UI 显示）
struct StreamStats {
    uint64_t framesSent{ 0 };
    uint64_t bytesSent{ 0 };
    double   actualFps{ 0.0 };
    bool     isRunning{ false };
    std::string lastError;
};

// 单例访问与生命周期
class StreamerEngine;
StreamerEngine* GetStreamerEngine();

// 启动/停止推流（在后台线程中执行 WGC 采集 + JPEG 压缩 + UDP 发送）
bool StartStreaming(const StreamConfig& config);
void StopStreaming();
bool IsStreaming();

// 获取当前统计（线程安全）
StreamStats GetStreamStats();

// 设置/清除 WDA 隐身（由 main 在收到 UI 请求时调用，hwnd 为主窗口）
void SetWdaExcludeFromCapture(void* hwnd, bool exclude);
void ResetWindowTitle(void* hwnd);

} // namespace NetStreamer

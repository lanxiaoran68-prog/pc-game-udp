#pragma once

#include <atomic>
#include <cstdint>
#include <vector>
#include <string>
#include <chrono>
#include <mutex>
#include <thread>

#include "capture_wgc.h"
#include "encoder_jpeg.h"
#include "udp_sender.h"

// AppCore 负责整体调度：Capture → Encode → Send，并统计 FPS，同时为 UI 提供预览帧。
class AppCore
{
public:
    struct Config
    {
        enum class CropMode
        {
            Center = 0,
            Custom = 1,
        };

        std::string ip         = "192.168.31.219";
        uint16_t    port       = 14523;
        int         width      = 640;
        int         height     = 640;
        int         jpegQuality = 85;
        int         targetFps   = 60;
        bool        hideBorder  = false;
        CropMode    cropMode    = CropMode::Center;
        int         customCropX = 0;
        int         customCropY = 0;
        int         customCropW = 640;
        int         customCropH = 640;
    };

    // 统一默认配置来源（避免默认值散落在 UI）
    static Config DefaultConfig();

    AppCore();
    ~AppCore();

    bool Initialize();
    void Run();  // 兼容老入口（阻塞）

    // UI 控制：在后台线程运行主循环
    bool Start();
    void Stop();
    bool IsRunning() const noexcept { return m_running.load(); }

    // 获取最新 BGRA 预览帧（拷贝输出，如无可用数据返回 false）
    bool GetLatestBgra(std::vector<uint8_t>& outBuffer, int& outWidth, int& outHeight, int& outStride);

    // 线程安全更新/读取配置
    void   SetConfig(const Config& newConfig);
    Config GetConfig() const;

    struct Stats
    {
        double fps = 0.0;
        double avgFrameBytes = 0.0;
        double sendBytesPerSec = 0.0;
        int    lastSendParts = 0;
        bool   lastSendOk = false;
        int    lastSendError = 0;        // WSAGetLastError
        std::string lastSendReason;     // 简要失败原因（如果可得）
        int    lastSendStage = 0;       // 0=none/ok, 1=senderNotReady, 2=endpointRebuildFailed, 3=sendFrameFailed
        std::string lastSendDetail;    // 更可诊断的简述（用于最近失败/日志）
    };

    Stats GetStats() const;

    struct Health
    {
        bool captureOk = false;
        bool encoderOk = false;
        bool senderReady = false; // 发送模块“可用/就绪”，不等同于“上一帧是否发送成功”
        std::string senderReadyReason; // senderReady=false 时，尽量给出明确原因（自检展示用）
    };

    Health GetHealth() const;

    struct HideBorderStatus
    {
        bool probed = false;
        bool supported = false;
        bool desiredHide = false;
        bool applied = false;
    };

    HideBorderStatus GetHideBorderStatus() const;

    struct StartupCheckResult
    {
        bool canStart = false;
        bool hasWarning = false;
        bool captureOk = false;
        bool encoderOk = false;
        bool senderReady = false;
        bool ipValid = false;
        bool portValid = false;
        bool resolutionValid = false;
        bool hideBorderSupportedKnown = false;
        bool hideBorderSupported = false;
        std::string primaryReason;
    };

    StartupCheckResult RunStartupCheck() const;

    struct RuntimeCaptureInfo
    {
        enum class PreviewSpaceMode
        {
            FullSource = 0,   // 预览缓冲对应“完整源空间”（fullSourceRect 坐标系）
            CroppedOutput = 1 // 预览缓冲对应“裁剪输出”（即 cropRect 区域被拷贝到 outBuffer）
        };

        int outputW = 0;
        int outputH = 0;
        RECT cropRect{};
        RECT fullSourceRect{}; // 裁剪计算所基于的完整源空间（坐标系与 cropRect 保持一致）
        bool usingCustomCrop = false;
        PreviewSpaceMode previewSpaceMode = PreviewSpaceMode::CroppedOutput;
    };
    RuntimeCaptureInfo GetRuntimeCaptureInfo() const;

    struct CriticalFailure
    {
        enum class Source
        {
            Startup = 0,
            Capture = 1,
            Encode  = 2,
            Send    = 3,
        };

        Source source = Source::Startup;
        std::string module;  // 可用于 UI 展示（中文/短语）
        std::string message; // 失败原因
        uint64_t seq = 0;     // 最近一次关键失败递增序号（简单即可）
    };

    CriticalFailure GetLastCriticalFailure() const;

private:
    void PrintStatsIfNeeded(uint64_t frameBytes);
    void WorkerLoop();

    void UpdateLastCriticalFailure(CriticalFailure::Source source, const std::string& message) const;

private:
    ScreenCaptureWGC m_capture;
    EncoderJPEG      m_encoder;
    UDPSender*       m_sender = nullptr;

    std::chrono::steady_clock::time_point m_lastStatsTime;
    uint64_t m_frameCount = 0;
    uint64_t m_totalBytes = 0;

    int m_lastWidth  = 0;
    int m_lastHeight = 0;

    static std::atomic<bool> s_shouldStop;
    static BOOL WINAPI ConsoleCtrlHandler(DWORD ctrlType);

    std::atomic<bool> m_running{ false };
    std::atomic<bool> m_stopRequested{ false };
    std::thread m_worker;

    std::mutex        m_previewMutex;
    std::vector<uint8_t> m_latestBgra;
    int m_latestW      = 0;
    int m_latestH      = 0;
    int m_latestStride = 0;

    // 运行时配置
    mutable std::mutex m_configMutex;
    Config             m_config;

    // 运行时统计
    mutable std::mutex m_statsMutex;
    Stats              m_stats;

    // 运行健康状态
    mutable std::mutex m_healthMutex;
    Health             m_health;

    // hideBorder 可验证状态
    mutable std::mutex m_hideBorderMutex;
    HideBorderStatus   m_hideBorderStatus;

    // runtime capture info：输出尺寸/裁剪区域等（与 hideBorder 语义隔离）
    mutable std::mutex m_runtimeInfoMutex;
    RuntimeCaptureInfo m_runtimeCaptureInfo;

    // 最近一次关键失败摘要（用于快速诊断，独立于日志系统）
    mutable std::mutex m_failureMutex;
    mutable CriticalFailure m_lastCriticalFailure;

    std::atomic<bool> m_initialized{ false };
};


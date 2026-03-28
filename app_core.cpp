#include "app_core.h"

#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>

std::atomic<bool> AppCore::s_shouldStop{ false };

AppCore::Config AppCore::DefaultConfig()
{
    Config c;
    c.ip = "192.168.31.219";
    c.port = 14523;
    c.width = 640;
    c.height = 640;
    c.jpegQuality = 85;
    c.targetFps = 60;
    c.hideBorder = false;
    c.cropMode = Config::CropMode::Center;
    c.customCropX = 0;
    c.customCropY = 0;
    c.customCropW = c.width;
    c.customCropH = c.height;
    return c;
}

AppCore::AppCore()
{
    m_config = DefaultConfig();
}

AppCore::~AppCore()
{
    Stop();
    if (m_sender)
    {
        delete m_sender;
        m_sender = nullptr;
    }
}

BOOL WINAPI AppCore::ConsoleCtrlHandler(DWORD ctrlType)
{
    if (ctrlType == CTRL_C_EVENT || ctrlType == CTRL_BREAK_EVENT || ctrlType == CTRL_CLOSE_EVENT)
    {
        s_shouldStop.store(true);
        return TRUE;
    }
    return FALSE;
}

bool AppCore::Initialize()
{
    if (!m_capture.Initialize())
    {
        LogError("AppCore: ScreenCaptureWGC Initialize failed.");
        return false;
    }

    // 使用默认配置中的 IP/端口 初始化 UDPSender
    Config cfg = GetConfig();
    // 让 hideBorder 立即生效（如果系统支持 WGC 边缘标记开关）
    m_capture.SetHideBorder(cfg.hideBorder);
    {
        auto st = m_capture.GetHideBorderStatus();
        std::lock_guard<std::mutex> lk(m_hideBorderMutex);
        m_hideBorderStatus.probed = st.probed;
        m_hideBorderStatus.supported = st.supported;
        m_hideBorderStatus.desiredHide = st.desiredHide;
        m_hideBorderStatus.applied = st.applied;
    }
    const uint8_t key = 0x5A;
    std::vector<uint8_t> obfIp(cfg.ip.size());
    for (size_t i = 0; i < cfg.ip.size(); ++i)
        obfIp[i] = static_cast<uint8_t>(cfg.ip[i] ^ key);
    uint16_t obfPort = static_cast<uint16_t>(cfg.port ^ key);

    m_sender = new UDPSender(obfIp.data(), obfIp.size(), obfPort, key);
    if (!m_sender || !m_sender->IsInitialized())
    {
        LogError("AppCore: UDPSender initialization failed.");
        {
            std::lock_guard<std::mutex> lk(m_healthMutex);
            m_health.senderReady = false;
            m_health.senderReadyReason = "UDPSender 初始化失败（端点或 Winsock 初始化失败）";
        }
        UpdateLastCriticalFailure(CriticalFailure::Source::Startup, "发送模块未就绪：UDPSender 初始化失败");
        return false;
    }

    m_lastStatsTime = std::chrono::steady_clock::now();
    m_frameCount = 0;
    m_totalBytes = 0;

    {
        std::lock_guard<std::mutex> lk(m_healthMutex);
        m_health.captureOk = m_capture.IsInitialized();
        m_health.encoderOk = m_encoder.IsValid();
        m_health.senderReady  = (m_sender && m_sender->IsInitialized());
        m_health.senderReadyReason.clear();
    }
    m_initialized.store(true, std::memory_order_release);

    if (!SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE))
    {
        LogError("SetConsoleCtrlHandler failed. GetLastError=%lu", GetLastError());
    }

    return true;
}

bool AppCore::Start()
{
    if (!m_initialized.load(std::memory_order_acquire))
    {
        LogError("AppCore::Start called before successful Initialize.");
        return false;
    }

    if (m_running.load())
        return true;

    m_stopRequested.store(false);
    s_shouldStop.store(false);
    try
    {
        m_worker = std::thread(&AppCore::WorkerLoop, this);
        m_running.store(true);
        return true;
    }
    catch (...)
    {
        LogError("AppCore::Start failed to create worker thread.");
        m_running.store(false);
        return false;
    }
}

void AppCore::Stop()
{
    m_stopRequested.store(true);
    s_shouldStop.store(true);

    if (m_worker.joinable())
        m_worker.join();

    m_running.store(false);
}

bool AppCore::GetLatestBgra(std::vector<uint8_t>& outBuffer, int& outWidth, int& outHeight, int& outStride)
{
    std::lock_guard<std::mutex> lock(m_previewMutex);
    if (m_latestBgra.empty() || m_latestW <= 0 || m_latestH <= 0 || m_latestStride <= 0)
        return false;

    outBuffer = m_latestBgra;
    outWidth  = m_latestW;
    outHeight = m_latestH;
    outStride = m_latestStride;
    return true;
}

void AppCore::SetConfig(const Config& newConfig)
{
    std::lock_guard<std::mutex> lock(m_configMutex);
    m_config = newConfig;
}

AppCore::Config AppCore::GetConfig() const
{
    std::lock_guard<std::mutex> lock(m_configMutex);
    return m_config;
}

AppCore::Stats AppCore::GetStats() const
{
    std::lock_guard<std::mutex> lock(m_statsMutex);
    return m_stats;
}

AppCore::Health AppCore::GetHealth() const
{
    std::lock_guard<std::mutex> lock(m_healthMutex);
    return m_health;
}

AppCore::HideBorderStatus AppCore::GetHideBorderStatus() const
{
    std::lock_guard<std::mutex> lock(m_hideBorderMutex);
    return m_hideBorderStatus;
}

AppCore::RuntimeCaptureInfo AppCore::GetRuntimeCaptureInfo() const
{
    std::lock_guard<std::mutex> lock(m_runtimeInfoMutex);
    return m_runtimeCaptureInfo;
}

AppCore::CriticalFailure AppCore::GetLastCriticalFailure() const
{
    std::lock_guard<std::mutex> lk(m_failureMutex);
    return m_lastCriticalFailure;
}

void AppCore::UpdateLastCriticalFailure(CriticalFailure::Source source, const std::string& message) const
{
    std::lock_guard<std::mutex> lk(m_failureMutex);
    // StartupCheck 会被 UI 每帧调用：避免重复递增导致“无关键错误/失败切换”看起来像在抖动。
    // 其它源（capture/encode/send）只在失败发生时才会更新，所以允许 seq 体现“最近一次”。
    if (source == CriticalFailure::Source::Startup &&
        m_lastCriticalFailure.source == source &&
        m_lastCriticalFailure.message == message)
        return;

    m_lastCriticalFailure.source = source;
    m_lastCriticalFailure.message = message;
    m_lastCriticalFailure.module = [&]() -> std::string {
        switch (source)
        {
            case CriticalFailure::Source::Startup: return "自检";
            case CriticalFailure::Source::Capture: return "捕获";
            case CriticalFailure::Source::Encode:  return "编码";
            case CriticalFailure::Source::Send:    return "发送";
            default: return "未知";
        }
    }();
    m_lastCriticalFailure.seq++;
}

AppCore::StartupCheckResult AppCore::RunStartupCheck() const
{
    StartupCheckResult r{};
    Config cfg = GetConfig();
    Health h = GetHealth();
    HideBorderStatus hb = GetHideBorderStatus();

    r.captureOk = h.captureOk;
    r.encoderOk = h.encoderOk;
    r.senderReady = h.senderReady;
    r.portValid = (cfg.port > 0 && cfg.port < 65536);
    r.resolutionValid = (cfg.width > 0 && cfg.height > 0);

    sockaddr_in sa{};
    r.ipValid = (InetPtonA(AF_INET, cfg.ip.c_str(), &(sa.sin_addr)) == 1);

    r.hideBorderSupportedKnown = hb.probed;
    r.hideBorderSupported = hb.supported;

    r.canStart = r.captureOk && r.encoderOk && r.senderReady && r.ipValid && r.portValid && r.resolutionValid;
    r.hasWarning = (!r.canStart) ? false : (hb.probed && !hb.supported && hb.desiredHide);

    if (!r.captureOk) r.primaryReason = "捕获模块未就绪";
    else if (!r.encoderOk) r.primaryReason = "编码模块未就绪";
    else if (!r.senderReady)
        r.primaryReason = (!h.senderReadyReason.empty()) ? h.senderReadyReason : "发送模块未就绪（senderReady=false）";
    else if (!r.ipValid) r.primaryReason = "目标IP无效";
    else if (!r.portValid) r.primaryReason = "目标端口无效";
    else if (!r.resolutionValid) r.primaryReason = "分辨率配置无效";
    else if (r.hasWarning) r.primaryReason = "hideBorder当前系统不支持";
    else r.primaryReason = "自检通过";

    // 只在“不可启动”时更新最近关键失败：避免 UI 每帧反复递增
    if (!r.canStart)
        UpdateLastCriticalFailure(CriticalFailure::Source::Startup, r.primaryReason);

    return r;
}

void AppCore::PrintStatsIfNeeded(uint64_t frameBytes)
{
    using namespace std::chrono;
    auto now = steady_clock::now();
    auto elapsed = duration_cast<seconds>(now - m_lastStatsTime);
    if (elapsed.count() >= 1)
    {
        double seconds = duration_cast<duration<double>>(now - m_lastStatsTime).count();
        double fps = seconds > 0.0 ? static_cast<double>(m_frameCount) / seconds : 0.0;
        double avgSize = m_frameCount > 0
                             ? static_cast<double>(m_totalBytes) / static_cast<double>(m_frameCount)
                             : 0.0;
        double sendBps = seconds > 0.0 ? static_cast<double>(m_totalBytes) / seconds : 0.0;

        {
            std::lock_guard<std::mutex> lock(m_statsMutex);
            m_stats.fps = fps;
            m_stats.avgFrameBytes = avgSize;
            m_stats.sendBytesPerSec = sendBps;
        }

        LogInfo("Stats: %dx%d, FPS=%.1f, AvgFrameSize=%.1f bytes",
                m_lastWidth, m_lastHeight, fps, avgSize);

        m_frameCount = 0;
        m_totalBytes = 0;
        m_lastStatsTime = now;
    }
    else
    {
        (void)frameBytes;
    }
}

void AppCore::WorkerLoop()
{
    std::vector<uint8_t> bgra;
    std::vector<uint8_t> jpeg;

    std::string lastIp;
    uint16_t lastPort = 0;
    const uint8_t key = 0x5A;

    while (!m_stopRequested.load())
    {
        // 拷贝当前配置
        Config cfg = GetConfig();
        if (cfg.targetFps <= 0)
            cfg.targetFps = 60;
        if (cfg.jpegQuality < 1) cfg.jpegQuality = 1;
        if (cfg.jpegQuality > 100) cfg.jpegQuality = 100;

        // 让 hideBorder 实时生效（无需重启推流）
        m_capture.SetHideBorder(cfg.hideBorder);
        m_capture.SetOutputSize(cfg.width, cfg.height);
        if (cfg.cropMode == Config::CropMode::Custom)
            m_capture.SetCustomCropMode(cfg.customCropX, cfg.customCropY, cfg.customCropW, cfg.customCropH);
        else
            m_capture.SetCenterCropMode();
        {
            auto st = m_capture.GetHideBorderStatus();
            std::lock_guard<std::mutex> lk(m_hideBorderMutex);
            m_hideBorderStatus.probed = st.probed;
            m_hideBorderStatus.supported = st.supported;
            m_hideBorderStatus.desiredHide = st.desiredHide;
            m_hideBorderStatus.applied = st.applied;
        }
        {
            std::lock_guard<std::mutex> lk(m_runtimeInfoMutex);
            m_runtimeCaptureInfo.fullSourceRect = m_capture.GetFullCaptureRect();
            m_runtimeCaptureInfo.cropRect = m_capture.GetCurrentCropRect();
            m_runtimeCaptureInfo.usingCustomCrop = (cfg.cropMode == Config::CropMode::Custom);
            // 当前 CaptureFrame 只拷贝 cropRect 对应的裁剪输出到 outBuffer
            m_runtimeCaptureInfo.previewSpaceMode = RuntimeCaptureInfo::PreviewSpaceMode::CroppedOutput;
        }

        auto frameInterval = std::chrono::milliseconds(1000 / cfg.targetFps);
        auto now = std::chrono::steady_clock::now();
        static auto nextFrameTime = now;
        if (now < nextFrameTime)
            std::this_thread::sleep_until(nextFrameTime);
        nextFrameTime = std::chrono::steady_clock::now() + frameInterval;

        int endpointRebuildFailedStage = 0; // 0=none, 2=endpointRebuildFailed
        std::string endpointRebuildFailedReason;

        // 如 IP/端口变更，则重建 UDPSender
        if (cfg.ip != lastIp || cfg.port != lastPort)
        {
            LogInfo("AppCore: endpoint changed to %s:%u, recreating UDPSender.", cfg.ip.c_str(), cfg.port);

            if (m_sender)
            {
                delete m_sender;
                m_sender = nullptr;
            }

            std::vector<uint8_t> obfIp(cfg.ip.size());
            for (size_t i = 0; i < cfg.ip.size(); ++i)
                obfIp[i] = static_cast<uint8_t>(cfg.ip[i] ^ key);
            uint16_t obfPort = static_cast<uint16_t>(cfg.port ^ key);

            m_sender = new UDPSender(obfIp.data(), obfIp.size(), obfPort, key);
            if (!m_sender || !m_sender->IsInitialized())
            {
                endpointRebuildFailedStage = 2;
                endpointRebuildFailedReason = "endpoint重建失败：UDPSender 初始化失败";
                {
                    std::lock_guard<std::mutex> lk(m_healthMutex);
                    m_health.senderReady = false;
                    m_health.senderReadyReason = endpointRebuildFailedReason;
                }
                LogError("AppCore: UDPSender recreation failed for %s:%u.", cfg.ip.c_str(), cfg.port);
                // 不更新 lastIp/lastPort，这样下次循环仍会尝试重建
            }
            else
            {
                lastIp   = cfg.ip;
                lastPort = cfg.port;
                {
                    std::lock_guard<std::mutex> lk(m_healthMutex);
                    m_health.senderReadyReason.clear();
                }
            }
        }

        int w = 0, h = 0, stride = 0;
        if (!m_capture.CaptureFrame(bgra, w, h, stride))
        {
            std::lock_guard<std::mutex> lk(m_healthMutex);
            m_health.captureOk = m_capture.IsInitialized();
            UpdateLastCriticalFailure(CriticalFailure::Source::Capture, "CaptureFrame 失败");
            continue;
        }

        {
            std::lock_guard<std::mutex> lock(m_previewMutex);
            m_latestBgra   = bgra;
            m_latestW      = w;
            m_latestH      = h;
            m_latestStride = stride;
        }

        m_lastWidth  = w;
        m_lastHeight = h;
        {
            std::lock_guard<std::mutex> lk(m_runtimeInfoMutex);
            m_runtimeCaptureInfo.outputW = w;
            m_runtimeCaptureInfo.outputH = h;
        }

        if (!m_encoder.Encode(bgra, w, h, stride, jpeg, cfg.jpegQuality))
        {
            std::lock_guard<std::mutex> lk(m_healthMutex);
            m_health.encoderOk = m_encoder.IsValid();
            UpdateLastCriticalFailure(CriticalFailure::Source::Encode, "Encode 失败");
            continue;
        }

        bool sendOk = false;
        int sendParts = 0;
        int lastSendErr = 0;

        int lastSendStage = 0; // 0=ok/none,1=senderNotReady,2=endpointRebuildFailed,3=sendFrameFailed
        std::string lastSendReason;
        std::string lastSendDetail;

        const bool senderReady = (m_sender && m_sender->IsInitialized());
        if (!senderReady)
        {
            sendOk = false;
            sendParts = 0;
            lastSendErr = 0;

            if (endpointRebuildFailedStage == 2)
            {
                lastSendStage = 2;
                lastSendReason = "endpoint重建失败";
                lastSendDetail = endpointRebuildFailedReason;
            }
            else
            {
                lastSendStage = 1;
                if (m_sender == nullptr)
                {
                    lastSendReason = "发送器未初始化";
                    lastSendDetail = "发送器未初始化（m_sender为空）";
                }
                else
                {
                    lastSendReason = "发送器未就绪";
                    lastSendDetail = "发送器未就绪（UDPSender未初始化）";
                }
            }
        }
        else
        {
            sendOk = m_sender->SendFrame(jpeg);
            auto last = m_sender->GetLastSendInfo();
            sendParts = last.parts;
            lastSendErr = last.lastError;

            if (sendOk)
            {
                lastSendStage = 0;
                lastSendReason = "OK";
                lastSendDetail = "SendFrame OK";
            }
            else
            {
                lastSendStage = 3;
                if (lastSendErr != 0)
                {
                    lastSendReason = "WSA错误码 " + std::to_string(lastSendErr);
                    lastSendDetail = "SendFrame 失败（" + lastSendReason + "）";
                }
                else
                {
                    lastSendReason = "发送失败（未知原因）";
                    lastSendDetail = "SendFrame 失败（未知原因）";
                }
            }
        }

        {
            std::lock_guard<std::mutex> lk(m_healthMutex);
            m_health.senderReady = senderReady;
            if (senderReady)
                m_health.senderReadyReason.clear(); // senderReady=true 时不保留“未就绪原因”
            else if (m_health.senderReadyReason.empty())
                m_health.senderReadyReason = lastSendDetail; // 尽量补齐原因
        }

        {
            std::lock_guard<std::mutex> lk(m_statsMutex);
            m_stats.lastSendOk = sendOk;
            m_stats.lastSendParts = sendParts;
            m_stats.lastSendError = lastSendErr;
            m_stats.lastSendReason = lastSendReason;
            m_stats.lastSendStage = lastSendStage;
            m_stats.lastSendDetail = lastSendDetail;
        }

        if (!sendOk)
            UpdateLastCriticalFailure(CriticalFailure::Source::Send, lastSendDetail);

        m_frameCount++;
        m_totalBytes += jpeg.size();
        PrintStatsIfNeeded(jpeg.size());
    }
}

void AppCore::Run()
{
    s_shouldStop.store(false);
    Start();
    while (!s_shouldStop.load())
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    Stop();
    LogInfo("AppCore::Run exiting due to Ctrl+C.");
}


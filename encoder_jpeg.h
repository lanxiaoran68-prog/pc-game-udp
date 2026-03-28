#pragma once

#include <vector>
#include <cstdint>

// TurboJPEG
#include <turbojpeg.h>

// 来自 capture_wgc.h 中的日志接口声明
void LogInfo(const char* fmt, ...);
void LogError(const char* fmt, ...);

// BGRA → JPEG 编码器（只在构造函数中初始化 TurboJPEG 句柄）
class EncoderJPEG
{
public:
    EncoderJPEG();
    ~EncoderJPEG();

    bool IsValid() const noexcept { return m_valid && m_handle != nullptr; }

    // 使用 TurboJPEG 将 BGRA 帧编码为 JPEG
    bool Encode(const std::vector<uint8_t>& bgraBuffer,
                int width,
                int height,
                int stride,
                std::vector<uint8_t>& outJpeg,
                int quality = 85);

private:
    tjhandle m_handle = nullptr;
    bool     m_valid  = false;
};


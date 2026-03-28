#include "encoder_jpeg.h"

EncoderJPEG::EncoderJPEG()
{
    // 只初始化一次 TurboJPEG 句柄
    m_handle = tjInitCompress();
    if (!m_handle)
    {
        const char* err = tjGetErrorStr();
        LogError("tjInitCompress failed: %s", err ? err : "unknown");
        m_valid = false;
    }
    else
    {
        m_valid = true;
        LogInfo("EncoderJPEG: TurboJPEG compressor initialized.");
    }
}

EncoderJPEG::~EncoderJPEG()
{
    if (m_handle)
    {
        tjDestroy(m_handle);
        m_handle = nullptr;
    }
}

bool EncoderJPEG::Encode(const std::vector<uint8_t>& bgraBuffer,
                         int width,
                         int height,
                         int stride,
                         std::vector<uint8_t>& outJpeg,
                         int quality)
{
    outJpeg.clear();

    if (!m_handle || !m_valid)
    {
        LogError("EncoderJPEG::Encode called with invalid TurboJPEG handle.");
        return false;
    }

    if (width <= 0 || height <= 0 || stride <= 0)
    {
        LogError("EncoderJPEG::Encode invalid params: %dx%d stride=%d", width, height, stride);
        return false;
    }

    if (bgraBuffer.empty())
    {
        LogError("EncoderJPEG::Encode input buffer empty.");
        return false;
    }

    if (quality < 1) quality = 1;
    if (quality > 100) quality = 100;

    unsigned char* jpegBuf = nullptr;
    unsigned long jpegSize = 0;

    int pixelFormat = TJPF_BGRA;
    int subsamp = TJSAMP_420;
    int flags = TJFLAG_NOREALLOC;

    unsigned long maxJpegSize = tjBufSize(width, height, subsamp);
    outJpeg.resize(maxJpegSize);
    jpegBuf = outJpeg.data();

    int ret = tjCompress2(
        m_handle,
        bgraBuffer.data(),
        width,
        stride,
        height,
        pixelFormat,
        &jpegBuf,
        &jpegSize,
        subsamp,
        quality,
        flags);

    if (ret != 0 || jpegSize == 0)
    {
        const char* err = tjGetErrorStr();
        LogError("tjCompress2 failed: %s", err ? err : "unknown");
        outJpeg.clear();
        return false;
    }

    outJpeg.resize(jpegSize);

    LogInfo("JPEG encoded: %dx%d quality=%d -> %zu bytes",
            width, height, quality, outJpeg.size());

    return true;
}


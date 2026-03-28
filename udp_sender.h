#pragma once

#include <vector>
#include <cstdint>
#include <string>

// Winsock2
#include <winsock2.h>
#include <ws2tcpip.h>

// 日志接口
void LogInfo(const char* fmt, ...);
void LogError(const char* fmt, ...);

// UDP 发送器：负责 MJPEG over UDP（单帧一个 UDP 包，带包长抖动和随机延迟）
class UDPSender
{
public:
    // obfIp:   XOR 过的 IP 字节数组
    // ipLen:   数组长度
    // obfPort: XOR 过的端口（uint16_t）
    // xorKey:  XOR 密钥
    UDPSender(const uint8_t* obfIp,
              size_t ipLen,
              uint16_t obfPort,
              uint8_t xorKey);

    ~UDPSender();

    bool IsInitialized() const noexcept { return m_initialized; }

    // 发送一帧 JPEG 数据
    //
    // - 按 1300~1420 字节范围随机选择实际发送长度
    // - 如果 jpegData.size() < 随机长度，则按实际大小发送
    // - 如果 jpegData.size() > 随机长度，目前仅发送前 randomLen 字节（单包，简单实现）
    //   后续如需完整可靠传输，可在此基础上做分包。
    //
    // - 在发送前加入 ±1ms 随机延迟
    // - sendto 成功/失败都会详细日志
    bool SendFrame(const std::vector<uint8_t>& jpegData);

    struct LastSendInfo
    {
        bool ok = false;
        int  parts = 0;
        int  bytes = 0;
        int  lastError = 0; // WSAGetLastError
    };

    LastSendInfo GetLastSendInfo() const noexcept { return m_last; }

private:
    bool InitializeWinsock();
    void DecryptEndpoint(const uint8_t* obfIp,
                         size_t ipLen,
                         uint16_t obfPort,
                         uint8_t xorKey);
    void CleanupWinsock();

private:
    bool   m_initialized = false;
    SOCKET m_socket      = INVALID_SOCKET;
    sockaddr_in m_addr{};

    std::string m_ip;
    uint16_t    m_port = 0;

    // 随机数引擎（用于包长抖动和延迟）
    uint32_t m_rngSeed = 0;

    // 全局 Winsock 引用计数
    static long s_refCount;
    static bool s_wsaInitialized;

    LastSendInfo m_last{};
};


#include "udp_sender.h"

#include <random>
#include <thread>
#include <chrono>
#include <cstring>

long UDPSender::s_refCount = 0;
bool UDPSender::s_wsaInitialized = false;

UDPSender::UDPSender(const uint8_t* obfIp,
                     size_t ipLen,
                     uint16_t obfPort,
                     uint8_t xorKey)
{
    if (!InitializeWinsock())
    {
        LogError("UDPSender: InitializeWinsock failed.");
        return;
    }

    DecryptEndpoint(obfIp, ipLen, obfPort, xorKey);

    if (m_ip.empty() || m_port == 0)
    {
        LogError("UDPSender: invalid endpoint after decryption.");
        CleanupWinsock();
        return;
    }

    m_socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (m_socket == INVALID_SOCKET)
    {
        int err = WSAGetLastError();
        LogError("UDPSender: socket() failed, err=%d", err);
        CleanupWinsock();
        return;
    }

    std::memset(&m_addr, 0, sizeof(m_addr));
    m_addr.sin_family = AF_INET;
    m_addr.sin_port = htons(m_port);

    if (inet_pton(AF_INET, m_ip.c_str(), &m_addr.sin_addr) != 1)
    {
        int err = WSAGetLastError();
        LogError("UDPSender: inet_pton failed for IP=%s, err=%d", m_ip.c_str(), err);
        ::closesocket(m_socket);
        m_socket = INVALID_SOCKET;
        CleanupWinsock();
        return;
    }

    // 简单随机种子：时间 + 线程 ID + 地址指针
    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    m_rngSeed = static_cast<uint32_t>(now ^ reinterpret_cast<uintptr_t>(this));

    LogInfo("UDPSender initialized. Target=%s:%u", m_ip.c_str(), m_port);
    m_initialized = true;
}

UDPSender::~UDPSender()
{
    if (m_socket != INVALID_SOCKET)
    {
        ::closesocket(m_socket);
        m_socket = INVALID_SOCKET;
    }

    CleanupWinsock();
}

bool UDPSender::InitializeWinsock()
{
    if (s_wsaInitialized)
    {
        ++s_refCount;
        return true;
    }

    WSADATA wsaData{};
    int ret = ::WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (ret != 0)
    {
        LogError("WSAStartup failed: %d", ret);
        return false;
    }

    s_wsaInitialized = true;
    s_refCount = 1;
    LogInfo("Winsock initialized.");
    return true;
}

void UDPSender::CleanupWinsock()
{
    if (!s_wsaInitialized)
    {
        return;
    }

    if (--s_refCount <= 0)
    {
        ::WSACleanup();
        s_wsaInitialized = false;
        s_refCount = 0;
        LogInfo("Winsock cleaned up.");
    }
}

void UDPSender::DecryptEndpoint(const uint8_t* obfIp,
                                size_t ipLen,
                                uint16_t obfPort,
                                uint8_t xorKey)
{
    if (!obfIp || ipLen == 0)
    {
        LogError("DecryptEndpoint: invalid obfIp.");
        return;
    }

    m_ip.clear();
    m_ip.reserve(ipLen);

    for (size_t i = 0; i < ipLen; ++i)
    {
        char c = static_cast<char>(obfIp[i] ^ xorKey);
        m_ip.push_back(c);
    }

    uint16_t port = static_cast<uint16_t>(obfPort ^ xorKey);
    m_port = port;
}

bool UDPSender::SendFrame(const std::vector<uint8_t>& jpegData)
{
    m_last = LastSendInfo{};
    if (!m_initialized || m_socket == INVALID_SOCKET)
    {
        LogError("UDPSender::SendFrame called on uninitialized sender.");
        m_last.ok = false;
        return false;
    }

    if (jpegData.empty())
    {
        LogError("UDPSender::SendFrame empty buffer.");
        m_last.ok = false;
        return false;
    }

    // 发送前的轻微抖动延迟（保持原逻辑）
    std::mt19937 rng(m_rngSeed + static_cast<uint32_t>(
        std::chrono::steady_clock::now().time_since_epoch().count()));
    std::uniform_int_distribution<int> delayDist(-1, 1);
    int delayMs = delayDist(rng);
    if (delayMs > 0)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
    }

    // 分包发送完整 JPEG（避免大 UDP 包导致丢包/不可达）
    // 不添加任何 header；接收端若按 JPEG SOI/EOI 边界拼接即可工作。
    constexpr int kMaxPayload = 1400;
    const size_t totalSize = jpegData.size();
    const int totalParts = static_cast<int>((totalSize + (kMaxPayload - 1)) / kMaxPayload);
    m_last.parts = totalParts;
    m_last.bytes = static_cast<int>(totalSize);

    for (int part = 0; part < totalParts; ++part)
    {
        const size_t offset = static_cast<size_t>(part) * kMaxPayload;
        const size_t remaining = totalSize - offset;
        const int sendLen = static_cast<int>(remaining > static_cast<size_t>(kMaxPayload) ? kMaxPayload : remaining);

        const char* buf = reinterpret_cast<const char*>(jpegData.data() + offset);

        int ret = ::sendto(
            m_socket,
            buf,
            sendLen,
            0,
            reinterpret_cast<const sockaddr*>(&m_addr),
            sizeof(m_addr));

        if (ret == SOCKET_ERROR)
        {
            int err = WSAGetLastError();
            LogError("sendto failed: err=%d, size=%d bytes (part %d/%d)",
                     err, sendLen, part + 1, totalParts);
            m_last.ok = false;
            m_last.lastError = err;
            return false;
        }

        LogInfo("sendto success: %d bytes (part %d/%d) to %s:%u",
                ret, part + 1, totalParts, m_ip.c_str(), m_port);
    }

    m_last.ok = true;
    return true;
}


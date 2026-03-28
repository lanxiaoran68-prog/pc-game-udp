#pragma once

#include <string>

// 认证链路开发态诊断日志总开关（置 false 可一键关闭 KeyAuth 请求/响应与解析透传）
inline constexpr bool kAuthDebugLog = true;

enum class AuthState
{
    CheckingSession = 0,
    Login,
    Register,
    Activate,
    Expired,
    Authenticated
};

struct AuthUserInfo
{
    bool valid = false;
    bool expired = false;
    bool expiryKnown = false;
    std::string username;
    std::string expiryText;
    long long expiryUnix = 0;
};

enum class AuthFlowResult
{
    Failed = 0,
    Success,
    Expired
};

class KeyAuthService
{
public:
    KeyAuthService();

    AuthFlowResult StartupCheckSession(std::string& errMsg);
    AuthFlowResult Login(const std::string& username, const std::string& password, std::string& errMsg);
    bool Register(const std::string& username, const std::string& password, const std::string& licenseKey, std::string& errMsg);
    AuthFlowResult Activate(const std::string& licenseKey, std::string& errMsg);
    AuthFlowResult RefreshSession(std::string& errMsg);
    void Logout();

    bool IsAuthenticated() const { return m_identityVerified && m_user.valid && !m_user.expired; }
    bool IsIdentityVerified() const { return m_identityVerified && m_user.valid; }
    /// 当前 KeyAuth 登录链路不依赖用户 token；通常为 false。仅兼容遗留或将来扩展。
    bool HasSessionToken() const { return !m_sessionToken.empty(); }
    const AuthUserInfo& GetUserInfo() const { return m_user; }
    /// 上次从 auth_session.ini 读取的用户名（仅用于启动时预填登录框，不代表已登录）
    const std::string& GetRememberedUsername() const { return m_rememberedUsername; }
    /// 仅写入用户名（注册成功后预填等），不表示已认证
    void RememberLoginUsername(const std::string& username) const;
    bool IsDeviceBindingEnabled() const { return true; }

private:
    bool EnsureInit(std::string& errMsg);
    AuthFlowResult ValidateToken(const std::string& token, std::string& errMsg);
    bool Request(const std::string& postBody, std::string& response, std::string& errMsg, const char* reqTag) const;
    void DbgLogPhase(const char* fn, const char* flowGoal, const std::string* optUsername, bool willSendHwid) const;

    bool ResolveHwid(std::string& hwid, std::string& errMsg) const;
    static bool IsHwidConflictError(const std::string& rawMsg);
    static std::string MapErrorToChinese(const std::string& rawMsg, const std::string& fallbackCn);
    static std::string UrlEncode(const std::string& s);
    static bool ParseBoolField(const std::string& json, const char* field);
    static std::string ParseStringField(const std::string& json, const char* field);
    static std::string ParseFirstStringField(const std::string& json, const char* const* keys, int keyCount);
    static std::string ParseErrorText(const std::string& json);
    static std::string ParseUserNameFromResponse(const std::string& json);
    static std::string ParseTokenFromResponse(const std::string& json);
    static bool IsExpiredError(const std::string& rawMsg);
    static long long ParseInt64Field(const std::string& json, const char* field);
    static long long ParseExpiryUnix(const std::string& json);
    static bool IsUserPayloadComplete(const AuthUserInfo& user);
    static std::string BuildExpiryText(long long unixTs);

    /// 仅持久化用户名（不写入 token，不作为免登录依据）
    void PersistRememberedUsername(const std::string& username) const;
    void LoadRememberedUsernameFromDisk();
    void ClearLocalSession();

    void ApplyLoginResponseUserState(const std::string& resp, const std::string& usernameFallback);

private:
    std::string m_sessionId;
    std::string m_sessionToken;
    std::string m_rememberedUsername;
    bool m_identityVerified = false;
    AuthUserInfo m_user;
};

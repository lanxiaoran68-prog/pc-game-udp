#include "auth_keyauth.h"

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <array>
#include <cctype>
#include <cstdarg>
#include <cstdio>

#pragma comment(lib, "winhttp.lib")

extern void LogInfo(const char* fmt, ...);

namespace
{
// 统一集中管理 KeyAuth 凭据（后续可切换为更安全注入方式）
constexpr const char* kKeyAuthName = "ScreenAi";
constexpr const char* kKeyAuthOwnerId = "SF8ETTjTut";
constexpr const char* kKeyAuthSecret = "efac0169956a15eaaaa66834938a0f11c2a196340dae6494961e00d4194d668b";
constexpr const char* kKeyAuthVersion = "1.0";
constexpr const wchar_t* kKeyAuthHost = L"keyauth.win";
constexpr const wchar_t* kKeyAuthPath = L"/api/1.2/";
constexpr const char* kSessionFile = "auth_session.ini";

void AuthDbgLog(const char* fmt, ...)
{
    if (!kAuthDebugLog)
        return;
    char buf[1000];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    LogInfo("[AuthDbg] %s", buf);
}

void AuthDbgLogHttpResponse(const char* reqTag, size_t rawLen, const std::string& sanitized)
{
    if (!kAuthDebugLog || !reqTag)
        return;
    LogInfo("[AuthDbg] HTTP %s resp rawLen=%zu sanitizedLen=%zu (chunked)", reqTag, rawLen, sanitized.size());
    constexpr size_t kChunk = 700;
    unsigned part = 0;
    for (size_t i = 0; i < sanitized.size(); i += kChunk, ++part)
    {
        const size_t remain = sanitized.size() - i;
        const size_t n = remain < kChunk ? remain : kChunk;
        LogInfo("[AuthDbg] HTTP %s body p%u: %.*s", reqTag, part, static_cast<int>(n), sanitized.c_str() + i);
    }
}

void LogAuthSessionAbnormal(const char* where, const char* detail)
{
    if (!kAuthDebugLog)
        return;
    LogInfo("[AuthDiag] session_abnormal where=%s detail=%s (user_hint=登录态信息异常，已清除本地会话，请重新登录)",
            where ? where : "?", detail ? detail : "?");
}

std::string MaskTokenForLog(const std::string& t)
{
    if (t.empty())
        return "(empty)";
    if (t.size() <= 8)
        return "****";
    return t.substr(0, 4) + "…(" + std::to_string(t.size()) + "chars)…" + t.substr(t.size() - 4);
}

std::string MaskHwidForLog(const std::string& h)
{
    if (h.empty())
        return "(empty)";
    if (h.size() <= 8)
        return "****";
    return h.substr(0, 3) + "…(" + std::to_string(h.size()) + ")…" + h.substr(h.size() - 3);
}

std::string SanitizeFormBodyForLog(std::string body)
{
    auto redactFull = [&](const char* name) {
        const std::string pref = std::string(name) + "=";
        size_t p = 0;
        while ((p = body.find(pref, p)) != std::string::npos)
        {
            const size_t s = p + pref.size();
            size_t e = body.find('&', s);
            if (e == std::string::npos)
                e = body.size();
            body.replace(s, e - s, "***");
            p = s + 3;
        }
    };
    redactFull("pass");
    redactFull("secret");
    redactFull("key");
    {
        const std::string pref = "token=";
        size_t p = 0;
        while ((p = body.find(pref, p)) != std::string::npos)
        {
            const size_t s = p + pref.size();
            size_t e = body.find('&', s);
            if (e == std::string::npos)
                e = body.size();
            const std::string raw = body.substr(s, e - s);
            const std::string masked = MaskTokenForLog(raw);
            body.replace(s, e - s, masked);
            p = s + masked.size();
        }
    }
    if (body.size() > 4000)
        body.resize(4000);
    return body;
}

void JsonRedactStringField(std::string& j, const char* field, bool asHwid)
{
    const std::string key = std::string("\"") + field + "\"";
    size_t pos = 0;
    while ((pos = j.find(key, pos)) != std::string::npos)
    {
        size_t colon = j.find(':', pos + key.size());
        if (colon == std::string::npos)
            break;
        size_t i = colon + 1;
        while (i < j.size() && (j[i] == ' ' || j[i] == '\t' || j[i] == '\n' || j[i] == '\r'))
            ++i;
        if (i >= j.size() || j[i] != '"')
        {
            pos = colon + 1;
            continue;
        }
        const size_t vstart = i + 1;
        const size_t vend = j.find('"', vstart);
        if (vend == std::string::npos)
            break;
        const std::string val = j.substr(vstart, vend - vstart);
        const std::string masked = asHwid ? MaskHwidForLog(val) : MaskTokenForLog(val);
        j.replace(vstart, vend - vstart, masked);
        pos = vstart + masked.size();
    }
}

std::string SanitizeKeyAuthJsonForLog(std::string j)
{
    static const char* kStringSecretFields[] = {
        "token", "session", "sessionid", "sessionId", "secret", "auth", "auth_token",
        "session_token", "sessiontoken", "password", "pass", "key"
    };
    for (const char* f : kStringSecretFields)
        JsonRedactStringField(j, f, false);
    JsonRedactStringField(j, "hwid", true);
    if (j.size() > 8000)
    {
        j.resize(8000);
        j += "…(truncated)";
    }
    return j;
}

struct ExpiryParseDetail
{
    long long unix = 0;
    const char* field = nullptr;
    std::string rawDigits;
    bool scaledFromMillis = false;
    bool fromSubscriptionsSlice = false;
};

ExpiryParseDetail ParseExpiryUnixDetail(const std::string& json)
{
    static const char* keys[] = {
        "expiry",
        "expires",
        "expire",
        "expiration",
        "expirydate",
        "expiresat",
        "expiry_at",
        "subscription_expiry",
        "subscription_expire",
        "expires_at",
        "expiry_timestamp"
    };

    auto parseLastInt64WithRaw = [&](const std::string& src, const char* field, std::string& outRaw) -> long long
    {
        const std::string key = std::string("\"") + field + "\"";
        const size_t p = src.rfind(key);
        if (p == std::string::npos)
            return 0;
        size_t c = src.find(':', p);
        if (c == std::string::npos)
            return 0;
        while (c + 1 < src.size() && (src[c + 1] == ' ' || src[c + 1] == '"' || src[c + 1] == '\t'))
            ++c;
        size_t b = c + 1;
        size_t e = b;
        while (e < src.size() && (src[e] == '-' || (src[e] >= '0' && src[e] <= '9')))
            ++e;
        if (e <= b)
            return 0;
        outRaw = src.substr(b, e - b);
        return _strtoi64(outRaw.c_str(), nullptr, 10);
    };

    auto trySlice = [&](const std::string& slice, bool subSlice) -> ExpiryParseDetail
    {
        ExpiryParseDetail d{};
        for (const char* k : keys)
        {
            std::string raw;
            long long v = parseLastInt64WithRaw(slice, k, raw);
            if (v > 0)
            {
                d.field = k;
                d.rawDigits = raw;
                d.scaledFromMillis = (v > 9999999999LL);
                if (d.scaledFromMillis)
                    v /= 1000LL;
                d.unix = v;
                d.fromSubscriptionsSlice = subSlice;
                return d;
            }
        }
        return d;
    };

    const size_t pos = json.find("\"subscriptions\"");
    if (pos != std::string::npos)
    {
        ExpiryParseDetail d = trySlice(json.substr(pos), true);
        if (d.unix > 0)
            return d;
    }
    return trySlice(json, false);
}

/// 提取 KeyAuth 顶层 `"info": { ... }` 对象文本（含大括号）；若无则返回空，由调用方回退用整包 resp。
std::string ExtractKeyAuthInfoObject(const std::string& json)
{
    const std::string key = "\"info\"";
    size_t p = json.find(key);
    if (p == std::string::npos)
        return {};
    p = json.find(':', p + key.size());
    if (p == std::string::npos)
        return {};
    ++p;
    while (p < json.size() && (json[p] == ' ' || json[p] == '\t' || json[p] == '\n' || json[p] == '\r'))
        ++p;
    if (p >= json.size() || json[p] != '{')
        return {};
    int depth = 0;
    const size_t start = p;
    for (size_t i = p; i < json.size(); ++i)
    {
        if (json[i] == '{')
            ++depth;
        else if (json[i] == '}')
        {
            --depth;
            if (depth == 0)
                return json.substr(start, i - start + 1);
        }
    }
    return {};
}

std::wstring ToWide(const std::string& s)
{
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 1) return L"";
    std::wstring ws;
    ws.resize(static_cast<size_t>(n - 1));
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, ws.data(), n);
    return ws;
}
}

KeyAuthService::KeyAuthService() = default;

void KeyAuthService::RememberLoginUsername(const std::string& username) const
{
    PersistRememberedUsername(username);
}

void KeyAuthService::ApplyLoginResponseUserState(const std::string& resp, const std::string& usernameFallback)
{
    const std::string info = ExtractKeyAuthInfoObject(resp);
    const std::string& primary = !info.empty() ? info : resp;
    std::string un = ParseStringField(primary, "username");
    if (un.empty())
        un = ParseUserNameFromResponse(primary);
    if (un.empty())
        un = ParseUserNameFromResponse(resp);
    if (un.empty())
        un = usernameFallback;

    m_user.username = un;
    m_sessionToken.clear();

    m_user.expiryUnix = ParseExpiryUnix(resp);
    m_user.expiryKnown = (m_user.expiryUnix > 0);
    m_user.expiryText = m_user.expiryKnown ? BuildExpiryText(m_user.expiryUnix) : std::string();

    const long long nowUnix = static_cast<long long>(std::time(nullptr));
    if (m_user.expiryKnown)
    {
        m_user.expired = (m_user.expiryUnix < nowUnix);
    }
    else
    {
        m_user.expired = ParseBoolField(primary, "expired") || ParseBoolField(primary, "subscription_expired") ||
                         ParseBoolField(resp, "expired") || ParseBoolField(resp, "subscription_expired");
    }
    m_user.valid = IsUserPayloadComplete(m_user);
    m_identityVerified = m_user.valid;
}

AuthFlowResult KeyAuthService::StartupCheckSession(std::string& errMsg)
{
    DbgLogPhase(__func__, "remember_username_only_no_auto_login", nullptr, false);
    m_identityVerified = false;
    m_user = {};
    m_sessionToken.clear();
    m_sessionId.clear();
    LoadRememberedUsernameFromDisk();
    errMsg.clear();
    if (kAuthDebugLog)
        AuthDbgLog("StartupCheckSession: token auto-login disabled; rememberedUsername=%s",
                   m_rememberedUsername.empty() ? "(none)" : m_rememberedUsername.c_str());
    return AuthFlowResult::Failed;
}

AuthFlowResult KeyAuthService::Login(const std::string& username, const std::string& password, std::string& errMsg)
{
    DbgLogPhase(__func__, "Success|Expired|Failed(login)", &username, true);
    if (!EnsureInit(errMsg))
        return AuthFlowResult::Failed;
    std::string hwid;
    if (!ResolveHwid(hwid, errMsg))
        return AuthFlowResult::Failed;
    if (kAuthDebugLog)
        AuthDbgLog("Login: hwid=%s", MaskHwidForLog(hwid).c_str());

    std::string body = "type=login&name=" + UrlEncode(kKeyAuthName) +
        "&ownerid=" + UrlEncode(kKeyAuthOwnerId) +
        "&sessionid=" + UrlEncode(m_sessionId) +
        "&username=" + UrlEncode(username) +
        "&pass=" + UrlEncode(password) +
        "&hwid=" + UrlEncode(hwid);

    std::string resp;
    if (!Request(body, resp, errMsg, "login"))
        return AuthFlowResult::Failed;

    if (!ParseBoolField(resp, "success"))
    {
        const std::string raw = ParseErrorText(resp);
        if (IsExpiredError(raw))
        {
            ApplyLoginResponseUserState(resp, username);
            m_user.expired = true;
            if (!m_user.valid)
            {
                LogAuthSessionAbnormal("Login:success_false_expired_branch", "username_missing_unable_to_confirm_identity");
                errMsg = "登录态信息异常，无法确认账号信息，请重试或联系管理员";
                m_identityVerified = false;
                m_user = {};
                return AuthFlowResult::Failed;
            }
            m_identityVerified = true;
            PersistRememberedUsername(m_user.username);
            errMsg = "账号已过期，暂不能进入主界面，请续费后继续使用";
            if (kAuthDebugLog)
                AuthDbgLog("Login: success=false expired message -> Expired from login payload (no token)");
            return AuthFlowResult::Expired;
        }
        errMsg = MapErrorToChinese(raw, "登录失败");
        if (IsHwidConflictError(raw))
        {
            ClearLocalSession();
            m_sessionToken.clear();
            m_rememberedUsername.clear();
            m_identityVerified = false;
            m_user = {};
        }
        return AuthFlowResult::Failed;
    }

    ApplyLoginResponseUserState(resp, username);
    if (!m_user.valid)
    {
        LogAuthSessionAbnormal("Login:success_true", "username_missing_after_login_response");
        errMsg = "登录态信息异常，无法确认账号信息，请检查网络或稍后重试";
        m_identityVerified = false;
        m_user = {};
        return AuthFlowResult::Failed;
    }

    PersistRememberedUsername(m_user.username);
    if (kAuthDebugLog)
        AuthDbgLog("Login: state from login response only (no user token). guardMode=local-only; manual backend expiry needs re-login or cached expiry reaching now. expired=%d expiryKnown=%d",
                   m_user.expired ? 1 : 0, m_user.expiryKnown ? 1 : 0);

    if (m_user.expired)
    {
        errMsg = "账号已过期，暂不能进入主界面，请续费后继续使用";
        return AuthFlowResult::Expired;
    }
    errMsg.clear();
    return AuthFlowResult::Success;
}

bool KeyAuthService::Register(const std::string& username, const std::string& password, const std::string& licenseKey, std::string& errMsg)
{
    DbgLogPhase(__func__, "bool register", &username, true);
    if (!EnsureInit(errMsg))
        return false;
    std::string hwid;
    if (!ResolveHwid(hwid, errMsg))
        return false;

    std::string body = "type=register&name=" + UrlEncode(kKeyAuthName) +
        "&ownerid=" + UrlEncode(kKeyAuthOwnerId) +
        "&sessionid=" + UrlEncode(m_sessionId) +
        "&username=" + UrlEncode(username) +
        "&pass=" + UrlEncode(password) +
        "&key=" + UrlEncode(licenseKey) +
        "&hwid=" + UrlEncode(hwid);

    std::string resp;
    if (!Request(body, resp, errMsg, "register"))
        return false;

    if (!ParseBoolField(resp, "success"))
    {
        const std::string raw = ParseErrorText(resp);
        errMsg = MapErrorToChinese(raw, "注册失败");
        if (kAuthDebugLog)
            AuthDbgLog("Register: success=false");
        return false;
    }
    if (kAuthDebugLog)
        AuthDbgLog("Register: success=true (user should login next)");
    return true;
}

AuthFlowResult KeyAuthService::Activate(const std::string& licenseKey, std::string& errMsg)
{
    DbgLogPhase(__func__, "Success|Failed(activate/upgrade)", nullptr, true);
    if (!IsIdentityVerified() || m_user.username.empty())
    {
        errMsg = "请先完成账号登录后再续费/激活";
        if (kAuthDebugLog)
            AuthDbgLog("Activate: blocked identityVerified=%d userEmpty=%d",
                       m_identityVerified ? 1 : 0, m_user.username.empty() ? 1 : 0);
        return AuthFlowResult::Failed;
    }
    if (!EnsureInit(errMsg))
        return AuthFlowResult::Failed;
    std::string hwid;
    if (!ResolveHwid(hwid, errMsg))
        return AuthFlowResult::Failed;

    // 已登录用户优先使用 upgrade，未登录则可用 license
    std::string body;
    const char* tag = "license";
    if (!m_user.username.empty())
    {
        tag = "upgrade";
        body = "type=upgrade&name=" + UrlEncode(kKeyAuthName) +
            "&ownerid=" + UrlEncode(kKeyAuthOwnerId) +
            "&sessionid=" + UrlEncode(m_sessionId) +
            "&username=" + UrlEncode(m_user.username) +
            "&key=" + UrlEncode(licenseKey) +
            "&hwid=" + UrlEncode(hwid);
    }
    else
    {
        body = "type=license&name=" + UrlEncode(kKeyAuthName) +
            "&ownerid=" + UrlEncode(kKeyAuthOwnerId) +
            "&sessionid=" + UrlEncode(m_sessionId) +
            "&key=" + UrlEncode(licenseKey) +
            "&hwid=" + UrlEncode(hwid);
    }

    if (kAuthDebugLog)
        AuthDbgLog("[RenewChain] Activate send type=%s user=%s keyLen=%zu sessionTok=%s",
                   tag, m_user.username.c_str(), licenseKey.size(), MaskTokenForLog(m_sessionToken).c_str());

    std::string resp;
    if (!Request(body, resp, errMsg, tag))
        return AuthFlowResult::Failed;

    if (!ParseBoolField(resp, "success"))
    {
        const std::string raw = ParseErrorText(resp);
        errMsg = MapErrorToChinese(raw, "激活失败");
        if (IsHwidConflictError(raw))
        {
            ClearLocalSession();
            m_sessionToken.clear();
            m_identityVerified = false;
            m_user = {};
        }
        return AuthFlowResult::Failed;
    }

    // 续费成功后：不要立刻依赖 ValidateToken 刷新 expiry。
    // 续费后的“最新会员状态同步”交给下一次重新登录，避免解析脆弱导致的登录态异常/死循环。
    if (kAuthDebugLog)
        AuthDbgLog("[RenewChain] Activate/upgrade HTTP success=true -> UI should hint re-login; next Logout clears session");
    errMsg = "续费成功";
    return AuthFlowResult::Success;
}

void KeyAuthService::Logout()
{
    if (kAuthDebugLog)
        AuthDbgLog("[RenewChain] Logout: clear auth memory; keep remembered username file only");
    const std::string lastUser = m_user.username;
    m_identityVerified = false;
    m_sessionToken.clear();
    m_sessionId.clear();
    m_user = {};
    if (!lastUser.empty())
    {
        PersistRememberedUsername(lastUser);
        m_rememberedUsername = lastUser;
    }
    else
    {
        ClearLocalSession();
        m_rememberedUsername.clear();
    }
}

AuthFlowResult KeyAuthService::RefreshSession(std::string& errMsg)
{
    DbgLogPhase(__func__, "Success|Expired|Failed(refresh)", nullptr, true);
    if (m_sessionToken.empty())
    {
        if (kAuthDebugLog)
            AuthDbgLog("RefreshSession: mode=local-only reason=no_user_token_for_keyauth_check available(sessionid=%d username=%d hwid=runtime)",
                       m_sessionId.empty() ? 0 : 1, m_user.username.empty() ? 0 : 1);
        if (!m_identityVerified || !m_user.valid)
        {
            errMsg = "本地会话已失效，请重新登录";
            if (kAuthDebugLog)
                AuthDbgLog("RefreshSession: local-only result=Failed reason=identity_not_established");
            return AuthFlowResult::Failed;
        }
        const long long nowUnix = static_cast<long long>(std::time(nullptr));
        if (m_user.expiryKnown && m_user.expiryUnix < nowUnix)
        {
            m_user.expired = true;
            errMsg = "账号已过期，暂不能进入主界面，请续费后继续使用";
            if (kAuthDebugLog)
                AuthDbgLog("RefreshSession: local-only result=Expired source=cached_expiry_unix");
            return AuthFlowResult::Expired;
        }
        m_user.expired = false;
        if (kAuthDebugLog)
            AuthDbgLog("RefreshSession: local-only result=Success source=cached_login_payload(no_remote_backend_probe)");
        return AuthFlowResult::Success;
    }
    if (kAuthDebugLog)
        AuthDbgLog("RefreshSession: mode=remote-check via type=check token=%s", MaskTokenForLog(m_sessionToken).c_str());
    AuthFlowResult vr = ValidateToken(m_sessionToken, errMsg);
    if (kAuthDebugLog)
        AuthDbgLog("RefreshSession: remote-check result=%d", static_cast<int>(vr));
    return vr;
}

bool KeyAuthService::EnsureInit(std::string& errMsg)
{
    DbgLogPhase(__func__, "obtain sessionId(init)", nullptr, false);
    if (!m_sessionId.empty())
    {
        if (kAuthDebugLog)
            AuthDbgLog("EnsureInit: reuse existing sid=%s", MaskTokenForLog(m_sessionId).c_str());
        return true;
    }

    // 注意：secret 仅发送给 KeyAuth，不在日志输出
    std::string body = "type=init&name=" + UrlEncode(kKeyAuthName) +
        "&ownerid=" + UrlEncode(kKeyAuthOwnerId) +
        "&ver=" + UrlEncode(kKeyAuthVersion) +
        "&secret=" + UrlEncode(kKeyAuthSecret);

    std::string resp;
    if (!Request(body, resp, errMsg, "init"))
        return false;

    if (!ParseBoolField(resp, "success"))
    {
        const std::string raw = ParseStringField(resp, "message");
        errMsg = MapErrorToChinese(raw, "KeyAuth 初始化失败");
        return false;
    }

    m_sessionId = ParseStringField(resp, "sessionid");
    if (m_sessionId.empty())
    {
        errMsg = "KeyAuth 会话初始化失败（sessionid 为空）";
        if (kAuthDebugLog)
            AuthDbgLog("EnsureInit: sessionid field empty in init response");
        return false;
    }
    if (kAuthDebugLog)
        AuthDbgLog("EnsureInit: new sid=%s", MaskTokenForLog(m_sessionId).c_str());
    return true;
}

AuthFlowResult KeyAuthService::ValidateToken(const std::string& token, std::string& errMsg)
{
    DbgLogPhase(__func__, "Success|Expired|Failed(check)", nullptr, true);
    if (token.empty())
    {
        errMsg = "本地会话已失效，请重新登录";
        if (kAuthDebugLog)
            AuthDbgLog("ValidateToken: fail reason=input_token_empty");
        return AuthFlowResult::Failed;
    }
    if (kAuthDebugLog)
        AuthDbgLog("ValidateToken: check with tok=%s", MaskTokenForLog(token).c_str());
    if (!EnsureInit(errMsg))
        return AuthFlowResult::Failed;
    std::string hwid;
    if (!ResolveHwid(hwid, errMsg))
        return AuthFlowResult::Failed;

    std::string body = "type=check&name=" + UrlEncode(kKeyAuthName) +
        "&ownerid=" + UrlEncode(kKeyAuthOwnerId) +
        "&sessionid=" + UrlEncode(m_sessionId) +
        "&token=" + UrlEncode(token) +
        "&hwid=" + UrlEncode(hwid);

    std::string resp;
    if (!Request(body, resp, errMsg, "check"))
        return AuthFlowResult::Failed;

    if (!ParseBoolField(resp, "success"))
    {
        const std::string raw = ParseErrorText(resp);
        if (kAuthDebugLog)
            AuthDbgLog("ValidateToken: check success=false rawErr=%s isExpiredErr=%d",
                        raw.c_str(), IsExpiredError(raw) ? 1 : 0);
        if (IsExpiredError(raw))
        {
            const std::string cachedUser = m_user.username;
            m_user.username = ParseUserNameFromResponse(resp);
            if (m_user.username.empty())
                m_user.username = cachedUser; // keep previous loaded username if any
            m_user.expiryUnix = ParseExpiryUnix(resp);
            m_user.expiryKnown = (m_user.expiryUnix > 0);
            if (kAuthDebugLog && !m_user.expiryKnown)
                AuthDbgLog("ValidateToken: expired branch expiry parse=0 (expiryKnown false, may show 待同步)");
            m_user.expiryText = m_user.expiryKnown ? BuildExpiryText(m_user.expiryUnix) : std::string();
            m_user.expired = true; // 后端明确提示已过期
            m_user.valid = !m_user.username.empty();
            m_identityVerified = m_user.valid;
            if (!m_user.valid)
            {
                LogAuthSessionAbnormal("ValidateToken:check_expired_branch",
                                       "username_empty_after_check_response_and_cache_empty");
                ClearLocalSession();
                m_sessionToken.clear();
                m_identityVerified = false;
                m_user = {};
                errMsg = "登录态信息异常，已清除本地会话，请重新登录";
                return AuthFlowResult::Failed;
            }
            errMsg = "账号已过期，暂不能进入主界面，请续费后继续使用";
            return AuthFlowResult::Expired;
        }
        errMsg = MapErrorToChinese(raw, "本地会话无效");
        if (kAuthDebugLog)
            AuthDbgLog("ValidateToken: non-expired error -> clear session (invalid/hwid/token)");
        // token 校验失败（特别是 HWID 冲突）后清理本地会话，允许管理员重置后直接重新登录。
        ClearLocalSession();
        m_sessionToken.clear();
        m_identityVerified = false;
        m_user = {};
        return AuthFlowResult::Failed;
    }

    const std::string cachedUser = m_user.username;
    m_user.username = ParseUserNameFromResponse(resp);
    if (m_user.username.empty())
        m_user.username = ParseStringField(resp, "username");
    if (m_user.username.empty())
        m_user.username = cachedUser;
    if (kAuthDebugLog && m_user.username.empty())
        AuthDbgLog("ValidateToken: warn username still empty after check success (cached also empty)");
    m_user.expiryUnix = ParseExpiryUnix(resp);
    m_user.expiryKnown = (m_user.expiryUnix > 0);
    m_user.expiryText = m_user.expiryKnown ? BuildExpiryText(m_user.expiryUnix) : std::string();
    const long long nowUnix = static_cast<long long>(std::time(nullptr));
    if (m_user.expiryKnown)
    {
        m_user.expired = (m_user.expiryUnix < nowUnix);
    }
    else
    {
        // expiry 解析失败时：不把会话当作异常，但允许根据后端布尔字段做最低限度判断
        // （避免过期账号因为 expiry 数值缺失而无法进入 Expired）。
        m_user.expired = ParseBoolField(resp, "expired") || ParseBoolField(resp, "subscription_expired");
    }
    if (kAuthDebugLog)
        AuthDbgLog("ValidateToken: check success user=%s expiryKnown=%d expired=%d vs_now=%lld",
                    m_user.username.c_str(), m_user.expiryKnown ? 1 : 0, m_user.expired ? 1 : 0,
                    static_cast<long long>(nowUnix));
    m_user.valid = IsUserPayloadComplete(m_user);
    m_sessionToken = token;
    m_identityVerified = m_user.valid;
    if (!m_user.valid)
    {
        LogAuthSessionAbnormal("ValidateToken:after_check_success",
                               "IsUserPayloadComplete_false_username_empty");
        ClearLocalSession();
        m_sessionToken.clear();
        m_identityVerified = false;
        m_user = {};
        errMsg = "登录态信息异常，已清除本地会话，请重新登录";
        return AuthFlowResult::Failed;
    }
    if (!m_user.username.empty())
        PersistRememberedUsername(m_user.username);
    if (m_user.expired)
    {
        errMsg = "账号已过期，暂不能进入主界面，请续费后继续使用";
        return AuthFlowResult::Expired;
    }
    return AuthFlowResult::Success;
}

void KeyAuthService::DbgLogPhase(const char* fn, const char* flowGoal, const std::string* optUsername,
                                 bool willSendHwid) const
{
    if (!kAuthDebugLog)
        return;
    const std::string& u = optUsername ? *optUsername : m_user.username;
    AuthDbgLog("%s goal=%s user=%s hasLocalToken=%d hasSessionId=%d willSendHwid=%d tok=%s sid=%s idVerified=%d",
               fn ? fn : "?", flowGoal ? flowGoal : "?",
               u.empty() ? "(none)" : u.c_str(),
               m_sessionToken.empty() ? 0 : 1,
               m_sessionId.empty() ? 0 : 1,
               willSendHwid ? 1 : 0,
               MaskTokenForLog(m_sessionToken).c_str(),
               MaskTokenForLog(m_sessionId).c_str(),
               m_identityVerified ? 1 : 0);
}

bool KeyAuthService::Request(const std::string& postBody, std::string& response, std::string& errMsg,
                             const char* reqTag) const
{
    response.clear();
    errMsg.clear();
    if (kAuthDebugLog && reqTag)
        AuthDbgLog("HTTP reqTag=%s body(sanitized)=%s", reqTag, SanitizeFormBodyForLog(postBody).c_str());

    HINTERNET hSession = WinHttpOpen(L"ScreenAi/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession)
    {
        errMsg = "网络初始化失败";
        return false;
    }

    HINTERNET hConnect = WinHttpConnect(hSession, kKeyAuthHost, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect)
    {
        WinHttpCloseHandle(hSession);
        errMsg = "连接 KeyAuth 失败";
        return false;
    }

    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST", kKeyAuthPath,
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!hRequest)
    {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        errMsg = "创建请求失败";
        return false;
    }

    const wchar_t* hdr = L"Content-Type: application/x-www-form-urlencoded\r\n";
    BOOL ok = WinHttpSendRequest(
        hRequest,
        hdr,
        static_cast<DWORD>(-1),
        reinterpret_cast<LPVOID>(const_cast<char*>(postBody.data())),
        static_cast<DWORD>(postBody.size()),
        static_cast<DWORD>(postBody.size()),
        0);
    if (ok) ok = WinHttpReceiveResponse(hRequest, nullptr);
    if (!ok)
    {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        errMsg = "请求 KeyAuth 失败";
        return false;
    }

    DWORD dwSize = 0;
    do
    {
        if (!WinHttpQueryDataAvailable(hRequest, &dwSize))
            break;
        if (dwSize == 0)
            break;
        std::string chunk;
        chunk.resize(dwSize);
        DWORD dwRead = 0;
        if (!WinHttpReadData(hRequest, chunk.data(), dwSize, &dwRead))
            break;
        chunk.resize(dwRead);
        response += chunk;
    } while (dwSize > 0);

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    if (response.empty())
    {
        errMsg = "KeyAuth 返回为空";
        if (kAuthDebugLog && reqTag)
            AuthDbgLog("HTTP reqTag=%s empty response", reqTag);
        return false;
    }
    if (kAuthDebugLog && reqTag)
        AuthDbgLogHttpResponse(reqTag, response.size(), SanitizeKeyAuthJsonForLog(response));
    return true;
}

bool KeyAuthService::ResolveHwid(std::string& hwid, std::string& errMsg) const
{
    // 首选 Windows MachineGuid：同机稳定、重启一致。
    HKEY hKey = nullptr;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Cryptography", 0,
                      KEY_READ | KEY_WOW64_64KEY, &hKey) == ERROR_SUCCESS)
    {
        std::array<char, 256> buf{};
        DWORD type = REG_SZ;
        DWORD cb = static_cast<DWORD>(buf.size());
        if (RegQueryValueExA(hKey, "MachineGuid", nullptr, &type,
                             reinterpret_cast<LPBYTE>(buf.data()), &cb) == ERROR_SUCCESS &&
            type == REG_SZ && buf[0] != '\0')
        {
            hwid = buf.data();
            RegCloseKey(hKey);
            return true;
        }
        RegCloseKey(hKey);
    }

    // 回退：卷序列号 + 计算机名，尽量保证稳定。
    char computer[256]{};
    DWORD cch = sizeof(computer);
    DWORD volSerial = 0;
    if (GetComputerNameA(computer, &cch) &&
        GetVolumeInformationA("C:\\", nullptr, 0, &volSerial, nullptr, nullptr, nullptr, 0))
    {
        std::ostringstream oss;
        oss << "FALLBACK-" << std::hex << std::uppercase << volSerial << "-" << computer;
        hwid = oss.str();
        return true;
    }

    errMsg = "设备标识获取失败，请以管理员权限运行后重试";
    return false;
}

std::string KeyAuthService::MapErrorToChinese(const std::string& rawMsg, const std::string& fallbackCn)
{
    if (rawMsg.empty())
        return fallbackCn;

    std::string s = rawMsg;
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    if (s.find("blank hwid") != std::string::npos)
        return "当前客户端未提供设备标识，无法完成登录校验。请重启后重试。";
    if (s.find("force hwid") != std::string::npos)
        return "账号启用了设备绑定保护，当前设备校验未通过。请联系管理员重置设备绑定后重试。";
    if (s.find("hwid mismatch") != std::string::npos || s.find("invalid hwid") != std::string::npos || s.find("hwid is invalid") != std::string::npos)
        return "该账号已绑定其它设备，当前机器无法登录。请联系管理员重置设备绑定后重试。";
    if (s.find("reset hwid") != std::string::npos || s.find("reset your hwid") != std::string::npos)
        return "该账号设备绑定冲突，请联系管理员重置 HWID；重置后可在当前机器重新登录绑定。";
    if (s.find("username already exists") != std::string::npos || s.find("already exists") != std::string::npos)
        return "用户名已存在，请更换用户名";
    if (s.find("invalid license") != std::string::npos || s.find("invalid key") != std::string::npos || s.find("key is invalid") != std::string::npos)
        return "注册码无效，请检查后重试";
    if (s.find("invalid username or password") != std::string::npos || s.find("invalid details") != std::string::npos || s.find("invalid username") != std::string::npos)
        return "用户名或密码错误";
    if (s.find("expired") != std::string::npos || s.find("subscription") != std::string::npos)
        return "账号已过期，暂不能进入主界面，请续费后继续使用";
    if (s.find("session") != std::string::npos || s.find("token") != std::string::npos)
        return "本地会话已失效，请重新登录";
    if (s.find("init") != std::string::npos)
        return "认证服务初始化失败，请稍后重试";
    if (s.find("network") != std::string::npos || s.find("request") != std::string::npos || s.find("timeout") != std::string::npos)
        return "网络请求失败，请检查网络后重试";

    return fallbackCn;
}

bool KeyAuthService::IsHwidConflictError(const std::string& rawMsg)
{
    if (rawMsg.empty())
        return false;
    std::string s = rawMsg;
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s.find("hwid") != std::string::npos ||
           s.find("force hwid") != std::string::npos ||
           s.find("blank hwid") != std::string::npos ||
           s.find("reset your hwid") != std::string::npos;
}

std::string KeyAuthService::UrlEncode(const std::string& s)
{
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s)
    {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~')
        {
            out.push_back(static_cast<char>(c));
        }
        else
        {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 15]);
        }
    }
    return out;
}

bool KeyAuthService::ParseBoolField(const std::string& json, const char* field)
{
    const std::string key = std::string("\"") + field + "\"";
    size_t p = json.find(key);
    if (p == std::string::npos) return false;
    p = json.find(':', p);
    if (p == std::string::npos) return false;
    const std::string tail = json.substr(p + 1, 8);
    return tail.find("true") != std::string::npos;
}

std::string KeyAuthService::ParseStringField(const std::string& json, const char* field)
{
    const std::string key = std::string("\"") + field + "\"";
    size_t p = json.find(key);
    if (p == std::string::npos) return "";
    p = json.find(':', p);
    if (p == std::string::npos) return "";
    p = json.find('"', p + 1);
    if (p == std::string::npos) return "";
    size_t q = json.find('"', p + 1);
    if (q == std::string::npos || q <= p + 1) return "";
    return json.substr(p + 1, q - p - 1);
}

std::string KeyAuthService::ParseFirstStringField(const std::string& json, const char* const* keys, int keyCount)
{
    for (int i = 0; i < keyCount; ++i)
    {
        const std::string v = ParseStringField(json, keys[i]);
        if (!v.empty())
            return v;
    }
    return "";
}

std::string KeyAuthService::ParseErrorText(const std::string& json)
{
    static const char* const kErrKeys[] = { "message", "info", "error", "msg", "response", "reason" };
    const int n = static_cast<int>(sizeof(kErrKeys) / sizeof(kErrKeys[0]));
    for (int i = 0; i < n; ++i)
    {
        const std::string v = ParseStringField(json, kErrKeys[i]);
        if (!v.empty())
        {
            if (kAuthDebugLog)
                AuthDbgLog("ParseErrorText: hit field=\"%s\" textLen=%zu", kErrKeys[i], v.size());
            return v;
        }
    }
    if (kAuthDebugLog)
        AuthDbgLog("ParseErrorText: no hit (checked message/info/error/msg/response/reason)");
    return "";
}

std::string KeyAuthService::ParseUserNameFromResponse(const std::string& json)
{
    static const char* const kUserKeys[] = { "username", "user", "user_name", "name" };
    const int n = static_cast<int>(sizeof(kUserKeys) / sizeof(kUserKeys[0]));
    for (int i = 0; i < n; ++i)
    {
        const std::string v = ParseStringField(json, kUserKeys[i]);
        if (!v.empty())
        {
            if (kAuthDebugLog)
                AuthDbgLog("ParseUserNameFromResponse: hit field=\"%s\"", kUserKeys[i]);
            return v;
        }
    }
    if (kAuthDebugLog)
        AuthDbgLog("ParseUserNameFromResponse: no hit");
    return "";
}

std::string KeyAuthService::ParseTokenFromResponse(const std::string& json)
{
    // KeyAuth 返回字段名可能随接口变化：这里做最小兼容扩展。
    // 重要：在调用方会过滤掉与本地 init sessionid 相同的值，避免误用 init session。
    static const char* const kTokenKeys[] = {
        "token",
        "session",
        "sessionid",
        "sessionId",
        "session_token",
        "sessiontoken",
        "auth",
        "auth_token"
    };
    const int n = static_cast<int>(sizeof(kTokenKeys) / sizeof(kTokenKeys[0]));
    for (int i = 0; i < n; ++i)
    {
        const std::string v = ParseStringField(json, kTokenKeys[i]);
        if (!v.empty())
        {
            if (kAuthDebugLog)
                AuthDbgLog("ParseTokenFromResponse: hit field=\"%s\" masked=%s", kTokenKeys[i],
                            MaskTokenForLog(v).c_str());
            return v;
        }
    }
    if (kAuthDebugLog)
        AuthDbgLog("ParseTokenFromResponse: no hit");
    return "";
}

bool KeyAuthService::IsExpiredError(const std::string& rawMsg)
{
    if (rawMsg.empty())
    {
        if (kAuthDebugLog)
            AuthDbgLog("IsExpiredError: raw empty -> false");
        return false;
    }
    std::string s = rawMsg;
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const bool hitExp = s.find("expired") != std::string::npos;
    const bool hitSub = s.find("subscription") != std::string::npos;
    const bool ok = hitExp || hitSub;
    if (kAuthDebugLog)
        AuthDbgLog("IsExpiredError: result=%d (token expired=%d subscription=%d)", ok ? 1 : 0, hitExp ? 1 : 0,
                   hitSub ? 1 : 0);
    return ok;
}

long long KeyAuthService::ParseInt64Field(const std::string& json, const char* field)
{
    const std::string key = std::string("\"") + field + "\"";
    size_t p = json.find(key);
    if (p == std::string::npos) return 0;
    p = json.find(':', p);
    if (p == std::string::npos) return 0;
    while (p + 1 < json.size() && (json[p + 1] == ' ' || json[p + 1] == '"' || json[p + 1] == '\t')) ++p;
    size_t b = p + 1;
    size_t e = b;
    while (e < json.size() && (json[e] == '-' || (json[e] >= '0' && json[e] <= '9'))) ++e;
    if (e <= b) return 0;
    return _strtoi64(json.substr(b, e - b).c_str(), nullptr, 10);
}

long long KeyAuthService::ParseExpiryUnix(const std::string& json)
{
    const ExpiryParseDetail d = ParseExpiryUnixDetail(json);
    if (kAuthDebugLog)
    {
        if (d.unix > 0)
            AuthDbgLog(
                "ParseExpiryUnix: field=\"%s\" raw=\"%s\" unix=%lld millisScaled=%s slice=%s",
                d.field ? d.field : "(none)", d.rawDigits.c_str(), static_cast<long long>(d.unix),
                d.scaledFromMillis ? "yes(/1000)" : "no",
                d.fromSubscriptionsSlice ? "subscriptions" : "global");
        else
            AuthDbgLog("ParseExpiryUnix: no numeric expiry field matched");
    }
    return d.unix;
}

bool KeyAuthService::IsUserPayloadComplete(const AuthUserInfo& user)
{
    // 会话有效判定至少要能确认“身份”（username）。
    // expiry 解析失败时不应直接判会话异常，避免登录/续费后陷入死循环。
    const bool ok = !user.username.empty();
    if (kAuthDebugLog)
        AuthDbgLog("IsUserPayloadComplete: %s — %s", ok ? "true" : "false",
                   ok ? "username non-empty" : "username empty (expiry not required for session valid flag)");
    return ok;
}

std::string KeyAuthService::BuildExpiryText(long long unixTs)
{
    if (unixTs <= 0) return "未知";
    std::time_t tt = static_cast<std::time_t>(unixTs);
    std::tm tmv{};
    localtime_s(&tmv, &tt);
    std::ostringstream oss;
    oss << std::put_time(&tmv, "%Y-%m-%d %H:%M:%S");
    return oss.str();
}

void KeyAuthService::PersistRememberedUsername(const std::string& username) const
{
    if (username.empty())
        return;
    std::ofstream f(kSessionFile, std::ios::out | std::ios::trunc);
    if (!f.is_open())
        return;
    f << "username=" << username << "\n";
}

void KeyAuthService::LoadRememberedUsernameFromDisk()
{
    m_rememberedUsername.clear();
    std::ifstream f(kSessionFile);
    if (!f.is_open())
        return;
    std::string line;
    while (std::getline(f, line))
    {
        const size_t p = line.find('=');
        if (p == std::string::npos)
            continue;
        const std::string k = line.substr(0, p);
        const std::string v = line.substr(p + 1);
        if (k == "username" && !v.empty())
            m_rememberedUsername = v;
    }
}

void KeyAuthService::ClearLocalSession()
{
    DeleteFileA(kSessionFile);
}

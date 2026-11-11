#include "StdInc.h"
#include "NetworkHooks.h"
#include "Logger.h"
#include "SteamConfig.h"
#include <cctype>
#include <cstring>
#include <ws2tcpip.h>

#pragma comment(lib, "Ws2_32.lib")

typedef int (WSAAPI* connectFn)(SOCKET, const sockaddr*, int);
typedef int (WSAAPI* WSAConnectFn)(SOCKET, const sockaddr*, int, LPWSABUF, LPWSABUF, LPQOS, LPQOS);
typedef int (WSAAPI* getaddrinfoFn)(PCSTR, PCSTR, const ADDRINFOA*, PADDRINFOA*);
typedef INT (WSAAPI* GetAddrInfoWFn)(PCWSTR, PCWSTR, const ADDRINFOW*, PADDRINFOW*);
typedef int (WSAAPI* sendtoFn)(SOCKET, const char*, int, int, const sockaddr*, int);
typedef int (WSAAPI* WSASendToFn)(SOCKET, LPWSABUF, DWORD, LPDWORD, DWORD, const sockaddr*, int, LPWSAOVERLAPPED, LPWSAOVERLAPPED_COMPLETION_ROUTINE);

static connectFn g_OriginalConnect = nullptr;
static WSAConnectFn g_OriginalWSAConnect = nullptr;
static getaddrinfoFn g_OriginalGetAddrInfo = nullptr;
static GetAddrInfoWFn g_OriginalGetAddrInfoW = nullptr;
static sendtoFn g_OriginalSendTo = nullptr;
static WSASendToFn g_OriginalWSASendTo = nullptr;

static std::mutex g_AllowedPublicMutex;
static std::set<uint32_t> g_AllowedPublicIPv4;

// Public hostnames are mapped to synthetic addresses in 127.77.0.0/16.
// When the game connects to one of those addresses, the destination is
// transparently rewritten to the configured local emulator backend.
static std::mutex g_RedirectMutex;
static std::unordered_map<uint32_t, std::string> g_RedirectedHosts;
static std::set<std::string> g_LoggedRedirectHosts;
static uint32_t g_BackendIPv4 = 0;

static bool IsAllowedIPv4(uint32_t ip)
{
    const uint8_t a = static_cast<uint8_t>((ip >> 24) & 0xFF);
    const uint8_t b = static_cast<uint8_t>((ip >> 16) & 0xFF);
    const uint8_t d = static_cast<uint8_t>(ip & 0xFF);

    if (a == 10) return true;
    if (a == 127) return true;
    if (a == 172 && b >= 16 && b <= 31) return true;
    if (a == 192 && b == 168) return true;
    if (a == 169 && b == 254) return true;
    if (a == 255 && b == 255 && ((ip >> 8) & 0xFF) == 255 && d == 255) return true;

    if (a == 25) return true;
    if (a == 26) return true;
    if (a == 100 && b >= 64 && b <= 127) return true;

    {
        std::lock_guard<std::mutex> lock(g_AllowedPublicMutex);
        if (g_AllowedPublicIPv4.find(ip) != g_AllowedPublicIPv4.end())
            return true;
    }

    return false;
}

static bool IsAllowedIPv6(const IN6_ADDR& ip)
{
    const auto* bytes = reinterpret_cast<const uint8_t*>(&ip);

    bool loopback = true;
    for (int i = 0; i < 15; ++i)
        loopback = loopback && bytes[i] == 0;

    if (loopback && bytes[15] == 1)
        return true;

    if ((bytes[0] & 0xFE) == 0xFC)
        return true;

    if (bytes[0] == 0xFE && (bytes[1] & 0xC0) == 0x80)
        return true;

    return false;
}

static bool TryParseIPv4HostOrder(const std::string& text, uint32_t& out)
{
    IN_ADDR addr{};
    if (inet_pton(AF_INET, text.c_str(), &addr) != 1)
        return false;

    out = ntohl(addr.S_un.S_addr);
    return true;
}

static bool EndsWith(const std::string& text, const char* suffix)
{
    if (!suffix)
        return false;

    const size_t suffixLength = strlen(suffix);
    return text.size() >= suffixLength &&
        text.compare(text.size() - suffixLength, suffixLength, suffix) == 0;
}

static std::string Lower(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return text;
}

static bool IsAllowedHostName(const std::string& host)
{
    if (host.empty() || host == "null")
        return true;

    const std::string lower = Lower(host);

    if (lower == "localhost")
        return true;

    if (lower.find('.') == std::string::npos && lower.find(':') == std::string::npos)
        return true;

    if (EndsWith(lower, ".local") || EndsWith(lower, ".lan"))
        return true;

    uint32_t numeric = 0;
    if (TryParseIPv4HostOrder(lower, numeric))
        return IsAllowedIPv4(numeric);

    const SteamConfig::Config& config = SteamConfig::Get();
    if (!config.CustomServerHost.empty() && lower == Lower(config.CustomServerHost))
        return true;

    if (!config.BackendHost.empty() && lower == Lower(config.BackendHost))
        return true;

    return false;
}

static bool ShouldRedirectPublicHost(const std::string& host)
{
    const SteamConfig::Config& config = SteamConfig::Get();
    return config.RedirectPublicHosts &&
        !host.empty() &&
        host != "null" &&
        !IsAllowedHostName(host);
}

static uint32_t SyntheticAddressForHost(const std::string& host)
{
    // FNV-1a gives a stable per-host address without a hard-coded host list.
    uint32_t hash = 2166136261u;
    for (unsigned char c : Lower(host))
    {
        hash ^= c;
        hash *= 16777619u;
    }

    uint8_t third = static_cast<uint8_t>((hash >> 8) & 0xFF);
    uint8_t fourth = static_cast<uint8_t>(hash & 0xFF);
    if (third == 0 || third == 255) third = 1;
    if (fourth == 0 || fourth == 255) fourth = 1;

    return (127u << 24) | (77u << 16) | (static_cast<uint32_t>(third) << 8) | fourth;
}

static std::string IPv4ToString(uint32_t hostOrder)
{
    IN_ADDR addr{};
    addr.S_un.S_addr = htonl(hostOrder);
    char buffer[INET_ADDRSTRLEN]{};
    if (!inet_ntop(AF_INET, &addr, buffer, sizeof(buffer)))
        return "0.0.0.0";
    return buffer;
}

static void RememberRedirect(const std::string& host, uint32_t synthetic)
{
    std::lock_guard<std::mutex> lock(g_RedirectMutex);
    g_RedirectedHosts[synthetic] = host;

    if (g_LoggedRedirectHosts.insert(Lower(host)).second)
    {
        Logger::Info(
            "Backend redirect: host=" + host +
            " synthetic=" + IPv4ToString(synthetic) +
            " backend=" + SteamConfig::Get().BackendHost + ":" +
            std::to_string(SteamConfig::Get().BackendPort));
    }
}

static bool TryGetRedirectHost(uint32_t synthetic, std::string& host)
{
    std::lock_guard<std::mutex> lock(g_RedirectMutex);
    auto it = g_RedirectedHosts.find(synthetic);
    if (it == g_RedirectedHosts.end())
        return false;
    host = it->second;
    return true;
}

static bool ResolveBackendIPv4()
{
    const std::string& host = SteamConfig::Get().BackendHost;
    if (TryParseIPv4HostOrder(host, g_BackendIPv4))
        return true;

    ADDRINFOA hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    PADDRINFOA result = nullptr;

    const int rc = ::getaddrinfo(host.c_str(), nullptr, &hints, &result);
    if (rc != 0 || !result)
        return false;

    auto* addr = reinterpret_cast<sockaddr_in*>(result->ai_addr);
    g_BackendIPv4 = ntohl(addr->sin_addr.S_un.S_addr);
    ::freeaddrinfo(result);
    return true;
}

static void RememberAllowedIPv4Results(PADDRINFOA result)
{
    std::lock_guard<std::mutex> lock(g_AllowedPublicMutex);
    for (ADDRINFOA* item = result; item; item = item->ai_next)
    {
        if (item->ai_family != AF_INET || !item->ai_addr)
            continue;

        auto* address = reinterpret_cast<sockaddr_in*>(item->ai_addr);
        g_AllowedPublicIPv4.insert(ntohl(address->sin_addr.S_un.S_addr));
    }
}

static void RememberAllowedIPv4ResultsW(PADDRINFOW result)
{
    std::lock_guard<std::mutex> lock(g_AllowedPublicMutex);
    for (ADDRINFOW* item = result; item; item = item->ai_next)
    {
        if (item->ai_family != AF_INET || !item->ai_addr)
            continue;

        auto* address = reinterpret_cast<sockaddr_in*>(item->ai_addr);
        g_AllowedPublicIPv4.insert(ntohl(address->sin_addr.S_un.S_addr));
    }
}

static bool IsConfiguredCustomServerIPv4(uint32_t ip)
{
    const SteamConfig::Config& config = SteamConfig::Get();

    if (config.CustomServerHost.empty())
        return false;

    uint32_t configured = 0;
    return TryParseIPv4HostOrder(config.CustomServerHost, configured) && configured == ip;
}

static bool IsAllowedEndpoint(const sockaddr* addr)
{
    if (!addr)
        return true;

    if (addr->sa_family == AF_INET)
    {
        const auto* ipv4 = reinterpret_cast<const sockaddr_in*>(addr);
        const uint32_t hostOrder = ntohl(ipv4->sin_addr.S_un.S_addr);
        return IsAllowedIPv4(hostOrder) || IsConfiguredCustomServerIPv4(hostOrder);
    }

    if (addr->sa_family == AF_INET6)
    {
        const auto* ipv6 = reinterpret_cast<const sockaddr_in6*>(addr);
        return IsAllowedIPv6(ipv6->sin6_addr);
    }

    return false;
}

static std::string WideToUtf8(const wchar_t* text)
{
    if (!text)
        return "null";

    int needed = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 0)
        return "wide-conversion-failed";

    std::string result;
    result.resize(needed);
    WideCharToMultiByte(CP_UTF8, 0, text, -1, &result[0], needed, nullptr, nullptr);
    if (!result.empty() && result.back() == '\0')
        result.pop_back();
    return result;
}

static bool ExtractIpAndPort(const sockaddr* addr, std::string& outIp, uint16_t& outPort)
{
    outIp.clear();
    outPort = 0;

    if (!addr)
        return false;

    char ip[INET6_ADDRSTRLEN]{};

    if (addr->sa_family == AF_INET)
    {
        const auto* ipv4 = reinterpret_cast<const sockaddr_in*>(addr);
        if (!inet_ntop(AF_INET, const_cast<IN_ADDR*>(&ipv4->sin_addr), ip, sizeof(ip)))
            return false;

        outIp = ip;
        outPort = ntohs(ipv4->sin_port);
        return true;
    }

    if (addr->sa_family == AF_INET6)
    {
        const auto* ipv6 = reinterpret_cast<const sockaddr_in6*>(addr);
        if (!inet_ntop(AF_INET6, const_cast<IN6_ADDR*>(&ipv6->sin6_addr), ip, sizeof(ip)))
            return false;

        outIp = ip;
        outPort = ntohs(ipv6->sin6_port);
        return true;
    }

    return false;
}

static std::string FormatEndpoint(const std::string& ip, uint16_t port)
{
    if (ip.empty())
        return "unknown";
    return ip + ":" + std::to_string(port);
}

static void LogNetworkDecision(const char* action, bool allowed, const std::string& endpoint)
{
    if (allowed)
        return;

    Logger::Info(std::string("BLOCK public ") + action + " -> " + endpoint);
}

static bool RewriteRedirectedIPv4(const sockaddr* source, sockaddr_in& rewritten, std::string& originalHost)
{
    if (!source || source->sa_family != AF_INET || g_BackendIPv4 == 0)
        return false;

    const auto* ipv4 = reinterpret_cast<const sockaddr_in*>(source);
    const uint32_t address = ntohl(ipv4->sin_addr.S_un.S_addr);
    if (!TryGetRedirectHost(address, originalHost))
        return false;

    rewritten = *ipv4;
    rewritten.sin_addr.S_un.S_addr = htonl(g_BackendIPv4);

    const uint16_t originalPort = ntohs(rewritten.sin_port);
    if ((originalPort == 80 || originalPort == 443) && SteamConfig::Get().BackendPort > 0)
        rewritten.sin_port = htons(static_cast<u_short>(SteamConfig::Get().BackendPort));

    return true;
}

static int WSAAPI HookedConnect(SOCKET s, const sockaddr* name, int namelen)
{
    sockaddr_in rewritten{};
    std::string originalHost;
    if (RewriteRedirectedIPv4(name, rewritten, originalHost))
    {
        Logger::Info(
            "Backend connect redirect: host=" + originalHost +
            " -> " + IPv4ToString(g_BackendIPv4) + ":" +
            std::to_string(ntohs(rewritten.sin_port)));
        return g_OriginalConnect ? g_OriginalConnect(s, reinterpret_cast<const sockaddr*>(&rewritten), sizeof(rewritten)) : SOCKET_ERROR;
    }

    std::string ip;
    uint16_t port = 0;
    const bool allowed = IsAllowedEndpoint(name);
    LogNetworkDecision("connect", allowed, ExtractIpAndPort(name, ip, port) ? FormatEndpoint(ip, port) : "unknown endpoint");

    if (!allowed)
    {
        WSASetLastError(WSAECONNREFUSED);
        return SOCKET_ERROR;
    }

    return g_OriginalConnect ? g_OriginalConnect(s, name, namelen) : SOCKET_ERROR;
}

static int WSAAPI HookedWSAConnect(
    SOCKET s,
    const sockaddr* name,
    int namelen,
    LPWSABUF lpCallerData,
    LPWSABUF lpCalleeData,
    LPQOS lpSQOS,
    LPQOS lpGQOS)
{
    sockaddr_in rewritten{};
    std::string originalHost;
    if (RewriteRedirectedIPv4(name, rewritten, originalHost))
    {
        Logger::Info(
            "Backend WSAConnect redirect: host=" + originalHost +
            " -> " + IPv4ToString(g_BackendIPv4) + ":" +
            std::to_string(ntohs(rewritten.sin_port)));
        return g_OriginalWSAConnect
            ? g_OriginalWSAConnect(s, reinterpret_cast<const sockaddr*>(&rewritten), sizeof(rewritten), lpCallerData, lpCalleeData, lpSQOS, lpGQOS)
            : SOCKET_ERROR;
    }

    std::string ip;
    uint16_t port = 0;
    const bool allowed = IsAllowedEndpoint(name);
    LogNetworkDecision("WSAConnect", allowed, ExtractIpAndPort(name, ip, port) ? FormatEndpoint(ip, port) : "unknown endpoint");

    if (!allowed)
    {
        WSASetLastError(WSAECONNREFUSED);
        return SOCKET_ERROR;
    }

    return g_OriginalWSAConnect
        ? g_OriginalWSAConnect(s, name, namelen, lpCallerData, lpCalleeData, lpSQOS, lpGQOS)
        : SOCKET_ERROR;
}

static int WSAAPI HookedSendTo(SOCKET s, const char* buf, int len, int flags, const sockaddr* to, int tolen)
{
    sockaddr_in rewritten{};
    std::string originalHost;
    if (RewriteRedirectedIPv4(to, rewritten, originalHost))
    {
        // UDP services retain their original port; only the address is redirected.
        rewritten.sin_port = reinterpret_cast<const sockaddr_in*>(to)->sin_port;
        return g_OriginalSendTo
            ? g_OriginalSendTo(s, buf, len, flags, reinterpret_cast<const sockaddr*>(&rewritten), sizeof(rewritten))
            : SOCKET_ERROR;
    }

    std::string ip;
    uint16_t port = 0;
    const bool allowed = IsAllowedEndpoint(to);
    LogNetworkDecision("sendto", allowed, ExtractIpAndPort(to, ip, port) ? FormatEndpoint(ip, port) : "connected-or-unknown endpoint");

    if (!allowed)
    {
        WSASetLastError(WSAEACCES);
        return SOCKET_ERROR;
    }

    return g_OriginalSendTo ? g_OriginalSendTo(s, buf, len, flags, to, tolen) : SOCKET_ERROR;
}

static int WSAAPI HookedWSASendTo(
    SOCKET s,
    LPWSABUF lpBuffers,
    DWORD dwBufferCount,
    LPDWORD lpNumberOfBytesSent,
    DWORD dwFlags,
    const sockaddr* lpTo,
    int iToLen,
    LPWSAOVERLAPPED lpOverlapped,
    LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine)
{
    sockaddr_in rewritten{};
    std::string originalHost;
    if (RewriteRedirectedIPv4(lpTo, rewritten, originalHost))
    {
        rewritten.sin_port = reinterpret_cast<const sockaddr_in*>(lpTo)->sin_port;
        return g_OriginalWSASendTo
            ? g_OriginalWSASendTo(s, lpBuffers, dwBufferCount, lpNumberOfBytesSent, dwFlags,
                reinterpret_cast<const sockaddr*>(&rewritten), sizeof(rewritten), lpOverlapped, lpCompletionRoutine)
            : SOCKET_ERROR;
    }

    std::string ip;
    uint16_t port = 0;
    const bool allowed = IsAllowedEndpoint(lpTo);
    LogNetworkDecision("WSASendTo", allowed, ExtractIpAndPort(lpTo, ip, port) ? FormatEndpoint(ip, port) : "connected-or-unknown endpoint");

    if (!allowed)
    {
        WSASetLastError(WSAEACCES);
        return SOCKET_ERROR;
    }

    return g_OriginalWSASendTo
        ? g_OriginalWSASendTo(s, lpBuffers, dwBufferCount, lpNumberOfBytesSent, dwFlags, lpTo, iToLen, lpOverlapped, lpCompletionRoutine)
        : SOCKET_ERROR;
}

static int WSAAPI HookedGetAddrInfo(PCSTR pNodeName, PCSTR pServiceName, const ADDRINFOA* pHints, PADDRINFOA* ppResult)
{
    const std::string host = pNodeName ? pNodeName : "null";
    const std::string service = pServiceName ? pServiceName : "null";

    if (ShouldRedirectPublicHost(host))
    {
        const uint32_t synthetic = SyntheticAddressForHost(host);
        RememberRedirect(host, synthetic);
        const std::string target = IPv4ToString(synthetic);

        ADDRINFOA adjustedHints{};
        const ADDRINFOA* hints = pHints;
        if (pHints && pHints->ai_family == AF_INET6)
        {
            adjustedHints = *pHints;
            adjustedHints.ai_family = AF_INET;
            hints = &adjustedHints;
        }

        return g_OriginalGetAddrInfo
            ? g_OriginalGetAddrInfo(target.c_str(), pServiceName, hints, ppResult)
            : EAI_FAIL;
    }

    const bool allowed = IsAllowedHostName(host);
    if (!allowed)
        Logger::Info("BLOCK public getaddrinfo -> host=" + host + " service=" + service);

    if (!allowed)
    {
        WSASetLastError(WSAHOST_NOT_FOUND);
        return WSAHOST_NOT_FOUND;
    }

    const int result = g_OriginalGetAddrInfo
        ? g_OriginalGetAddrInfo(pNodeName, pServiceName, pHints, ppResult)
        : EAI_FAIL;

    if (result == 0 && ppResult && *ppResult)
        RememberAllowedIPv4Results(*ppResult);

    return result;
}

static INT WSAAPI HookedGetAddrInfoW(PCWSTR pNodeName, PCWSTR pServiceName, const ADDRINFOW* pHints, PADDRINFOW* ppResult)
{
    const std::string host = WideToUtf8(pNodeName);
    const std::string service = WideToUtf8(pServiceName);

    if (ShouldRedirectPublicHost(host))
    {
        const uint32_t synthetic = SyntheticAddressForHost(host);
        RememberRedirect(host, synthetic);
        const std::string target = IPv4ToString(synthetic);
        std::wstring wideTarget(target.begin(), target.end());

        ADDRINFOW adjustedHints{};
        const ADDRINFOW* hints = pHints;
        if (pHints && pHints->ai_family == AF_INET6)
        {
            adjustedHints = *pHints;
            adjustedHints.ai_family = AF_INET;
            hints = &adjustedHints;
        }

        return g_OriginalGetAddrInfoW
            ? g_OriginalGetAddrInfoW(wideTarget.c_str(), pServiceName, hints, ppResult)
            : EAI_FAIL;
    }

    const bool allowed = IsAllowedHostName(host);
    if (!allowed)
        Logger::Info("BLOCK public GetAddrInfoW -> host=" + host + " service=" + service);

    if (!allowed)
    {
        WSASetLastError(WSAHOST_NOT_FOUND);
        return WSAHOST_NOT_FOUND;
    }

    const INT result = g_OriginalGetAddrInfoW
        ? g_OriginalGetAddrInfoW(pNodeName, pServiceName, pHints, ppResult)
        : EAI_FAIL;

    if (result == 0 && ppResult && *ppResult)
        RememberAllowedIPv4ResultsW(*ppResult);

    return result;
}

static bool HookExport(HMODULE module, const char* exportName, void* detour, void** original)
{
    void* target = reinterpret_cast<void*>(GetProcAddress(module, exportName));
    if (!target)
    {
        Logger::Error(std::string("Network hook missing export: ") + exportName);
        return false;
    }

    const MH_STATUS createStatus = MH_CreateHook(target, detour, original);
    if (createStatus != MH_OK && createStatus != MH_ERROR_ALREADY_CREATED)
    {
        Logger::Error(std::string("Network hook create failed: ") + exportName);
        return false;
    }

    const MH_STATUS enableStatus = MH_EnableHook(target);
    if (enableStatus != MH_OK && enableStatus != MH_ERROR_ENABLED)
    {
        Logger::Error(std::string("Network hook enable failed: ") + exportName);
        return false;
    }

    Logger::Info(std::string("Network hook enabled: ") + exportName);
    return true;
}

namespace NetworkHooks
{
    bool Init()
    {
        HMODULE ws2 = GetModuleHandleA("Ws2_32.dll");
        if (!ws2)
            ws2 = LoadLibraryA("Ws2_32.dll");

        if (!ws2)
        {
            Logger::Error("Failed to load Ws2_32.dll");
            return false;
        }

        if (SteamConfig::Get().RedirectPublicHosts)
        {
            if (!ResolveBackendIPv4())
            {
                Logger::Error("Failed to resolve configured backend_host");
                return false;
            }

            Logger::Info(
                "Initializing backend redirect: public hostnames -> " +
                SteamConfig::Get().BackendHost + ":" +
                std::to_string(SteamConfig::Get().BackendPort));
        }
        else
        {
            Logger::Info("Initializing network hooks: public internet blocked, LAN/VPN allowed");
        }

        bool ok = true;
        ok &= HookExport(ws2, "connect", reinterpret_cast<void*>(&HookedConnect), reinterpret_cast<void**>(&g_OriginalConnect));
        ok &= HookExport(ws2, "WSAConnect", reinterpret_cast<void*>(&HookedWSAConnect), reinterpret_cast<void**>(&g_OriginalWSAConnect));
        ok &= HookExport(ws2, "getaddrinfo", reinterpret_cast<void*>(&HookedGetAddrInfo), reinterpret_cast<void**>(&g_OriginalGetAddrInfo));
        ok &= HookExport(ws2, "GetAddrInfoW", reinterpret_cast<void*>(&HookedGetAddrInfoW), reinterpret_cast<void**>(&g_OriginalGetAddrInfoW));
        ok &= HookExport(ws2, "sendto", reinterpret_cast<void*>(&HookedSendTo), reinterpret_cast<void**>(&g_OriginalSendTo));
        ok &= HookExport(ws2, "WSASendTo", reinterpret_cast<void*>(&HookedWSASendTo), reinterpret_cast<void**>(&g_OriginalWSASendTo));

        if (!ok)
        {
            Logger::Error("Network hooks initialized with failures");
            return false;
        }

        Logger::Info(
            SteamConfig::Get().RedirectPublicHosts
                ? "Network hooks initialized: public game traffic redirected to local emulator; LAN/VPN allowed"
                : "Network hooks initialized: public internet blocked; loopback, LAN, and VPN ranges allowed");

        return true;
    }

    void Shutdown()
    {
        std::lock_guard<std::mutex> lock(g_RedirectMutex);
        g_RedirectedHosts.clear();
        g_LoggedRedirectHosts.clear();
        Logger::Info("Network hooks shutdown");
    }
}

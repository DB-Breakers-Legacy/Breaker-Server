#include "StdInc.h"
#include "HTTP/HttpTransport.h"
#include "SteamConfig.h"
#include "Logger.h"
#include <WinHttp.h>
#include <cctype>

#pragma comment(lib, "Winhttp.lib")

namespace
{
    struct ParsedUrl
    {
        std::wstring Scheme;
        std::wstring Host;
        INTERNET_PORT Port = 0;
        std::wstring Path;
        bool Secure = false;
    };

    std::string Lower(std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return value;
    }

    bool EndsWith(const std::string& value, const char* suffix)
    {
        if (!suffix)
            return false;
        const size_t length = std::strlen(suffix);
        return value.size() >= length && value.compare(value.size() - length, length, suffix) == 0;
    }

    std::wstring Utf8ToWide(const std::string& value)
    {
        if (value.empty())
            return {};

        const int required = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0);
        if (required <= 0)
            return {};

        std::wstring result(static_cast<size_t>(required), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), result.data(), required);
        return result;
    }

    std::string WideToUtf8(const std::wstring& value)
    {
        if (value.empty())
            return {};

        const int required = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
        if (required <= 0)
            return {};

        std::string result(static_cast<size_t>(required), '\0');
        WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), result.data(), required, nullptr, nullptr);
        return result;
    }

    bool ParseUrl(const std::string& value, ParsedUrl& out)
    {
        const std::wstring wide = Utf8ToWide(value);
        if (wide.empty())
            return false;

        URL_COMPONENTS components{};
        components.dwStructSize = sizeof(components);
        components.dwSchemeLength = static_cast<DWORD>(-1);
        components.dwHostNameLength = static_cast<DWORD>(-1);
        components.dwUrlPathLength = static_cast<DWORD>(-1);
        components.dwExtraInfoLength = static_cast<DWORD>(-1);

        if (!WinHttpCrackUrl(wide.c_str(), static_cast<DWORD>(wide.size()), 0, &components))
            return false;

        out.Scheme.assign(components.lpszScheme, components.dwSchemeLength);
        out.Host.assign(components.lpszHostName, components.dwHostNameLength);
        out.Port = components.nPort;
        out.Path.assign(components.lpszUrlPath, components.dwUrlPathLength);
        if (components.lpszExtraInfo && components.dwExtraInfoLength > 0)
            out.Path.append(components.lpszExtraInfo, components.dwExtraInfoLength);

        if (out.Path.empty())
            out.Path = L"/";

        out.Secure = components.nScheme == INTERNET_SCHEME_HTTPS;
        return !out.Host.empty();
    }

    bool IsPrivateIPv4(const std::string& host)
    {
        IN_ADDR addr{};
        if (inet_pton(AF_INET, host.c_str(), &addr) != 1)
            return false;

        const uint32_t ip = ntohl(addr.S_un.S_addr);
        const uint8_t a = static_cast<uint8_t>((ip >> 24) & 0xFF);
        const uint8_t b = static_cast<uint8_t>((ip >> 16) & 0xFF);

        if (a == 10 || a == 127 || a == 25 || a == 26)
            return true;
        if (a == 172 && b >= 16 && b <= 31)
            return true;
        if (a == 192 && b == 168)
            return true;
        if (a == 169 && b == 254)
            return true;
        if (a == 100 && b >= 64 && b <= 127)
            return true;
        return false;
    }

    bool IsLocalHost(const std::string& host)
    {
        if (host.empty())
            return true;

        const std::string lower = Lower(host);
        const auto& config = SteamConfig::Get();

        if (lower == "localhost")
            return true;
        if (!config.BackendHost.empty() && lower == Lower(config.BackendHost))
            return true;
        if (!config.CustomServerHost.empty() && lower == Lower(config.CustomServerHost))
            return true;
        if (lower.find('.') == std::string::npos && lower.find(':') == std::string::npos)
            return true;
        if (EndsWith(lower, ".local") || EndsWith(lower, ".lan"))
            return true;
        return IsPrivateIPv4(lower);
    }

    std::string UrlEncode(const std::string& value)
    {
        static const char* hex = "0123456789ABCDEF";
        std::string out;
        out.reserve(value.size() * 3);

        for (unsigned char c : value)
        {
            const bool safe = (c >= 'a' && c <= 'z') ||
                (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') ||
                c == '-' || c == '_' || c == '.' || c == '~';

            if (safe)
            {
                out.push_back(static_cast<char>(c));
            }
            else
            {
                out.push_back('%');
                out.push_back(hex[(c >> 4) & 0x0F]);
                out.push_back(hex[c & 0x0F]);
            }
        }
        return out;
    }

    std::string FormEncode(const std::vector<std::pair<std::string, std::string>>& parameters)
    {
        std::string body;
        for (size_t i = 0; i < parameters.size(); ++i)
        {
            if (i != 0)
                body.push_back('&');
            body += UrlEncode(parameters[i].first);
            body.push_back('=');
            body += UrlEncode(parameters[i].second);
        }
        return body;
    }

    std::wstring MethodName(int method)
    {
        switch (method)
        {
        case 1: return L"GET";
        case 2: return L"HEAD";
        case 3: return L"POST";
        case 4: return L"PUT";
        case 5: return L"DELETE";
        case 6: return L"OPTIONS";
        case 7: return L"PATCH";
        default: return {};
        }
    }

    void ParseResponseHeaders(HINTERNET requestHandle, HttpResponse& response)
    {
        DWORD bytes = 0;
        WinHttpQueryHeaders(
            requestHandle,
            WINHTTP_QUERY_RAW_HEADERS_CRLF,
            WINHTTP_HEADER_NAME_BY_INDEX,
            nullptr,
            &bytes,
            WINHTTP_NO_HEADER_INDEX);

        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || bytes < sizeof(wchar_t))
            return;

        std::vector<wchar_t> buffer(bytes / sizeof(wchar_t));
        if (!WinHttpQueryHeaders(
                requestHandle,
                WINHTTP_QUERY_RAW_HEADERS_CRLF,
                WINHTTP_HEADER_NAME_BY_INDEX,
                buffer.data(),
                &bytes,
                WINHTTP_NO_HEADER_INDEX))
        {
            return;
        }

        const std::string raw = WideToUtf8(std::wstring(buffer.data()));
        std::istringstream stream(raw);
        std::string line;
        bool first = true;
        while (std::getline(stream, line))
        {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();

            if (first)
            {
                first = false;
                continue;
            }

            const size_t colon = line.find(':');
            if (colon == std::string::npos)
                continue;

            std::string name = Lower(line.substr(0, colon));
            std::string value = line.substr(colon + 1);
            while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
                value.erase(value.begin());
            response.Headers[name] = value;
        }
    }

    bool ReadResponseBody(HINTERNET requestHandle, std::vector<unsigned char>& body)
    {
        body.clear();

        for (;;)
        {
            DWORD available = 0;
            if (!WinHttpQueryDataAvailable(requestHandle, &available))
                return false;

            if (available == 0)
                return true;

            const size_t oldSize = body.size();
            body.resize(oldSize + available);

            DWORD read = 0;
            if (!WinHttpReadData(requestHandle, body.data() + oldSize, available, &read))
                return false;

            body.resize(oldSize + read);
            if (read == 0)
                return true;
        }
    }

    std::wstring BuildPath(const ParsedUrl& parsed, const HttpRequest& request, std::vector<unsigned char>& body, std::wstring& contentType)
    {
        std::string path = WideToUtf8(parsed.Path);

        if (request.Method == 1 && !request.Parameters.empty())
        {
            path += path.find('?') == std::string::npos ? "?" : "&";
            path += FormEncode(request.Parameters);
        }

        if (!request.RawPostBody.empty())
        {
            body = request.RawPostBody;
            contentType = Utf8ToWide(request.RawPostContentType);
        }
        else if (request.Method == 3 && !request.Parameters.empty())
        {
            const std::string encoded = FormEncode(request.Parameters);
            body.assign(encoded.begin(), encoded.end());
            contentType = L"application/x-www-form-urlencoded";
        }

        return Utf8ToWide(path);
    }
}

namespace HttpTransport
{
    HttpResponse Execute(const HttpRequest& request)
    {
        HttpResponse response;
        ParsedUrl parsed;
        if (!ParseUrl(request.Url, parsed))
        {
            Logger::Error("SteamHTTP invalid URL: " + request.Url);
            return response;
        }

        const std::string originalHost = WideToUtf8(parsed.Host);
        std::wstring targetHost = parsed.Host;
        INTERNET_PORT targetPort = parsed.Port;
        bool secure = parsed.Secure;

        const auto& config = SteamConfig::Get();
        const bool publicHost = !IsLocalHost(originalHost);
        const bool redirect = publicHost && config.RedirectPublicHosts;

        if (redirect)
        {
            targetHost = Utf8ToWide(config.BackendHost);
            targetPort = static_cast<INTERNET_PORT>(config.BackendPort);
            secure = false;

            Logger::Info(
                "SteamHTTP redirect host=" + originalHost +
                " -> " + config.BackendHost + ":" + std::to_string(config.BackendPort));
        }
        else if (publicHost && config.BlockOfficialTraffic)
        {
            Logger::Info("SteamHTTP blocked public host=" + originalHost);
            return response;
        }

        const std::wstring method = MethodName(request.Method);
        if (method.empty() || targetHost.empty() || targetPort == 0)
            return response;

        std::vector<unsigned char> requestBody;
        std::wstring contentType;
        const std::wstring path = BuildPath(parsed, request, requestBody, contentType);

        std::wstring userAgent = L"BreakersRevived/";
        userAgent += Utf8ToWide(Breakers_VERSION);
        if (!request.UserAgentInfo.empty())
        {
            userAgent += L" ";
            userAgent += Utf8ToWide(request.UserAgentInfo);
        }

        HINTERNET session = WinHttpOpen(
            userAgent.c_str(),
            WINHTTP_ACCESS_TYPE_NO_PROXY,
            WINHTTP_NO_PROXY_NAME,
            WINHTTP_NO_PROXY_BYPASS,
            0);
        if (!session)
            return response;

        const int networkTimeout = request.NetworkActivityTimeoutSeconds > 0
            ? static_cast<int>(request.NetworkActivityTimeoutSeconds * 1000u)
            : 60000;
        const int totalTimeout = request.AbsoluteTimeoutMs > 0
            ? static_cast<int>(request.AbsoluteTimeoutMs)
            : networkTimeout;
        WinHttpSetTimeouts(session, totalTimeout, networkTimeout, networkTimeout, totalTimeout);

        HINTERNET connection = WinHttpConnect(session, targetHost.c_str(), targetPort, 0);
        if (!connection)
        {
            response.TimedOut = GetLastError() == ERROR_WINHTTP_TIMEOUT;
            WinHttpCloseHandle(session);
            return response;
        }

        DWORD flags = secure ? WINHTTP_FLAG_SECURE : 0;
        HINTERNET httpRequest = WinHttpOpenRequest(
            connection,
            method.c_str(),
            path.c_str(),
            nullptr,
            WINHTTP_NO_REFERER,
            WINHTTP_DEFAULT_ACCEPT_TYPES,
            flags);
        if (!httpRequest)
        {
            WinHttpCloseHandle(connection);
            WinHttpCloseHandle(session);
            return response;
        }

        if (secure && !request.RequireVerifiedCertificate)
        {
            DWORD securityFlags = SECURITY_FLAG_IGNORE_UNKNOWN_CA |
                SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
                SECURITY_FLAG_IGNORE_CERT_DATE_INVALID |
                SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
            WinHttpSetOption(httpRequest, WINHTTP_OPTION_SECURITY_FLAGS, &securityFlags, sizeof(securityFlags));
        }

        for (const auto& header : request.Headers)
        {
            const std::wstring line = Utf8ToWide(header.first + ": " + header.second);
            WinHttpAddRequestHeaders(
                httpRequest,
                line.c_str(),
                static_cast<DWORD>(-1),
                WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
        }

        if (!contentType.empty())
        {
            const std::wstring line = L"Content-Type: " + contentType;
            WinHttpAddRequestHeaders(
                httpRequest,
                line.c_str(),
                static_cast<DWORD>(-1),
                WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
        }

        const DWORD bodySize = static_cast<DWORD>(requestBody.size());
        LPVOID bodyPointer = requestBody.empty() ? WINHTTP_NO_REQUEST_DATA : requestBody.data();

        const bool sent = WinHttpSendRequest(
            httpRequest,
            WINHTTP_NO_ADDITIONAL_HEADERS,
            0,
            bodyPointer,
            bodySize,
            bodySize,
            0) != FALSE;

        if (!sent || !WinHttpReceiveResponse(httpRequest, nullptr))
        {
            response.TimedOut = GetLastError() == ERROR_WINHTTP_TIMEOUT;
            WinHttpCloseHandle(httpRequest);
            WinHttpCloseHandle(connection);
            WinHttpCloseHandle(session);
            return response;
        }

        DWORD status = 0;
        DWORD statusSize = sizeof(status);
        if (WinHttpQueryHeaders(
                httpRequest,
                WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX,
                &status,
                &statusSize,
                WINHTTP_NO_HEADER_INDEX))
        {
            response.StatusCode = static_cast<int>(status);
        }

        ParseResponseHeaders(httpRequest, response);
        response.RequestSuccessful = ReadResponseBody(httpRequest, response.Body);

        Logger::Info(
            "SteamHTTP response method=" + WideToUtf8(method) +
            " url=" + request.Url +
            " status=" + std::to_string(response.StatusCode) +
            " bytes=" + std::to_string(static_cast<unsigned long long>(response.Body.size())) +
            " success=" + (response.RequestSuccessful ? "1" : "0"));

        WinHttpCloseHandle(httpRequest);
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return response;
    }
}

#include "StdInc.h"
#include "HTTP/HttpRequestManager.h"
#include "HTTP/HttpCookieStore.h"

namespace
{
    std::mutex g_Mutex;
    uint32_t g_NextHandle = 1;
    std::unordered_map<uint32_t, HttpRequest> g_Requests;

    HttpRequest* Find(uint32_t handle)
    {
        auto it = g_Requests.find(handle);
        return it == g_Requests.end() ? nullptr : &it->second;
    }
}

namespace HttpRequestManager
{
    uint32_t Create(int method, const char* url)
    {
        if (!url || !*url || method <= 0 || method > 7)
            return 0;

        std::lock_guard<std::mutex> lock(g_Mutex);

        uint32_t handle = g_NextHandle++;
        if (handle == 0)
            handle = g_NextHandle++;

        HttpRequest request;
        request.Handle = handle;
        request.Method = method;
        request.Url = url;
        g_Requests[handle] = std::move(request);
        return handle;
    }

    bool SetContext(uint32_t handle, uint64_t context)
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        HttpRequest* request = Find(handle);
        if (!request || request->Sent)
            return false;
        request->ContextValue = context;
        return true;
    }

    bool SetNetworkTimeout(uint32_t handle, uint32_t seconds)
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        HttpRequest* request = Find(handle);
        if (!request || request->Sent)
            return false;
        request->NetworkActivityTimeoutSeconds = seconds;
        return true;
    }

    bool SetHeader(uint32_t handle, const char* name, const char* value)
    {
        if (!name || !*name || !value)
            return false;

        std::lock_guard<std::mutex> lock(g_Mutex);
        HttpRequest* request = Find(handle);
        if (!request || request->Sent)
            return false;
        request->Headers[name] = value;
        return true;
    }

    bool SetParameter(uint32_t handle, const char* name, const char* value)
    {
        if (!name || !value)
            return false;

        std::lock_guard<std::mutex> lock(g_Mutex);
        HttpRequest* request = Find(handle);
        if (!request || request->Sent || !request->RawPostBody.empty())
            return false;
        request->Parameters.emplace_back(name, value);
        return true;
    }

    bool SetRawPostBody(uint32_t handle, const char* contentType, const unsigned char* data, uint32_t size)
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        HttpRequest* request = Find(handle);
        if (!request || request->Sent || request->Method != 3 || !request->Parameters.empty())
            return false;
        if (size > 0 && !data)
            return false;

        request->RawPostContentType = contentType ? contentType : "";
        request->RawPostBody.clear();
        if (data && size > 0)
            request->RawPostBody.assign(data, data + size);
        return true;
    }

    bool SetUserAgentInfo(uint32_t handle, const char* value)
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        HttpRequest* request = Find(handle);
        if (!request || request->Sent)
            return false;
        request->UserAgentInfo = value ? value : "";
        return true;
    }

    bool SetCookieContainer(uint32_t handle, uint32_t cookieContainer)
    {
        if (cookieContainer != 0 && !HttpCookieStore::Exists(cookieContainer))
            return false;

        std::lock_guard<std::mutex> lock(g_Mutex);
        HttpRequest* request = Find(handle);
        if (!request || request->Sent)
            return false;
        request->CookieContainer = cookieContainer;
        return true;
    }

    bool SetRequiresVerifiedCertificate(uint32_t handle, bool required)
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        HttpRequest* request = Find(handle);
        if (!request || request->Sent)
            return false;
        request->RequireVerifiedCertificate = required;
        return true;
    }

    bool SetAbsoluteTimeout(uint32_t handle, uint32_t milliseconds)
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        HttpRequest* request = Find(handle);
        if (!request || request->Sent)
            return false;
        request->AbsoluteTimeoutMs = milliseconds;
        return true;
    }

    bool MarkStreaming(uint32_t handle, bool streaming)
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        HttpRequest* request = Find(handle);
        if (!request || request->Sent)
            return false;
        request->Streaming = streaming;
        return true;
    }

    bool MarkSent(uint32_t handle)
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        HttpRequest* request = Find(handle);
        if (!request || request->Sent)
            return false;
        request->Sent = true;
        return true;
    }

    bool StoreResponse(uint32_t handle, const HttpResponse& response)
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        HttpRequest* request = Find(handle);
        if (!request)
            return false;
        request->Response = response;
        return true;
    }

    bool Get(uint32_t handle, HttpRequest& request)
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        HttpRequest* found = Find(handle);
        if (!found)
            return false;
        request = *found;
        return true;
    }

    bool GetResponse(uint32_t handle, HttpResponse& response)
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        HttpRequest* request = Find(handle);
        if (!request || !request->Sent)
            return false;
        response = request->Response;
        return true;
    }

    bool Release(uint32_t handle)
    {
        if (handle == 0)
            return false;
        std::lock_guard<std::mutex> lock(g_Mutex);
        return g_Requests.erase(handle) != 0;
    }

    bool Exists(uint32_t handle)
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        return Find(handle) != nullptr;
    }
}

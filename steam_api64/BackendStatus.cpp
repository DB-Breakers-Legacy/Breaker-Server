#include "StdInc.h"
#include "BackendStatus.h"
#include "SteamConfig.h"
#include "Logger.h"

namespace BackendStatus
{
    bool Check()
    {
        WSADATA wsa{};
        const bool startedWinsock = WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
        if (!startedWinsock)
        {
            Logger::Info("Local backend check skipped: Winsock initialization failed");
            return false;
        }

        const auto& config = SteamConfig::Get();

        ADDRINFOA hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;

        PADDRINFOA result = nullptr;
        const std::string port = std::to_string(config.BackendPort);
        const int resolveResult = getaddrinfo(config.BackendHost.c_str(), port.c_str(), &hints, &result);
        if (resolveResult != 0 || !result)
        {
            Logger::Info(
                "Local backend unavailable: could not resolve " +
                config.BackendHost + ":" + port);
            WSACleanup();
            return false;
        }

        bool reachable = false;
        for (ADDRINFOA* current = result; current; current = current->ai_next)
        {
            SOCKET socketHandle = socket(current->ai_family, current->ai_socktype, current->ai_protocol);
            if (socketHandle == INVALID_SOCKET)
                continue;

            DWORD timeout = 300;
            setsockopt(socketHandle, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
            setsockopt(socketHandle, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));

            u_long nonBlocking = 1;
            ioctlsocket(socketHandle, FIONBIO, &nonBlocking);

            const int rc = connect(socketHandle, current->ai_addr, static_cast<int>(current->ai_addrlen));
            if (rc == 0)
            {
                reachable = true;
            }
            else if (WSAGetLastError() == WSAEWOULDBLOCK)
            {
                fd_set writeSet;
                FD_ZERO(&writeSet);
                FD_SET(socketHandle, &writeSet);

                timeval tv{};
                tv.tv_sec = 0;
                tv.tv_usec = 300000;

                reachable = select(0, nullptr, &writeSet, nullptr, &tv) > 0;
            }

            closesocket(socketHandle);
            if (reachable)
                break;
        }

        freeaddrinfo(result);

        Logger::Info(
            std::string("Local backend ") +
            (reachable ? "reachable: " : "not reachable yet: ") +
            config.BackendHost + ":" + port);

        WSACleanup();
        return reachable;
    }
}

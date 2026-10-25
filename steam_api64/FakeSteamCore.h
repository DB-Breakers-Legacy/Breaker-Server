#pragma once
#include "StdInc.h"

namespace FakeSteamCore
{
    bool Init();
    void Shutdown();

    HSteamUser UserHandle();
    HSteamPipe PipeHandle();

    void* Client();
    void* User();
    void* UserForVersion(const char* version);
    void* Utils();
    void* Apps();
    void* GameServer();
    void* GameServerStats();

    const char* InstallPath();
}

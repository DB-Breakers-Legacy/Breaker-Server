#pragma once

namespace OnlineCheckBypass
{
    bool Init();
    void RevalidateSignatures();
    const char* SignatureReport();
    void Shutdown();
}

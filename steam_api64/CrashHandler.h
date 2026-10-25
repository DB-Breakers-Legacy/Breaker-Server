#pragma once

#include "StdInc.h"

namespace CrashHandler
{
    bool Init();
    void Shutdown();
    std::string WriteMinidump(PEXCEPTION_POINTERS info, const std::string& reason);
    std::string LastDumpPath();
}

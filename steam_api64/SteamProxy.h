#pragma once
#include "StdInc.h"

namespace SteamProxy
{
    bool Init();
    FARPROC GetExport(const char* name);
}
#pragma once
#include "StdInc.h"

namespace SteamFactoryRegistry
{
    void Register(const std::string& name, void* ptr);
    void* Find(const std::string& name);
    void Dump();
}
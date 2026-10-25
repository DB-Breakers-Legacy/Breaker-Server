#pragma once
#include "StdInc.h"

namespace HttpCookieStore
{
    uint32_t Create(bool allowResponsesToModify);
    bool Release(uint32_t handle);
    bool Set(uint32_t handle, const char* host, const char* url, const char* cookie);
    bool Exists(uint32_t handle);
}

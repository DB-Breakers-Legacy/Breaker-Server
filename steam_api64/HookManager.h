#pragma once

namespace HookManager
{
    bool Init();
    void Shutdown();

    bool Create(void* target, void* detour, void** original);
    bool Enable(void* target);
    bool Disable(void* target);
}
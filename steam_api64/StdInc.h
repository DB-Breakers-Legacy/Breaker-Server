#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#include <Windows.h>
#include <Psapi.h>
#include <TlHelp32.h>
#include <WinSock2.h>
#include <Ws2tcpip.h>

#include "MinHook.h"

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <memory>
#include <utility>
#include <string>
#include <sstream>
#include <fstream>
#include <iostream>
#include <cstdio>
#include <vector>
#include <array>
#include <map>
#include <set>
#include <unordered_map>
#include <mutex>
#include <thread>
#include <atomic>
#include <filesystem>
#include <queue>
#include <algorithm>

typedef uint32_t AppId_t;
typedef uint64_t SteamAPICall_t;
typedef int32_t HSteamPipe;
typedef int32_t HSteamUser;
typedef uint64_t CSteamID;

typedef int32_t CallbackMsg_t;
typedef int32_t ESteamAPIInitResult;

#pragma comment(lib, "Ws2_32.lib")

#define Breakers_VERSION "0.1.0"

#define NSR_UNUSED(x) (void)(x)

#ifndef SAFE_DELETE
#define SAFE_DELETE(x) \
    do                 \
    {                  \
        if (x)         \
        {              \
            delete x;  \
            x = nullptr; \
        }              \
    } while (0)
#endif

#ifndef SAFE_RELEASE
#define SAFE_RELEASE(x) \
    do                  \
    {                   \
        if (x)          \
        {               \
            (x)->Release(); \
            x = nullptr; \
        }               \
    } while (0)
#endif

#ifndef ARRAYSIZE
#define ARRAYSIZE(x) (sizeof(x) / sizeof(x[0]))
#endif

#include "StdInc.h"
#include "ModLoader.h"
#include "Logger.h"

static std::string GetExeDirectory()
{
    char exePath[MAX_PATH]{};
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);

    std::string path = exePath;
    size_t slash = path.find_last_of("\\/");

    if (slash == std::string::npos)
        return "";

    return path.substr(0, slash + 1);
}

static bool DirectoryExists(const std::string& path)
{
    DWORD attr = GetFileAttributesA(path.c_str());

    return attr != INVALID_FILE_ATTRIBUTES &&
        (attr & FILE_ATTRIBUTE_DIRECTORY);
}

namespace ModLoader
{
    void LoadMods()
    {
        std::string rootDir = GetExeDirectory() + "BreakersRevived\\";
        std::string modsDir = rootDir + "mods\\";

        CreateDirectoryA(rootDir.c_str(), nullptr);
        CreateDirectoryA(modsDir.c_str(), nullptr);

        if (!DirectoryExists(modsDir))
        {
            Logger::Error("Failed to create mods folder: " + modsDir);
            return;
        }

        Logger::Info("Mods folder: " + modsDir);

        WIN32_FIND_DATAA findData{};
        HANDLE findHandle = FindFirstFileA(
            (modsDir + "*.dll").c_str(),
            &findData);

        if (findHandle == INVALID_HANDLE_VALUE)
        {
            Logger::Info("No mod DLLs found");
            return;
        }

        do
        {
            if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                continue;

            std::string modPath = modsDir + findData.cFileName;

            HMODULE modModule = LoadLibraryA(modPath.c_str());

            if (modModule)
            {
                Logger::Info("Loaded mod: " + modPath);
            }
            else
            {
                Logger::Error("Failed to load mod: " + modPath);
            }

        } while (FindNextFileA(findHandle, &findData));

        FindClose(findHandle);
    }
}
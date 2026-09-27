#include "StdInc.h"
#include "SteamConfig.h"
#include "Logger.h"

namespace
{
    SteamConfig::Config g_Config;
    std::string g_Path;

    std::string GetExeFolder()
    {
        char path[MAX_PATH]{};
        GetModuleFileNameA(nullptr, path, MAX_PATH);
        return std::filesystem::path(path).parent_path().string();
    }


	

    std::string ReadString(const char* key, const char* fallback)
    {
        char buffer[512]{};
        GetPrivateProfileStringA("steam", key, fallback, buffer, sizeof(buffer), g_Path.c_str());
        return buffer;
    }

    int ReadInt(const char* key, int fallback)
    {
        return GetPrivateProfileIntA("steam", key, fallback, g_Path.c_str());
    }

    void WriteString(const char* key, const std::string& value)
    {
        WritePrivateProfileStringA("steam", key, value.c_str(), g_Path.c_str());
    }

    void WriteInt(const char* key, int value)
    {
        WritePrivateProfileStringA("steam", key, std::to_string(value).c_str(), g_Path.c_str());
    }

    SteamConfig::Mode ParseMode(std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c){ return (char)std::tolower(c); });
        if (value == "revived" || value == "official" || value == "breakersrevived")
            return SteamConfig::Mode::RevivedServer;
        if (value == "custom" || value == "customserver" || value == "server")
            return SteamConfig::Mode::CustomServer;
        if (value == "steam" || value == "passthrough")
            return SteamConfig::Mode::SteamPassthrough;
        return SteamConfig::Mode::Offline;
    }
}

namespace SteamConfig
{
    bool Init()
    {
        g_Path = (std::filesystem::path(GetExeFolder()) / "BreakersRevived" / "steam_config.ini").string();
        try { std::filesystem::create_directories(std::filesystem::path(g_Path).parent_path()); } catch (...) {}
        Load();
        Logger::Info(std::string("SteamConfig initialized mode=") + ModeName(g_Config.CurrentMode) + " path=" + g_Path);
        return true;
    }


    bool Load()
    {
        if (g_Path.empty())
            g_Path = (std::filesystem::path(GetExeFolder()) / "BreakersRevived" / "steam_config.ini").string();

        if (!std::filesystem::exists(g_Path))
            Save();

        Mode requestedMode = ParseMode(ReadString("mode", "offline"));
        if (requestedMode == Mode::SteamPassthrough)
        {
            Logger::Info(
                std::string("SteamConfig blocked Steam passthrough; ignored requested mode=") +
                ModeName(requestedMode));
            requestedMode = Mode::Offline;
        }

        g_Config.CurrentMode = requestedMode;
        g_Config.PersonaName = ReadString("persona", "BreakersRevived");
        g_Config.SteamID = std::strtoull(ReadString("steamid", "76561199006065889").c_str(), nullptr, 10);
        g_Config.CustomServerHost = ReadString("custom_server", "127.0.0.1");
        g_Config.CustomServerPort = ReadInt("custom_port", 47584);
        g_Config.BackendHost = ReadString("backend_host", "127.0.0.1");
        g_Config.BackendPort = ReadInt("backend_port", 5000);
        g_Config.RedirectPublicHosts = ReadInt("redirect_public_hosts", 1) != 0;
        g_Config.BlockOfficialTraffic = true;
        g_Config.LanOnly = ReadInt("lan_only", 1) != 0;
        g_Config.LogPackets = ReadInt("log_packets", 1) != 0;
        g_Config.EnableLocalStorage = ReadInt("local_storage", 1) != 0;
        g_Config.EnableLocalStats = ReadInt("local_stats", 1) != 0;
        g_Config.EnableLocalMatchmaking = ReadInt("local_matchmaking", 1) != 0;
        g_Config.AutoInstanceSteamID = ReadInt("auto_instance_steamid", 1) != 0;
        g_Config.EnableEmbeddedMasterServer = ReadInt("embedded_master", 1) != 0;
        return true;
    }

    bool Save()
    {
        if (g_Path.empty())
            g_Path = (std::filesystem::path(GetExeFolder()) / "BreakersRevived" / "steam_config.ini").string();
        try { std::filesystem::create_directories(std::filesystem::path(g_Path).parent_path()); } catch (...) {}
        WriteString("mode", ModeName(g_Config.CurrentMode));
        WriteString("persona", g_Config.PersonaName);
        WriteString("steamid", std::to_string(g_Config.SteamID));
        WriteString("custom_server", g_Config.CustomServerHost);
        WriteInt("custom_port", g_Config.CustomServerPort);
        WriteString("backend_host", g_Config.BackendHost);
        WriteInt("backend_port", g_Config.BackendPort);
        WriteInt("redirect_public_hosts", g_Config.RedirectPublicHosts ? 1 : 0);
        WriteInt("block_official", g_Config.BlockOfficialTraffic ? 1 : 0);
        WriteInt("lan_only", g_Config.LanOnly ? 1 : 0);
        WriteInt("log_packets", g_Config.LogPackets ? 1 : 0);
        WriteInt("local_storage", g_Config.EnableLocalStorage ? 1 : 0);
        WriteInt("local_stats", g_Config.EnableLocalStats ? 1 : 0);
        WriteInt("local_matchmaking", g_Config.EnableLocalMatchmaking ? 1 : 0);
        WriteInt("auto_instance_steamid", g_Config.AutoInstanceSteamID ? 1 : 0);
        WriteInt("embedded_master", g_Config.EnableEmbeddedMasterServer ? 1 : 0);
        return true;
    }

    const Config& Get() { return g_Config; }
    Config& Mutable() { return g_Config; }
    bool IsOffline() { return g_Config.CurrentMode == Mode::Offline; }
    bool IsCustomServer() { return g_Config.CurrentMode == Mode::CustomServer; }
    bool IsRevivedServer() { return g_Config.CurrentMode == Mode::RevivedServer; }
    bool ShouldBlockOfficialTraffic() { return g_Config.BlockOfficialTraffic; }
    bool IsLanOnly() { return g_Config.LanOnly; }
    const std::string& GetPath() { return g_Path; }
	bool IsSteamPassthrough() { return g_Config.CurrentMode == Mode::SteamPassthrough; }
	bool IsEmbeddedMasterServerEnabled() { return g_Config.EnableEmbeddedMasterServer; }
	bool IsLoggingPackets() { return g_Config.LogPackets; }
	bool IsLocalStorageEnabled() { return g_Config.EnableLocalStorage; }
	bool IsLocalStatsEnabled() { return g_Config.EnableLocalStats; }
	bool IsLocalMatchmakingEnabled() { return g_Config.EnableLocalMatchmaking; }
	bool AutoInstanceSteamID() { return g_Config.AutoInstanceSteamID; }
	bool ShouldUseRevivedServer() { return IsRevivedServer() || IsCustomServer(); }
	bool ShouldUseCustomServer() { return IsCustomServer(); }
	bool ShouldUseSteamPassthrough() { return IsSteamPassthrough(); }
	bool ShouldUseOfficialServer() { return !ShouldUseRevivedServer() && !IsSteamPassthrough(); }
	bool ShouldUseAnyServer() { return ShouldUseRevivedServer() || IsSteamPassthrough(); }
	bool ShouldBlockSteam() { return ShouldBlockOfficialTraffic() || ShouldUseRevivedServer(); }
	bool ShouldBlockRevived() { return ShouldBlockOfficialTraffic() || ShouldUseCustomServer(); }
	bool ShouldBlockCustom() { return ShouldBlockOfficialTraffic() || ShouldUseRevivedServer(); }
	bool ShouldBlockAny() { return ShouldBlockOfficialTraffic() || ShouldUseRevivedServer() || ShouldUseCustomServer(); }
	bool  ShouldLogPackets() { return g_Config.LogPackets; }
	bool ShouldLanOnly() { return g_Config.LanOnly; }
	bool ShouldAutoInstanceSteamID() { return g_Config.AutoInstanceSteamID; }
	bool ShouldEnableEmbeddedMasterServer() { return g_Config.EnableEmbeddedMasterServer; }
	bool ShouldEnableLocalStorage() { return g_Config.EnableLocalStorage; }
	bool ShouldEnableLocalStats() { return g_Config.EnableLocalStats; }
	bool ShouldEnableLocalMatchmaking() { return g_Config.EnableLocalMatchmaking; }
	bool ShouldEnableLocalFeatures() { return ShouldEnableLocalStorage() || ShouldEnableLocalStats() || ShouldEnableLocalMatchmaking(); }
	bool ShouldEnableRevivedServer() { return IsRevivedServer(); }
	bool ShouldEnableCustomServer() { return IsCustomServer(); }
	bool ShouldEnableSteamPassthrough() { return IsSteamPassthrough(); }
	bool ShouldEnableOffline() { return IsOffline(); }
	bool ShouldEnableAnyServer() { return ShouldEnableRevivedServer() || ShouldEnableCustomServer() || ShouldUseSteamPassthrough(); }
	bool ShouldBlockOfficial() { return ShouldBlockOfficialTraffic(); }
	bool ShouldBlockLAN() { return ShouldLanOnly(); }
	
    
    const auto& GetInstanceFolders() {
		static std::vector<std::string> folders = []() {
			std::vector<std::string> out;
			for (int i = 1; i <= 4; ++i)
				out.push_back((std::filesystem::path(GetExeFolder()) / "BreakersRevived" / "Instances" / std::to_string(i)).string());
			return out;
			}(); return folders;
	}
    

    const char* ModeName(Mode mode)
    {
        switch (mode)
        {
        case Mode::CustomServer: return "custom";
        case Mode::RevivedServer: return "revived";
        case Mode::SteamPassthrough: return "steam";
        default: return "offline";
        }
    }
}

// "Save logs for sharing" (2.0.12): VR Options > Controls and the console
// command l4d2vr_save_logs. Included by game.cpp. The file work runs on its
// own thread (never the game or render thread); the selection, size caps,
// redaction and zip are std-only in vr_log_share.h (unit tested).
#include "vr_log_share.h"
#include <shellapi.h>
#include <objbase.h>
#include <thread>
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

namespace
{
    std::atomic<bool> g_LogShareBusy{ false };
    std::mutex g_LogShareMutex;
    std::string g_LogShareMenuStatus;            // taken by the VR Options overlay
    std::vector<std::string> g_LogShareConsoleLines; // echoed by the main thread

    std::string LogShareWideToUtf8(const std::wstring& text)
    {
        if (text.empty())
            return {};
        const int n = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0, nullptr, nullptr);
        std::string out(n > 0 ? (size_t)n : 0, '\0');
        if (n > 0)
            WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), &out[0], n, nullptr, nullptr);
        return out;
    }

    std::filesystem::path LogShareGameRoot()
    {
        wchar_t exePath[MAX_PATH] = {};
        const DWORD n = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        if (n == 0 || n >= MAX_PATH)
            return std::filesystem::current_path();
        return std::filesystem::path(exePath).parent_path();
    }

    // Facts read on the calling thread (cheap, no file access).
    struct LogShareSnapshot
    {
        bool vrEnabled = false;
        bool desktopMode = false;
        bool inGame = false;
        int matQueueMode = -1;
        uint32_t serverFeatures = 0;
        bool serverUnderstandsVR = false;
        std::string trigger;
    };

    std::string LogShareSystemInfo(const std::filesystem::path& root, const LogShareSnapshot& snap,
        const std::string& vrmodLogTail, const std::string& consoleTail)
    {
        std::string out = "L4D2VR system info (written by Save logs for sharing)\n\n";
        {
            std::ifstream in(root / L"VR" / L"version.txt", std::ios::binary);
            std::string version;
            std::getline(in, version);
            while (!version.empty() && (version.back() == '\r' || version.back() == ' '))
                version.pop_back();
            out += "mod version: " + (version.empty() ? std::string("unknown (VR\\version.txt missing)") : version) + "\n";
        }
        using WineGetVersion = const char* (__cdecl*)();
        const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        const auto wineVersion = ntdll ? reinterpret_cast<WineGetVersion>(GetProcAddress(ntdll, "wine_get_version")) : nullptr;
        if (wineVersion)
            out += std::string("system: Wine/Proton ") + wineVersion() + "\n";
        else
        {
            using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
            const auto rtlGetVersion = ntdll ? reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion")) : nullptr;
            OSVERSIONINFOW info{};
            info.dwOSVersionInfoSize = sizeof(info);
            if (rtlGetVersion && rtlGetVersion(&info) == 0)
                out += "system: Windows " + std::to_string(info.dwMajorVersion) + "." + std::to_string(info.dwMinorVersion) +
                    " build " + std::to_string(info.dwBuildNumber) + "\n";
            else
                out += "system: Windows\n";
        }
        for (DWORD i = 0; i < 8; ++i)
        {
            DISPLAY_DEVICEW device{};
            device.cb = sizeof(device);
            if (!EnumDisplayDevicesW(nullptr, i, &device, 0))
                break;
            if (device.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE)
            {
                out += "GPU (primary display): " + LogShareWideToUtf8(device.DeviceString) + "\n";
                break;
            }
        }
        out += "render path: the mod's DXVK (Direct3D 9 -> Vulkan) in d3d9.dll; VR through OpenXR\n";
        out += std::string("VR: ") + (snap.vrEnabled ? "on" : "off") + (snap.desktopMode ? " (desktop mode)" : "") +
            ", mat_queue_mode " + std::to_string(snap.matQueueMode) + "\n";
        char features[160];
        std::snprintf(features, sizeof(features), "server: %s, mod plugin %s, feature bits 0x%08X\n",
            snap.inGame ? "in a game" : "not in a game", snap.serverUnderstandsVR ? "present" : "not seen",
            (unsigned)snap.serverFeatures);
        out += features;
        std::string connected = l4d2vr_logshare::LastLineContaining(consoleTail, "Connected to ");
        if (connected.empty())
            connected = l4d2vr_logshare::LastLineContaining(consoleTail, "Connecting to ");
        out += "server address: " + (connected.empty() ? std::string("not in console.log") : connected) + "\n";
        out += "saved by: " + snap.trigger + "\n";
        out += "\nFrom vrmod_log.txt (start of this session):\n";
        out += l4d2vr_logshare::ExtractSystemLines(vrmodLogTail);
        return out;
    }

    void LogShareRun(LogShareSnapshot snap)
    {
        namespace ls = l4d2vr_logshare;
        const std::filesystem::path root = LogShareGameRoot();
        const std::filesystem::path vrDir = root / L"VR";
        const std::time_t now = std::time(nullptr);
        std::tm local{};
        localtime_s(&local, &now);
        const std::string folderName = ls::FolderName(local);

        std::vector<ls::Item> items = {
            { root / L"vrmod_log.txt", "vrmod_log.txt", 12ull * 1024 * 1024, true },
            { root / L"vrmod_log.prev.txt", "vrmod_log.prev.txt", 8ull * 1024 * 1024, true },
            { vrDir / L"config.txt", "config.txt", 1024 * 1024, true },
            { vrDir / L"version.txt", "version.txt", 64 * 1024, true },
            { vrDir / L"config_migrations_applied.txt", "config_migrations_applied.txt", 256 * 1024, true },
            { vrDir / L"vr_saved_game_cvars.txt", "vr_saved_game_cvars.txt", 256 * 1024, true },
            { root / L"left4dead2" / L"console.log", "console.log", 5ull * 1024 * 1024, true },
        };
        std::error_code ec;
        const std::filesystem::path debugLog = std::filesystem::exists(root / L"debug.log", ec)
            ? root / L"debug.log" : root / L"left4dead2" / L"debug.log";
        items.push_back({ debugLog, "debug.log", 2ull * 1024 * 1024, true });
        items.push_back({ root / L"left4dead2_d3d9.log", "left4dead2_d3d9.log", 2ull * 1024 * 1024, true });

        // The startup lines are at the beginning of the mod log; the server
        // address at the end of the console log.
        std::string vrmodHead;
        {
            std::ifstream in(root / L"vrmod_log.txt", std::ios::binary);
            vrmodHead.resize(256 * 1024);
            in.read(&vrmodHead[0], (std::streamsize)vrmodHead.size());
            vrmodHead.resize((size_t)(std::max<std::streamsize>)(0, in.gcount()));
        }
        std::string consoleTail;
        uint64_t consoleSize = 0;
        ls::ReadTail(root / L"left4dead2" / L"console.log", 1024 * 1024, consoleTail, consoleSize);

        std::vector<ls::Generated> generated = {
            { "system-info.txt", LogShareSystemInfo(root, snap, vrmodHead, consoleTail) },
        };
        const ls::Result result = ls::Collect(items, generated, vrDir / L"shared-logs", folderName, local, true);

        const std::string where = LogShareWideToUtf8((result.zip.empty() ? result.folder : result.zip).wstring());
        std::string menu, console;
        if (result.ok)
        {
            menu = "Logs saved: " + where + " (" + std::to_string(result.files) + " files, " + ls::FormatMb(result.bytes) + "). Send that file with your report.";
            console = "[L4D2VR] Logs for sharing saved to " + where;
        }
        else
        {
            menu = "Saving the logs failed" + (result.notes.empty() ? std::string() : ": " + result.notes.front());
            console = "[L4D2VR] " + menu;
        }
        Game::logMsg("[VR][LogShare] %s: folder %s, zip %s, %d files, %llu bytes, %d line(s) redacted (%s)",
            result.ok ? "saved" : "failed", LogShareWideToUtf8(result.folder.wstring()).c_str(),
            result.zip.empty() ? "none" : "written", result.files, (unsigned long long)result.bytes,
            result.redactedLines, snap.trigger.c_str());
        {
            std::lock_guard<std::mutex> lock(g_LogShareMutex);
            g_LogShareMenuStatus = menu;
            g_LogShareConsoleLines.push_back(console);
        }
        // Not in VR: show the folder (Explorer on Windows; Wine's explorer or
        // file browser under Proton). Best effort.
        if (result.ok && !snap.vrEnabled)
        {
            const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
            ShellExecuteW(nullptr, L"open", result.folder.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            if (SUCCEEDED(co))
                CoUninitialize();
        }
        g_LogShareBusy.store(false, std::memory_order_release);
    }
}

// Starts the copy on a worker thread. False when one is already running.
bool L4D2VR_StartSaveLogsForSharing(const char* trigger)
{
    bool expected = false;
    if (!g_LogShareBusy.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
        return false;
    LogShareSnapshot snap;
    snap.trigger = trigger ? trigger : "?";
    if (g_Game)
    {
        if (VR* vr = g_Game->m_VR)
        {
            snap.vrEnabled = vr->m_IsVREnabled;
            snap.serverFeatures = vr->m_ServerFeatureMask.load(std::memory_order_acquire);
        }
        snap.inGame = g_Game->m_EngineClient && g_Game->m_EngineClient->IsInGame();
        snap.matQueueMode = g_Game->GetMatQueueMode();
    }
    snap.desktopMode = !snap.vrEnabled;
    snap.serverUnderstandsVR = Hooks::s_ServerUnderstandsVR;
    Game::logMsg("[VR][LogShare] saving logs for sharing (%s)", snap.trigger.c_str());
    Game::FlushLog();
    try
    {
        std::thread(LogShareRun, std::move(snap)).detach();
    }
    catch (...)
    {
        g_LogShareBusy.store(false, std::memory_order_release);
        return false;
    }
    return true;
}

// VR Options overlay: the finished message, once.
bool L4D2VR_TakeSaveLogsMenuStatus(std::string& out)
{
    std::lock_guard<std::mutex> lock(g_LogShareMutex);
    if (g_LogShareMenuStatus.empty())
        return false;
    out.swap(g_LogShareMenuStatus);
    g_LogShareMenuStatus.clear();
    return true;
}

// Main thread (every frame): echo the finished message into the console.
void L4D2VR_FlushSaveLogsConsole(Game* game)
{
    std::vector<std::string> lines;
    {
        std::lock_guard<std::mutex> lock(g_LogShareMutex);
        if (g_LogShareConsoleLines.empty())
            return;
        lines.swap(g_LogShareConsoleLines);
    }
    for (std::string line : lines)
    {
        // A ';' or quote would end the echo and run the rest as a command.
        for (char& c : line)
            if (c == ';' || c == '"' || c == '\n' || c == '\r')
                c = ' ';
        game->ClientCmd_Unrestricted(("echo " + line).c_str());
    }
}

namespace
{
    void __cdecl OnSaveLogsCommand(const SourceCCommand&)
    {
        if (!L4D2VR_StartSaveLogsForSharing("console command"))
            Game::logMsg("[VR][LogShare] already saving the logs");
    }
}

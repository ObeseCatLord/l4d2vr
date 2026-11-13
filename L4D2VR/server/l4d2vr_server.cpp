#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cctype>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>

#include "MinHook.h"
#include "sigscanner.h"
#include "sdk/vector.h"

// Shared, std-only dedicated-server core (also used by the Linux shell).
#include "../server_core/vr_server_session.h"
#include "../server_core/vr_server_limits.h"
#include "../server_core/vr_usercmd_codec.h"

class Server_BaseEntity;

class IHandleEntity
{
public:
    virtual ~IHandleEntity() {}
    virtual void SetRefEHandle(int handle) = 0;
    virtual void GetRefEHandle() const = 0;
};

class IServerUnknown : public IHandleEntity
{
public:
    virtual void* GetCollideable() = 0;
    virtual void* GetNetworkable() = 0;
    virtual Server_BaseEntity* GetBaseEntity() = 0;
};

// L4D2's CBaseEdict: four 32-bit fields and no cached index (see
// vr_edict_identity.h, verified on the Windows server.dll); the player index
// is derived from the edict array that ServerActivate supplies.
struct edict_t
{
    int m_fStateFlags;
    int m_NetworkSerialNumber;
    void* m_pNetworkable;
    IServerUnknown* m_pUnk;
};
static_assert(sizeof(edict_t) == 16, "Unexpected L4D2 edict layout.");

class CCommand
{
public:
    int ArgC() const { return m_nArgc; }
    const char* Arg(int index) const
    {
        if (index < 0 || index >= m_nArgc) return "";
        return m_ppArgv[index] ? m_ppArgv[index] : "";
    }
private:
    enum { COMMAND_MAX_ARGC = 64, COMMAND_MAX_LENGTH = 512 };
    int m_nArgc;
    int m_nArgv0Size;
    char m_pArgSBuffer[COMMAND_MAX_LENGTH];
    char m_pArgvBuffer[COMMAND_MAX_LENGTH];
    const char* m_ppArgv[COMMAND_MAX_ARGC];
};
static_assert(sizeof(CCommand) == 1288, "Unexpected 32-bit Source CCommand layout.");

#include "sdk/sdk_server.h"

class matrix3x4_t;

struct cplane_t
{
    Vector normal;
    float dist;
    unsigned char type;
    unsigned char signbits;
    unsigned char pad[2];
};

struct csurface_t
{
    const char* name;
    short surfaceProps;
    unsigned short flags;
};

constexpr unsigned int CONTENTS_SOLID = 0x1;
constexpr unsigned int CONTENTS_WINDOW = 0x2;
constexpr unsigned int CONTENTS_GRATE = 0x8;
constexpr unsigned int CONTENTS_MOVEABLE = 0x4000;
constexpr unsigned int CONTENTS_PLAYERCLIP = 0x10000;
constexpr unsigned int CONTENTS_MONSTER = 0x2000000;

class CBaseTrace
{
public:
    Vector startpos;
    Vector endpos;
    cplane_t plane;
    float fraction = 1.0f;
    int contents = 0;
    unsigned short dispFlags = 0;
    bool allsolid = false;
    bool startsolid = false;
};

class CGameTrace : public CBaseTrace
{
public:
    float fractionleftsolid = 0.0f;
    csurface_t surface{};
    int hitgroup = 0;
    short physicsbone = 0;
    void* m_pEnt = nullptr;
    int hitbox = 0;
};

enum class TraceType
{
    TRACE_EVERYTHING = 0,
    TRACE_WORLD_ONLY,
    TRACE_ENTITIES_ONLY,
    TRACE_EVERYTHING_FILTER_PROPS,
};

class ITraceFilter
{
public:
    virtual bool ShouldHitEntity(IHandleEntity* entity, int contentsMask) = 0;
    virtual TraceType GetTraceType() const = 0;
};

class CTraceFilter : public ITraceFilter
{
public:
    CTraceFilter(IHandleEntity* passEntity, int collisionGroup)
        : m_pPassEnt(passEntity)
        , m_collisionGroup(collisionGroup)
    {
    }

    TraceType GetTraceType() const override
    {
        return TraceType::TRACE_EVERYTHING;
    }

    IHandleEntity* m_pPassEnt = nullptr;
    int m_collisionGroup = 0;
    void* m_pExtraShouldHitCheckFunction = nullptr;
};

class CTraceFilterSkipSelf : public CTraceFilter
{
public:
    CTraceFilterSkipSelf(IHandleEntity* passEntity, int collisionGroup)
        : CTraceFilter(passEntity, collisionGroup)
    {
    }

    bool ShouldHitEntity(IHandleEntity* entity, int) override
    {
        return entity && entity != m_pPassEnt;
    }
};

struct Ray_t
{
    VectorAligned m_Start;
    VectorAligned m_Delta;
    VectorAligned m_StartOffset;
    VectorAligned m_Extents;
    const matrix3x4_t* m_pWorldAxisTransform = nullptr;
    bool m_IsRay = true;
    bool m_IsSwept = false;

    void Init(const Vector& start, const Vector& end)
    {
        VectorSubtract(end, start, m_Delta);
        m_IsSwept = (m_Delta.LengthSqr() != 0.0f);
        VectorClear(m_Extents);
        m_IsRay = true;
        m_pWorldAxisTransform = nullptr;
        VectorClear(m_StartOffset);
        VectorCopy(start, m_Start);
    }

    void Init(const Vector& start, const Vector& end, const Vector& mins, const Vector& maxs)
    {
        VectorSubtract(end, start, m_Delta);
        m_IsSwept = (m_Delta.LengthSqr() != 0.0f);
        VectorSubtract(maxs, mins, m_Extents);
        m_Extents *= 0.5f;
        m_IsRay = (m_Extents.LengthSqr() < 1e-6f);
        m_pWorldAxisTransform = nullptr;
        VectorAdd(mins, maxs, m_StartOffset);
        m_StartOffset *= 0.5f;
        VectorAdd(start, m_StartOffset, m_Start);
        m_StartOffset *= -1.0f;
    }
};

using trace_t = CGameTrace;

class IEngineTrace
{
public:
    virtual void fn0() = 0;
    virtual void fn1() = 0;
    virtual void fn2() = 0;
    virtual void fn3() = 0;
    virtual void fn4() = 0;
    virtual void TraceRay(const Ray_t& ray, unsigned int mask, CTraceFilter* filter, trace_t* trace) = 0;
};

class IVEngineServer
{
public:
    virtual void ChangeLevel(const char*, const char*) = 0;
    virtual int IsMapValid(const char*) = 0;
    virtual bool IsDedicatedServer() = 0;
    virtual int IsInEditMode() = 0;
    virtual void* GetLaunchOptions() = 0;
    virtual int PrecacheModel(const char*, bool = false) = 0;
    virtual int PrecacheSentenceFile(const char*, bool = false) = 0;
    virtual int PrecacheDecal(const char*, bool = false) = 0;
    virtual int PrecacheGeneric(const char*, bool = false) = 0;
    virtual bool IsModelPrecached(const char*) const = 0;
    virtual bool IsDecalPrecached(const char*) const = 0;
    virtual bool IsGenericPrecached(const char*) const = 0;
    virtual int GetClusterForOrigin(const Vector&) = 0;
    virtual int GetPVSForCluster(int, int, unsigned char*) = 0;
    virtual bool CheckOriginInPVS(const Vector&, const unsigned char*, int) = 0;
    virtual bool CheckBoxInPVS(const Vector&, const Vector&, const unsigned char*, int) = 0;
    virtual int GetPlayerUserId(const edict_t*) = 0;
    virtual const char* GetPlayerNetworkIDString(const edict_t*) = 0;
    virtual bool IsUserIDInUse(int) = 0;
    virtual int GetLoadingProgressForUserID(int) = 0;
    virtual int GetEntityCount() = 0;
    virtual void* GetPlayerNetInfo(int) = 0;
    virtual edict_t* CreateEdict(int = -1) = 0;
    virtual void RemoveEdict(edict_t*) = 0;
    virtual void* PvAllocEntPrivateData(long) = 0;
    virtual void FreeEntPrivateData(void*) = 0;
    virtual void* SaveAllocMemory(size_t, size_t) = 0;
    virtual void SaveFreeMemory(void*) = 0;
    virtual void EmitAmbientSound(int, const Vector&, const char*, float, int, int, int, float = 0.0f) = 0;
    virtual void FadeClientVolume(const edict_t*, float, float, float, float) = 0;
    virtual int SentenceGroupPick(int, char*, int) = 0;
    virtual int SentenceGroupPickSequential(int, char*, int, int, int) = 0;
    virtual int SentenceIndexFromName(const char*) = 0;
    virtual const char* SentenceNameFromIndex(int) = 0;
    virtual int SentenceGroupIndexFromName(const char*) = 0;
    virtual const char* SentenceGroupNameFromIndex(int) = 0;
    virtual float SentenceLength(int) = 0;
    virtual void ServerCommand(const char*) = 0;
    virtual void ServerExecute() = 0;
    virtual void ClientCommand(edict_t*, const char*, ...) = 0;
};

namespace
{
    static_assert(sizeof(void*) == 4, "L4D2VR dedicated server plugin must be built as 32-bit/x86.");

    constexpr int kMaxPlayers = 65;
    constexpr int kWeaponMelee = 19;
    constexpr int kWeaponMolotov = 13;
    constexpr int kWeaponPipeBomb = 14;
    constexpr int kWeaponVomitJar = 25;
    constexpr int kUsingMountedGunOffset = 0x1EBA;
    constexpr int kUsingMountedWeaponOffset = 0x1EBB;
    constexpr int kMeleeSwingHistoryCapacity = 16;
    constexpr float kDefaultTeleportMaxDistanceUnits = 2500.0f;
    // Files live next to the plugin in left4dead2/addons/ (as documented),
    // resolved against the server's working directory at load.
    constexpr const char* kServerFileDirectories[] = { "left4dead2/addons/", "addons/", "" };
    char kServerLogFileName[300] = "l4d2vr_server_log.txt";
    char kServerConfigFileName[300] = "l4d2vr_server_config.txt";

    constexpr int kIsIncapacitatedOffset = 0x1EA9;
    constexpr int kEntitiesHitThisSwingOffset = 0x17F0;   // Server_WeaponCSBase::entitiesHitThisSwing
    constexpr int kTongueOwnerOffset = 0x1F6C;
    constexpr int kIsHangingFromTongueOffset = 0x1F84;
    constexpr int kCarryAttackerOffset = 0x2714;
    constexpr int kPummelAttackerOffset = 0x2720;
    constexpr int kPounceAttackerOffset = 0x272C;
    constexpr int kJockeyAttackerOffset = 0x274C;

    struct ServerConfig
    {
        bool teleportEnabled = true;
        bool teleportBlockWhileControlled = true;
        bool teleportBlockWhileIncapacitated = true;
        float teleportMaxDistanceUnits = kDefaultTeleportMaxDistanceUnits;
        float teleportCooldownSeconds = 1.0f;

        bool meleeSwingEnabled = true;
        bool meleeSwingBlockWhileControlled = true;
        bool meleeSwingBlockWhileIncapacitated = true;
        float meleeSwingCooldownSeconds = 0.35f;
        float meleeSwingBurstWindowSeconds = 2.0f;
        int meleeSwingBurstMax = 4;
    };

    struct PlayerVrState
    {
        bool isUsingVR = false;
        bool isMeleeing = false;
        bool physicalCrouch = false;
        bool isNewSwing = false;
        bool currentMeleeSwingAllowed = false;
        bool haveViewYaw = false;
        float viewYaw = 0.0f;
        std::chrono::steady_clock::time_point lastTeleportTime{};
        std::chrono::steady_clock::time_point lastMeleeSwingTime{};
        std::array<std::chrono::steady_clock::time_point, kMeleeSwingHistoryCapacity> meleeSwingHistory{};
        Vector controllerPos = Vector(0.0f, 0.0f, 0.0f);
        QAngle controllerAngle = QAngle(0.0f, 0.0f, 0.0f);
        QAngle prevControllerAngle = QAngle(0.0f, 0.0f, 0.0f);
        Vector prevControllerPos = Vector(0.0f, 0.0f, 0.0f);
        bool havePrevControllerPose = false;
        uint64_t lastDecodedMs = 0;              // GetTickCount64 of the newest VR command
        uint64_t swingStartMs = 0;
        // Throw-aim state (was shared by every player: B14).
        uintptr_t throwableAimWeapon = 0;
        bool prevAttackDown = false;
        bool prevWeaponThrowable = false;
        int throwableAimTicks = 0;
        // Execution-time eye override armed by the newest decoded command:
        // commands run after ProcessUsercmds returns (PlayerRunCommand), so the
        // override is keyed by player and decode time, not by the decode call.
        void* aimPlayer = nullptr;
        int aimReason = 0;
        uint64_t aimArmedMs = 0;
        Vector aimOrigin = Vector(0.0f, 0.0f, 0.0f);
        QAngle aimAngles = QAngle(0.0f, 0.0f, 0.0f);
        l4d2vr_server::LogBudget logBudget{};
    };

    // An armed eye override applies to the commands of the packet it was
    // decoded from; they execute within a frame or two.
    constexpr uint64_t kAimOverrideMaxAgeMs = 150;
    // The newest decoded pose is used for shots only while fresh.
    constexpr uint64_t kPoseMaxAgeMs = 500;
    // Decode happens before the packet's commands move the player: allow this
    // much on top of the reach limit there (bullets clamp exactly at fire time).
    constexpr float kDecodeReachSlackUnits = 24.0f;

    class CUserCmd
    {
    public:
        virtual ~CUserCmd() {}

        int command_number;
        int tick_count;
        QAngle viewangles;
        float forwardmove;
        float sidemove;
        float upmove;
        int buttons;
        unsigned char impulse;
        int weaponselect;
        int weaponsubtype;
        int random_seed;
        short mousedx;
        short mousedy;
        bool hasbeenpredicted;
        char pad[25];
    };

    static_assert(sizeof(CUserCmd) == 0x58, "Unexpected CUserCmd layout.");

    using tServerFireTerrorBullets = int(__cdecl*)(int playerId, const Vector& origin, const QAngle& angles, int a4, int a5, int a6, float a7);
    using tProcessUsercmds = float(__thiscall*)(void* thisptr, edict_t* player, void* buf, int numcmds, int totalcmds, int droppedPackets, bool ignore, bool paused);
    using tReadUsercmd = int(__cdecl*)(void* buf, CUserCmd* move, CUserCmd* from);
    using tTestMeleeSwingCollision = int(__thiscall*)(void* thisptr, Vector const& direction);
    using tGetPrimaryAttackActivity = int(__thiscall*)(void* thisptr, void* meleeInfo);
    using tEyePosition = Vector* (__thiscall*)(void* thisptr, Vector* eyePos);
    using tEyeAngles = const QAngle* (__thiscall*)(void* thisptr);
    using tFindUseEntity = Server_BaseEntity* (__thiscall*)(void* thisptr, float radius, float dotLimit, float defaultDotLimit, void* traceResult, void* extra);
    using tPlayerUse = void(__thiscall*)(void* thisptr, void* useEntity);
    using tGetActiveWeapon = Server_WeaponCSBase* (__thiscall*)(void* thisptr);
    using tGetMeleeWeaponInfo = void* (__thiscall*)(void* thisptr);
    using tGetAbsOriginServer = Vector* (__thiscall*)(void* thisptr);
    using tSetOriginServer = void(__thiscall*)(void* thisptr, const Vector& origin);

    std::mutex g_logMutex;
    std::array<PlayerVrState, kMaxPlayers> g_players{};
    std::atomic_bool g_hooksInstalled{ false };
    std::atomic_bool g_unloading{ false };
    HMODULE g_module = nullptr;
    IEngineTrace* g_engineTraceServer = nullptr;
    IVEngineServer* g_engineServer = nullptr;

    // engine.dll "VoiceServer002" (CVoiceServer): MSVC puts one deleting
    // destructor first, so GetClientListening is slot 1 (checked in engine.dll).
    class IVoiceServer
    {
    public:
        virtual ~IVoiceServer() {}
        virtual bool GetClientListening(int receiver, int sender) = 0;   // 1-based entity indices
        virtual bool SetClientListening(int receiver, int sender, bool listen) = 0;
        virtual bool SetClientProximity(int receiver, int sender, bool proximity) = 0;
    };
    IVoiceServer* g_voiceServer = nullptr;
    ServerConfig g_config{};
    // Keys shared with the Linux plugin (MaxHandReachMeters, UnitsPerMeter,
    // FeatureMask, MeleeMin*/MeleeMaxSwingSeconds, HandTraceEnabled, Verbose).
    l4d2vr_server::ServerConfig g_shared{};

    thread_local int g_currentUsercmdPlayerIndex = -1;
    thread_local Server_BaseEntity* g_currentUsercmdPlayer = nullptr;
    thread_local IServerUnknown* g_currentUsercmdUnknown = nullptr;
    thread_local bool g_performingMeleeTrace = false;
    thread_local bool g_useControllerAimOverride = false;
    thread_local void* g_useControllerAimPlayer = nullptr;
    thread_local Vector g_useControllerAimOrigin = Vector(0.0f, 0.0f, 0.0f);
    thread_local QAngle g_useControllerAimAngles = QAngle(0.0f, 0.0f, 0.0f);
    thread_local QAngle g_returnEyeAngles = QAngle(0.0f, 0.0f, 0.0f);

    void ResolveServerFilePaths()
    {
        // The first directory that exists wins (a config already there wins first).
        const char* chosen = "";
        for (const char* dir : kServerFileDirectories)
        {
            char probe[300];
            std::snprintf(probe, sizeof(probe), "%sl4d2vr_server_config.txt", dir);
            FILE* f = nullptr;
            if (fopen_s(&f, probe, "r") == 0 && f) { fclose(f); chosen = dir; break; }
            if (*dir)
            {
                const DWORD attributes = GetFileAttributesA(dir);
                if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 && !*chosen)
                    chosen = dir;
            }
        }
        std::snprintf(kServerConfigFileName, sizeof(kServerConfigFileName), "%sl4d2vr_server_config.txt", chosen);
        std::snprintf(kServerLogFileName, sizeof(kServerLogFileName), "%sl4d2vr_server_log.txt", chosen);
    }

    void ResetLogFile()
    {
        std::lock_guard<std::mutex> lock(g_logMutex);

        FILE* file = nullptr;
        if (fopen_s(&file, kServerLogFileName, "w") == 0 && file)
        {
            fputs("L4D2VR dedicated server log\n", file);
            fclose(file);
        }
    }

    void Log(const char* fmt, ...)
    {
        std::lock_guard<std::mutex> lock(g_logMutex);

        SYSTEMTIME st{};
        GetLocalTime(&st);

        char message[2048] = {};
        va_list args;
        va_start(args, fmt);
        vsnprintf_s(message, sizeof(message), _TRUNCATE, fmt, args);
        va_end(args);

        char line[2300] = {};
        snprintf(line, sizeof(line),
            "[%04u-%02u-%02u %02u:%02u:%02u] %s\n",
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, message);

        OutputDebugStringA(line);

        FILE* file = nullptr;
        if (fopen_s(&file, kServerLogFileName, "a") == 0 && file)
        {
            fputs(line, file);
            fclose(file);
        }
    }

    bool IsValidPlayerIndex(int index)
    {
        return index >= 0 && index < static_cast<int>(g_players.size());
    }

    std::string Trim(std::string value)
    {
        auto isSpace = [](unsigned char ch) { return std::isspace(ch) != 0; };
        while (!value.empty() && isSpace(static_cast<unsigned char>(value.front())))
            value.erase(value.begin());
        while (!value.empty() && isSpace(static_cast<unsigned char>(value.back())))
            value.pop_back();
        return value;
    }

    std::string ToLowerAscii(std::string value)
    {
        for (char& ch : value)
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        return value;
    }

    bool ParseBoolValue(const std::string& value, bool& out)
    {
        const std::string lower = ToLowerAscii(Trim(value));
        if (lower == "1" || lower == "true" || lower == "yes" || lower == "on")
        {
            out = true;
            return true;
        }

        if (lower == "0" || lower == "false" || lower == "no" || lower == "off")
        {
            out = false;
            return true;
        }

        return false;
    }

    bool ParseFloatValue(const std::string& value, float& out)
    {
        char* end = nullptr;
        const float parsed = std::strtof(value.c_str(), &end);
        if (end == value.c_str() || !std::isfinite(parsed))
            return false;

        out = parsed;
        return true;
    }

    bool ParseIntValue(const std::string& value, int& out)
    {
        char* end = nullptr;
        const long parsed = std::strtol(value.c_str(), &end, 10);
        if (end == value.c_str())
            return false;

        out = static_cast<int>(std::clamp<long>(parsed, -1000000L, 1000000L));
        return true;
    }

    void WriteDefaultServerConfigIfMissing()
    {
        FILE* existing = nullptr;
        if (fopen_s(&existing, kServerConfigFileName, "r") == 0 && existing)
        {
            fclose(existing);
            return;
        }

        FILE* file = nullptr;
        if (fopen_s(&file, kServerConfigFileName, "w") != 0 || !file)
            return;

        fputs(
            "# L4D2VR dedicated server config. Edit values, then restart the server.\n"
            "# Distances are Source units. Rough reference: 43.2 units is about 1 meter.\n"
            "# Pose sanity: a hand farther than this from the eye is pulled back (max 1.2).\n"
            "UnitsPerMeter=43.2\n"
            "MaxHandReachMeters=1.2\n"
            "HandTraceEnabled=true\n"
            "\n"
            "MeleeSwingEnabled=true\n"
            "MeleeSwingBlockWhileControlled=true\n"
            "MeleeSwingBlockWhileIncapacitated=true\n"
            "MeleeSwingCooldownSeconds=0.35\n"
            "MeleeSwingBurstWindowSeconds=2.0\n"
            "MeleeSwingBurstMax=4\n"
            "MeleeMinHandSpeed=0.5\n"
            "MeleeMinAngularSpeed=90\n"
            "MeleeMaxSwingSeconds=1.5\n",
            file);
        fclose(file);
    }

    void ApplyServerConfigValue(const std::string& rawKey, const std::string& rawValue)
    {
        const std::string key = ToLowerAscii(Trim(rawKey));
        const std::string value = Trim(rawValue);

        bool boolValue = false;
        float floatValue = 0.0f;
        int intValue = 0;

        if (key == "teleportenabled" && ParseBoolValue(value, boolValue))
            g_config.teleportEnabled = boolValue;
        else if (key == "teleportblockwhilecontrolled" && ParseBoolValue(value, boolValue))
            g_config.teleportBlockWhileControlled = boolValue;
        else if (key == "teleportblockwhileincapacitated" && ParseBoolValue(value, boolValue))
            g_config.teleportBlockWhileIncapacitated = boolValue;
        else if (key == "teleportmaxdistanceunits" && ParseFloatValue(value, floatValue))
            g_config.teleportMaxDistanceUnits = std::clamp(floatValue, 1.0f, 100000.0f);
        else if (key == "teleportmaxdistancemeters" && ParseFloatValue(value, floatValue))
            g_config.teleportMaxDistanceUnits = std::clamp(floatValue * 43.2f, 1.0f, 100000.0f);
        else if (key == "teleportcooldownseconds" && ParseFloatValue(value, floatValue))
            g_config.teleportCooldownSeconds = std::clamp(floatValue, 0.0f, 600.0f);
        else if (key == "meleeswingenabled" && ParseBoolValue(value, boolValue))
            g_config.meleeSwingEnabled = boolValue;
        else if (key == "meleeswingblockwhilecontrolled" && ParseBoolValue(value, boolValue))
            g_config.meleeSwingBlockWhileControlled = boolValue;
        else if (key == "meleeswingblockwhileincapacitated" && ParseBoolValue(value, boolValue))
            g_config.meleeSwingBlockWhileIncapacitated = boolValue;
        else if (key == "meleeswingcooldownseconds" && ParseFloatValue(value, floatValue))
            g_config.meleeSwingCooldownSeconds = std::clamp(floatValue, 0.0f, 60.0f);
        else if (key == "meleeswingburstwindowseconds" && ParseFloatValue(value, floatValue))
            g_config.meleeSwingBurstWindowSeconds = std::clamp(floatValue, 0.0f, 60.0f);
        else if (key == "meleeswingburstmax" && ParseIntValue(value, intValue))
            g_config.meleeSwingBurstMax = std::clamp(intValue, 0, kMeleeSwingHistoryCapacity);
        // Keys shared with the Linux plugin; unknown keys stay ignored.
        l4d2vr_server::ApplyConfigLine(g_shared, rawKey + "=" + rawValue);
    }

    void LoadServerConfig()
    {
        g_config = ServerConfig{};
        g_shared = l4d2vr_server::ServerConfig{};
        WriteDefaultServerConfigIfMissing();

        FILE* file = nullptr;
        if (fopen_s(&file, kServerConfigFileName, "r") != 0 || !file)
        {
            Log("server config not found; using defaults");
            return;
        }

        char lineBuffer[512] = {};
        while (fgets(lineBuffer, sizeof(lineBuffer), file))
        {
            std::string line(lineBuffer);
            const size_t comment = line.find('#');
            if (comment != std::string::npos)
                line.erase(comment);

            line = Trim(line);
            if (line.empty())
                continue;

            const size_t equals = line.find('=');
            if (equals == std::string::npos)
                continue;

            ApplyServerConfigValue(line.substr(0, equals), line.substr(equals + 1));
        }
        fclose(file);

        Log("server config loaded: teleport enabled=%d controlledBlock=%d incapBlock=%d maxUnits=%.1f cooldown=%.2f melee enabled=%d controlledBlock=%d incapBlock=%d cooldown=%.2f burstWindow=%.2f burstMax=%d",
            g_config.teleportEnabled ? 1 : 0,
            g_config.teleportBlockWhileControlled ? 1 : 0,
            g_config.teleportBlockWhileIncapacitated ? 1 : 0,
            g_config.teleportMaxDistanceUnits,
            g_config.teleportCooldownSeconds,
            g_config.meleeSwingEnabled ? 1 : 0,
            g_config.meleeSwingBlockWhileControlled ? 1 : 0,
            g_config.meleeSwingBlockWhileIncapacitated ? 1 : 0,
            g_config.meleeSwingCooldownSeconds,
            g_config.meleeSwingBurstWindowSeconds,
            g_config.meleeSwingBurstMax);
        Log("server config file %s: reach=%.2f m (%.1f units) handTrace=%d featureMask=0x%X",
            kServerConfigFileName, g_shared.maxHandReachMeters, g_shared.MaxHandReachUnits(),
            g_shared.handTraceEnabled ? 1 : 0, static_cast<unsigned>(g_shared.featureMask));
    }

    // Player index from the engine's edict array (ServerActivate); Windows and
    // Linux L4D2 edicts carry no index of their own.
    edict_t* g_edictList = nullptr;
    int g_edictCount = 0;
    int g_clientMax = l4d2vr_server::kMaxPlayerSlots;

    int GetPlayerIndexFromEdict(const edict_t* entity)
    {
        if (!entity || !g_edictList || g_edictCount <= 0)
            return -1;
        const auto base = reinterpret_cast<uintptr_t>(g_edictList);
        const auto address = reinterpret_cast<uintptr_t>(entity);
        if (address < base || (address - base) % sizeof(edict_t) != 0u)
            return -1;
        const uintptr_t index = (address - base) / sizeof(edict_t);
        if (index >= static_cast<uintptr_t>(g_edictCount) || index < 1u ||
            index > static_cast<uintptr_t>(g_clientMax) || index >= static_cast<uintptr_t>(kMaxPlayers))
            return -1;
        return static_cast<int>(index);
    }

    // Handshake, pose relay (v2 + v3) and capability bundle from the shared core.
    class SessionHost final : public l4d2vr_server::SessionHost
    {
    public:
        void SendToClient(const void* entity, const char* text) override
        {
            if (!g_engineServer || !entity || !text)
                return;
            edict_t* edict = const_cast<edict_t*>(static_cast<const edict_t*>(entity));
#ifdef _MSC_VER
            __try
            {
                g_engineServer->ClientCommand(edict, "%s", text);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
            }
#else
            g_engineServer->ClientCommand(edict, "%s", text);
#endif
        }
        uint64_t NowMs() const override { return static_cast<uint64_t>(GetTickCount64()); }
        uint64_t NowUs() const override
        {
            return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
        }
        bool ViewYaw(int playerIndex, float& yaw) const override;
        void Log(const char* line) override;
        // Opus voice follows the game's own voice rules (team, alltalk, mutes).
        bool VoiceCanHear(int receiver, int sender) const override;
    };

    SessionHost g_sessionHost;
    l4d2vr_server::Session g_session(g_sessionHost);

    bool IsFiniteVector(const Vector& value)
    {
        return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
    }

    bool IsFiniteAngles(const QAngle& value)
    {
        return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
    }

    template <typename T>
    bool TryReadEntityValue(const Server_BaseEntity* entity, int offset, T& out)
    {
        if (!entity || offset < 0)
            return false;

#ifdef _MSC_VER
        __try
        {
            out = *reinterpret_cast<const T*>(reinterpret_cast<const unsigned char*>(entity) + offset);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
#else
        out = *reinterpret_cast<const T*>(reinterpret_cast<const unsigned char*>(entity) + offset);
        return true;
#endif
    }

    bool HandleValid(int handle)
    {
        return handle != 0 && handle != -1;
    }

    // The status offsets are compile-time constants for one server.dll build.
    // A read that cannot be a bool/EHANDLE means the layout moved: log once and
    // treat the state as unknown (not blocked) instead of trusting garbage.
    std::atomic_bool g_statusLayoutSuspect{ false };
    void NoteSuspectStatusLayout(const char* what, int value)
    {
        if (!g_statusLayoutSuspect.exchange(true))
            Log("status offset check failed (%s=0x%X): player status offsets look stale for this server.dll; "
                "pinned/incapacitated melee blocks are unreliable", what, static_cast<unsigned>(value));
    }

    bool PlausibleBool(unsigned char value, const char* what)
    {
        if (value <= 1) return true;
        NoteSuspectStatusLayout(what, value);
        return false;
    }

    bool PlausibleHandle(int value, const char* what)
    {
        // EHANDLE: -1/0 (none), or 12 index bits + serial bits (< 2^22 networked, < 2^31 on the server).
        if (value == -1 || (value >= 0 && (value & 0xFFF) < 2048)) return true;
        NoteSuspectStatusLayout(what, value);
        return false;
    }

    bool IsServerPlayerIncapacitated(const Server_BaseEntity* player)
    {
        unsigned char incapacitated = 0;
        return TryReadEntityValue(player, kIsIncapacitatedOffset, incapacitated) &&
            PlausibleBool(incapacitated, "m_isIncapacitated") && incapacitated != 0;
    }

    bool IsServerPlayerControlledBySI(const Server_BaseEntity* player)
    {
        if (!player)
            return false;

        int tongueOwner = 0;
        unsigned char hangingFromTongue = 0;
        int carryAttacker = 0;
        int pummelAttacker = 0;
        int pounceAttacker = 0;
        int jockeyAttacker = 0;

        TryReadEntityValue(player, kTongueOwnerOffset, tongueOwner);
        TryReadEntityValue(player, kIsHangingFromTongueOffset, hangingFromTongue);
        TryReadEntityValue(player, kCarryAttackerOffset, carryAttacker);
        TryReadEntityValue(player, kPummelAttackerOffset, pummelAttacker);
        TryReadEntityValue(player, kPounceAttackerOffset, pounceAttacker);
        TryReadEntityValue(player, kJockeyAttackerOffset, jockeyAttacker);

        if (!PlausibleBool(hangingFromTongue, "m_isHangingFromTongue") || !PlausibleHandle(tongueOwner, "m_tongueOwner") ||
            !PlausibleHandle(carryAttacker, "m_carryAttacker") || !PlausibleHandle(pummelAttacker, "m_pummelAttacker") ||
            !PlausibleHandle(pounceAttacker, "m_pounceAttacker") || !PlausibleHandle(jockeyAttacker, "m_jockeyAttacker"))
            return false;
        return hangingFromTongue != 0 ||
            HandleValid(tongueOwner) ||
            HandleValid(carryAttacker) ||
            HandleValid(pummelAttacker) ||
            HandleValid(pounceAttacker) ||
            HandleValid(jockeyAttacker);
    }

    void NormalizeAndClampViewAngles(QAngle& angles)
    {
        while (angles.x > 180.0f) angles.x -= 360.0f;
        while (angles.x < -180.0f) angles.x += 360.0f;
        while (angles.y > 180.0f) angles.y -= 360.0f;
        while (angles.y < -180.0f) angles.y += 360.0f;
        angles.z = 0.0f;
        angles.x = std::clamp(angles.x, -89.0f, 89.0f);
    }

    Server_BaseEntity* GetBaseEntityFromEdict(edict_t* player)
    {
        if (!player || !player->m_pUnk)
            return nullptr;

#ifdef _MSC_VER
        __try
        {
            return player->m_pUnk->GetBaseEntity();
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
#else
        return player->m_pUnk->GetBaseEntity();
#endif
    }

    struct DedicatedOffset
    {
        const char* name = nullptr;
        const char* moduleName = nullptr;
        int offset = 0;
        const char* signature = nullptr;
        int sigOffset = 0;
        bool optional = false;
        uintptr_t address = 0;
        bool valid = false;

        bool Resolve()
        {
            address = 0;
            valid = false;

            if (!moduleName || !signature)
                return false;

            const int verifiedOffset = SigScanner::VerifyOffset(moduleName, offset, signature, sigOffset);
            if (verifiedOffset > 0)
                offset = verifiedOffset;
            else if (verifiedOffset == -1)
            {
                if (!optional)
                    Log("required signature not found: %s", name ? name : signature);
                else
                    Log("optional signature not found: %s", name ? name : signature);
                return optional;
            }

            HMODULE module = GetModuleHandleA(moduleName);
            if (!module)
            {
                if (!optional)
                    Log("required module not loaded: %s", moduleName);
                return optional;
            }

            address = reinterpret_cast<uintptr_t>(module) + static_cast<uintptr_t>(offset);
            valid = address != 0;
            if (valid)
                Log("resolved %s at %s+0x%X", name ? name : "<offset>", moduleName, offset);
            return valid || optional;
        }
    };

    struct DedicatedOffsets
    {
        DedicatedOffset ServerFireTerrorBullets{ "ServerFireTerrorBullets", "server.dll", 0x3C3FC0, "55 8B EC 81 EC ? ? ? ? A1 ? ? ? ? 33 C5 89 45 FC 8B 45 08 8B 4D 10" };
        DedicatedOffset ReadUserCmd{ "ReadUserCmd", "server.dll", 0x205100, "55 8B EC 53 8B 5D 10 56 57 8B 7D 0C 53" };
        DedicatedOffset ProcessUsercmds{ "ProcessUsercmds", "server.dll", 0xEF710, "55 8B EC B8 ? ? ? ? E8 ? ? ? ? A1 ? ? ? ? 33 C5 89 45 FC 8B 45 0C 8B 55 08" };
        DedicatedOffset TestMeleeSwingServer{ "TestMeleeSwingServer", "server.dll", 0x3E79E0, "24 FF D2 5B 5F 5E C3", 20 };
        DedicatedOffset GetPrimaryAttackActivity{ "GetPrimaryAttackActivity", "server.dll", 0x3E7630, "55 8B EC 53 8B 5D 08 56 57 8B BB ? ? ? ?" };
        DedicatedOffset GetActiveWeapon{ "GetActiveWeapon", "server.dll", 0x464F0, "55 8B EC 8B 45 0C 56 8B 75 08 50 56 E8 ? ? ? ? 84 C0 74 47 8B", -64 };
        DedicatedOffset GetMeleeWeaponInfo{ "GetMeleeWeaponInfo", "server.dll", 0x3E67D0, "8B 81 ? ? ? ? 50 B9 ? ? ? ? E8 ? ? ? ? C3" };
        DedicatedOffset EyePosition{ "EyePosition", "server.dll", 0x6D610, "55 8B EC 56 8B F1 8B 86 ? ? ? ? C1 E8 0B A8 01 74 05 E8 ? ? ? ? 8B 45 08 F3" };
        DedicatedOffset ServerPlayerEyeAngles{ "ServerPlayerEyeAngles", "server.dll", 0x7B0F0, "55 8B EC 83 EC 64 A1 ? ? ? ? 33 C5 89 45 FC 56 57 8B F9 8B 0D ? ? ? ? E8 ? ? ? ? 84 C0 74 64 80 3D ? ? ? ? 00 75 5B 8B 87 ? ? ? ? 83 F8 FF 74 50", 0, true };
        DedicatedOffset ServerPlayerEyePosition{ "ServerPlayerEyePosition", "server.dll", 0x7B2A0, "55 8B EC 56 57 8B F9 8B 0D ? ? ? ? E8 ? ? ? ? 84 C0 74 71 80 3D ? ? ? ? 00 75 68 8B 87 ? ? ? ? 83 F8 FF 74 5D", 0, true };
        DedicatedOffset FindUseEntity{ "FindUseEntity", "server.dll", 0x34E6C0, "55 8B EC B8 80 13 00 00 E8 ? ? ? ? A1 ? ? ? ? 33 C5 89 45 FC 0F 57 C9 F3 0F 10 45 0C 0F 2F C8 F3 0F 10 55 08", 0, true };
        DedicatedOffset PlayerUse{ "PlayerUse", "server.dll", 0x312AC0, "55 8B EC 81 EC 94 00 00 00 A1 ? ? ? ? 33 C5 89 45 FC 53 56 8B F1 8B 9E B8 1C 00 00 8B 06 8B 90 28 01 00 00", 0, true };
        DedicatedOffset CBaseEntity_GetAbsOrigin_Server{ "CBaseEntity_GetAbsOrigin_Server", "server.dll", 0x28D10, "56 8B F1 8B 86 ? ? ? ? C1 E8 0B A8 01 74 05 E8 ? ? ? ? 8D 86 ? ? ? ? 5E C3", 0, true };
        DedicatedOffset CBaseEntity_SetOrigin_Server{ "CBaseEntity_SetOrigin_Server", "server.dll", 0x521A0, "55 8B EC 8B 01 8B 55 08 8B 80 ? ? ? ? 6A 00 6A 00 52 FF D0 5D C2 04 00", 0, true };

        bool ResolveAll()
        {
            bool ok = true;
            ok &= ServerFireTerrorBullets.Resolve();
            ok &= ReadUserCmd.Resolve();
            ok &= ProcessUsercmds.Resolve();
            ok &= TestMeleeSwingServer.Resolve();
            ok &= GetPrimaryAttackActivity.Resolve();
            ok &= GetActiveWeapon.Resolve();
            ok &= GetMeleeWeaponInfo.Resolve();
            ok &= EyePosition.Resolve();
            ok &= ServerPlayerEyeAngles.Resolve();
            ok &= ServerPlayerEyePosition.Resolve();
            ok &= FindUseEntity.Resolve();
            ok &= PlayerUse.Resolve();
            ok &= CBaseEntity_GetAbsOrigin_Server.Resolve();
            ok &= CBaseEntity_SetOrigin_Server.Resolve();
            return ok;
        }
    };

    DedicatedOffsets g_offsets;

    template <typename T>
    struct Hook
    {
        T original = nullptr;
        void* target = nullptr;
        const char* name = nullptr;

        bool Create(void* targetFunc, void* detourFunc, const char* hookName, bool required = true)
        {
            name = hookName;
            target = targetFunc;

            if (!targetFunc)
            {
                if (required)
                    Log("hook target missing: %s", hookName);
                return !required;
            }

            const MH_STATUS status = MH_CreateHook(targetFunc, detourFunc, reinterpret_cast<void**>(&original));
            if (status != MH_OK)
            {
                Log("MH_CreateHook failed for %s: %d", hookName, static_cast<int>(status));
                return !required;
            }

            return true;
        }
    };

    Hook<tServerFireTerrorBullets> g_serverFireTerrorBullets;
    Hook<tProcessUsercmds> g_processUsercmds;
    Hook<tReadUsercmd> g_readUsercmd;
    Hook<tTestMeleeSwingCollision> g_testMeleeSwingServer;
    Hook<tGetPrimaryAttackActivity> g_getPrimaryAttackActivity;
    Hook<tEyePosition> g_eyePosition;
    Hook<tEyePosition> g_serverPlayerEyePosition;
    Hook<tEyeAngles> g_serverPlayerEyeAngles;
    Hook<tFindUseEntity> g_findUseEntity;
    Hook<tPlayerUse> g_playerUse;

    bool TryGetNativeEye(Server_BaseEntity* player, Vector& eye)
    {
        if (!player || !g_eyePosition.original)
            return false;
        Vector result{};
#ifdef _MSC_VER
        __try
        {
            if (!g_eyePosition.original(player, &result))
                return false;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
#else
        if (!g_eyePosition.original(player, &result))
            return false;
#endif
        eye = result;
        return IsFiniteVector(eye);
    }

    Server_WeaponCSBase* SafeGetActiveWeapon(Server_BaseEntity* player)
    {
        if (!player || !g_offsets.GetActiveWeapon.valid)
            return nullptr;

        auto getActiveWeapon = reinterpret_cast<tGetActiveWeapon>(g_offsets.GetActiveWeapon.address);
#ifdef _MSC_VER
        __try
        {
            return getActiveWeapon(player);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
#else
        return getActiveWeapon(player);
#endif
    }

    int SafeGetWeaponId(Server_WeaponCSBase* weapon)
    {
        if (!weapon)
            return 0;

#ifdef _MSC_VER
        __try
        {
            return weapon->GetWeaponID();
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
#else
        return weapon->GetWeaponID();
#endif
    }

    void* SafeGetMeleeWeaponInfo(Server_WeaponCSBase* weapon)
    {
        if (!weapon || !g_offsets.GetMeleeWeaponInfo.valid)
            return nullptr;

        auto getInfo = reinterpret_cast<tGetMeleeWeaponInfo>(g_offsets.GetMeleeWeaponInfo.address);
#ifdef _MSC_VER
        __try
        {
            return getInfo(weapon);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
#else
        return getInfo(weapon);
#endif
    }

    bool IsThrowableWeapon(int weaponId)
    {
        return weaponId == kWeaponMolotov || weaponId == kWeaponPipeBomb || weaponId == kWeaponVomitJar;
    }

    bool IsCurrentServerPlayerUsingMountedWeapon()
    {
        if (!g_currentUsercmdPlayer)
            return false;

#ifdef _MSC_VER
        __try
        {
            const auto* base = reinterpret_cast<const unsigned char*>(g_currentUsercmdPlayer);
            return base[kUsingMountedGunOffset] != 0 || base[kUsingMountedWeaponOffset] != 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
#else
        const auto* base = reinterpret_cast<const unsigned char*>(g_currentUsercmdPlayer);
        return base[kUsingMountedGunOffset] != 0 || base[kUsingMountedWeaponOffset] != 0;
#endif
    }

    bool TryGetCurrentServerWeapon(Server_WeaponCSBase*& weapon, int& weaponId)
    {
        weapon = SafeGetActiveWeapon(g_currentUsercmdPlayer);
        weaponId = SafeGetWeaponId(weapon);
        return weapon != nullptr;
    }

    // Player slot of a server player entity (the edicts of slots 1..maxclients).
    int PlayerIndexFromEntity(const void* entity)
    {
        if (!entity || !g_edictList)
            return -1;
        const int last = (std::min)(g_clientMax, g_edictCount - 1);
        for (int index = 1; index <= last && index < kMaxPlayers; ++index)
            if (GetBaseEntityFromEdict(&g_edictList[index]) == entity)
                return index;
        return -1;
    }

    bool PoseFresh(const PlayerVrState& vrPlayer, uint64_t now, uint64_t maxAgeMs)
    {
        return vrPlayer.isUsingVR && vrPlayer.lastDecodedMs != 0 && now >= vrPlayer.lastDecodedMs &&
            now - vrPlayer.lastDecodedMs <= maxAgeMs;
    }

    // Use traces run inside PlayerRunCommand, after ProcessUsercmds returned:
    // find the VR player by entity, not by the decode-time globals.
    bool TryBuildServerUseControllerPose(void* player, Vector& origin, QAngle& angles)
    {
        const int index = PlayerIndexFromEntity(player);
        if (!IsValidPlayerIndex(index))
            return false;

        const PlayerVrState& vrPlayer = g_players[index];
        if (!PoseFresh(vrPlayer, GetTickCount64(), kPoseMaxAgeMs))
            return false;

        origin = vrPlayer.controllerPos;
        angles = vrPlayer.controllerAngle;
        NormalizeAndClampViewAngles(angles);
        return IsFiniteVector(origin) && IsFiniteAngles(angles);
    }

    bool TryGetServerControllerAimOverride(void* player, Vector& origin, QAngle& angles, int& reason)
    {
        if (!player)
            return false;

        if (g_useControllerAimOverride && player == g_useControllerAimPlayer)
        {
            origin = g_useControllerAimOrigin;
            angles = g_useControllerAimAngles;
            reason = 1;
            return IsFiniteVector(origin) && IsFiniteAngles(angles);
        }

        // Override armed by the player's newest decoded command (mounted gun,
        // grip pickup, throw grace), applied while that packet's commands run.
        const uint64_t now = GetTickCount64();
        for (int index = 1; index < kMaxPlayers; ++index)
        {
            const PlayerVrState& vrPlayer = g_players[index];
            if (vrPlayer.aimPlayer != player || vrPlayer.aimReason == 0)
                continue;
            if (!vrPlayer.isUsingVR || now < vrPlayer.aimArmedMs || now - vrPlayer.aimArmedMs > kAimOverrideMaxAgeMs)
                return false;
            origin = vrPlayer.aimOrigin;
            angles = vrPlayer.aimAngles;
            reason = vrPlayer.aimReason;
            return IsFiniteVector(origin) && IsFiniteAngles(angles);
        }
        return false;
    }

    // ---- Hand wall trace (eye -> hand) --------------------------------------
    using tGetRefEHandle = const unsigned int* (__thiscall*)(void* thisptr);

    bool IsPlayerHandleEntity(IHandleEntity* entity)
    {
        if (!entity)
            return false;
#ifdef _MSC_VER
        __try
        {
            auto fn = reinterpret_cast<tGetRefEHandle>((*reinterpret_cast<void***>(entity))[2]);
            const unsigned int* handle = fn(entity);
            const unsigned int index = handle ? (*handle & 0xFFFu) : 0u;
            return index >= 1u && index <= static_cast<unsigned int>(g_clientMax);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
#else
        auto fn = reinterpret_cast<tGetRefEHandle>((*reinterpret_cast<void***>(entity))[2]);
        const unsigned int* handle = fn(entity);
        const unsigned int index = handle ? (*handle & 0xFFFu) : 0u;
        return index >= 1u && index <= static_cast<unsigned int>(g_clientMax);
#endif
    }

    class HandTraceFilter final : public CTraceFilter
    {
    public:
        explicit HandTraceFilter(IHandleEntity* self) : CTraceFilter(self, 0) {}
        // Players never stop the hand; world, doors, brushes and props do.
        bool ShouldHitEntity(IHandleEntity* entity, int) override
        {
            return entity && entity != m_pPassEnt && !IsPlayerHandleEntity(entity);
        }
    };

    // Pulls `hand` back in front of the first solid surface on eye -> hand.
    bool TraceHandFromEye(void* player, const Vector& eye, Vector& hand)
    {
        if (!g_engineTraceServer || !g_shared.handTraceEnabled || !IsFiniteVector(eye) || !IsFiniteVector(hand))
            return false;
        const Vector delta = hand - eye;
        const float length = delta.Length();
        if (!(length > 1.0f))
            return false;
        Ray_t ray;
        ray.Init(eye, hand);
        trace_t trace{};
        trace.fraction = 1.0f;
        HandTraceFilter filter(static_cast<IHandleEntity*>(player));
        constexpr unsigned int kMask = CONTENTS_SOLID | CONTENTS_WINDOW | CONTENTS_MOVEABLE;
#ifdef _MSC_VER
        __try
        {
            g_engineTraceServer->TraceRay(ray, kMask, &filter, &trace);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
#else
        g_engineTraceServer->TraceRay(ray, kMask, &filter, &trace);
#endif
        if (trace.startsolid || trace.allsolid || !std::isfinite(trace.fraction) || trace.fraction >= 1.0f)
            return false;
        const float keep = (std::max)(0.0f, trace.fraction * length - 2.0f) / length;
        hand = eye + delta * keep;
        return true;
    }

    // Native eye of the decoding player (EyePosition's original, no override).
    bool TryGetNativeEye(Server_BaseEntity* player, Vector& eye);

    class ScopedServerUseControllerAimOverride
    {
    public:
        ScopedServerUseControllerAimOverride(void* player, const Vector& origin, const QAngle& angles)
            : m_prevActive(g_useControllerAimOverride)
            , m_prevPlayer(g_useControllerAimPlayer)
            , m_prevOrigin(g_useControllerAimOrigin)
            , m_prevAngles(g_useControllerAimAngles)
        {
            g_useControllerAimOverride = true;
            g_useControllerAimPlayer = player;
            g_useControllerAimOrigin = origin;
            g_useControllerAimAngles = angles;
        }

        ~ScopedServerUseControllerAimOverride()
        {
            g_useControllerAimOverride = m_prevActive;
            g_useControllerAimPlayer = m_prevPlayer;
            g_useControllerAimOrigin = m_prevOrigin;
            g_useControllerAimAngles = m_prevAngles;
        }

    private:
        bool m_prevActive;
        void* m_prevPlayer;
        Vector m_prevOrigin;
        QAngle m_prevAngles;
    };

    int __cdecl DetourServerFireTerrorBullets(int playerId, const Vector& origin, const QAngle& angles, int a4, int a5, int a6, float a7)
    {
        Vector correctedOrigin = origin;
        QAngle correctedAngles = angles;

        // Hand-origin bullets: a VR player after the handshake (AkimboRays),
        // with a fresh pose, held to the reach limit around the engine's own
        // shoot position (B01).
        if (IsValidPlayerIndex(playerId) &&
            (g_session.EffectiveFeatures(playerId) & l4d2vr_server::kFeatureAkimboRays) != 0u)
        {
            const PlayerVrState& player = g_players[playerId];
            if (PoseFresh(player, GetTickCount64(), kPoseMaxAgeMs) &&
                IsFiniteVector(player.controllerPos) && IsFiniteAngles(player.controllerAngle) && IsFiniteVector(origin))
            {
                l4d2vr_server::Vec3 hand(player.controllerPos.x, player.controllerPos.y, player.controllerPos.z);
                l4d2vr_server::ClampHandReach(l4d2vr_server::Vec3(origin.x, origin.y, origin.z), hand,
                    g_shared.MaxHandReachUnits());
                correctedOrigin = Vector(hand.x, hand.y, hand.z);
                correctedAngles = player.controllerAngle;
                NormalizeAndClampViewAngles(correctedAngles);
            }
        }

        return g_serverFireTerrorBullets.original(playerId, correctedOrigin, correctedAngles, a4, a5, a6, a7);
    }

    int __cdecl DetourReadUsercmd(void* buf, CUserCmd* move, CUserCmd* from)
    {
        const int originalResult = g_readUsercmd.original(buf, move, from);
        if (!move)
            return originalResult;

        const int playerIndex = g_currentUsercmdPlayerIndex;
        const bool hasValidPlayer = IsValidPlayerIndex(playerIndex);
        if (move->tick_count >= 0)
        {
            // Plain (desktop) command: never modified.
            if (hasValidPlayer)
            {
                g_players[playerIndex].isUsingVR = false;
                g_players[playerIndex].isMeleeing = false;
                g_players[playerIndex].currentMeleeSwingAllowed = false;
                g_players[playerIndex].aimReason = 0;
            }
            return originalResult;
        }

        // The decoding player's own eye: reach limit and exact-pitch reference.
        Vector nativeEye{};
        const bool haveEye = hasValidPlayer && TryGetNativeEye(g_currentUsercmdPlayer, nativeEye);
        const float referenceZ = nativeEye.z - 64.0f + l4d2vr_server::pitch_pack::kReferenceAboveOrigin;

        // Shared codec: identical decode/strip to the listen host's dReadUsercmd.
        l4d2vr_server::UsercmdData cmd{};
        cmd.command_number = move->command_number;
        cmd.tick_count = move->tick_count;
        cmd.viewangles[0] = move->viewangles.x;
        cmd.viewangles[1] = move->viewangles.y;
        cmd.viewangles[2] = move->viewangles.z;
        cmd.forwardmove = move->forwardmove;
        cmd.sidemove = move->sidemove;
        cmd.upmove = move->upmove;
        cmd.buttons = move->buttons;
        cmd.impulse = move->impulse;
        cmd.weaponselect = move->weaponselect;
        cmd.weaponsubtype = move->weaponsubtype;
        cmd.random_seed = move->random_seed;
        cmd.mousedx = move->mousedx;
        cmd.mousedy = move->mousedy;
        const l4d2vr_server::DecodedUsercmd decoded = l4d2vr_server::DecodeVRUsercmd(cmd, haveEye ? &referenceZ : nullptr);
        move->command_number = cmd.command_number;
        move->tick_count = cmd.tick_count;
        move->viewangles.x = cmd.viewangles[0];
        move->viewangles.z = cmd.viewangles[2];
        move->upmove = cmd.upmove;
        move->buttons = cmd.buttons;
        move->impulse = cmd.impulse;
        move->weaponselect = cmd.weaponselect;
        move->weaponsubtype = cmd.weaponsubtype;
        move->forwardmove = cmd.forwardmove;
        move->sidemove = cmd.sidemove;

        // VR treatment only after the handshake; the command is stripped either way.
        const l4d2vr_server::SessionClient* client = hasValidPlayer ? g_session.Client(playerIndex) : nullptr;
        if (!client || !client->helloSeen)
            return originalResult;

        PlayerVrState& vrPlayer = g_players[playerIndex];
        g_session.NoteVRUsercmd(playerIndex);
        vrPlayer.viewYaw = move->viewangles.y;
        vrPlayer.haveViewYaw = std::isfinite(vrPlayer.viewYaw);
        vrPlayer.isMeleeing = decoded.meleeSwing;
        if (!decoded.meleeSwing)
            vrPlayer.currentMeleeSwingAllowed = false;
        // Without the server's own eye the hand cannot be held to the reach
        // limit: no hand pose at all (fail closed).
        vrPlayer.isUsingVR = haveEye && decoded.poseValid && l4d2vr_server::SaneControllerAngles(decoded.controllerAngles);
        l4d2vr_server::Vec3 hand = decoded.controllerPos;
        if (vrPlayer.isUsingVR)
        {
            l4d2vr_server::ClampHandReach(l4d2vr_server::Vec3(nativeEye.x, nativeEye.y, nativeEye.z), hand,
                g_shared.MaxHandReachUnits() + kDecodeReachSlackUnits);
            vrPlayer.controllerPos = Vector(hand.x, hand.y, hand.z);
            TraceHandFromEye(g_currentUsercmdPlayer, nativeEye, vrPlayer.controllerPos);
            vrPlayer.lastDecodedMs = GetTickCount64();
        }
        vrPlayer.controllerAngle = QAngle(decoded.controllerAngles.x, decoded.controllerAngles.y, decoded.controllerAngles.z);
        NormalizeAndClampViewAngles(vrPlayer.controllerAngle);
        if (!vrPlayer.isUsingVR)
        {
            vrPlayer.isMeleeing = false;
            vrPlayer.currentMeleeSwingAllowed = false;
            vrPlayer.aimReason = 0;
            return originalResult;
        }

        // Throw grace after a grenade release, per player (B14), like the
        // Linux plugin: the throw runs with the hand's aim for 48 commands.
        Server_WeaponCSBase* serverWeapon = nullptr;
        int serverWeaponId = 0;
        TryGetCurrentServerWeapon(serverWeapon, serverWeaponId);
        const uintptr_t weaponTag = reinterpret_cast<uintptr_t>(serverWeapon);
        if (weaponTag != vrPlayer.throwableAimWeapon)
        {
            if (vrPlayer.prevWeaponThrowable && vrPlayer.prevAttackDown)
                vrPlayer.throwableAimTicks = (std::max)(vrPlayer.throwableAimTicks, 48);
            vrPlayer.throwableAimWeapon = weaponTag;
            vrPlayer.prevAttackDown = false;
            vrPlayer.prevWeaponThrowable = false;
        }
        constexpr int kInAttack = (1 << 0);
        const bool attackDown = (move->buttons & kInAttack) != 0;
        const bool activeWeaponIsThrowable = IsThrowableWeapon(serverWeaponId);
        if (activeWeaponIsThrowable && vrPlayer.prevAttackDown && !attackDown)
            vrPlayer.throwableAimTicks = (std::max)(vrPlayer.throwableAimTicks, 48);
        int commandControllerAimReason = 0;
        if (vrPlayer.throwableAimTicks > 0)
        {
            commandControllerAimReason = 3;
            --vrPlayer.throwableAimTicks;
        }
        vrPlayer.prevAttackDown = activeWeaponIsThrowable && attackDown;
        vrPlayer.prevWeaponThrowable = activeWeaponIsThrowable;

        if (IsCurrentServerPlayerUsingMountedWeapon())
            commandControllerAimReason = 4;
        if (decoded.intent == l4d2vr_server::Intent::GripPickup && decoded.useDown)
            commandControllerAimReason = 8;

        vrPlayer.aimReason = commandControllerAimReason;
        if (commandControllerAimReason != 0)
        {
            vrPlayer.aimPlayer = g_currentUsercmdPlayer;
            vrPlayer.aimArmedMs = GetTickCount64();
            vrPlayer.aimOrigin = vrPlayer.controllerPos;
            vrPlayer.aimAngles = vrPlayer.controllerAngle;
            NormalizeAndClampViewAngles(vrPlayer.aimAngles);
        }
        return originalResult;
    }

    bool SessionHost::VoiceCanHear(int receiver, int sender) const
    {
        if (!g_voiceServer) return true;
#ifdef _MSC_VER
        __try { return g_voiceServer->GetClientListening(receiver, sender); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return true; }
#else
        return g_voiceServer->GetClientListening(receiver, sender);
#endif
    }

    bool SessionHost::ViewYaw(int playerIndex, float& yaw) const
    {
        if (!IsValidPlayerIndex(playerIndex))
            return false;
        if (g_players[playerIndex].haveViewYaw)
        {
            yaw = g_players[playerIndex].viewYaw;
            return true;
        }
        // Before the sender's first VR usercmd: the player's server eye angles.
        if (!g_serverPlayerEyeAngles.original || !g_edictList || playerIndex >= g_edictCount)
            return false;
        Server_BaseEntity* player = GetBaseEntityFromEdict(&g_edictList[playerIndex]);
        if (!player)
            return false;
        const QAngle* angles = nullptr;
#ifdef _MSC_VER
        __try { angles = g_serverPlayerEyeAngles.original(player); }
        __except (EXCEPTION_EXECUTE_HANDLER) { angles = nullptr; }
#else
        angles = g_serverPlayerEyeAngles.original(player);
#endif
        if (!angles || !std::isfinite(angles->y))
            return false;
        yaw = angles->y;
        return true;
    }

    void SessionHost::Log(const char* line)
    {
        char trimmed[512];
        std::snprintf(trimmed, sizeof(trimmed), "%s", line ? line : "");
        const size_t length = std::strlen(trimmed);
        if (length > 0 && trimmed[length - 1] == '\n')
            trimmed[length - 1] = '\0';
        ::Log("%s", trimmed);
    }

    int CountRecentMeleeSwings(const PlayerVrState& vrPlayer, std::chrono::steady_clock::time_point now)
    {
        if (g_config.meleeSwingBurstWindowSeconds <= 0.0f || g_config.meleeSwingBurstMax <= 0)
            return 0;

        const auto windowMs = std::chrono::milliseconds(
            static_cast<int>(g_config.meleeSwingBurstWindowSeconds * 1000.0f));
        int count = 0;
        for (const auto& swingTime : vrPlayer.meleeSwingHistory)
        {
            if (swingTime.time_since_epoch().count() != 0 && now - swingTime <= windowMs)
                ++count;
        }
        return count;
    }

    void RecordMeleeSwing(PlayerVrState& vrPlayer, std::chrono::steady_clock::time_point now)
    {
        vrPlayer.lastMeleeSwingTime = now;

        size_t writeIndex = 0;
        for (size_t i = 0; i < vrPlayer.meleeSwingHistory.size(); ++i)
        {
            if (vrPlayer.meleeSwingHistory[i].time_since_epoch().count() == 0)
            {
                writeIndex = i;
                break;
            }

            if (vrPlayer.meleeSwingHistory[i] < vrPlayer.meleeSwingHistory[writeIndex])
                writeIndex = i;
        }

        vrPlayer.meleeSwingHistory[writeIndex] = now;
    }

    // Client-driven lines (a client can toggle the swing flag every command)
    // go through the player's log budget.
    bool MeleeLogAllowed(PlayerVrState& vrPlayer, int playerIndex)
    {
        if (g_shared.verbose)
            return true;
        switch (vrPlayer.logBudget.Take(GetTickCount64()))
        {
        case l4d2vr_server::LogBudget::Verdict::Log: return true;
        case l4d2vr_server::LogBudget::Verdict::LogSuppressedNote:
            Log("melee player=%d: further swing lines suppressed for 10 s", playerIndex);
            return false;
        default: return false;
        }
    }

    bool CanStartMeleeSwing(Server_BaseEntity* player, int playerIndex, PlayerVrState& vrPlayer)
    {
        if (!g_config.meleeSwingEnabled)
        {
            if (MeleeLogAllowed(vrPlayer, playerIndex)) Log("melee swing rejected player=%d reason=disabled", playerIndex);
            return false;
        }

        if (g_config.meleeSwingBlockWhileControlled && IsServerPlayerControlledBySI(player))
        {
            if (MeleeLogAllowed(vrPlayer, playerIndex)) Log("melee swing rejected player=%d reason=controlled", playerIndex);
            return false;
        }

        if (g_config.meleeSwingBlockWhileIncapacitated && IsServerPlayerIncapacitated(player))
        {
            if (MeleeLogAllowed(vrPlayer, playerIndex)) Log("melee swing rejected player=%d reason=incapacitated", playerIndex);
            return false;
        }

        const auto now = std::chrono::steady_clock::now();
        if (g_config.meleeSwingCooldownSeconds > 0.0f &&
            vrPlayer.lastMeleeSwingTime.time_since_epoch().count() != 0)
        {
            const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - vrPlayer.lastMeleeSwingTime).count();
            const float elapsedSeconds = static_cast<float>(elapsedMs) / 1000.0f;
            if (elapsedSeconds < g_config.meleeSwingCooldownSeconds)
            {
                if (MeleeLogAllowed(vrPlayer, playerIndex)) Log("melee swing rejected player=%d reason=cooldown remaining=%.2f", playerIndex, g_config.meleeSwingCooldownSeconds - elapsedSeconds);
                return false;
            }
        }

        if (g_config.meleeSwingBurstWindowSeconds > 0.0f &&
            g_config.meleeSwingBurstMax > 0 &&
            CountRecentMeleeSwings(vrPlayer, now) >= g_config.meleeSwingBurstMax)
        {
            if (MeleeLogAllowed(vrPlayer, playerIndex)) Log("melee swing rejected player=%d reason=burst-limit window=%.2f max=%d",
                playerIndex,
                g_config.meleeSwingBurstWindowSeconds,
                g_config.meleeSwingBurstMax);
            return false;
        }

        RecordMeleeSwing(vrPlayer, now);
        return true;
    }

    // entitiesHitThisSwing sits at a fixed offset of one server.dll build; it
    // only ever holds a small count, so anything else means the layout moved.
    bool ResetMeleeHitList(Server_WeaponCSBase* weapon)
    {
        static std::atomic_bool s_warned{ false };
        int count = 0;
        if (!TryReadEntityValue(reinterpret_cast<const Server_BaseEntity*>(weapon),
                kEntitiesHitThisSwingOffset, count))
            return false;
        if (count < 0 || count > 64)
        {
            if (!s_warned.exchange(true))
                Log("melee hit-list offset check failed (value %d): physical melee sweeps disabled for this server.dll", count);
            return false;
        }
        weapon->entitiesHitThisSwing = 0;
        return true;
    }

    void ApplyVrMeleeTrace(Server_BaseEntity* player, int playerIndex, int commandsInPacket)
    {
        if (!player || !IsValidPlayerIndex(playerIndex))
            return;

        PlayerVrState& vrPlayer = g_players[playerIndex];
        if (!vrPlayer.isUsingVR || !vrPlayer.isMeleeing)
        {
            vrPlayer.isNewSwing = true;
            vrPlayer.currentMeleeSwingAllowed = false;
            return;
        }

        Server_WeaponCSBase* weapon = SafeGetActiveWeapon(player);
        if (!weapon || SafeGetWeaponId(weapon) != kWeaponMelee)
        {
            vrPlayer.currentMeleeSwingAllowed = false;
            return;
        }

        const uint64_t nowMs = GetTickCount64();
        if (vrPlayer.isNewSwing)
        {
            vrPlayer.isNewSwing = false;
            vrPlayer.currentMeleeSwingAllowed = CanStartMeleeSwing(player, playerIndex, vrPlayer);
            if (!vrPlayer.currentMeleeSwingAllowed)
                return;

            if (!ResetMeleeHitList(weapon))
            {
                vrPlayer.currentMeleeSwingAllowed = false;
                return;
            }
            vrPlayer.swingStartMs = nowMs;
        }

        if (!vrPlayer.currentMeleeSwingAllowed)
            return;

        // A swing deals damage for a limited time only (S14).
        const auto maxSwingMs = static_cast<uint64_t>((std::max)(0.0f, g_shared.meleeMaxSwingSeconds) * 1000.0f);
        if (maxSwingMs > 0 && nowMs >= vrPlayer.swingStartMs && nowMs - vrPlayer.swingStartMs > maxSwingMs)
        {
            vrPlayer.currentMeleeSwingAllowed = false;
            return;
        }

        if ((g_config.meleeSwingBlockWhileControlled && IsServerPlayerControlledBySI(player)) ||
            (g_config.meleeSwingBlockWhileIncapacitated && IsServerPlayerIncapacitated(player)))
        {
            vrPlayer.currentMeleeSwingAllowed = false;
            if (MeleeLogAllowed(vrPlayer, playerIndex))
                Log("melee swing stopped player=%d reason=state-changed", playerIndex);
            return;
        }

        void* meleeInfo = SafeGetMeleeWeaponInfo(weapon);
        Vector initialForward, initialRight, initialUp;
        QAngle::AngleVectors(vrPlayer.prevControllerAngle, &initialForward, &initialRight, &initialUp);
        Vector initialMeleeDirection = VectorRotate(initialForward, initialRight, 50.0f);
        VectorNormalize(initialMeleeDirection);

        Vector finalForward, finalRight, finalUp;
        QAngle::AngleVectors(vrPlayer.controllerAngle, &finalForward, &finalRight, &finalUp);
        Vector finalMeleeDirection = VectorRotate(finalForward, finalRight, 50.0f);
        VectorNormalize(finalMeleeDirection);

        Vector pivot;
        CrossProduct(initialMeleeDirection, finalMeleeDirection, pivot);
        if (VectorNormalize(pivot) <= 0.0001f)
            return;

        const float dot = std::clamp(DotProduct(initialMeleeDirection, finalMeleeDirection), -1.0f, 1.0f);
        const float swingAngle = acosf(dot) * 180.0f / 3.14159265f;
        if (!std::isfinite(swingAngle) || swingAngle <= 0.01f)
            return;

        // The hand must really move (S14): this packet's motion over its commands.
        const float packetSeconds = (std::max)(1, commandsInPacket) / 30.0f;
        const float moved = vrPlayer.havePrevControllerPose ? (vrPlayer.controllerPos - vrPlayer.prevControllerPos).Length() : 0.0f;
        if (!l4d2vr_server::MeleeMotionSufficient(moved, swingAngle, packetSeconds, g_shared))
            return;

        g_getPrimaryAttackActivity.original(weapon, meleeInfo);

        g_performingMeleeTrace = true;
        Vector traceDirection = initialMeleeDirection;
        constexpr int kNumTraces = 10;
        const float traceAngle = swingAngle / static_cast<float>(kNumTraces);
        for (int i = 0; i < kNumTraces; ++i)
        {
            traceDirection = VectorRotate(traceDirection, pivot, traceAngle);
            g_testMeleeSwingServer.original(weapon, traceDirection);
        }
        g_performingMeleeTrace = false;
    }

    float __fastcall DetourProcessUsercmds(void* ecx, void* edx, edict_t* player, void* buf, int numcmds, int totalcmds, int droppedPackets, bool ignore, bool paused)
    {

        const int previousIndex = g_currentUsercmdPlayerIndex;
        Server_BaseEntity* const previousPlayer = g_currentUsercmdPlayer;
        IServerUnknown* const previousUnknown = g_currentUsercmdUnknown;

        g_currentUsercmdPlayerIndex = GetPlayerIndexFromEdict(player);
        g_currentUsercmdUnknown = player ? player->m_pUnk : nullptr;
        g_currentUsercmdPlayer = GetBaseEntityFromEdict(player);

        const float result = g_processUsercmds.original(ecx, player, buf, numcmds, totalcmds, droppedPackets, ignore, paused);

        // The melee sweep runs once per packet on the newest decoded pose.
        ApplyVrMeleeTrace(g_currentUsercmdPlayer, g_currentUsercmdPlayerIndex, numcmds);

        if (IsValidPlayerIndex(g_currentUsercmdPlayerIndex))
        {
            PlayerVrState& vrPlayer = g_players[g_currentUsercmdPlayerIndex];
            vrPlayer.prevControllerAngle = vrPlayer.controllerAngle;
            vrPlayer.prevControllerPos = vrPlayer.controllerPos;
            vrPlayer.havePrevControllerPose = vrPlayer.isUsingVR;
        }

        g_currentUsercmdPlayerIndex = previousIndex;
        g_currentUsercmdPlayer = previousPlayer;
        g_currentUsercmdUnknown = previousUnknown;
        return result;
    }

    int __fastcall DetourTestMeleeSwingCollisionServer(void* ecx, void* edx, Vector const& direction)
    {
        return g_testMeleeSwingServer.original(ecx, direction);
    }

    int __fastcall DetourGetPrimaryAttackActivity(void* ecx, void* edx, void* meleeInfo)
    {
        return g_getPrimaryAttackActivity.original(ecx, meleeInfo);
    }

    Vector* __fastcall DetourEyePosition(void* ecx, void* edx, Vector* eyePos)
    {
        Vector controllerOrigin;
        QAngle controllerAngles;
        int overrideReason = 0;
        if (eyePos && TryGetServerControllerAimOverride(ecx, controllerOrigin, controllerAngles, overrideReason))
        {
            *eyePos = controllerOrigin;
            return eyePos;
        }

        Vector* result = g_eyePosition.original(ecx, eyePos);
        if (g_performingMeleeTrace && result && ecx == g_currentUsercmdPlayer &&
            IsValidPlayerIndex(g_currentUsercmdPlayerIndex))
            *result = g_players[g_currentUsercmdPlayerIndex].controllerPos;

        return result;
    }

    Vector* __fastcall DetourServerPlayerEyePosition(void* ecx, void* edx, Vector* eyePos)
    {
        Vector controllerOrigin;
        QAngle controllerAngles;
        int overrideReason = 0;
        if (eyePos && TryGetServerControllerAimOverride(ecx, controllerOrigin, controllerAngles, overrideReason))
        {
            *eyePos = controllerOrigin;
            return eyePos;
        }

        return g_serverPlayerEyePosition.original(ecx, eyePos);
    }

    const QAngle* __fastcall DetourServerPlayerEyeAngles(void* ecx, void* edx)
    {
        Vector controllerOrigin;
        QAngle controllerAngles;
        int overrideReason = 0;
        if (TryGetServerControllerAimOverride(ecx, controllerOrigin, controllerAngles, overrideReason))
        {
            g_returnEyeAngles = controllerAngles;
            NormalizeAndClampViewAngles(g_returnEyeAngles);
            return &g_returnEyeAngles;
        }

        return g_serverPlayerEyeAngles.original(ecx);
    }

    void __fastcall DetourPlayerUse(void* ecx, void* edx, void* useEntity)
    {
        Vector controllerOrigin;
        QAngle controllerAngles;
        if (TryBuildServerUseControllerPose(ecx, controllerOrigin, controllerAngles))
        {
            ScopedServerUseControllerAimOverride useAim(ecx, controllerOrigin, controllerAngles);
            g_playerUse.original(ecx, useEntity);
            return;
        }

        g_playerUse.original(ecx, useEntity);
    }

    Server_BaseEntity* __fastcall DetourFindUseEntity(void* ecx, void* edx, float radius, float dotLimit, float defaultDotLimit, void* traceResult, void* extra)
    {
        Vector controllerOrigin;
        QAngle controllerAngles;
        if (TryBuildServerUseControllerPose(ecx, controllerOrigin, controllerAngles))
        {
            ScopedServerUseControllerAimOverride useAim(ecx, controllerOrigin, controllerAngles);
            return g_findUseEntity.original(ecx, radius, dotLimit, defaultDotLimit, traceResult, extra);
        }

        return g_findUseEntity.original(ecx, radius, dotLimit, defaultDotLimit, traceResult, extra);
    }

    bool WaitForModule(const char* moduleName, DWORD timeoutMs)
    {
        const DWORD start = GetTickCount();
        while (!g_unloading.load())
        {
            if (GetModuleHandleA(moduleName))
                return true;

            if (GetTickCount() - start >= timeoutMs)
                return false;

            Sleep(100);
        }

        return false;
    }

    bool InstallHooks()
    {
        bool expected = false;
        if (!g_hooksInstalled.compare_exchange_strong(expected, true))
            return true;

        if (!WaitForModule("server.dll", 60000))
        {
            Log("server.dll did not load before timeout; dedicated hooks not installed");
            g_hooksInstalled.store(false);
            return false;
        }

        if (!g_offsets.ResolveAll())
        {
            Log("dedicated offset resolution failed; hooks not installed");
            g_hooksInstalled.store(false);
            return false;
        }

        const MH_STATUS initStatus = MH_Initialize();
        if (initStatus != MH_OK && initStatus != MH_ERROR_ALREADY_INITIALIZED)
        {
            Log("MH_Initialize failed: %d", static_cast<int>(initStatus));
            g_hooksInstalled.store(false);
            return false;
        }

        bool ok = true;
        ok &= g_serverFireTerrorBullets.Create(reinterpret_cast<void*>(g_offsets.ServerFireTerrorBullets.address), reinterpret_cast<void*>(&DetourServerFireTerrorBullets), "ServerFireTerrorBullets");
        ok &= g_processUsercmds.Create(reinterpret_cast<void*>(g_offsets.ProcessUsercmds.address), reinterpret_cast<void*>(&DetourProcessUsercmds), "ProcessUsercmds");
        ok &= g_readUsercmd.Create(reinterpret_cast<void*>(g_offsets.ReadUserCmd.address), reinterpret_cast<void*>(&DetourReadUsercmd), "ReadUsercmd");
        ok &= g_testMeleeSwingServer.Create(reinterpret_cast<void*>(g_offsets.TestMeleeSwingServer.address), reinterpret_cast<void*>(&DetourTestMeleeSwingCollisionServer), "TestMeleeSwingServer");
        ok &= g_getPrimaryAttackActivity.Create(reinterpret_cast<void*>(g_offsets.GetPrimaryAttackActivity.address), reinterpret_cast<void*>(&DetourGetPrimaryAttackActivity), "GetPrimaryAttackActivity");
        ok &= g_eyePosition.Create(reinterpret_cast<void*>(g_offsets.EyePosition.address), reinterpret_cast<void*>(&DetourEyePosition), "EyePosition");
        ok &= g_serverPlayerEyePosition.Create(reinterpret_cast<void*>(g_offsets.ServerPlayerEyePosition.address), reinterpret_cast<void*>(&DetourServerPlayerEyePosition), "ServerPlayerEyePosition", false);
        ok &= g_serverPlayerEyeAngles.Create(reinterpret_cast<void*>(g_offsets.ServerPlayerEyeAngles.address), reinterpret_cast<void*>(&DetourServerPlayerEyeAngles), "ServerPlayerEyeAngles", false);
        ok &= g_findUseEntity.Create(reinterpret_cast<void*>(g_offsets.FindUseEntity.address), reinterpret_cast<void*>(&DetourFindUseEntity), "FindUseEntity", false);
        ok &= g_playerUse.Create(reinterpret_cast<void*>(g_offsets.PlayerUse.address), reinterpret_cast<void*>(&DetourPlayerUse), "PlayerUse", false);

        if (!ok)
        {
            Log("required hook creation failed; rolling back");
            MH_Uninitialize();
            g_hooksInstalled.store(false);
            return false;
        }

        const MH_STATUS enableStatus = MH_EnableHook(MH_ALL_HOOKS);
        if (enableStatus != MH_OK)
        {
            Log("MH_EnableHook failed: %d", static_cast<int>(enableStatus));
            MH_Uninitialize();
            g_hooksInstalled.store(false);
            return false;
        }

        Log("L4D2VR dedicated server hooks installed");
        return true;
    }

    void UninstallHooks()
    {
        if (!g_hooksInstalled.exchange(false))
            return;

        MH_DisableHook(MH_ALL_HOOKS);
        MH_Uninitialize();
        Log("L4D2VR dedicated server hooks uninstalled");
    }

    using CreateInterfaceFn = void* (*)(const char* name, int* returnCode);
    using QueryCvarCookie_t = int;

    enum
    {
        IFACE_OK = 0,
        IFACE_FAILED = 1,
    };

    enum PLUGIN_RESULT
    {
        PLUGIN_CONTINUE = 0,
        PLUGIN_OVERRIDE = 1,
        PLUGIN_STOP = 2,
    };

    enum EQueryCvarValueStatus
    {
        eQueryCvarValueStatus_ValueIntact = 0,
        eQueryCvarValueStatus_CvarNotFound = 1,
        eQueryCvarValueStatus_NotACvar = 2,
        eQueryCvarValueStatus_CvarProtected = 3,
    };

    class IServerPluginCallbacks
    {
    public:
        virtual bool Load(CreateInterfaceFn interfaceFactory, CreateInterfaceFn gameServerFactory) = 0;
        virtual void Unload() = 0;
        virtual void Pause() = 0;
        virtual void UnPause() = 0;
        virtual const char* GetPluginDescription() = 0;
        virtual void LevelInit(const char* mapName) = 0;
        virtual void ServerActivate(edict_t* edictList, int edictCount, int clientMax) = 0;
        virtual void GameFrame(bool simulating) = 0;
        virtual void LevelShutdown() = 0;
        virtual void ClientActive(edict_t* entity) = 0;
        virtual void ClientDisconnect(edict_t* entity) = 0;
        virtual void ClientPutInServer(edict_t* entity, const char* playerName) = 0;
        virtual void SetCommandClient(int index) = 0;
        virtual void ClientSettingsChanged(edict_t* edict) = 0;
        virtual PLUGIN_RESULT ClientConnect(bool* allowConnect, edict_t* entity, const char* name, const char* address, char* reject, int maxRejectLen) = 0;
        virtual PLUGIN_RESULT ClientCommand(edict_t* entity, const CCommand& args) = 0;
        virtual PLUGIN_RESULT NetworkIDValidated(const char* userName, const char* networkId) = 0;
        virtual void OnQueryCvarValueFinished(QueryCvarCookie_t cookie, edict_t* playerEntity, EQueryCvarValueStatus status, const char* cvarName, const char* cvarValue) = 0;
        virtual void OnEdictAllocated(edict_t* edict) = 0;
        virtual void OnEdictFreed(const edict_t* edict) = 0;
    };

    void PublishCapabilities()
    {
        // This shell decodes VR usercmds (server_ack 4) and relays poses (v2 +
        // v3). It implements none of the newer per-feature server paths, so
        // only PoseV3 is advertised; see l4d2vr_server_linux.cpp for those.
        l4d2vr_server::SessionCapabilities caps{};
        caps.usercmdDecoder = g_hooksInstalled.load() && g_readUsercmd.original != nullptr;
        caps.interaction = false;
        caps.features = l4d2vr_server::kFeatureOpusVoice;
        // With the decoder: hand-origin bullets (AkimboRays), both pitch
        // packings (ExactPitch) and the view yaw v3 -> v2 needs (PoseV3).
        if (caps.usercmdDecoder)
            caps.features |= l4d2vr_server::kFeaturePoseV3 | l4d2vr_server::kFeatureAkimboRays |
                l4d2vr_server::kFeatureExactPitch;
        caps.features &= g_shared.featureMask;
        g_session.SetCapabilities(caps);
        Log("capabilities: usercmd_decoder=%d interaction=0 features=0x%03X",
            caps.usercmdDecoder ? 1 : 0, static_cast<unsigned>(caps.features));
    }

    class L4D2VRServerPlugin final : public IServerPluginCallbacks
    {
    public:
        bool Load(CreateInterfaceFn interfaceFactory, CreateInterfaceFn) override
        {
            ResolveServerFilePaths();
            ResetLogFile();
            LoadServerConfig();

            g_unloading.store(false);
            g_engineTraceServer = nullptr;
            g_engineServer = nullptr;
            g_voiceServer = nullptr;
            g_edictList = nullptr;
            g_edictCount = 0;
            g_clientMax = l4d2vr_server::kMaxPlayerSlots;
            g_session.ResetAll();
            g_session.SetClientMax(g_clientMax);
            g_session.SeedTokens(static_cast<uint32_t>(GetTickCount64()) ^ (GetCurrentProcessId() << 16));
            if (interfaceFactory)
            {
                int returnCode = IFACE_FAILED;
                g_engineServer = static_cast<IVEngineServer*>(interfaceFactory("VEngineServer022", &returnCode));
                g_engineTraceServer = static_cast<IEngineTrace*>(interfaceFactory("EngineTraceServer003", &returnCode));
                g_voiceServer = static_cast<IVoiceServer*>(interfaceFactory("VoiceServer002", &returnCode));
            }

            if (!g_engineServer)
            {
                Log("VEngineServer022 unavailable");
                return false;
            }
            // server.dll is loaded before plugins: install synchronously.
            InstallHooks();
            PublishCapabilities();
            Log("L4D2VR dedicated server plugin loaded (pose relay v2+v3)");
            return true;
        }

        void Unload() override
        {
            g_unloading.store(true);
            UninstallHooks();
            g_session.ResetAll();
            g_engineServer = nullptr;
            g_edictList = nullptr;
            g_edictCount = 0;
            Log("L4D2VR dedicated server plugin unloaded");
        }

        void Pause() override {}
        void UnPause() override {}

        const char* GetPluginDescription() override
        {
            return "L4D2VR dedicated server: VR usercmd decoder + body/hand/finger pose relay (v2/v3)";
        }

        void LevelInit(const char*) override
        {
            LoadServerConfig();
            for (PlayerVrState& player : g_players)
                player = PlayerVrState{};
            g_session.ResetAll();
            g_edictList = nullptr;
            g_edictCount = 0;
        }

        void ServerActivate(edict_t* edictList, int edictCount, int clientMax) override
        {
            g_clientMax = (clientMax >= 1 && clientMax <= l4d2vr_server::kMaxPlayerSlots)
                ? clientMax : l4d2vr_server::kMaxPlayerSlots;
            g_edictList = edictList;
            g_edictCount = edictCount > 0 ? edictCount : 0;
            g_session.SetClientMax(g_clientMax);
        }

        void GameFrame(bool) override
        {
            g_session.GameFrame();
        }

        void LevelShutdown() override
        {
            g_session.ResetAll();
            g_session.SetClientMax(l4d2vr_server::kMaxPlayerSlots);
            g_clientMax = l4d2vr_server::kMaxPlayerSlots;
            g_edictList = nullptr;
            g_edictCount = 0;
        }

        void ClientActive(edict_t* entity) override
        {
            const int playerIndex = GetPlayerIndexFromEdict(entity);
            if (playerIndex >= 0)
                g_session.ClientActive(playerIndex, entity);
        }

        void ClientDisconnect(edict_t* entity) override
        {
            const int playerIndex = GetPlayerIndexFromEdict(entity);
            if (playerIndex < 0)
                return;
            g_players[playerIndex] = PlayerVrState{};
            g_session.ClientDisconnect(playerIndex, entity);
        }

        void ClientPutInServer(edict_t* entity, const char*) override
        {
            const int playerIndex = GetPlayerIndexFromEdict(entity);
            if (playerIndex < 0)
                return;
            g_players[playerIndex] = PlayerVrState{};
            g_session.ClientPutInServer(playerIndex, entity);
        }

        void SetCommandClient(int) override {}
        void ClientSettingsChanged(edict_t*) override {}
        PLUGIN_RESULT ClientConnect(bool*, edict_t*, const char*, const char*, char*, int) override { return PLUGIN_CONTINUE; }

        PLUGIN_RESULT ClientCommand(edict_t* entity, const CCommand& args) override
        {
            const int argc = (std::min)(args.ArgC(), 16);
            const char* argv[16] = {};
            for (int i = 0; i < argc; ++i)
                argv[i] = args.Arg(i);
            if (argc > 0 && args.ArgC() > 16)
                return std::strncmp(argv[0], "l4d2vr_", 7) == 0 ? PLUGIN_STOP : PLUGIN_CONTINUE;
            const auto result = g_session.ClientCommand(GetPlayerIndexFromEdict(entity), entity, argc, argv, nullptr);
            return result == l4d2vr_server::CommandResult::Stop ? PLUGIN_STOP : PLUGIN_CONTINUE;
        }

        PLUGIN_RESULT NetworkIDValidated(const char*, const char*) override { return PLUGIN_CONTINUE; }
        void OnQueryCvarValueFinished(QueryCvarCookie_t, edict_t*, EQueryCvarValueStatus, const char*, const char*) override {}
        void OnEdictAllocated(edict_t*) override {}

        void OnEdictFreed(const edict_t* entity) override
        {
            const int playerIndex = GetPlayerIndexFromEdict(entity);
            if (playerIndex >= 0)
                g_session.EdictFreed(playerIndex, entity);
        }
    };

    L4D2VRServerPlugin g_plugin;
}

extern "C" __declspec(dllexport) void* CreateInterface(const char* name, int* returnCode)
{
    if (name && std::strcmp(name, "ISERVERPLUGINCALLBACKS003") == 0)
    {
        if (returnCode)
            *returnCode = IFACE_OK;
        return &g_plugin;
    }

    if (returnCode)
        *returnCode = IFACE_FAILED;
    return nullptr;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_module = module;
        DisableThreadLibraryCalls(module);
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        g_unloading.store(true);
    }

    return TRUE;
}

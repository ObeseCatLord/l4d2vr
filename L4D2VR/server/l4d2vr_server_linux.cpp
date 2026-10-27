// L4D2VR dedicated-server plugin for Linux i386 srcds (ISERVERPLUGINCALLBACKS003).
//
// One plugin provides:
//   * the proven protocol-v2 pose relay (byte-identical behaviour) plus pose
//     protocol v3 relay with downconversion for v2 peers;
//   * server-side fidelity for VR players: usercmd decode/strip, controller-ray
//     bullets with per-command (akimbo) poses, physical melee sweep, eye
//     overrides (throw grace, use/grip, mounted guns, shove, pill give), manual
//     throw velocity, PR409 magazine/shell transactions, holster/inventory drops
//     and instant deploy.
// Desktop players (commands without the VR flag) are never modified.
//
// Every engine address comes from the on-disk symbol table of the loaded
// server_srv.so (local .symtab symbols included) plus the runtime load base,
// is checked against a recorded prologue pattern (l4d2vr_linux_targets.h) and
// disables only its own features on mismatch. Network-property offsets come
// from walking the game's SendTables and are cross-checked against offsets in
// accessor code. Portable logic lives in L4D2VR/server_core.

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <link.h>
#include <sys/mman.h>
#include <unistd.h>

#include "../server_core/vr_elf_image.h"
#include "../server_core/vr_server_exec.h"
#include "../server_core/vr_server_limits.h"
#include "../server_core/vr_server_player.h"
#include "../server_core/vr_server_protocol.h"
#include "../server_core/vr_server_session.h"
#include "../server_core/vr_usercmd_codec.h"
#include "../server_core/vr_x86_prologue.h"
#include "l4d2vr_libstdcxx_shims.h"
#include "l4d2vr_linux_targets.h"

#define L4D2VR_EXPORT __attribute__((visibility("default")))

// ---- Minimal Source ABI (32-bit Linux, L4D2) --------------------------------
// No virtual destructors on engine interfaces: that would shift every slot.
using CreateInterfaceFn = void* (*)(const char* name, int* returnCode);
using QueryCvarCookie_t = int;
enum { IFACE_OK = 0, IFACE_FAILED = 1 };
enum PLUGIN_RESULT { PLUGIN_CONTINUE = 0, PLUGIN_OVERRIDE = 1, PLUGIN_STOP = 2 };
enum EQueryCvarValueStatus
{
    eQueryCvarValueStatus_ValueIntact = 0,
    eQueryCvarValueStatus_CvarNotFound = 1,
    eQueryCvarValueStatus_NotACvar = 2,
    eQueryCvarValueStatus_CvarProtected = 3,
};

struct Vector { float x, y, z; };
struct QAngle { float x, y, z; };

// Linux CBaseEdict: four 32-bit fields, no cached index.
struct edict_t
{
    int m_fStateFlags;
    int m_NetworkSerialNumber;
    void* m_pNetworkable;
    void* m_pUnk;
};

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
    virtual void* SaveAllocMemory(std::size_t, std::size_t) = 0;
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
    virtual PLUGIN_RESULT ClientConnect(bool* allowConnect, edict_t* entity, const char* name, const char* address,
        char* reject, int maxRejectLength) = 0;
    virtual PLUGIN_RESULT ClientCommand(edict_t* entity, const CCommand& args) = 0;
    virtual PLUGIN_RESULT NetworkIDValidated(const char* userName, const char* networkId) = 0;
    virtual void OnQueryCvarValueFinished(QueryCvarCookie_t cookie, edict_t* playerEntity, EQueryCvarValueStatus status,
        const char* cvarName, const char* cvarValue) = 0;
    virtual void OnEdictAllocated(edict_t* edict) = 0;
    virtual void OnEdictFreed(const edict_t* edict) = 0;
};

// CUserCmd as laid out by server_srv.so's ReadUsercmd (vptr + fields, 0x58).
struct CUserCmdLinux
{
    void* vtable;
    int command_number;
    int tick_count;
    float viewangles[3];
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
    float headangles[3];
    float headoffset[3];
};

static_assert(sizeof(void*) == 4, "Build the L4D2VR server plugin as 32-bit x86.");
static_assert(sizeof(edict_t) == 16, "Unexpected 32-bit Linux Source edict layout.");
static_assert(sizeof(CCommand) == 1288, "Unexpected 32-bit Source CCommand layout.");
static_assert(sizeof(CUserCmdLinux) == 0x58, "Unexpected Linux CUserCmd layout.");
static_assert(offsetof(CUserCmdLinux, buttons) == 0x24 && offsetof(CUserCmdLinux, impulse) == 0x28 &&
    offsetof(CUserCmdLinux, mousedx) == 0x38, "Unexpected Linux CUserCmd field offsets.");
static_assert(sizeof(Vector) == 12 && sizeof(QAngle) == 12, "Unexpected vector layout.");

namespace
{
    using namespace l4d2vr_server;

    constexpr char kPluginInterfaceName[] = "ISERVERPLUGINCALLBACKS003";
    constexpr int kSurvivorTeam = 2;
    constexpr std::uint32_t kEntityEdictOffset = 0x30u;   // CBaseEntity::entindex (guarded)
    constexpr std::uint32_t kGlobalsEdictsOffset = 0x58u; // CBaseEntity::entindex (guarded)
    constexpr std::uint32_t kGlobalsCurtime = 0x0Cu;      // CTerrorMeleeWeapon::GetPrimaryAttackActivity
    constexpr std::uint32_t kGlobalsMaxClients = 0x14u;
    constexpr std::uint32_t kGlobalsInterval = 0x1Cu;

    // ---- Logging ---------------------------------------------------------------
    void Log(const char* format, ...) __attribute__((format(printf, 1, 2)));
    void Log(const char* format, ...)
    {
        char message[1024];
        va_list args;
        va_start(args, format);
        std::vsnprintf(message, sizeof(message), format, args);
        va_end(args);
        std::fprintf(stderr, "L4D2VR server: %s\n", message);
    }

    std::uint64_t SteadyMs()
    {
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    }

    std::uint64_t SteadyUs()
    {
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    }

    Vec3 ToVec(const Vector& v) { return { v.x, v.y, v.z }; }
    Vector ToVector(const Vec3& v) { return { v.x, v.y, v.z }; }
    QAngle ToQAngle(const Angles& a) { return { a.x, a.y, a.z }; }

    // ---- Server module ---------------------------------------------------------
    struct Range { std::uintptr_t begin, end; };

    struct ServerModule
    {
        bool found = false;
        std::uintptr_t base = 0u;
        std::string path;
        std::vector<Range> ranges;

        bool Contains(std::uintptr_t address, std::size_t size) const
        {
            for (const Range& r : ranges)
                if (address >= r.begin && address < r.end && size <= r.end - address) return true;
            return false;
        }
    };

    ServerModule g_Module;
    l4d2vr_elf::Image g_Image;

    int FindModuleCallback(dl_phdr_info* info, std::size_t, void* data)
    {
        auto* out = static_cast<ServerModule*>(data);
        if (!info || !info->dlpi_name) return 0;
        const std::string path(info->dlpi_name);
        const auto slash = path.find_last_of('/');
        const std::string file = slash == std::string::npos ? path : path.substr(slash + 1);
        if (file != "server_srv.so" && file != "server.so") return 0;
        out->found = true;
        out->base = static_cast<std::uintptr_t>(info->dlpi_addr);
        out->path = path;
        for (int i = 0; i < info->dlpi_phnum; ++i)
        {
            const auto& ph = info->dlpi_phdr[i];
            if (ph.p_type != PT_LOAD) continue;
            out->ranges.push_back({ out->base + ph.p_vaddr, out->base + ph.p_vaddr + ph.p_memsz });
        }
        return 1;
    }

    template <class T> bool ReadMem(std::uintptr_t address, T& out)
    {
        if (!g_Module.Contains(address, sizeof(T))) return false;
        std::memcpy(&out, reinterpret_cast<const void*>(address), sizeof(T));
        return true;
    }

    class RuntimeView final : public l4d2vr_linux::CodeView
    {
    public:
        bool Symbol(const char* name, std::uint32_t& address, std::uint32_t& size) const override
        {
            l4d2vr_elf::Symbol s{};
            if (!g_Image.Resolve(name, s)) return false;
            address = static_cast<std::uint32_t>(g_Module.base + s.value);
            size = s.size;
            return true;
        }
        bool Read(std::uint32_t address, void* out, std::size_t size) const override
        {
            if (!g_Module.Contains(address, size)) return false;
            std::memcpy(out, reinterpret_cast<const void*>(static_cast<std::uintptr_t>(address)), size);
            return true;
        }
        void ForEachVtable(void (*fn)(void*, const char*, std::uint32_t, std::uint32_t), void* ctx) const override
        {
            g_Image.ForEachSymbol([&](const std::string& name, const l4d2vr_elf::Symbol& s)
            {
                if (name.compare(0, 4, "_ZTV") == 0 && s.type == l4d2vr_elf::kSttObject && s.size >= 12u && s.duplicates == 0)
                    fn(ctx, name.c_str(), static_cast<std::uint32_t>(g_Module.base + s.value), s.size);
            });
        }
    };

    // ---- Memory patching ---------------------------------------------------------
    int CurrentProtection(std::uintptr_t address)
    {
        FILE* maps = std::fopen("/proc/self/maps", "r");
        if (!maps) return -1;
        char line[512];
        int prot = -1;
        while (std::fgets(line, sizeof(line), maps))
        {
            unsigned long begin = 0, end = 0;
            char perms[8] = {};
            if (std::sscanf(line, "%lx-%lx %7s", &begin, &end, perms) != 3) continue;
            if (address < begin || address >= end) continue;
            prot = (perms[0] == 'r' ? PROT_READ : 0) | (perms[1] == 'w' ? PROT_WRITE : 0) | (perms[2] == 'x' ? PROT_EXEC : 0);
            break;
        }
        std::fclose(maps);
        return prot;
    }

    bool PatchBytes(std::uintptr_t address, const void* bytes, std::size_t size)
    {
        const long page = sysconf(_SC_PAGESIZE);
        if (page <= 0 || !g_Module.Contains(address, size)) return false;
        const std::uintptr_t mask = ~(static_cast<std::uintptr_t>(page) - 1u);
        const std::uintptr_t first = address & mask;
        const std::uintptr_t last = (address + size - 1u) & mask;
        const int protFirst = CurrentProtection(first);
        const int protLast = CurrentProtection(last);
        if (protFirst < 0 || protLast < 0) return false;
        const std::size_t length = last - first + static_cast<std::size_t>(page);
        if (mprotect(reinterpret_cast<void*>(first), length, protFirst | protLast | PROT_READ | PROT_WRITE) != 0)
            return false;
        std::memcpy(reinterpret_cast<void*>(address), bytes, size);
        __builtin___clear_cache(reinterpret_cast<char*>(address), reinterpret_cast<char*>(address + size));
        mprotect(reinterpret_cast<void*>(first), static_cast<std::size_t>(page), protFirst);
        if (last != first) mprotect(reinterpret_cast<void*>(last), static_cast<std::size_t>(page), protLast);
        return true;
    }

    struct InlineHook
    {
        const char* name = nullptr;
        std::uintptr_t target = 0u;
        std::uint8_t saved[16]{};
        std::size_t stolen = 0u;
        void* trampoline = nullptr;
        bool active = false;

        bool Install(const char* label, std::uintptr_t address, void* detour)
        {
            name = label;
            target = address;
            std::uint8_t code[32]{};
            if (!g_Module.Contains(address, sizeof(code))) return false;
            std::memcpy(code, reinterpret_cast<const void*>(address), sizeof(code));
            const l4d2vr_x86::Plan plan = l4d2vr_x86::PlanHook(code, sizeof(code), 5u);
            if (!plan.ok || plan.stolen > sizeof(saved))
            {
                Log("inline hook %s refused: %s", label, plan.error.c_str());
                return false;
            }
            void* page = mmap(nullptr, 64u, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (page == MAP_FAILED) return false;
            auto* tramp = static_cast<std::uint8_t*>(page);
            if (!l4d2vr_x86::BuildTrampoline(code, static_cast<std::uint32_t>(address), plan, tramp,
                    static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(tramp))) ||
                mprotect(page, 64u, PROT_READ | PROT_EXEC) != 0)
            {
                munmap(page, 64u);
                return false;
            }
            std::uint8_t patch[16];
            std::memset(patch, 0x90, sizeof(patch));
            patch[0] = 0xE9u;
            const std::int32_t rel = static_cast<std::int32_t>(reinterpret_cast<std::uintptr_t>(detour) - (address + 5u));
            std::memcpy(patch + 1, &rel, 4);
            std::memcpy(saved, code, plan.stolen);
            stolen = plan.stolen;
            if (!PatchBytes(address, patch, plan.stolen))
            {
                munmap(page, 64u);
                return false;
            }
            trampoline = page;
            active = true;
            return true;
        }

        void Remove()
        {
            if (!active) return;
            PatchBytes(target, saved, stolen);
            active = false;
            // The trampoline page is intentionally kept: a caller that entered it
            // just before removal must still find valid code.
        }
    };

    struct SlotPatch { std::uintptr_t slot; std::uint32_t original; };

    struct VtableHook
    {
        const char* name = nullptr;
        std::uint32_t original = 0u;
        std::vector<SlotPatch> patches;
        bool active = false;

        bool Install(const char* label, const std::vector<l4d2vr_linux::SlotRef>& slots, std::uint32_t expected, void* detour)
        {
            name = label;
            original = expected;
            const auto replacement = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(detour));
            for (const auto& slot : slots)
            {
                std::uint32_t current = 0u;
                if (!ReadMem(slot.slotAddress, current) || current != expected) continue;
                if (!PatchBytes(slot.slotAddress, &replacement, sizeof(replacement))) continue;
                patches.push_back({ slot.slotAddress, current });
            }
            active = !patches.empty() && patches.size() == slots.size();
            if (!active) Remove();
            return active;
        }

        void Remove()
        {
            for (const SlotPatch& p : patches)
                PatchBytes(p.slot, &p.original, sizeof(p.original));
            patches.clear();
            active = false;
        }
    };

    // ---- Native function types (i386 SysV; `this` is the first stack argument) ----
    using FnReadUsercmd = void (*)(void* buf, CUserCmdLinux* move, CUserCmdLinux* from);
    using FnFireTerrorBullets = int (*)(int, const Vector&, const QAngle&, int, int, int, float);
    using FnProcessUsercmds = float (*)(void*, edict_t*, void*, int, int, int, bool, bool);
    using FnPlayerRunCommand = void (*)(void*, CUserCmdLinux*, void*);
    using FnEyePosition = Vector (*)(void*);           // struct return: hidden pointer, callee pops it
    using FnEyeAngles = const QAngle* (*)(void*);
    using FnShootPosition = Vector (*)(void*);
    using FnReload = bool (*)(void*);
    using FnCreate = void* (*)(const Vector&, const QAngle&, const Vector&, const Vector&, void*);
    using FnCreatePipe = void* (*)(const Vector&, const QAngle&, const Vector&, const Vector&, void*, float);
    using FnEntity = void* (*)(void*);
    using FnInt = int (*)(void*);
    using FnBool = bool (*)(void*);
    using FnAbsOrigin = const Vector* (*)(void*);
    using FnTestMelee = int (*)(void*, const Vector&);
    using FnPrimaryActivity = int (*)(void*, void*);
    using FnMeleeInfo = void* (*)(void*, int);
    using FnWeaponDrop = void (*)(void*, void*, const Vector*, const Vector*);
    using FnAmmoCount = int (*)(void*, int);
    using FnAmmoDef = void* (*)();
    using FnInfinite = bool (*)(void*, int);
    using FnChangeAccessor = std::uint16_t* (*)(edict_t*);

    struct Natives
    {
        FnReadUsercmd readUsercmd = nullptr;
        FnFireTerrorBullets fireBullets = nullptr;
        FnProcessUsercmds processUsercmds = nullptr;
        FnPlayerRunCommand playerRunCommand = nullptr;
        FnEyePosition eyePosition = nullptr;
        FnEyeAngles eyeAngles = nullptr;
        FnShootPosition shootPosition = nullptr;
        FnReload gunReload = nullptr;
        FnReload shotgunReload = nullptr;
        FnCreate molotovCreate = nullptr;
        FnCreatePipe pipeCreate = nullptr;
        FnCreate vomitCreate = nullptr;
        FnEntity getActiveWeapon = nullptr;
        FnInt entindex = nullptr;
        FnInt teamNumber = nullptr;
        FnBool isAlive = nullptr;
        FnBool isIncapacitated = nullptr;
        FnAbsOrigin absOrigin = nullptr;
        FnTestMelee testMelee = nullptr;
        FnPrimaryActivity primaryActivity = nullptr;
        FnMeleeInfo meleeInfo = nullptr;
        void* meleeStore = nullptr;
        FnWeaponDrop weaponDrop = nullptr;
        FnInt clip1 = nullptr;
        FnEntity getOwner = nullptr;
        FnBool isDualWielding = nullptr;
        FnAmmoCount ammoCount = nullptr;
        FnAmmoDef ammoDef = nullptr;
        FnInfinite infiniteAmmo = nullptr;
        FnChangeAccessor changeAccessor = nullptr;
        std::uintptr_t gpGlobalsVar = 0u;
        std::uintptr_t serverClassHeadVar = 0u;
        int weaponIdSlot = -1;
        int maxClip1Slot = -1;
        std::uintptr_t playerVtable = 0u;   // &_ZTV13CTerrorPlayer + 8
        l4d2vr_linux::MeleeOperands melee{};
    };

    struct Netprops
    {
        int teamNum = -1, lifeState = -1, activeWeapon = -1, incapacitated = -1;
        int ammo = -1, nextAttack = -1, usingMountedGun = -1, usingMountedWeapon = -1;
        int tongueOwner = -1, hangingFromTongue = -1, carryAttacker = -1, pummelAttacker = -1,
            pounceAttacker = -1, jockeyAttacker = -1;
        int clip1 = -1, primaryAmmoType = -1, inReload = -1, nextPrimary = -1, nextSecondary = -1, dualWielding = -1;
        int reloadState = -1;
        bool status = false, ammoOk = false, shell = false, deploy = false;
    };

    Natives g_N;
    Netprops g_P;
    InlineHook g_HookReadUsercmd, g_HookFireBullets, g_HookMolotov, g_HookPipe, g_HookVomit;
    VtableHook g_HookProcessUsercmds, g_HookRunCommand, g_HookEyePosition, g_HookEyeAngles, g_HookShootPosition,
        g_HookGunReload, g_HookShotgunReload;

    IVEngineServer* g_Engine = nullptr;
    ServerConfig g_Config{};
    l4d2vr_linux::FeatureResolution g_Resolved{};
    std::uint32_t g_Features = 0u;     // resolved & operator-enabled
    bool g_EyeHooks = false;
    std::array<PlayerVRState, kMaxPlayerSlots + 1> g_Players{};
    edict_t* g_EdictList = nullptr;
    int g_EdictCount = 0;
    int g_ClientMax = kMaxPlayerSlots;
    bool g_GlobalsValidated = false;

    // ---- Small entity helpers -------------------------------------------------------
    template <class T> T& Field(void* entity, int offset)
    {
        return *reinterpret_cast<T*>(static_cast<unsigned char*>(entity) + offset);
    }

    std::uintptr_t VirtualAt(void* object, int slot)
    {
        if (!object || slot < 0) return 0u;
        const auto vtable = *reinterpret_cast<std::uintptr_t*>(object);
        std::uintptr_t fn = 0u;
        if (!ReadMem(vtable + static_cast<std::uintptr_t>(slot) * 4u, fn) || !g_Module.Contains(fn, 1u)) return 0u;
        return fn;
    }

    int WeaponId(void* weapon)
    {
        const std::uintptr_t fn = VirtualAt(weapon, g_N.weaponIdSlot);
        return fn ? reinterpret_cast<FnInt>(fn)(weapon) : 0;
    }

    int MaxClip1(void* weapon)
    {
        const std::uintptr_t fn = VirtualAt(weapon, g_N.maxClip1Slot);
        return fn ? reinterpret_cast<FnInt>(fn)(weapon) : 0;
    }

    bool IsValidPlayerIndex(int index) { return index >= 1 && index <= kMaxPlayerSlots && index <= g_ClientMax; }

    edict_t* EdictBase()
    {
        if (g_EdictList) return g_EdictList;
        if (!g_N.gpGlobalsVar || !g_GlobalsValidated) return nullptr;
        const auto globals = *reinterpret_cast<std::uintptr_t*>(g_N.gpGlobalsVar);
        return globals ? *reinterpret_cast<edict_t**>(globals + kGlobalsEdictsOffset) : nullptr;
    }

    int PlayerIndexFromEdict(const edict_t* entity)
    {
        const edict_t* base = EdictBase();
        if (!entity || !base) return -1;
        const auto b = reinterpret_cast<std::uintptr_t>(base);
        const auto a = reinterpret_cast<std::uintptr_t>(entity);
        if (a < b || (a - b) % sizeof(edict_t) != 0u) return -1;
        const std::uintptr_t index = (a - b) / sizeof(edict_t);
        if (g_EdictList && index >= static_cast<std::uintptr_t>(g_EdictCount)) return -1;
        return IsValidPlayerIndex(static_cast<int>(index)) ? static_cast<int>(index) : -1;
    }

    // True for a CTerrorPlayer object (human survivor/infected player class).
    bool IsTerrorPlayer(void* entity)
    {
        return entity && g_N.playerVtable && *reinterpret_cast<std::uintptr_t*>(entity) == g_N.playerVtable;
    }

    int PlayerIndexFromEntity(void* entity)
    {
        if (!IsTerrorPlayer(entity) || !g_N.entindex) return -1;
        const int index = g_N.entindex(entity);
        return IsValidPlayerIndex(index) ? index : -1;
    }

    void* EntityFromEdict(edict_t* edict)
    {
        if (!edict || (edict->m_fStateFlags & 2) != 0) return nullptr;   // FL_EDICT_FREE
        void* entity = edict->m_pUnk;
        return IsTerrorPlayer(entity) ? entity : nullptr;
    }

    float Curtime()
    {
        if (!g_GlobalsValidated) return -1.0f;
        const auto globals = *reinterpret_cast<std::uintptr_t*>(g_N.gpGlobalsVar);
        return globals ? *reinterpret_cast<const float*>(globals + kGlobalsCurtime) : -1.0f;
    }

    float TickSeconds()
    {
        if (!g_GlobalsValidated) return 1.0f / 30.0f;
        const auto globals = *reinterpret_cast<std::uintptr_t*>(g_N.gpGlobalsVar);
        const float interval = globals ? *reinterpret_cast<const float*>(globals + kGlobalsInterval) : 0.0f;
        return interval > 0.001f && interval < 0.2f ? interval : 1.0f / 30.0f;
    }

    bool HandleValid(std::uint32_t handle) { return handle != 0u && handle != 0xFFFFFFFFu; }

    struct Status { bool live = false, incap = false, controlled = false, mounted = false; };

    Status ReadStatus(void* player)
    {
        Status s{};
        if (!player) return s;
        if (g_N.isAlive && g_N.teamNumber)
            s.live = g_N.isAlive(player) && g_N.teamNumber(player) == kSurvivorTeam;
        if (g_N.isIncapacitated)
            s.incap = g_N.isIncapacitated(player);
        if (!g_P.status) return s;
        s.controlled = Field<unsigned char>(player, g_P.hangingFromTongue) != 0 ||
            HandleValid(Field<std::uint32_t>(player, g_P.tongueOwner)) ||
            HandleValid(Field<std::uint32_t>(player, g_P.carryAttacker)) ||
            HandleValid(Field<std::uint32_t>(player, g_P.pummelAttacker)) ||
            HandleValid(Field<std::uint32_t>(player, g_P.pounceAttacker)) ||
            HandleValid(Field<std::uint32_t>(player, g_P.jockeyAttacker));
        s.mounted = Field<unsigned char>(player, g_P.usingMountedGun) != 0 ||
            Field<unsigned char>(player, g_P.usingMountedWeapon) != 0;
        return s;
    }

    void MarkChanged(void* entity)
    {
        if (!entity || !g_N.changeAccessor) return;
        edict_t* edict = *reinterpret_cast<edict_t**>(static_cast<unsigned char*>(entity) + kEntityEdictOffset);
        if (!edict) return;
        edict->m_fStateFlags |= 0x101;     // FL_EDICT_CHANGED | FL_FULL_EDICT_CHANGED
        std::uint16_t* accessor = g_N.changeAccessor(edict);
        if (accessor) accessor[1] = 0u;    // CBaseEdict::StateChanged(): change serial 0
    }

    // ---- Execution context (the game simulates and reads usercmds on one thread) -----
    struct ExecFrame
    {
        void* player = nullptr;
        int index = -1;
        ExecContext ctx{};
        QAngle angles{};
    };
    struct SweepFrame
    {
        void* player = nullptr;
        Vector origin{};
        QAngle angles{};
    };
    ExecFrame* g_Exec = nullptr;
    const SweepFrame* g_Sweep = nullptr;
    int g_DecodeIndex = -1;
    bool g_DecodeHaveOrigin = false;
    Vec3 g_DecodeOrigin{};

    // ---- Session host ------------------------------------------------------------------
    class Host final : public SessionHost
    {
    public:
        void SendToClient(const void* entity, const char* text) override
        {
            if (g_Engine && entity && text)
                g_Engine->ClientCommand(const_cast<edict_t*>(static_cast<const edict_t*>(entity)), "%s", text);
        }
        std::uint64_t NowMs() const override { return SteadyMs(); }
        std::uint64_t NowUs() const override { return SteadyUs(); }
        bool ViewYaw(int index, float& yaw) const override
        {
            if (!IsValidPlayerIndex(index) || !g_Players[static_cast<std::size_t>(index)].haveViewYaw) return false;
            yaw = g_Players[static_cast<std::size_t>(index)].viewYaw;
            return std::isfinite(yaw);
        }
        void Log(const char* line) override { std::fputs(line, stderr); }
    };

    Host g_Host;
    Session g_Session(g_Host);

    // ---- Ammo backend for PR409 transactions --------------------------------------------
    class Ammo final : public AmmoBackend
    {
    public:
        bool ReadAmmo(int index, int requestedWeaponId, bool magazine, AmmoState& state, bool& unsupported) override
        {
            state = AmmoState{};
            unsupported = !g_P.ammoOk || (!magazine && !g_P.shell);
            if (unsupported || !IsValidPlayerIndex(index) || !g_EdictList) return false;
            edict_t* edict = &g_EdictList[index];
            void* player = EntityFromEdict(edict);
            PlayerVRState& vr = g_Players[static_cast<std::size_t>(index)];
            if (!player || !vr.isUsingVR) return false;
            const Status status = ReadStatus(player);
            if (!status.live || status.incap || status.controlled) return false;
            void* weapon = g_N.getActiveWeapon(player);
            if (!weapon || g_N.getOwner(weapon) != player) return false;
            const int id = WeaponId(weapon);
            if (id != requestedWeaponId || (magazine ? !l4d2vr_remote_mag::IsDetachable(id) : !l4d2vr_shell::IsShotgun(id)))
                return false;
            if (magazine && id == weapon::kPistol && g_N.isDualWielding(weapon)) return false;   // native dual fallback
            const int weaponIndex = g_N.entindex(weapon);
            const auto activeHandle = Field<std::uint32_t>(player, g_P.activeWeapon);
            if (activeHandle == 0xFFFFFFFFu || static_cast<int>(activeHandle & 0xFFFu) != weaponIndex) return false;
            const std::uint32_t handle = (activeHandle & 0xFFFu) | (((activeHandle >> 12) & 0x3FFu) << 12);
            if (!l4d2vr_shell::ValidHandle(handle)) return false;
            const int ammoType = Field<int>(weapon, g_P.primaryAmmoType);
            if (ammoType < 0 || ammoType >= 32) { unsupported = true; return false; }
            bool infinite = false;
            if (magazine)
            {
                void* def = g_N.ammoDef();
                if (!def) { unsupported = true; return false; }
                infinite = g_N.infiniteAmmo(def, ammoType);
            }
            const int clip = Field<int>(weapon, g_P.clip1);
            const int reserve = Field<int>(player, g_P.ammo + ammoType * 4);
            if (!infinite && g_N.ammoCount(player, ammoType) != reserve) { unsupported = true; return false; }
            const std::uint64_t now = SteadyMs();
            state.eligible = true;
            state.handle = handle;
            state.latestCommand = vr.InputFresh(now) ? vr.lastDecodedCommand : 0u;
            state.weaponId = id;
            state.clip = clip;
            state.reserve = reserve;
            state.capacity = MaxClip1(weapon);
            state.infinite = infinite;
            state.nativeWeapon = weapon;
            state.nativePlayer = player;
            state.ammoType = ammoType;
            return true;
        }

        bool WriteAmmo(const AmmoState& s, int clip, int reserve, bool magazine) override
        {
            if (!s.nativeWeapon || !s.nativePlayer || s.ammoType < 0 || s.ammoType >= 32) return false;
            int& nativeClip = Field<int>(s.nativeWeapon, g_P.clip1);
            int& nativeReserve = Field<int>(s.nativePlayer, g_P.ammo + s.ammoType * 4);
            if (nativeClip != s.clip || nativeReserve != s.reserve) return false;
            nativeReserve = reserve;
            nativeClip = clip;
            // End a conventional reload that started before negotiation.
            Field<unsigned char>(s.nativeWeapon, g_P.inReload) = 0;
            if (!magazine && g_P.reloadState >= 0) Field<int>(s.nativeWeapon, g_P.reloadState) = 0;
            MarkChanged(s.nativePlayer);
            MarkChanged(s.nativeWeapon);
            return true;
        }
    };

    Ammo g_Ammo;

    // ---- Detours ---------------------------------------------------------------------------
    float DetourProcessUsercmds(void* self, edict_t* edict, void* buf, int numcmds, int totalcmds, int dropped,
        bool ignore, bool paused)
    {
        const int previousIndex = g_DecodeIndex;
        const bool previousHaveOrigin = g_DecodeHaveOrigin;
        const Vec3 previousOrigin = g_DecodeOrigin;
        g_DecodeIndex = PlayerIndexFromEdict(edict);
        g_DecodeHaveOrigin = false;
        if (g_DecodeIndex > 0)
        {
            void* player = EntityFromEdict(edict);
            const Vector* origin = player ? g_N.absOrigin(player) : nullptr;
            if (origin && std::isfinite(origin->x) && std::isfinite(origin->y) && std::isfinite(origin->z))
            {
                g_DecodeOrigin = ToVec(*origin);
                g_DecodeHaveOrigin = true;
            }
        }
        const float result = g_N.processUsercmds(self, edict, buf, numcmds, totalcmds, dropped, ignore, paused);
        g_DecodeIndex = previousIndex;
        g_DecodeHaveOrigin = previousHaveOrigin;
        g_DecodeOrigin = previousOrigin;
        return result;
    }

    void DetourReadUsercmd(void* buf, CUserCmdLinux* move, CUserCmdLinux* from)
    {
        g_N.readUsercmd(buf, move, from);
        if (!move || move->tick_count >= 0)
        {
            // Plain command: untouched. Only end a previous VR session of this slot.
            if (move && IsValidPlayerIndex(g_DecodeIndex) && g_Players[static_cast<std::size_t>(g_DecodeIndex)].isUsingVR)
            {
                UsercmdData plain{};
                RecordDecodedCommand(g_Players[static_cast<std::size_t>(g_DecodeIndex)], plain, DecodedUsercmd{}, false, {}, 0u);
            }
            return;
        }
        UsercmdData cmd{};
        cmd.command_number = move->command_number;
        cmd.tick_count = move->tick_count;
        std::memcpy(cmd.viewangles, move->viewangles, sizeof(cmd.viewangles));
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
        const DecodedUsercmd decoded = DecodeVRUsercmd(cmd);
        // Write back exactly the fields the listen-host decoder strips.
        move->command_number = cmd.command_number;
        move->tick_count = cmd.tick_count;
        std::memcpy(move->viewangles, cmd.viewangles, sizeof(cmd.viewangles));
        move->upmove = cmd.upmove;
        move->buttons = cmd.buttons;
        move->impulse = cmd.impulse;
        move->weaponselect = cmd.weaponselect;
        move->weaponsubtype = cmd.weaponsubtype;
        if (!IsValidPlayerIndex(g_DecodeIndex)) return;
        PlayerVRState& p = g_Players[static_cast<std::size_t>(g_DecodeIndex)];
        RecordDecodedCommand(p, cmd, decoded, g_DecodeHaveOrigin, g_DecodeOrigin, SteadyMs());
        g_Session.NoteVRUsercmd(g_DecodeIndex);
    }

    int DetourFireTerrorBullets(int playerId, const Vector& origin, const QAngle& angles, int weaponId, int a5, int a6, float a7)
    {
        if (g_Exec && g_Exec->index == playerId && g_Exec->ctx.poseValid)
        {
            const Vector position = ToVector(g_Exec->ctx.position);
            return g_N.fireBullets(playerId, position, g_Exec->angles, weaponId, a5, a6, a7);
        }
        if (IsValidPlayerIndex(playerId))
        {
            const PlayerVRState& p = g_Players[static_cast<std::size_t>(playerId)];
            if (p.InputFresh(SteadyMs(), 500u) && p.latestPoseValid)
            {
                Vec3 hand = p.latestPos;
                ClampHandReach(ToVec(origin), hand, g_Config.MaxHandReachUnits());
                const Vector position = ToVector(hand);
                const QAngle aim = ToQAngle(NormalizeAndClampViewAngles(p.latestAngles));
                return g_N.fireBullets(playerId, position, aim, weaponId, a5, a6, a7);
            }
        }
        return g_N.fireBullets(playerId, origin, angles, weaponId, a5, a6, a7);
    }

    Vector DetourEyePosition(void* self)
    {
        if (g_Exec && g_Exec->player == self && g_Exec->ctx.aim != AimReason::None)
            return ToVector(g_Exec->ctx.position);
        return g_N.eyePosition(self);
    }

    const QAngle* DetourEyeAngles(void* self)
    {
        if (g_Sweep && g_Sweep->player == self)
            return &g_Sweep->angles;
        if (g_Exec && g_Exec->player == self && g_Exec->ctx.aim != AimReason::None)
            return &g_Exec->angles;
        return g_N.eyeAngles(self);
    }

    Vector DetourShootPosition(void* self)
    {
        if (g_Sweep && g_Sweep->player == self)
            return g_Sweep->origin;
        return g_N.shootPosition(self);
    }

    bool ReloadBlocked(void* weapon)
    {
        if (!weapon || !g_N.getOwner || !g_Resolved.interaction) return false;
        void* owner = g_N.getOwner(weapon);
        const int index = PlayerIndexFromEntity(owner);
        if (index < 0) return false;
        const PlayerVRState& p = g_Players[static_cast<std::size_t>(index)];
        return p.isUsingVR && p.reloadLedger.Blocks(reinterpret_cast<std::uintptr_t>(weapon));
    }

    bool DetourGunReload(void* weapon)
    {
        if (ReloadBlocked(weapon)) return false;
        return g_N.gunReload(weapon);
    }

    bool DetourShotgunReload(void* weapon)
    {
        if (ReloadBlocked(weapon)) return false;
        return g_N.shotgunReload(weapon);
    }

    // Returns the pending throw to apply for `owner`, or nullptr.
    const PendingThrow* TakePendingThrow(void* owner, int expectedWeaponId, PendingThrow& storage)
    {
        if ((g_Features & kFeatureThrowVelocity) == 0u) return nullptr;
        const int index = PlayerIndexFromEntity(owner);
        if (index < 0) return nullptr;
        PlayerVRState& p = g_Players[static_cast<std::size_t>(index)];
        if ((g_Session.EffectiveFeatures(index) & kFeatureThrowVelocity) == 0u) return nullptr;
        PendingThrow& pending = p.pendingThrow;
        if (!p.isUsingVR || !pending.valid || pending.owner != reinterpret_cast<std::uintptr_t>(owner) ||
            pending.weaponId != expectedWeaponId || pending.executedSinceRelease > 48 || pending.release.belowMinimum)
            return nullptr;
        storage = pending;
        pending = {};
        Log("throw player=%d weapon=%d velocity=(%.1f %.1f %.1f)", index, expectedWeaponId,
            storage.release.velocity.x, storage.release.velocity.y, storage.release.velocity.z);
        return &storage;
    }

    void* DetourMolotovCreate(const Vector& pos, const QAngle& ang, const Vector& vel, const Vector& angVel, void* owner)
    {
        PendingThrow t{};
        if (const PendingThrow* p = TakePendingThrow(owner, weapon::kMolotov, t))
            return g_N.molotovCreate(ToVector(p->release.origin), ToQAngle(p->release.angles),
                ToVector(p->release.velocity), ToVector(p->release.angularVelocity), owner);
        return g_N.molotovCreate(pos, ang, vel, angVel, owner);
    }

    void* DetourPipeCreate(const Vector& pos, const QAngle& ang, const Vector& vel, const Vector& angVel, void* owner, float timer)
    {
        PendingThrow t{};
        if (const PendingThrow* p = TakePendingThrow(owner, weapon::kPipeBomb, t))
            return g_N.pipeCreate(ToVector(p->release.origin), ToQAngle(p->release.angles),
                ToVector(p->release.velocity), ToVector(p->release.angularVelocity), owner, timer);
        return g_N.pipeCreate(pos, ang, vel, angVel, owner, timer);
    }

    void* DetourVomitCreate(const Vector& pos, const QAngle& ang, const Vector& vel, const Vector& angVel, void* owner)
    {
        PendingThrow t{};
        if (const PendingThrow* p = TakePendingThrow(owner, weapon::kVomitJar, t))
            return g_N.vomitCreate(ToVector(p->release.origin), ToQAngle(p->release.angles),
                ToVector(p->release.velocity), ToVector(p->release.angularVelocity), owner);
        return g_N.vomitCreate(pos, ang, vel, angVel, owner);
    }

    void RunMeleeSweep(void* player, void* weapon, const MeleeSweepState::Step& step)
    {
        const auto& m = g_N.melee;
        if (!weapon || !m.ok) return;
        if (step.newSwing)
            Field<int>(weapon, static_cast<int>(m.hitCountOffset)) = 0;   // new physical swing: clear hit list
        void* info = g_N.meleeInfo(g_N.meleeStore, Field<int>(weapon, static_cast<int>(m.infoIndexOffset)));
        if (!info) return;
        g_N.primaryActivity(weapon, info);   // selects the swing data TestMeleeSwingCollision reads
        for (int i = 0; i < step.plan.count; ++i)
        {
            const MeleeTrace& t = step.plan.traces[static_cast<std::size_t>(i)];
            const SweepFrame frame{ player, ToVector(t.origin), ToQAngle(t.angles) };
            const Vector direction = ToVector(t.direction);
            g_Sweep = &frame;
            g_N.testMelee(weapon, direction);
            g_Sweep = nullptr;
        }
    }

    void DetourPlayerRunCommand(void* self, CUserCmdLinux* ucmd, void* moveHelper)
    {
        const int index = (ucmd && g_N.entindex) ? g_N.entindex(self) : -1;
        if (!IsValidPlayerIndex(index) || !g_Players[static_cast<std::size_t>(index)].isUsingVR)
        {
            g_N.playerRunCommand(self, ucmd, moveHelper);
            return;
        }
        PlayerVRState& p = g_Players[static_cast<std::size_t>(index)];
        const CommandPose* pose = p.commands.Find(ucmd->command_number);
        const std::uint32_t features = g_Session.EffectiveFeatures(index);
        const Status status = ReadStatus(self);
        void* weaponBefore = g_N.getActiveWeapon(self);

        ExecInput in{};
        in.features = features;
        in.interaction = g_Resolved.interaction;
        in.eyeHooks = g_EyeHooks;
        in.owner = reinterpret_cast<std::uintptr_t>(self);
        in.weaponTag = reinterpret_cast<std::uintptr_t>(weaponBefore);
        in.weaponId = weaponBefore ? WeaponId(weaponBefore) : 0;
        in.liveSurvivor = status.live;
        in.incapacitated = status.incap;
        in.controlled = status.controlled;
        in.usingMounted = status.mounted;
        in.nowMs = SteadyMs();
        in.tickSeconds = TickSeconds();
        if (pose && pose->poseValid && g_N.eyePosition)
        {
            const Vector eye = g_N.eyePosition(self);
            in.eye = ToVec(eye);
            in.haveEye = in.eye.Finite();
        }

        ExecFrame frame{};
        frame.player = self;
        frame.index = index;
        frame.ctx = PreExecute(p, pose, ucmd->command_number, ucmd->buttons, in, g_Config);
        frame.angles = ToQAngle(frame.ctx.aimAngles);
        ucmd->buttons |= frame.ctx.addButtons;
        if (g_Config.verbose && frame.ctx.aim != AimReason::None)
            Log("player=%d command=%d eye override %s", index, ucmd->command_number, AimReasonName(frame.ctx.aim));

        ExecFrame* previous = g_Exec;
        g_Exec = &frame;
        g_N.playerRunCommand(self, ucmd, moveHelper);
        g_Exec = previous;

        void* weaponAfter = g_N.getActiveWeapon(self);
        const Status after = ReadStatus(self);
        PostInput post{};
        post.features = features;
        post.interaction = g_Resolved.interaction;
        post.owner = in.owner;
        post.weaponTagBefore = in.weaponTag;
        post.weaponTagAfter = reinterpret_cast<std::uintptr_t>(weaponAfter);
        post.weaponIdAfter = weaponAfter ? WeaponId(weaponAfter) : 0;
        post.liveSurvivor = after.live;
        post.incapacitated = after.incap;
        post.controlled = after.controlled;
        post.meleeBackend = (g_Features & kFeatureMeleeSweepV2) != 0u;
        post.dropBackend = (g_Features & kFeatureHolsterDrop) != 0u;
        post.deployBackend = (g_Features & kFeatureInstantDeploy) != 0u;
        post.nowMs = in.nowMs;
        post.tickSeconds = in.tickSeconds;
        const PostActions actions = PostExecute(p, pose, frame.ctx, post, g_Config);

        if (actions.melee.rejected && g_Config.verbose)
            Log("melee swing rejected player=%d reason=%s", index,
                actions.melee.gate == MeleeSwingGate::Result::Cooldown ? "cooldown" : "burst-limit");
        if (actions.melee.sweep)
            RunMeleeSweep(self, weaponAfter, actions.melee);
        if (actions.drop && weaponAfter && g_N.getActiveWeapon(self) == weaponAfter)
        {
            const Vector velocity = ToVector(actions.dropRequest.velocity);
            g_N.weaponDrop(self, weaponAfter, nullptr, &velocity);
            Log("%s player=%d weapon=%d velocity=(%.1f %.1f %.1f)", actions.dropRequest.holster ? "holster drop" : "inventory drop",
                index, actions.dropRequest.weaponId, velocity.x, velocity.y, velocity.z);
        }
        if (actions.instantDeploy && weaponAfter)
        {
            const float now = Curtime();
            if (now >= 0.0f && std::isfinite(now))
            {
                Field<float>(weaponAfter, g_P.nextPrimary) = std::min(Field<float>(weaponAfter, g_P.nextPrimary), now);
                Field<float>(weaponAfter, g_P.nextSecondary) = std::min(Field<float>(weaponAfter, g_P.nextSecondary), now);
                Field<float>(self, g_P.nextAttack) = std::min(Field<float>(self, g_P.nextAttack), now);
                MarkChanged(weaponAfter);
                MarkChanged(self);
            }
        }
    }

    // ---- Network properties -------------------------------------------------------------
    constexpr std::uint32_t kPropSize = 0x54u, kPropType = 0x08u, kPropName = 0x30u, kPropFlags = 0x3Cu,
        kPropTable = 0x48u, kPropOffset = 0x4Cu;
    constexpr int kTypeDataTable = 6;
    constexpr int kFlagExclude = 1 << 6;

    bool ReadCString(std::uintptr_t address, std::string& out)
    {
        out.clear();
        for (int i = 0; i < 128; ++i)
        {
            char c = 0;
            if (!ReadMem(address + static_cast<std::uintptr_t>(i), c)) return false;
            if (!c) return true;
            out.push_back(c);
        }
        return false;
    }

    int SearchTable(std::uintptr_t table, const char* prop, int base, int depth)
    {
        if (!table || depth > 16) return -1;
        std::uintptr_t props = 0u;
        int count = 0;
        if (!ReadMem(table, props) || !ReadMem(table + 4u, count) || count < 0 || count > 4096 ||
            !g_Module.Contains(props, static_cast<std::size_t>(count) * kPropSize))
            return -1;
        for (int i = 0; i < count; ++i)
        {
            const std::uintptr_t p = props + static_cast<std::uintptr_t>(i) * kPropSize;
            int type = 0, flags = 0, offset = 0;
            std::uintptr_t namePtr = 0u, sub = 0u;
            std::string name;
            if (!ReadMem(p + kPropType, type) || !ReadMem(p + kPropFlags, flags) || !ReadMem(p + kPropOffset, offset) ||
                !ReadMem(p + kPropName, namePtr) || !ReadMem(p + kPropTable, sub) || !ReadCString(namePtr, name))
                return -1;
            if ((flags & kFlagExclude) != 0) continue;
            if (name == prop) return base + offset;
            if (type == kTypeDataTable && sub)
            {
                const int found = SearchTable(sub, prop, base + offset, depth + 1);
                if (found >= 0) return found;
            }
        }
        return -1;
    }

    std::uintptr_t FindServerClassTable(const char* className)
    {
        std::uintptr_t node = 0u;
        if (!ReadMem(g_N.serverClassHeadVar, node)) return 0u;
        for (int guard = 0; node && guard < 4096; ++guard)
        {
            std::uintptr_t namePtr = 0u, table = 0u, next = 0u;
            std::string name;
            if (!ReadMem(node, namePtr) || !ReadMem(node + 4u, table) || !ReadMem(node + 8u, next) || !ReadCString(namePtr, name))
                return 0u;
            if (name == className) return table;
            node = next;
        }
        return 0u;
    }

    int Netprop(const char* className, const char* prop)
    {
        const std::uintptr_t table = FindServerClassTable(className);
        const int offset = table ? SearchTable(table, prop, 0, 0) : -1;
        return offset > 0 && offset < 0x10000 ? offset : -1;
    }

    // Offset operand at `at` within a resolved call target's guarded bytes.
    int CodeOperand(const std::vector<l4d2vr_linux::TargetResult>& results, const char* label, std::size_t at)
    {
        const auto* r = l4d2vr_linux::FindResult(results, label);
        if (!r || !r->ok || r->byteCount < at + 4u) return -2;
        return l4d2vr_x86::ReadI32(r->bytes + at);
    }

    void ResolveNetprops(const std::vector<l4d2vr_linux::TargetResult>& results, std::uint32_t groups)
    {
        g_P = Netprops{};
        if (!g_N.serverClassHeadVar) return;
        const char* player = "CTerrorPlayer";
        g_P.teamNum = Netprop(player, "m_iTeamNum");
        g_P.lifeState = Netprop(player, "m_lifeState");
        g_P.activeWeapon = Netprop(player, "m_hActiveWeapon");
        g_P.incapacitated = Netprop(player, "m_isIncapacitated");
        g_P.ammo = Netprop(player, "m_iAmmo");
        g_P.nextAttack = Netprop(player, "m_flNextAttack");
        g_P.usingMountedGun = Netprop(player, "m_usingMountedGun");
        g_P.usingMountedWeapon = Netprop(player, "m_usingMountedWeapon");
        g_P.tongueOwner = Netprop(player, "m_tongueOwner");
        g_P.hangingFromTongue = Netprop(player, "m_isHangingFromTongue");
        g_P.carryAttacker = Netprop(player, "m_carryAttacker");
        g_P.pummelAttacker = Netprop(player, "m_pummelAttacker");
        g_P.pounceAttacker = Netprop(player, "m_pounceAttacker");
        g_P.jockeyAttacker = Netprop(player, "m_jockeyAttacker");
        g_P.clip1 = Netprop("CPistol", "m_iClip1");
        g_P.primaryAmmoType = Netprop("CPistol", "m_iPrimaryAmmoType");
        g_P.inReload = Netprop("CPistol", "m_bInReload");
        g_P.nextPrimary = Netprop("CPistol", "m_flNextPrimaryAttack");
        g_P.nextSecondary = Netprop("CPistol", "m_flNextSecondaryAttack");
        g_P.dualWielding = Netprop("CPistol", "m_isDualWielding");
        g_P.reloadState = Netprop("CPumpShotgun", "m_reloadState");

        // Cross-check the SendProp layout against offsets inside accessor code.
        const bool layout =
            g_P.teamNum == CodeOperand(results, "GetTeamNumber", 9) &&
            g_P.lifeState == CodeOperand(results, "IsAlive", 9) &&
            g_P.activeWeapon == CodeOperand(results, "GetActiveWeapon", 14) &&
            g_P.incapacitated == CodeOperand(results, "IsIncapacitated", 20) &&
            g_P.clip1 == CodeOperand(results, "Clip1", 9) &&
            g_P.dualWielding == CodeOperand(results, "IsDualWielding", 10);
        if (!layout)
        {
            Log("netprop cross-check failed (team %d lifeState %d activeWeapon %d incap %d clip1 %d dual %d); "
                "netprop-based features disabled", g_P.teamNum, g_P.lifeState, g_P.activeWeapon, g_P.incapacitated,
                g_P.clip1, g_P.dualWielding);
            return;
        }
        const bool statusGroup = (groups & l4d2vr_linux::kGroupStatus) != 0u && (groups & l4d2vr_linux::kGroupBase) != 0u;
        g_P.status = statusGroup && g_P.usingMountedGun > 0 && g_P.usingMountedWeapon > 0 && g_P.tongueOwner > 0 &&
            g_P.hangingFromTongue > 0 && g_P.carryAttacker > 0 && g_P.pummelAttacker > 0 && g_P.pounceAttacker > 0 &&
            g_P.jockeyAttacker > 0;
        g_P.ammoOk = (groups & l4d2vr_linux::kGroupAmmo) != 0u && g_P.ammo > 0 && g_P.primaryAmmoType > 0 && g_P.inReload > 0;
        g_P.shell = g_P.ammoOk && g_P.reloadState > 0;
        g_P.deploy = (groups & l4d2vr_linux::kGroupDeploy) != 0u && g_P.nextPrimary > 0 && g_P.nextSecondary > 0 &&
            g_P.nextAttack > 0;
        Log("netprops: activeWeapon=0x%X ammo=0x%X clip1=0x%X ammoType=0x%X inReload=0x%X reloadState=0x%X "
            "nextAttack=0x%X nextPrimary=0x%X mounted=0x%X/0x%X", g_P.activeWeapon, g_P.ammo, g_P.clip1,
            g_P.primaryAmmoType, g_P.inReload, g_P.reloadState, g_P.nextAttack, g_P.nextPrimary,
            g_P.usingMountedGun, g_P.usingMountedWeapon);
    }

    // ---- Configuration -------------------------------------------------------------------
    void LoadConfig()
    {
        g_Config = ServerConfig{};
        const char* candidates[] = { "left4dead2/addons/l4d2vr_server_config.txt", "addons/l4d2vr_server_config.txt",
            "l4d2vr_server_config.txt" };
        for (const char* path : candidates)
        {
            FILE* file = std::fopen(path, "r");
            if (!file) continue;
            char line[512];
            int lineNumber = 0;
            while (std::fgets(line, sizeof(line), file))
            {
                ++lineNumber;
                if (!ApplyConfigLine(g_Config, line))
                    Log("config %s:%d ignored: %s", path, lineNumber, line);
            }
            std::fclose(file);
            Log("config loaded from %s", path);
            return;
        }
    }

    // ---- Installation ----------------------------------------------------------------------
    std::uintptr_t Address(const std::vector<l4d2vr_linux::TargetResult>& results, const char* label)
    {
        const auto* r = l4d2vr_linux::FindResult(results, label);
        return r && r->ok ? r->address : 0u;
    }

    template <class T> T As(std::uintptr_t address) { return reinterpret_cast<T>(address); }

    void UninstallHooks()
    {
        g_HookMolotov.Remove();
        g_HookPipe.Remove();
        g_HookVomit.Remove();
        g_HookFireBullets.Remove();
        g_HookReadUsercmd.Remove();
        g_HookGunReload.Remove();
        g_HookShotgunReload.Remove();
        g_HookShootPosition.Remove();
        g_HookEyeAngles.Remove();
        g_HookEyePosition.Remove();
        g_HookRunCommand.Remove();
        g_HookProcessUsercmds.Remove();
        g_EyeHooks = false;
    }

    void InstallAll()
    {
        // The pose relay (v2 + v3) needs no game hooks and is always available.
        g_Resolved = {};
        g_Resolved.features = kFeaturePoseV3;
        g_Features = g_Resolved.features & g_Config.featureMask;
        g_Module = ServerModule{};
        dl_iterate_phdr(FindModuleCallback, &g_Module);
        if (!g_Module.found || g_Module.ranges.empty())
        {
            Log("server_srv.so is not loaded; only the pose relay is active");
            return;
        }
        std::string error;
        if (!g_Image.LoadFile(g_Module.path, error))
        {
            Log("cannot read symbols of %s: %s; only the pose relay is active", g_Module.path.c_str(), error.c_str());
            return;
        }

        RuntimeView view;
        std::vector<l4d2vr_linux::TargetResult> results;
        for (const auto& spec : l4d2vr_linux::Targets())
        {
            results.push_back(l4d2vr_linux::EvaluateTarget(view, spec));
            if (!results.back().ok)
                Log("target %s rejected: %s", spec.label, results.back().detail.c_str());
        }
        std::uint32_t groups = l4d2vr_linux::ResolvedGroups(results);
        g_N.melee = l4d2vr_linux::ExtractMeleeOperands(view, results);
        if (!g_N.melee.ok) Log("melee operands unavailable: %s", g_N.melee.detail.c_str());

        // Call targets and data.
        g_N.getActiveWeapon = As<FnEntity>(Address(results, "GetActiveWeapon"));
        g_N.entindex = As<FnInt>(Address(results, "entindex"));
        g_N.teamNumber = As<FnInt>(Address(results, "GetTeamNumber"));
        g_N.isAlive = As<FnBool>(Address(results, "IsAlive"));
        g_N.isIncapacitated = As<FnBool>(Address(results, "IsIncapacitated"));
        g_N.absOrigin = As<FnAbsOrigin>(Address(results, "GetAbsOrigin"));
        g_N.testMelee = As<FnTestMelee>(Address(results, "TestMeleeSwingCollision"));
        g_N.primaryActivity = As<FnPrimaryActivity>(Address(results, "GetPrimaryAttackActivity"));
        g_N.meleeInfo = As<FnMeleeInfo>(Address(results, "GetMeleeWeaponInfo"));
        g_N.meleeStore = reinterpret_cast<void*>(Address(results, "g_MeleeWeaponInfoStore"));
        g_N.weaponDrop = As<FnWeaponDrop>(Address(results, "Weapon_Drop"));
        g_N.clip1 = As<FnInt>(Address(results, "Clip1"));
        g_N.getOwner = As<FnEntity>(Address(results, "GetOwner"));
        g_N.isDualWielding = As<FnBool>(Address(results, "IsDualWielding"));
        g_N.ammoCount = As<FnAmmoCount>(Address(results, "GetAmmoCount"));
        g_N.ammoDef = As<FnAmmoDef>(Address(results, "GetAmmoDef"));
        g_N.infiniteAmmo = As<FnInfinite>(Address(results, "CanCarryInfiniteAmmo"));
        g_N.changeAccessor = As<FnChangeAccessor>(Address(results, "GetChangeAccessor"));
        g_N.gpGlobalsVar = Address(results, "gpGlobals");
        g_N.serverClassHeadVar = Address(results, "g_pServerClassHead");
        if (const auto* r = l4d2vr_linux::FindResult(results, "GetWeaponID(slot)"); r && r->ok) g_N.weaponIdSlot = r->slots[0].index;
        if (const auto* r = l4d2vr_linux::FindResult(results, "GetMaxClip1(slot)"); r && r->ok) g_N.maxClip1Slot = r->slots[0].index;
        {
            std::uint32_t vt = 0u, size = 0u;
            if (view.Symbol("_ZTV13CTerrorPlayer", vt, size)) g_N.playerVtable = vt + 8u;
            if (!g_N.playerVtable) groups &= ~l4d2vr_linux::kGroupBase;
        }

        ResolveNetprops(results, groups);

        // Hooks, group by group; a failed install clears only its group.
        auto vtable = [&](VtableHook& hook, const char* label, void* detour) -> bool
        {
            const auto* r = l4d2vr_linux::FindResult(results, label);
            if (!r || !r->ok || !hook.Install(label, r->slots, r->address, detour))
            {
                Log("vtable hook %s not installed", label);
                return false;
            }
            return true;
        };
        auto inlineHook = [&](InlineHook& hook, const char* label, void* detour) -> bool
        {
            const std::uintptr_t address = Address(results, label);
            if (!address || !hook.Install(label, address, detour))
            {
                Log("inline hook %s not installed", label);
                return false;
            }
            return true;
        };

        if ((groups & l4d2vr_linux::kGroupBase) != 0u)
        {
            const bool base = vtable(g_HookProcessUsercmds, "ProcessUsercmds", reinterpret_cast<void*>(&DetourProcessUsercmds)) &&
                vtable(g_HookRunCommand, "PlayerRunCommand", reinterpret_cast<void*>(&DetourPlayerRunCommand)) &&
                inlineHook(g_HookFireBullets, "FireTerrorBullets", reinterpret_cast<void*>(&DetourFireTerrorBullets)) &&
                inlineHook(g_HookReadUsercmd, "ReadUsercmd", reinterpret_cast<void*>(&DetourReadUsercmd));
            if (base)
            {
                g_N.processUsercmds = reinterpret_cast<FnProcessUsercmds>(g_HookProcessUsercmds.original);
                g_N.playerRunCommand = reinterpret_cast<FnPlayerRunCommand>(g_HookRunCommand.original);
                g_N.fireBullets = reinterpret_cast<FnFireTerrorBullets>(g_HookFireBullets.trampoline);
                g_N.readUsercmd = reinterpret_cast<FnReadUsercmd>(g_HookReadUsercmd.trampoline);
            }
            else
            {
                UninstallHooks();
                groups = 0u;
            }
        }
        if ((groups & l4d2vr_linux::kGroupBase) != 0u)
        {
            if ((groups & l4d2vr_linux::kGroupEye) != 0u &&
                vtable(g_HookEyePosition, "EyePosition", reinterpret_cast<void*>(&DetourEyePosition)) &&
                vtable(g_HookEyeAngles, "EyeAngles", reinterpret_cast<void*>(&DetourEyeAngles)))
            {
                g_N.eyePosition = reinterpret_cast<FnEyePosition>(g_HookEyePosition.original);
                g_N.eyeAngles = reinterpret_cast<FnEyeAngles>(g_HookEyeAngles.original);
                g_EyeHooks = true;
            }
            else
            {
                g_HookEyePosition.Remove();
                g_HookEyeAngles.Remove();
                groups &= ~(l4d2vr_linux::kGroupEye | l4d2vr_linux::kGroupMelee);
                // PreExecute still needs the native eye position for the reach clamp.
                g_N.eyePosition = As<FnEyePosition>(Address(results, "EyePosition"));
            }
            if ((groups & l4d2vr_linux::kGroupMelee) != 0u &&
                vtable(g_HookShootPosition, "Weapon_ShootPosition", reinterpret_cast<void*>(&DetourShootPosition)))
                g_N.shootPosition = reinterpret_cast<FnShootPosition>(g_HookShootPosition.original);
            else
                groups &= ~l4d2vr_linux::kGroupMelee;
            if ((groups & l4d2vr_linux::kGroupReloadGate) != 0u &&
                vtable(g_HookGunReload, "CTerrorGun::Reload", reinterpret_cast<void*>(&DetourGunReload)) &&
                vtable(g_HookShotgunReload, "CBaseShotgun::Reload", reinterpret_cast<void*>(&DetourShotgunReload)))
            {
                g_N.gunReload = reinterpret_cast<FnReload>(g_HookGunReload.original);
                g_N.shotgunReload = reinterpret_cast<FnReload>(g_HookShotgunReload.original);
            }
            else
            {
                g_HookGunReload.Remove();
                g_HookShotgunReload.Remove();
                groups &= ~l4d2vr_linux::kGroupReloadGate;
            }
            if ((groups & l4d2vr_linux::kGroupThrow) != 0u &&
                inlineHook(g_HookMolotov, "CMolotovProjectile::Create", reinterpret_cast<void*>(&DetourMolotovCreate)) &&
                inlineHook(g_HookPipe, "CPipeBombProjectile::Create", reinterpret_cast<void*>(&DetourPipeCreate)) &&
                inlineHook(g_HookVomit, "CVomitJarProjectile::Create", reinterpret_cast<void*>(&DetourVomitCreate)))
            {
                g_N.molotovCreate = reinterpret_cast<FnCreate>(g_HookMolotov.trampoline);
                g_N.pipeCreate = reinterpret_cast<FnCreatePipe>(g_HookPipe.trampoline);
                g_N.vomitCreate = reinterpret_cast<FnCreate>(g_HookVomit.trampoline);
            }
            else
            {
                g_HookMolotov.Remove();
                g_HookPipe.Remove();
                g_HookVomit.Remove();
                groups &= ~l4d2vr_linux::kGroupThrow;
            }
        }

        l4d2vr_linux::FeatureInputs inputs{};
        inputs.groups = groups;
        inputs.netpropStatus = g_P.status;
        inputs.netpropAmmo = g_P.ammoOk;
        inputs.netpropShell = g_P.shell;
        inputs.netpropDeploy = g_P.deploy;   // curtime is validated at ServerActivate
        inputs.meleeOperands = g_N.melee.ok;
        g_Resolved = l4d2vr_linux::ResolveFeatures(inputs);
        g_Features = g_Resolved.features & g_Config.featureMask;
        g_Image = l4d2vr_elf::Image{};   // ~20 MB file copy is only needed while installing
    }

    void PublishCapabilities()
    {
        std::uint32_t features = g_Features;
        if (!g_GlobalsValidated) features &= ~kFeatureInstantDeploy;
        SessionCapabilities caps{};
        caps.usercmdDecoder = g_Resolved.usercmdDecoder;
        caps.interaction = g_Resolved.interaction;
        caps.features = features;
        g_Session.SetCapabilities(caps);
    }

    void LogFeatureLine(const char* when)
    {
        const auto& caps = g_Session.Capabilities();
        std::string names;
        for (std::uint32_t bit = 1u; bit <= kFeatureInstantDeploy; bit <<= 1u)
            if (caps.features & bit) { if (!names.empty()) names += ' '; names += FeatureName(bit); }
        Log("%s: usercmd_decoder=%d interaction=%d features=0x%03X [%s]", when, caps.usercmdDecoder ? 1 : 0,
            caps.interaction ? 1 : 0, caps.features, names.c_str());
    }

    // plugin_load on a running map: no ServerActivate will follow. Adopt the
    // engine's edict array from gpGlobals (arithmetic only, never dereferenced).
    void AdoptRunningMap()
    {
        if (g_EdictList || !g_N.gpGlobalsVar) return;
        const auto globals = *reinterpret_cast<std::uintptr_t*>(g_N.gpGlobalsVar);
        if (!globals) return;
        const int maxClients = *reinterpret_cast<const int*>(globals + kGlobalsMaxClients);
        const float interval = *reinterpret_cast<const float*>(globals + kGlobalsInterval);
        edict_t* edicts = *reinterpret_cast<edict_t**>(globals + kGlobalsEdictsOffset);
        if (!edicts || maxClients < 1 || maxClients > kMaxPlayerSlots || !(interval > 0.001f && interval < 0.2f))
            return;
        g_EdictList = edicts;
        g_EdictCount = 2048;
        g_ClientMax = maxClients;
        g_Session.SetClientMax(maxClients);
        g_GlobalsValidated = true;
        Log("adopted the running map (maxClients %d); reconnect clients for the handshake", maxClients);
    }

    void ResetPlayers()
    {
        for (PlayerVRState& p : g_Players) p.Reset();
    }

    class L4D2VRServerPlugin final : public IServerPluginCallbacks
    {
    public:
        bool Load(CreateInterfaceFn interfaceFactory, CreateInterfaceFn) override
        {
            ResetPlayers();
            g_Session.ResetAll();
            g_Session.SetClientMax(kMaxPlayerSlots);
            g_Session.SeedTokens(static_cast<std::uint32_t>(SteadyUs()) ^ (static_cast<std::uint32_t>(getpid()) << 16));
            g_ClientMax = kMaxPlayerSlots;
            g_EdictList = nullptr;
            g_EdictCount = 0;
            g_Engine = nullptr;
            if (interfaceFactory)
            {
                int code = IFACE_FAILED;
                g_Engine = static_cast<IVEngineServer*>(interfaceFactory("VEngineServer022", &code));
            }
            if (!g_Engine)
            {
                Log("VEngineServer interface unavailable");
                return false;
            }
            LoadConfig();
            InstallAll();
            AdoptRunningMap();
            PublishCapabilities();
            Log("loaded (pose relay v2%s, module %s base 0x%08X)",
                (g_Session.Capabilities().features & kFeaturePoseV3) ? "+v3" : "",
                g_Module.found ? g_Module.path.c_str() : "<none>", static_cast<unsigned>(g_Module.base));
            LogFeatureLine("startup");
            return true;
        }

        void Unload() override
        {
            UninstallHooks();
            ResetPlayers();
            g_Session.ResetAll();
            g_Engine = nullptr;
            g_EdictList = nullptr;
            g_EdictCount = 0;
            Log("unloaded");
        }

        void Pause() override {}
        void UnPause() override {}

        const char* GetPluginDescription() override
        {
            return "L4D2VR dedicated server: VR usercmd fidelity + body/hand/finger pose relay (v2/v3)";
        }

        void LevelInit(const char*) override
        {
            ResetPlayers();
            g_Session.ResetAll();
            g_EdictList = nullptr;
            g_EdictCount = 0;
        }

        void ServerActivate(edict_t* edictList, int edictCount, int clientMax) override
        {
            g_ClientMax = (clientMax >= 1 && clientMax <= kMaxPlayerSlots) ? clientMax : kMaxPlayerSlots;
            g_Session.SetClientMax(g_ClientMax);
            g_EdictList = edictList;
            g_EdictCount = edictCount > 0 ? edictCount : 0;
            // Validate the CGlobalVars fields used for curtime/tick interval.
            bool valid = false;
            if (g_N.gpGlobalsVar)
            {
                const auto globals = *reinterpret_cast<std::uintptr_t*>(g_N.gpGlobalsVar);
                if (globals)
                {
                    const int maxClients = *reinterpret_cast<const int*>(globals + kGlobalsMaxClients);
                    const float interval = *reinterpret_cast<const float*>(globals + kGlobalsInterval);
                    const float curtime = *reinterpret_cast<const float*>(globals + kGlobalsCurtime);
                    edict_t* edicts = *reinterpret_cast<edict_t**>(globals + kGlobalsEdictsOffset);
                    valid = maxClients == clientMax && interval > 0.001f && interval < 0.2f &&
                        std::isfinite(curtime) && curtime >= 0.0f && edicts == edictList;
                }
            }
            if (valid != g_GlobalsValidated || !valid)
                Log("gpGlobals %s (curtime/tick interval/edicts)", valid ? "validated" : "NOT validated; InstantDeploy off");
            g_GlobalsValidated = valid;
            PublishCapabilities();
            LogFeatureLine("map");
        }

        void GameFrame(bool) override { g_Session.GameFrame(); }

        void LevelShutdown() override
        {
            ResetPlayers();
            g_Session.ResetAll();
            g_Session.SetClientMax(kMaxPlayerSlots);
            g_ClientMax = kMaxPlayerSlots;
            g_EdictList = nullptr;
            g_EdictCount = 0;
        }

        void ClientActive(edict_t* entity) override
        {
            const int index = PlayerIndexFromEdict(entity);
            if (index < 0) return;
            const SessionClient* c = g_Session.Client(index);
            if (c && c->entity != entity) g_Players[static_cast<std::size_t>(index)].Reset();
            g_Session.ClientActive(index, entity);
        }

        void ClientDisconnect(edict_t* entity) override
        {
            const int index = PlayerIndexFromEdict(entity);
            if (index < 0) return;
            g_Players[static_cast<std::size_t>(index)].Reset();
            g_Session.ClientDisconnect(index, entity);
        }

        void ClientPutInServer(edict_t* entity, const char*) override
        {
            const int index = PlayerIndexFromEdict(entity);
            if (index < 0) return;
            g_Players[static_cast<std::size_t>(index)].Reset();
            g_Session.ClientPutInServer(index, entity);
        }

        void SetCommandClient(int) override {}
        void ClientSettingsChanged(edict_t*) override {}
        PLUGIN_RESULT ClientConnect(bool*, edict_t*, const char*, const char*, char*, int) override { return PLUGIN_CONTINUE; }

        PLUGIN_RESULT ClientCommand(edict_t* entity, const CCommand& args) override
        {
            const int argc = std::min(args.ArgC(), 16);
            const char* argv[16];
            for (int i = 0; i < argc; ++i) argv[i] = args.Arg(i);
            const int index = PlayerIndexFromEdict(entity);
            if (argc > 0 && args.ArgC() > 16)
                return std::strncmp(argv[0], "l4d2vr_", 7) == 0 ? PLUGIN_STOP : PLUGIN_CONTINUE;
            const CommandResult result = g_Session.ClientCommand(index, entity, argc, argv, &g_Ammo);
            return result == CommandResult::Stop ? PLUGIN_STOP : PLUGIN_CONTINUE;
        }

        PLUGIN_RESULT NetworkIDValidated(const char*, const char*) override { return PLUGIN_CONTINUE; }
        void OnQueryCvarValueFinished(QueryCvarCookie_t, edict_t*, EQueryCvarValueStatus, const char*, const char*) override {}
        void OnEdictAllocated(edict_t*) override {}

        void OnEdictFreed(const edict_t* entity) override
        {
            const int index = PlayerIndexFromEdict(entity);
            if (index < 0) return;
            const SessionClient* c = g_Session.Client(index);
            if (c && c->entity == entity) g_Players[static_cast<std::size_t>(index)].Reset();
            g_Session.EdictFreed(index, entity);
        }
    };

    L4D2VRServerPlugin g_Plugin;
}

extern "C" L4D2VR_EXPORT void* CreateInterface(const char* name, int* returnCode)
{
    if (name && std::strcmp(name, kPluginInterfaceName) == 0)
    {
        if (returnCode) *returnCode = IFACE_OK;
        return &g_Plugin;
    }
    if (returnCode) *returnCode = IFACE_FAILED;
    return nullptr;
}

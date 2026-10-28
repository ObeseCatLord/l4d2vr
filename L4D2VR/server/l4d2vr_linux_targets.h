#pragma once

// Hook/call targets of the Linux i386 server_srv.so (L4D2 2.2.4.3, build
// 10097) used by l4d2vr_server_linux.cpp, plus the evaluation shared with the
// self-test tool (tools/l4d2vr_server_symcheck.cpp). Every target is located
// by its exact mangled symbol (local .symtab symbols included) and verified
// against a recorded prologue pattern ("build guard"); `??` marks bytes that
// the loader relocates (TEXTREL absolute operands). A mismatch disables only
// the features that depend on that target.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../server_core/vr_server_protocol.h"
#include "../server_core/vr_x86_prologue.h"

namespace l4d2vr_linux
{
    enum class Strategy : std::uint8_t
    {
        Inline,        // entry jmp to a detour; trampoline from decoded prologue
        Vtable,        // replace the slot holding this function in one vtable
        AllVtables,    // replace every slot holding this function in every _ZTV*
        Call,          // called directly; guard only
        Slot,          // virtual slot index derived from a vtable (no patch)
        Data,          // data symbol (global variable / object)
    };

    // Requirement groups; features are the AND of their groups.
    enum Group : std::uint32_t
    {
        kGroupBase = 1u << 0,          // usercmd decoder + execution context
        kGroupEye = 1u << 1,           // EyePosition/EyeAngles overrides
        kGroupMelee = 1u << 2,
        kGroupAmmo = 1u << 3,          // magazine/shell native access
        kGroupReloadGate = 1u << 4,
        kGroupDrop = 1u << 5,
        kGroupThrow = 1u << 6,
        kGroupDeploy = 1u << 7,
        kGroupStatus = 1u << 8,        // survivor status accessors
    };

    struct TargetSpec
    {
        const char* label;
        const char* symbol;
        Strategy strategy;
        const char* vtable;    // Vtable/Slot strategies
        const char* guard;     // prologue pattern; nullptr = none
        std::uint32_t groups;
    };

    // Patterns were recorded from the Marmot Cafe server_srv.so
    // (BuildID c9b0cd3d1800cb19083026f349a9de15933c05ac).
    inline const std::vector<TargetSpec>& Targets()
    {
        static const std::vector<TargetSpec> targets = {
            { "ReadUsercmd", "_Z11ReadUsercmdP7bf_readP8CUserCmdS2_", Strategy::Inline, nullptr,
              "55 89 E5 57 56 53 83 EC 2C 8B 75 0C 8B 45 10 8B 5D 08", kGroupBase },
            { "FireTerrorBullets", "_Z17FireTerrorBulletsiRK6VectorRK6QAngle10CSWeaponIDiif", Strategy::Inline, nullptr,
              "55 89 E5 57 56 53 81 EC CC 00 00 00 8B 45 10 8B 75 0C", kGroupBase },
            { "ProcessUsercmds", "_ZN18CServerGameClients15ProcessUsercmdsEP7edict_tP7bf_readiiibb", Strategy::Vtable,
              "_ZTV18CServerGameClients", "55 89 E5 57 56 8D BD E8 E9 FF FF 53", kGroupBase },
            { "PlayerRunCommand", "_ZN13CTerrorPlayer16PlayerRunCommandEP8CUserCmdP11IMoveHelper", Strategy::Vtable,
              "_ZTV13CTerrorPlayer", "55 89 E5 57 56 53 83 EC 6C", kGroupBase },
            { "GetActiveWeapon", "_ZNK20CBaseCombatCharacter15GetActiveWeaponEv", Strategy::Call, nullptr,
              "55 8B 0D ?? ?? ?? ?? 89 E5 8B 55 08 8B 92 ?? ?? ?? ?? 83 FA FF", kGroupBase },
            { "entindex", "_ZNK11CBaseEntity8entindexEv", Strategy::Call, nullptr,
              "55 89 E5 8B 45 08 8B 40 30 85 C0 74 13 8B 15 ?? ?? ?? ?? 5D 2B 42 58 C1 F8 04 C3", kGroupBase },
            { "GetWeaponID(slot)", "_ZNK7CPistol11GetWeaponIDEv", Strategy::Slot, "_ZTV7CPistol", nullptr, kGroupBase },
            { "GetWeaponID(check)", "_ZNK11CRifle_AK4711GetWeaponIDEv", Strategy::Slot, "_ZTV11CRifle_AK47", nullptr, kGroupBase },
            { "GetTeamNumber", "_ZNK11CBaseEntity13GetTeamNumberEv", Strategy::Call, nullptr,
              "55 89 E5 8B 45 08 5D 8B 80 ?? ?? ?? ?? C3", kGroupBase },
            { "IsAlive", "_ZNK11CBaseEntity7IsAliveEv", Strategy::Call, nullptr,
              "55 89 E5 8B 45 08 5D 80 B8 ?? ?? ?? ?? 00 0F 94 C0 C3", kGroupBase },
            { "GetAbsOrigin", "_ZNK11CBaseEntity12GetAbsOriginEv", Strategy::Call, nullptr,
              "55 89 E5 53 83 EC 14 8B 5D 08 F6 83 ?? ?? ?? ?? 08", kGroupBase },
            { "IsIncapacitated", "_ZNK13CTerrorPlayer15IsIncapacitatedEv", Strategy::Call, nullptr,
              "55 31 C0 89 E5 8B 55 08 80 BA ?? ?? ?? ?? 00 75 07 0F B6 82 ?? ?? ?? ?? 5D C3", kGroupStatus },
            { "EyePosition", "_ZN11CBasePlayer11EyePositionEv", Strategy::Vtable, "_ZTV13CTerrorPlayer",
              "55 89 E5 57 56 53 83 EC 1C A1 ?? ?? ?? ?? 8B 5D 08 8B 75 0C", kGroupEye },
            { "EyeAngles", "_ZN11CBasePlayer9EyeAnglesEv", Strategy::Vtable, "_ZTV13CTerrorPlayer",
              "55 89 E5 57 56 53 83 EC 7C A1 ?? ?? ?? ?? 8B 5D 08", kGroupEye },
            { "Weapon_ShootPosition", "_ZN13CTerrorPlayer20Weapon_ShootPositionEv", Strategy::Vtable, "_ZTV13CTerrorPlayer",
              "55 89 E5 56 53 83 EC 10 8B 75 0C 8B 5D 08", kGroupMelee },
            { "TestMeleeSwingCollision", "_ZN18CTerrorMeleeWeapon23TestMeleeSwingCollisionERK6Vector", Strategy::Call, nullptr,
              "55 89 E5 57 56 53 81 EC 3C 02 00 00 8B 45 08 8B 75 0C", kGroupMelee },
            { "GetPrimaryAttackActivity", "_ZN18CTerrorMeleeWeapon24GetPrimaryAttackActivityEP16CMeleeWeaponInfo", Strategy::Call, nullptr,
              "55 B8 BF 00 00 00 89 E5 57 56 53 83 EC 1C 8B 75 0C 8B 5D 08", kGroupMelee },
            { "GetMeleeWeaponInfo", "_ZN21CMeleeWeaponInfoStore18GetMeleeWeaponInfoEi", Strategy::Call, nullptr,
              "55 89 E5 53 83 EC 24 8B 45 0C 8B 5D 08", kGroupMelee },
            { "g_MeleeWeaponInfoStore", "g_MeleeWeaponInfoStore", Strategy::Data, nullptr, nullptr, kGroupMelee },
            { "CTerrorGun::Reload", "_ZN10CTerrorGun6ReloadEv", Strategy::AllVtables, nullptr,
              "55 89 E5 57 56 53 83 EC 2C 8B 5D 08 89 1C 24 E8", kGroupReloadGate },
            { "CBaseShotgun::Reload", "_ZN12CBaseShotgun6ReloadEv", Strategy::AllVtables, nullptr,
              "55 89 E5 57 56 53 83 EC 2C 8B 5D 08 89 1C 24 E8", kGroupReloadGate },
            { "Weapon_Drop", "_ZN13CTerrorPlayer11Weapon_DropEP17CBaseCombatWeaponPK6VectorS4_", Strategy::Call, nullptr,
              "55 89 E5 57 56 53 83 EC 2C 8B 45 14 8B 75 08 8B 7D 0C", kGroupDrop },
            { "CMolotovProjectile::Create", "_ZN18CMolotovProjectile6CreateERK6VectorRK6QAngleS2_S2_P20CBaseCombatCharacter",
              Strategy::Inline, nullptr, "55 89 E5 57 56 53 83 EC 4C 8B 55 08 C7 04 24 ?? ?? ?? ??", kGroupThrow },
            { "CPipeBombProjectile::Create", "_ZN19CPipeBombProjectile6CreateERK6VectorRK6QAngleS2_S2_P20CBaseCombatCharacterf",
              Strategy::Inline, nullptr, "55 89 E5 57 56 53 83 EC 4C 8B 55 08 C7 04 24 ?? ?? ?? ??", kGroupThrow },
            { "CVomitJarProjectile::Create", "_ZN19CVomitJarProjectile6CreateERK6VectorRK6QAngleS2_S2_P20CBaseCombatCharacter",
              Strategy::Inline, nullptr, "55 89 E5 57 56 53 83 EC 4C 8B 55 08 C7 04 24 ?? ?? ?? ??", kGroupThrow },
            { "Clip1", "_ZNK17CBaseCombatWeapon5Clip1Ev", Strategy::Call, nullptr,
              "55 89 E5 8B 45 08 5D 8B 80 ?? ?? ?? ?? C3", kGroupAmmo },
            { "GetOwner", "_ZNK17CBaseCombatWeapon8GetOwnerEv", Strategy::Call, nullptr,
              "55 8B 0D ?? ?? ?? ?? 89 E5 8B 45 08 8B 80 ?? ?? ?? ?? 83 F8 FF", kGroupAmmo },
            { "GetMaxClip1(slot)", "_ZNK10CTerrorGun11GetMaxClip1Ev", Strategy::Slot, "_ZTV7CPistol", nullptr, kGroupAmmo },
            { "GetMaxClip1(check)", "_ZNK17CBaseCombatWeapon11GetMaxClip1Ev", Strategy::Slot, "_ZTV17CBaseCombatWeapon", nullptr, kGroupAmmo },
            { "IsDualWielding", "_ZNK10CTerrorGun14IsDualWieldingEv", Strategy::Call, nullptr,
              "55 89 E5 8B 45 08 5D 0F B6 80 ?? ?? ?? ?? C3", kGroupAmmo },
            { "GetAmmoCount", "_ZNK20CBaseCombatCharacter12GetAmmoCountEi", Strategy::Call, nullptr,
              "55 89 E5 8B 55 0C 8B 45 08 83 FA FF 74 0A 5D E9", kGroupAmmo },
            { "GetAmmoDef", "_Z10GetAmmoDefv", Strategy::Call, nullptr, "55 B8 ?? ?? ?? ?? 89 E5 5D C3", kGroupAmmo },
            { "CanCarryInfiniteAmmo", "_ZN8CAmmoDef20CanCarryInfiniteAmmoEi", Strategy::Call, nullptr,
              "55 31 C0 89 E5 53 8B 55 0C 8B 4D 08 85 D2 7E 1D", kGroupAmmo },
            { "GetChangeAccessor", "_ZN10CBaseEdict17GetChangeAccessorEv", Strategy::Call, nullptr,
              "55 89 E5 83 EC 18 A1 ?? ?? ?? ?? 8B 4D 08 8B 10 89 04 24 89 4C 24 04 FF 92", kGroupAmmo | kGroupDeploy },
            { "gpGlobals", "gpGlobals", Strategy::Data, nullptr, nullptr, kGroupDeploy },
            { "g_pServerClassHead", "g_pServerClassHead", Strategy::Data, nullptr, nullptr, kGroupAmmo | kGroupDeploy | kGroupStatus },
        };
        return targets;
    }

    // Reads code/data by runtime (plugin) or link-time (tool) address.
    class CodeView
    {
    public:
        virtual bool Symbol(const char* name, std::uint32_t& address, std::uint32_t& size) const = 0;
        virtual bool Read(std::uint32_t address, void* out, std::size_t size) const = 0;
        // Calls fn(name, address, size) for every vtable symbol (_ZTV*).
        virtual void ForEachVtable(void (*fn)(void*, const char*, std::uint32_t, std::uint32_t), void* ctx) const = 0;
    protected:
        ~CodeView() = default;
    };

    struct SlotRef { std::uint32_t slotAddress; int index; };

    struct TargetResult
    {
        const TargetSpec* spec = nullptr;
        bool ok = false;
        std::uint32_t address = 0u;
        std::uint32_t size = 0u;
        std::uint8_t bytes[32]{};
        std::size_t byteCount = 0u;
        std::size_t stolen = 0u;          // Inline
        std::vector<SlotRef> slots;       // Vtable/AllVtables/Slot
        std::string detail;
    };

    // Finds every slot of the vtable [address, address+size) equal to `fn`.
    // The first two words (offset-to-top, typeinfo) are skipped.
    inline std::vector<SlotRef> FindSlots(const CodeView& view, std::uint32_t vtable, std::uint32_t size, std::uint32_t fn)
    {
        std::vector<SlotRef> out;
        for (std::uint32_t off = 8u; off + 4u <= size; off += 4u)
        {
            std::uint32_t value = 0u;
            if (!view.Read(vtable + off, &value, 4u)) break;
            if (value == fn) out.push_back({ vtable + off, static_cast<int>((off - 8u) / 4u) });
        }
        return out;
    }

    inline TargetResult EvaluateTarget(const CodeView& view, const TargetSpec& spec)
    {
        TargetResult r{};
        r.spec = &spec;
        if (!view.Symbol(spec.symbol, r.address, r.size))
        {
            r.detail = "symbol not found or ambiguous";
            return r;
        }
        if (spec.strategy == Strategy::Data)
        {
            r.ok = r.address != 0u;
            r.detail = "data";
            return r;
        }
        r.byteCount = sizeof(r.bytes);
        if (!view.Read(r.address, r.bytes, r.byteCount))
        {
            r.byteCount = 0u;
            r.detail = "code not readable";
            return r;
        }
        if (spec.guard)
        {
            const l4d2vr_x86::Pattern guard = l4d2vr_x86::Pattern::Parse(spec.guard);
            if (guard.size() == 0u || !guard.Matches(r.bytes, r.byteCount))
            {
                r.detail = std::string("prologue differs from build guard ") + spec.guard;
                return r;
            }
        }
        switch (spec.strategy)
        {
        case Strategy::Inline:
        {
            const l4d2vr_x86::Plan plan = l4d2vr_x86::PlanHook(r.bytes, r.byteCount, 5u);
            const std::size_t guarded = spec.guard ? l4d2vr_x86::Pattern::Parse(spec.guard).size() : 0u;
            if (!plan.ok) { r.detail = "decoder refused: " + plan.error; return r; }
            if (plan.stolen > guarded) { r.detail = "stolen bytes exceed the guarded prologue"; return r; }
            r.stolen = plan.stolen;
            char buffer[128];
            std::snprintf(buffer, sizeof(buffer), "inline jmp, %zu stolen bytes, %zu rel32 fixups", plan.stolen, plan.relFields.size());
            r.detail = buffer;
            r.ok = true;
            return r;
        }
        case Strategy::Vtable:
        case Strategy::Slot:
        {
            std::uint32_t vt = 0u, vtSize = 0u;
            if (!spec.vtable || !view.Symbol(spec.vtable, vt, vtSize) || vtSize < 12u)
            {
                r.detail = "vtable symbol missing";
                return r;
            }
            r.slots = FindSlots(view, vt, vtSize, r.address);
            if (r.slots.size() != 1u)
            {
                char buffer[160];
                std::snprintf(buffer, sizeof(buffer), "expected exactly one slot in %s, found %zu", spec.vtable, r.slots.size());
                r.detail = buffer;
                return r;
            }
            char buffer[160];
            std::snprintf(buffer, sizeof(buffer), "%s slot %d", spec.strategy == Strategy::Vtable ? "vtable" : "virtual",
                r.slots[0].index);
            r.detail = buffer;
            r.ok = true;
            return r;
        }
        case Strategy::AllVtables:
        {
            struct Ctx { const CodeView* view; std::uint32_t fn; std::vector<SlotRef>* out; };
            Ctx ctx{ &view, r.address, &r.slots };
            view.ForEachVtable([](void* p, const char*, std::uint32_t address, std::uint32_t size)
            {
                Ctx& c = *static_cast<Ctx*>(p);
                const auto found = FindSlots(*c.view, address, size, c.fn);
                c.out->insert(c.out->end(), found.begin(), found.end());
            }, &ctx);
            if (r.slots.empty()) { r.detail = "no vtable references"; return r; }
            int index = r.slots[0].index;
            for (const SlotRef& s : r.slots)
                if (s.index != index) index = -1;
            char buffer[160];
            std::snprintf(buffer, sizeof(buffer), "%zu vtable slots (index %d)", r.slots.size(), index);
            r.detail = buffer;
            r.ok = true;
            return r;
        }
        case Strategy::Call:
            r.ok = true;
            r.detail = "direct call";
            return r;
        default:
            r.detail = "unknown strategy";
            return r;
        }
    }

    inline const TargetResult* FindResult(const std::vector<TargetResult>& results, const char* label)
    {
        for (const TargetResult& r : results)
            if (r.spec && std::strcmp(r.spec->label, label) == 0) return &r;
        return nullptr;
    }

    // Groups whose every target resolved.
    inline std::uint32_t ResolvedGroups(const std::vector<TargetResult>& results)
    {
        std::uint32_t failed = 0u, seen = 0u;
        for (const TargetResult& r : results)
        {
            if (!r.spec) continue;
            seen |= r.spec->groups;
            if (!r.ok) failed |= r.spec->groups;
        }
        // Both GetWeaponID slots must agree, likewise both GetMaxClip1 slots.
        const TargetResult* a = FindResult(results, "GetWeaponID(slot)");
        const TargetResult* b = FindResult(results, "GetWeaponID(check)");
        if (!a || !b || !a->ok || !b->ok || a->slots[0].index != b->slots[0].index) failed |= kGroupBase;
        const TargetResult* c = FindResult(results, "GetMaxClip1(slot)");
        const TargetResult* d = FindResult(results, "GetMaxClip1(check)");
        if (!c || !d || !c->ok || !d->ok || c->slots[0].index != d->slots[0].index) failed |= kGroupAmmo;
        return seen & ~failed;
    }

    // Offsets the melee sweep needs that are not network properties, read out of
    // CTerrorMeleeWeapon::TestMeleeSwingCollision's own instructions:
    //   mov eax,[ebp+8]; mov eax,[eax+INFO]; mov [esp],&g_MeleeWeaponInfoStore;
    //   mov [esp+4],eax; call CMeleeWeaponInfoStore::GetMeleeWeaponInfo
    //   mov edx,[eax+HITCOUNT]; test edx,edx; jle ..; mov ecx,[eax+HITLIST]
    // HITLIST/HITCOUNT are the CUtlVector of entities already hit this swing
    // (m_Memory.m_pMemory and m_Size, 12 bytes apart).
    struct MeleeOperands
    {
        bool ok = false;
        std::uint32_t infoIndexOffset = 0u;
        std::uint32_t hitCountOffset = 0u;
        std::uint32_t hitListOffset = 0u;
        std::string detail;
    };

    inline MeleeOperands ExtractMeleeOperands(const CodeView& view, const std::vector<TargetResult>& results)
    {
        MeleeOperands m{};
        const TargetResult* test = FindResult(results, "TestMeleeSwingCollision");
        const TargetResult* store = FindResult(results, "g_MeleeWeaponInfoStore");
        const TargetResult* getter = FindResult(results, "GetMeleeWeaponInfo");
        if (!test || !test->ok || !store || !store->ok || !getter || !getter->ok)
        {
            m.detail = "melee targets unresolved";
            return m;
        }
        const std::uint32_t scan = test->size > 0u && test->size < 0x4000u ? test->size : 0x1000u;
        std::vector<std::uint8_t> code(scan);
        if (!view.Read(test->address, code.data(), code.size()))
        {
            m.detail = "TestMeleeSwingCollision body unreadable";
            return m;
        }
        const auto info = l4d2vr_x86::Pattern::Parse("8B 45 08 8B 80 ?? ?? ?? ?? C7 04 24 ?? ?? ?? ?? 89 44 24 04 E8");
        const long at = info.Find(code.data(), std::min<std::size_t>(code.size(), 0x80u));
        if (at < 0)
        {
            m.detail = "melee info load not found";
            return m;
        }
        const std::size_t a = static_cast<std::size_t>(at);
        const std::uint32_t offset = static_cast<std::uint32_t>(l4d2vr_x86::ReadI32(&code[a + 5]));
        const std::uint32_t storeImm = static_cast<std::uint32_t>(l4d2vr_x86::ReadI32(&code[a + 12]));
        const std::uint32_t callNext = test->address + static_cast<std::uint32_t>(a + 25u);
        const std::uint32_t callTarget = callNext + static_cast<std::uint32_t>(l4d2vr_x86::ReadI32(&code[a + 21]));
        if (storeImm != store->address || callTarget != getter->address || offset == 0u || offset > 0x10000u)
        {
            m.detail = "melee info load operands do not match the resolved symbols";
            return m;
        }
        const auto hits = l4d2vr_x86::Pattern::Parse("8B 90 ?? ?? ?? ?? 85 D2 7E ?? 8B 88 ?? ?? ?? ??");
        const long hit = hits.Find(code.data(), code.size());
        if (hit < 0)
        {
            m.detail = "hit list access not found";
            return m;
        }
        const std::size_t h = static_cast<std::size_t>(hit);
        const std::uint32_t count = static_cast<std::uint32_t>(l4d2vr_x86::ReadI32(&code[h + 2]));
        const std::uint32_t list = static_cast<std::uint32_t>(l4d2vr_x86::ReadI32(&code[h + 12]));
        if (count != list + 12u || count == 0u || count > 0x10000u)
        {
            m.detail = "hit list operands are not a CUtlVector";
            return m;
        }
        m.ok = true;
        m.infoIndexOffset = offset;
        m.hitCountOffset = count;
        m.hitListOffset = list;
        char buffer[128];
        std::snprintf(buffer, sizeof(buffer), "info index +0x%X, hit list +0x%X, hit count +0x%X", offset, list, count);
        m.detail = buffer;
        return m;
    }

    // Feature bits from target groups. `netprops` are the runtime-resolved
    // netprop groups (the tool assumes all).
    struct FeatureInputs
    {
        std::uint32_t groups = 0u;
        bool netpropStatus = false;   // SI/mounted netprops
        bool netpropAmmo = false;     // clip/ammo/reload netprops
        bool netpropShell = false;    // m_reloadState
        bool netpropDeploy = false;   // next attack netprops + curtime
        bool meleeOperands = false;   // code-derived melee offsets
        bool netpropGrenade = false;  // CBaseCSGrenade pin/throw-time/idle + player velocity
    };

    struct FeatureResolution
    {
        bool usercmdDecoder = false;
        bool interaction = false;
        std::uint32_t features = 0u;
    };

    inline FeatureResolution ResolveFeatures(const FeatureInputs& in)
    {
        using namespace l4d2vr_server;
        FeatureResolution out{};
        const auto has = [&](std::uint32_t g) { return (in.groups & g) == g; };
        out.features |= kFeaturePoseV3;
        if (!has(kGroupBase))
            return out;
        out.usercmdDecoder = true;
        const bool status = has(kGroupStatus) && in.netpropStatus;
        out.features |= kFeatureAkimboRays;
        if (has(kGroupEye)) out.features |= kFeatureShoveAim | kFeaturePillGive;
        if (has(kGroupMelee) && status && in.meleeOperands) out.features |= kFeatureMeleeSweepV2;
        const bool ammo = has(kGroupAmmo | kGroupReloadGate) && status && in.netpropAmmo;
        if (ammo) out.features |= kFeatureMagTxn;
        if (ammo && in.netpropShell) out.features |= kFeatureShellTxn;
        if (has(kGroupDrop) && status) out.features |= kFeatureHolsterDrop;
        if (has(kGroupThrow)) out.features |= kFeatureThrowVelocity;
        if (has(kGroupDeploy) && in.netpropDeploy) out.features |= kFeatureInstantDeploy;
        // Hand throws drive the grenade's own throw state machine (netprops,
        // curtime) and need the projectile Create hooks for position/velocity.
        if (has(kGroupThrow | kGroupDeploy) && in.netpropDeploy && in.netpropGrenade && status)
            out.features |= kFeatureHandThrow;
        out.interaction = has(kGroupReloadGate | kGroupDrop | kGroupEye) && status;
        return out;
    }
}

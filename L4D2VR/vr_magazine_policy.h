#pragma once

#include <cstdint>
#include <array>
#include <algorithm>
#include <cmath>

namespace l4d2vr_magazine
{
    constexpr unsigned kAuthoritativeMagazineInputMode = 64u;
    constexpr unsigned kAuthoritativeShellInputMode = 128u;
    constexpr unsigned kAuthoritativePistolInputMode = 256u;
    constexpr float kMinimumBoltPullSourceUnits = 0.001f;
    inline float ResolveBoltPullDistanceMeters(float configured, bool pump, float sourceUnitsPerMeter)
    {
        const float fallback = pump ? 0.09f : 0.055f;
        if (!std::isfinite(configured) || configured <= 0.0f) return fallback;
        const float bounded = std::min(configured, 0.25f);
        const float stroke = bounded * sourceUnitsPerMeter;
        return std::isfinite(stroke) && stroke > kMinimumBoltPullSourceUnits ? bounded : fallback;
    }
    inline bool TrackingAllowsGesture(bool hmd, bool left, bool right,
        std::uint64_t captured, std::uint64_t now)
    {
        return hmd && left && right && captured != 0u && now >= captured && now - captured <= 150u;
    }
    inline bool UseNativeReloadFallback(bool enabled, int weaponId, bool physicalReload, bool negotiated)
    {
        // The launcher reloads one round through its stock native mechanism.
        return enabled && (physicalReload || weaponId == 21) && !negotiated;
    }
    inline bool UseAuthoritativeMagazine(bool supported, bool detachable, bool nativeDual, bool leftHandPistol)
    {
        return supported && detachable && !nativeDual && !leftHandPistol;
    }
    inline bool WantsNativeReload(bool buttonOrGesture, bool manualBackend, bool explicitBackendReload)
    {
        return explicitBackendReload || (buttonOrGesture && !manualBackend);
    }
    class NativeFallbackPulse
    {
    public:
        void Reset() { m_Active = false; m_HaveScope = false; }
        bool Update(bool eligible, bool pressed, std::uint32_t generation, std::uint32_t now)
        {
            if (!m_HaveScope || generation != m_Generation)
            { m_Active = false; m_Generation = generation; m_HaveScope = true; }
            if (!eligible) { m_Active = false; return false; }
            if (pressed) { m_Started = now; m_Active = true; }
            if (m_Active && now - m_Started >= 350u) m_Active = false;
            return m_Active;
        }
    private:
        bool m_Active = false, m_HaveScope = false;
        std::uint32_t m_Generation = 0u, m_Started = 0u;
    };
    // Prediction can refill a clip before a physical slide has been cycled.
    // Retain the empty chamber across inventory switches, using native handle
    // serials to distinguish a replacement entity at the same address.
    class ChamberHistory
    {
    public:
        void ObserveOwner(bool active, std::uintptr_t owner)
        {
            if (!active || !owner || owner != m_Owner)
            {
                m_Entries = {}; m_Next = 0u; m_ActiveWeapon = 0u; m_ActiveHandle = 0u;
                m_Owner = active ? owner : 0u;
            }
        }
        bool Observe(std::uintptr_t weapon, std::uint32_t handle, int weaponId, int clip)
        {
            return Observe(weapon, handle, weaponId, clip, -1, true, 0u);
        }
        // The client predicts the last shot. When the host has not fired it
        // yet and the trigger is let go, the host never fires it: the clip
        // comes back above zero with the reserve untouched and nothing
        // reloading. That round is still chambered, so the latch drops; a
        // rack must not seem to load it. Any reload activity (or a reserve
        // change) while the zero is provisional makes the latch permanent.
        static constexpr std::uint64_t kHostOverturnWindowMs = 1000u;
        bool Observe(std::uintptr_t weapon, std::uint32_t handle, int weaponId, int clip,
            int reserve, bool refillPossible, std::uint64_t nowMs)
        {
            if (!m_Owner) return false;
            if (weapon != m_ActiveWeapon || handle != m_ActiveHandle)
                for (auto& entry : m_Entries)
                    if (entry.handle == 0u) entry = {}; // no serial: retain only the current draw
            m_ActiveWeapon = weapon; m_ActiveHandle = handle;
            if (!weapon || weaponId <= 0 || clip < 0) return false;
            Entry* selected = nullptr;
            for (auto& entry : m_Entries)
                if (entry.weapon == weapon && entry.handle == handle && entry.weaponId == weaponId)
                { selected = &entry; break; }
            if (!selected)
            {
                selected = &m_Entries[m_Next++ % m_Entries.size()];
                *selected = {weapon, handle, weaponId, false};
            }
            if (clip == 0)
            {
                if (!selected->empty)
                {
                    selected->empty = true;
                    selected->provisional = reserve >= 0 && !refillPossible;
                    selected->emptyReserve = reserve;
                    selected->emptyAt = nowMs;
                }
            }
            if (selected->provisional &&
                (refillPossible || reserve != selected->emptyReserve || nowMs < selected->emptyAt ||
                    nowMs - selected->emptyAt > kHostOverturnWindowMs))
                selected->provisional = false;
            if (clip > 0 && selected->provisional)
                selected->empty = selected->provisional = false;
            return selected->empty;
        }
        bool HasEmptyChamber(std::uintptr_t owner, std::uintptr_t weapon, std::uint32_t handle, int weaponId) const
        {
            if (!owner || owner != m_Owner || !weapon || !handle || weaponId <= 0) return false;
            for (const auto& entry : m_Entries)
                if (entry.weapon == weapon && entry.handle == handle && entry.weaponId == weaponId)
                    return entry.empty;
            return false;
        }
        void Complete(std::uintptr_t weapon)
        {
            if (weapon != m_ActiveWeapon) return;
            for (auto& entry : m_Entries)
                if (entry.weapon == weapon && entry.handle == m_ActiveHandle) entry.empty = entry.provisional = false;
        }
        void Forget(std::uintptr_t weapon)
        {
            for (auto& entry : m_Entries) if (entry.weapon == weapon) entry = {};
            if (m_ActiveWeapon == weapon) { m_ActiveWeapon = 0u; m_ActiveHandle = 0u; }
        }
    private:
        struct Entry
        {
            std::uintptr_t weapon = 0u;
            std::uint32_t handle = 0u;
            int weaponId = 0;
            bool empty = false;
            bool provisional = false; // emptied by a predicted shot the host may still overturn
            int emptyReserve = -1;
            std::uint64_t emptyAt = 0u;
        };
        std::array<Entry, 16> m_Entries{};
        unsigned m_Next = 0u;
        std::uintptr_t m_Owner = 0u, m_ActiveWeapon = 0u;
        std::uint32_t m_ActiveHandle = 0u;
    };

    // Pavlov-style release: a gun's magazine drops out once. Until a magazine
    // is seated again the release button does nothing, across session resets
    // and weapon switches. Native ammo above the rounds left in the gun after
    // the ejection (a stock reload finished, or a host refill) means a
    // magazine is seated again.
    class MagazineSeatHistory
    {
    public:
        void ObserveOwner(std::uintptr_t owner)
        {
            // Incapacitation or a brief loss of interaction keeps the history.
            if (!owner || owner == m_Owner) return;
            m_Entries = {}; m_Next = 0u; m_ActiveWeapon = 0u; m_ActiveHandle = 0u;
            m_Owner = owner;
        }
        void Observe(std::uintptr_t weapon, std::uint32_t handle, int weaponId, int clip)
        {
            if (!m_Owner) return;
            if (weapon != m_ActiveWeapon || handle != m_ActiveHandle)
                for (auto& entry : m_Entries)
                    if (entry.handle == 0u) entry = {}; // no serial: retain only the current draw
            m_ActiveWeapon = weapon; m_ActiveHandle = handle;
            Entry* const entry = Find(weapon, handle, weaponId);
            if (!entry || clip < 0) return;
            if (clip > entry->clip) *entry = {};
            else entry->clip = clip; // a spent chamber round is not a refill
        }
        void Eject(std::uintptr_t weapon, std::uint32_t handle, int weaponId, int clipAfter)
        {
            if (!m_Owner || !weapon || weaponId <= 0) return;
            Entry* entry = Find(weapon, handle, weaponId);
            if (!entry) entry = &m_Entries[m_Next++ % m_Entries.size()];
            *entry = {weapon, handle, weaponId, std::max(0, clipAfter)};
        }
        void Seat(std::uintptr_t weapon, std::uint32_t handle, int weaponId)
        {
            if (Entry* const entry = Find(weapon, handle, weaponId)) *entry = {};
        }
        bool Ejected(std::uintptr_t owner, std::uintptr_t weapon, std::uint32_t handle, int weaponId) const
        {
            if (!owner || owner != m_Owner || !weapon || weaponId <= 0) return false;
            for (const auto& entry : m_Entries)
                if (entry.weapon == weapon && entry.handle == handle && entry.weaponId == weaponId) return true;
            return false;
        }
    private:
        struct Entry
        {
            std::uintptr_t weapon = 0u;
            std::uint32_t handle = 0u;
            int weaponId = 0;
            int clip = 0;
        };
        Entry* Find(std::uintptr_t weapon, std::uint32_t handle, int weaponId)
        {
            if (!weapon || weaponId <= 0) return nullptr;
            for (auto& entry : m_Entries)
                if (entry.weapon == weapon && entry.handle == handle && entry.weaponId == weaponId) return &entry;
            return nullptr;
        }
        std::array<Entry, 16> m_Entries{};
        unsigned m_Next = 0u;
        std::uintptr_t m_Owner = 0u, m_ActiveWeapon = 0u;
        std::uint32_t m_ActiveHandle = 0u;
    };

    inline bool ShellSettlementPending(std::uint64_t now, std::uint64_t expires)
    {
        return expires != 0u && now < expires;
    }

    inline bool ShotgunBlocksFire(bool chamberEmpty, bool shellHeld,
        bool cycling, bool backendReload, bool ammoSettlement)
    {
        return chamberEmpty || shellHeld || cycling || backendReload || ammoSettlement;
    }
    inline bool ShouldResumeMagazineCycle(bool supported, bool activeSession, bool retainedEmpty, int clip)
    { return supported && !activeSession && retainedEmpty && clip > 0; }
    inline bool MayFinishShotgunLoading(bool shotgun, bool chamberEmpty, int clip, int shellsLoaded, bool gripDown)
    { return shotgun && chamberEmpty && gripDown && (clip > 0 || shellsLoaded > 0); }
    inline bool PumpShouldStartAfterShot(bool needsCycle, int clip)
    {
        // Empty tubes must accept shells first. Once the tube holds a shell
        // the pump waits to be cycled; shells can still be loaded meanwhile
        // (the waiting pump never takes the shells away from the off hand).
        return needsCycle && clip > 0;
    }
    // Fresh ammo comes from the pouch only while the hand is at the pouch and
    // not on the gun's bolt, slide or pump: racking next to the pouch is a rack.
    inline bool MayTakeFreshAmmo(float pouchDistance, float pouchRange, float boltDistance, float boltRange)
    {
        if (!std::isfinite(pouchDistance) || !std::isfinite(pouchRange) || pouchDistance < 0.0f ||
            pouchRange < 0.0f || pouchDistance > pouchRange)
            return false;
        const bool onBolt = std::isfinite(boltDistance) && std::isfinite(boltRange) && boltDistance >= 0.0f &&
            boltDistance <= boltRange && boltDistance <= pouchDistance;
        return !onBolt;
    }
    // A fresh magazine or shell seats only after it has been outside the well
    // since it was taken, so ammo taken with the hand already at the gun never
    // loads in the same motion.
    class FreshInsertGate
    {
    public:
        void Reset() { m_SeenOutside = false; }
        bool Update(bool fitsSocket)
        {
            if (!fitsSocket) m_SeenOutside = true;
            return fitsSocket && m_SeenOutside;
        }
    private:
        bool m_SeenOutside = false;
    };

    class NativeReloadLedger
    {
    public:
        void Observe(int command, std::uintptr_t weapon, bool block, bool preserve = false,
            std::uintptr_t owner = 0u, unsigned serial = 0u)
        {
            if (owner != m_Owner || serial != m_Serial)
            {
                m_Owner = owner; m_Serial = serial; m_Command = 0; m_Weapon = 0u;
            }
            if (command <= m_Command) return;
            m_Command = command;
            if (preserve)
            {
                if (m_Weapon != weapon) m_Weapon = 0u;
                return;
            }
            m_Weapon = block ? weapon : 0u;
        }
        bool Blocks(std::uintptr_t weapon) const { return weapon != 0u && m_Weapon == weapon; }
    private:
        int m_Command = 0;
        std::uintptr_t m_Weapon = 0u;
        std::uintptr_t m_Owner = 0u;
        unsigned m_Serial = 0u;
    };
    inline bool IsLocalPlayerCommand(int playerIndex, int localPlayerIndex)
    {
        return playerIndex > 0 && localPlayerIndex > 0 && playerIndex == localPlayerIndex;
    }

    inline bool MayCommitAmmo(int expectedWeaponId, int serverWeaponId, int clientWeaponId,
        std::uint32_t queuedGeneration, std::uint32_t currentGeneration)
    {
        return expectedWeaponId > 0 && expectedWeaponId == serverWeaponId &&
            expectedWeaponId == clientWeaponId && queuedGeneration == currentGeneration;
    }

    class SessionTracker
    {
    public:
        std::uintptr_t OwnerTag() const { return m_Ready ? m_Player : 0u; }
        // Compare opaque identities without dereferencing objects that may
        // already have been deleted. Completion of a reload is not a boundary.
        bool Observe(bool ready, std::uintptr_t player, std::uintptr_t weapon,
            int weaponId, unsigned int inputMode, std::uint32_t handle = 0u)
        {
            ready = ready && player != 0u && weapon != 0u && weaponId > 0;
            if (!ready)
            {
                const bool changed = m_Ready;
                m_Ready = false;
                m_Player = m_Weapon = 0u;
                m_WeaponId = 0;
                m_InputMode = 0u;
                m_Handle = 0u;
                return changed;
            }
            const bool changed = !m_Ready || player != m_Player || weapon != m_Weapon ||
                weaponId != m_WeaponId || inputMode != m_InputMode || handle != m_Handle;
            m_Ready = true;
            m_Player = player;
            m_Weapon = weapon;
            m_WeaponId = weaponId;
            m_InputMode = inputMode;
            m_Handle = handle;
            return changed;
        }

    private:
        bool m_Ready = false;
        std::uintptr_t m_Player = 0u;
        std::uintptr_t m_Weapon = 0u;
        int m_WeaponId = 0;
        unsigned int m_InputMode = 0u;
        std::uint32_t m_Handle = 0u;
    };

    inline bool ShouldEject(bool buttonRequired, bool buttonPressed,
        bool suppressEmptyAutoReload, int clip, bool detachable, bool magazineOut = false)
    {
        // A magazine that is already out stays out until the next insertion;
        // pressing release again drops nothing.
        if (!detachable || magazineOut)
            return false;
        return buttonRequired ? buttonPressed : (suppressEmptyAutoReload && clip == 0);
    }

    // Pavlov-style release: detachable magazines drop with the release button,
    // except rifles whose magazine is rocked out by hand (the AK-47, id 26).
    inline bool MagazineRemovedByHand(int weaponId)
    {
        return weaponId == 26;
    }

    inline bool ReleaseButtonRequiredFor(bool configured, int weaponId)
    {
        return configured && !MagazineRemovedByHand(weaponId);
    }

    inline int ChamberRoundsAfterEject(int clip)
    {
        // Reserve ammunition cannot add a round to an empty chamber or
        // remove the round already loaded in a nonempty weapon.
        return clip > 0 ? 1 : 0;
    }
    inline bool ChamberReadyAfterEject(bool observedEmpty, int nativeRetainedClip)
    {
        // Source can refill its clip before the physical slide is cycled.
        // A native count confirms ammunition, not completion of that cycle.
        return !observedEmpty && nativeRetainedClip > 0;
    }
    inline bool MayCatchEjectedMagazine(bool gripDown, bool tracked, float distance, float padding)
    {
        return gripDown && tracked && std::isfinite(distance) && std::isfinite(padding) &&
            distance >= 0.0f && padding >= 0.0f && distance <= padding;
    }
    class RetainedInsertGate
    {
    public:
        void Reset() { m_Withdrawn = false; }
        bool Update(bool fitsSocket, float travel, float required)
        {
            if (!std::isfinite(travel) || !std::isfinite(required) || travel < 0.0f || required <= 0.0f) return false;
            if (!fitsSocket && travel >= required) m_Withdrawn = true;
            return m_Withdrawn && fitsSocket;
        }
    private:
        bool m_Withdrawn = false;
    };
}

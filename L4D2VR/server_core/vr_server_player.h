#pragma once

// Per-player server-side VR state. std-only.
//
// Decode and execution are decoupled in Source: CServerGameClients::
// ProcessUsercmds decodes every command of a packet (including backups), and
// the commands execute later through CBasePlayer::PlayerRunCommand. The
// per-command ring stores the decoded pose and intents keyed by command_number
// so execution-time hooks (bullets, eye overrides, throws, melee) use exactly
// the pose that belongs to the executing command (akimbo per-shot hands).

#include <array>
#include <cstdint>

#include "../vr_magazine_policy.h"
#include "vr_server_limits.h"
#include "vr_server_melee.h"
#include "vr_server_throw.h"
#include "vr_usercmd_codec.h"

namespace l4d2vr_server
{
    struct CommandPose
    {
        int command = 0;
        int tick = 0;
        bool poseValid = false;
        bool meleeSwing = false;
        Vec3 position{};
        Angles angles{};
        Intent intent = Intent::None;
        unsigned carryWord = 0u;
        int carryWeaponId = 0;
        l4d2vr_dual::Hand shotHand = l4d2vr_dual::Hand::None;
        l4d2vr_dual::Hand dropHand = l4d2vr_dual::Hand::None;
        l4d2vr_server_pistol::ReloadPolicy reload = l4d2vr_server_pistol::ReloadPolicy::Native;
        bool attackDown = false;
        bool useDown = false;
        bool inventoryRelease = false;
        l4d2vr_hand_throw::Payload handThrow{};
        std::uint64_t decodedAtMs = 0u;
    };

    class CommandPoseRing
    {
    public:
        static constexpr std::size_t kSize = 150u;   // CUserCmd history depth used by the client

        // First decoded copy of a command wins: backups re-sent in later packets
        // must not replace the input the command was first decoded with.
        bool Store(const CommandPose& pose)
        {
            if (pose.command <= 0)
                return false;
            if (m_Latest > 0 && static_cast<long long>(m_Latest) - pose.command >= static_cast<long long>(kSize))
                return false;
            CommandPose& slot = m_Entries[static_cast<std::size_t>(pose.command) % kSize];
            if (slot.command == pose.command)
                return false;
            slot = pose;
            if (pose.command > m_Latest)
                m_Latest = pose.command;
            return true;
        }

        const CommandPose* Find(int command) const
        {
            if (command <= 0)
                return nullptr;
            const CommandPose& slot = m_Entries[static_cast<std::size_t>(command) % kSize];
            return slot.command == command ? &slot : nullptr;
        }

        int Latest() const { return m_Latest; }
        void Reset() { m_Entries = {}; m_Latest = 0; }

    private:
        std::array<CommandPose, kSize> m_Entries{};
        int m_Latest = 0;
    };

    // Reason codes for an execution-time eye override (log names mirror the
    // listen host's ServerControllerAimReasonName).
    enum class AimReason : int { None = 0, Throw = 3, Mounted = 4, Use = 8, Shove = 9, PillGive = 10 };

    inline const char* AimReasonName(AimReason r)
    {
        switch (r)
        {
        case AimReason::Throw: return "throw-grace";
        case AimReason::Mounted: return "mounted";
        case AimReason::Use: return "use";
        case AimReason::Shove: return "shove";
        case AimReason::PillGive: return "pill-give";
        default: return "none";
        }
    }

    struct PendingThrow
    {
        bool valid = false;
        bool hand = false;            // HandThrow: spawn in the release command itself
        std::uint64_t decodedAtMs = 0u;
        Vec3 handVelocity{};          // client-measured, before scaling
        Vec3 playerVelocity{};
        int weaponId = 0;
        int releaseCommand = 0;
        int executedSinceRelease = 0;
        std::uintptr_t owner = 0u;
        ThrowRelease release{};
    };

    struct PendingDrop
    {
        bool valid = false;
        bool holster = false;         // impulse 237 (active weapon)
        bool gripRelease = false;     // impulse 235: no toss velocity
        int weaponId = 0;             // expected active weapon id (inventory release)
        int command = 0;
        Vec3 velocity{};
        Vec3 origin{};                // reach-clamped hand position at the release
        bool haveOrigin = false;
    };

    struct PlayerVRState
    {
        // Latest decoded state (decode time).
        bool isUsingVR = false;
        bool haveViewYaw = false;
        float viewYaw = 0.0f;
        std::uint64_t lastDecodedMs = 0u;
        std::uint32_t lastDecodedCommand = 0u;
        Vec3 latestPos{};
        Angles latestAngles{};
        bool latestPoseValid = false;
        std::uint32_t vrCommandCount = 0u;

        CommandPoseRing commands{};
        ThrowPoseRing throwRing{};

        // Execution-time state.
        int lastExecutedCommand = 0;
        ThrowableReleaseTracker throwTracker{};
        PendingThrow pendingThrow{};
        PendingDrop pendingDrop{};
        int throwAimRemaining = 0;
        int shoveAimRemaining = 0;
        int pillAimRemaining = 0;
        MeleeSweepState melee{};
        l4d2vr_magazine::NativeReloadLedger reloadLedger{};
        MinIntervalLimiter holsterLimiter{};
        MinIntervalLimiter shoveLimiter{};
        MinIntervalLimiter pillLimiter{};
        MinIntervalLimiter handThrowLimiter{};
        IntentBudget intentBudget{};
        std::uint32_t reachClamps = 0u;
        std::uintptr_t lastActiveWeapon = 0u; // active item after the last executed command

        void Reset() { *this = PlayerVRState{}; }

        bool InputFresh(std::uint64_t nowMs, std::uint64_t maxAgeMs = 2500u) const
        {
            return isUsingVR && lastDecodedMs != 0u && nowMs >= lastDecodedMs && nowMs - lastDecodedMs < maxAgeMs;
        }
    };

    // Records one decoded VR command into the player's state. `playerOrigin`
    // (optional) gives the throw ring a locomotion-free relative position.
    inline void RecordDecodedCommand(PlayerVRState& p, const UsercmdData& stripped, const DecodedUsercmd& d,
        bool haveOrigin, const Vec3& playerOrigin, std::uint64_t nowMs)
    {
        if (!d.vr)
        {
            // A plain command ends any VR session for this player.
            if (p.isUsingVR)
            {
                p.isUsingVR = false;
                p.throwRing.Clear();
                p.pendingThrow = {};
                p.pendingDrop = {};
                p.throwAimRemaining = p.shoveAimRemaining = p.pillAimRemaining = 0;
            }
            return;
        }
        p.isUsingVR = true;
        ++p.vrCommandCount;
        p.lastDecodedMs = nowMs;
        if (std::isfinite(stripped.viewangles[1]))
        {
            p.viewYaw = stripped.viewangles[1];
            p.haveViewYaw = true;
        }
        if (stripped.command_number > 0 && static_cast<std::uint32_t>(stripped.command_number) > p.lastDecodedCommand)
            p.lastDecodedCommand = static_cast<std::uint32_t>(stripped.command_number);

        const bool poseValid = d.poseValid && SaneControllerAngles(d.controllerAngles);
        if (poseValid)
        {
            p.latestPos = d.controllerPos;
            p.latestAngles = d.controllerAngles;
            p.latestPoseValid = true;
            const Vec3 relative = haveOrigin ? d.controllerPos - playerOrigin : Vec3{};
            p.throwRing.Record(stripped.tick_count, d.controllerPos, relative, haveOrigin, d.controllerAngles);
        }

        CommandPose pose{};
        pose.command = stripped.command_number;
        pose.tick = stripped.tick_count;
        pose.poseValid = poseValid;
        pose.meleeSwing = d.meleeSwing;
        pose.position = d.controllerPos;
        pose.angles = d.controllerAngles;
        pose.intent = d.intent;
        pose.carryWord = d.carryWord;
        pose.carryWeaponId = d.carryWeaponId;
        pose.shotHand = d.shotHand;
        pose.dropHand = d.dropHand;
        pose.reload = d.reload;
        pose.attackDown = d.attackDown;
        pose.useDown = d.useDown;
        pose.inventoryRelease = IsInventoryRelease(d);
        pose.handThrow = d.intent == Intent::HandThrow ? d.handThrow : l4d2vr_hand_throw::Payload{};
        pose.decodedAtMs = nowMs;
        p.commands.Store(pose);
    }
}

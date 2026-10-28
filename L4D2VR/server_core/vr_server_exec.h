#pragma once

// Execution-time decisions for one VR command (CBasePlayer::PlayerRunCommand).
// The shell gathers engine facts into ExecInput/PostInput, applies the
// returned context (eye override, extra buttons) while the original command
// runs, and performs the returned post actions (melee traces, drops, instant
// deploy) afterwards. std-only so the policy is unit-tested offline.

#include <cstdint>

#include "vr_server_limits.h"
#include "vr_server_math.h"
#include "vr_server_player.h"
#include "vr_server_protocol.h"

namespace l4d2vr_server
{
    struct ExecInput
    {
        std::uint32_t features = 0u;       // effective feature bits for this player
        bool interaction = false;          // grip pickup aim / reload gate available
        bool eyeHooks = false;             // EyePosition/EyeAngles overrides installed
        std::uintptr_t owner = 0u;
        std::uintptr_t weaponTag = 0u;     // active weapon before the command
        int weaponId = 0;
        bool liveSurvivor = false;
        bool incapacitated = false;
        bool controlled = false;           // pinned by special infected
        bool usingMounted = false;
        bool haveEye = false;
        Vec3 eye{};                        // native EyePosition (not overridden)
        std::uint64_t nowMs = 0u;
        float tickSeconds = 1.0f / 30.0f;
        bool handThrowBackend = false;     // grenade netprops + Create hooks resolved
        bool havePlayerVelocity = false;
        Vec3 playerVelocity{};             // the body's own velocity (units/s)
    };

    struct ExecContext
    {
        bool vr = false;                   // a decoded VR pose belongs to this command
        bool poseValid = false;
        int command = 0;
        Vec3 position{};                   // reach-clamped controller position
        Angles aimAngles{};                // normalized + clamped controller angles
        AimReason aim = AimReason::None;   // eye override for this command
        int addButtons = 0;
        bool reachClamped = false;
        bool throwPrepared = false;
        bool handThrow = false;            // throw the grenade inside this command
        bool shoveStarted = false;
        bool pillGiveStarted = false;
    };

    // Before the original PlayerRunCommand. `pose` may be null (unknown command).
    inline ExecContext PreExecute(PlayerVRState& p, const CommandPose* pose, int command, int buttons,
        const ExecInput& in, const ServerConfig& c)
    {
        ExecContext ctx{};
        ctx.command = command;
        if (!p.isUsingVR || !pose || !pose->poseValid)
            return ctx;
        ctx.vr = true;
        ctx.poseValid = true;
        ctx.position = pose->position;
        ctx.aimAngles = NormalizeAndClampViewAngles(pose->angles);
        if (in.haveEye && ClampHandReach(in.eye, ctx.position, c.MaxHandReachUnits()))
        {
            ctx.reachClamped = true;
            ++p.reachClamps;
        }
        if (command > p.lastExecutedCommand)
            p.lastExecutedCommand = command;

        // Native reload gate policy (listen host: physicalReloadLedger.Observe).
        if (in.interaction)
            p.reloadLedger.Observe(command, in.weaponTag,
                pose->reload == l4d2vr_server_pistol::ReloadPolicy::Block && weapon::IsFirearm(in.weaponId),
                pose->reload == l4d2vr_server_pistol::ReloadPolicy::Preserve, in.owner);

        // Grenade release -> pending throw + throw aim grace window.
        const bool attackDown = (buttons & kInAttack) != 0;
        const bool throwable = weapon::IsThrowable(in.weaponId);
        const int released = p.throwTracker.Update(in.weaponTag, in.weaponId, attackDown, throwable);
        if (released != 0 && in.liveSurvivor)
        {
            if ((in.features & kFeatureThrowVelocity) != 0u)
            {
                PendingThrow pending{};
                pending.release = ComputeThrowRelease(p.throwRing, pose->tick, ctx.position, ctx.aimAngles,
                    in.tickSeconds, c);
                pending.valid = pending.release.valid;
                pending.weaponId = released;
                pending.releaseCommand = command;
                pending.owner = in.owner;
                p.pendingThrow = pending;
                ctx.throwPrepared = pending.valid;
            }
            p.throwAimRemaining = c.throwAimCommands;
        }

        // Hand throw: the projectile leaves the hand inside this very command,
        // at the (reach-clamped) hand, with the client-measured hand velocity.
        if (pose->intent == Intent::HandThrow && pose->handThrow.valid && in.handThrowBackend &&
            (in.features & kFeatureHandThrow) != 0u && in.liveSurvivor && !in.incapacitated && !in.controlled &&
            !in.usingMounted && weapon::IsThrowable(in.weaponId) &&
            in.weaponId == l4d2vr_hand_throw::WeaponIdFromKind(pose->handThrow.kind) &&
            p.handThrowLimiter.Allow(in.nowMs, c.handThrowMinIntervalMs))
        {
            const l4d2vr_hand_throw::V3& h = pose->handThrow.velocity;
            const Vec3 player = in.havePlayerVelocity && in.playerVelocity.Finite() ? in.playerVelocity : Vec3{};
            const l4d2vr_hand_throw::V3 v = l4d2vr_hand_throw::ComposeVelocity(h,
                l4d2vr_hand_throw::V3(player.x, player.y, player.z), c.throwHandVelocityScale, c.throwMaxSpeed);
            PendingThrow pending{};
            pending.valid = true;
            pending.hand = true;
            pending.decodedAtMs = pose->decodedAtMs;
            pending.handVelocity = Vec3(h.x, h.y, h.z);
            pending.playerVelocity = player;
            pending.weaponId = in.weaponId;
            pending.releaseCommand = command;
            pending.owner = in.owner;
            pending.release.valid = true;
            pending.release.origin = ctx.position;
            pending.release.angles = ctx.aimAngles;
            pending.release.velocity = Vec3(v.x, v.y, v.z);
            p.pendingThrow = pending;
            p.throwAimRemaining = 0;
            ctx.handThrow = true;
        }

        // Intents 237..239 share one per-second budget plus per-intent intervals.
        if (pose->intent == Intent::Shove && (in.features & kFeatureShoveAim) != 0u && in.liveSurvivor &&
            !in.incapacitated && p.shoveLimiter.Allow(in.nowMs, c.shoveMinIntervalMs) &&
            p.intentBudget.Consume(in.nowMs, c.intentBudgetPerSecond))
        {
            ctx.addButtons |= kInAttack2;
            p.shoveAimRemaining = c.shoveAimCommands;
            ctx.shoveStarted = true;
        }
        if (pose->intent == Intent::PillGive && (in.features & kFeaturePillGive) != 0u && in.liveSurvivor &&
            !in.incapacitated && weapon::IsGiveable(in.weaponId) && p.pillLimiter.Allow(in.nowMs, c.pillGiveMinIntervalMs) &&
            p.intentBudget.Consume(in.nowMs, c.intentBudgetPerSecond))
        {
            ctx.addButtons |= kInAttack2;
            p.pillAimRemaining = c.pillAimCommands;
            ctx.pillGiveStarted = true;
        }

        // Eye override reason, strongest first.
        if (in.eyeHooks)
        {
            const bool useAim = (c.useAimMode == 2 && pose->useDown) ||
                (c.useAimMode >= 1 && in.interaction && pose->intent == Intent::GripPickup && pose->useDown);
            if (in.usingMounted) ctx.aim = AimReason::Mounted;
            else if (useAim) ctx.aim = AimReason::Use;
            else if (p.pillAimRemaining > 0 && (in.features & kFeaturePillGive) != 0u) ctx.aim = AimReason::PillGive;
            else if (p.shoveAimRemaining > 0 && (in.features & kFeatureShoveAim) != 0u) ctx.aim = AimReason::Shove;
            else if (p.throwAimRemaining > 0) ctx.aim = AimReason::Throw;
        }
        // Handing over pills: the give traces from the real eye through the
        // hand, so touching the item to a teammate aims at that teammate.
        if (ctx.aim == AimReason::PillGive && in.haveEye)
        {
            Vec3 toHand{ ctx.position.x - in.eye.x, ctx.position.y - in.eye.y, ctx.position.z - in.eye.z };
            if (Normalize(toHand) > 1.0f)
            {
                ctx.aimAngles = NormalizeAndClampViewAngles(VectorAngles(toHand));
                ctx.position = in.eye;
            }
        }
        if (p.pillAimRemaining > 0) --p.pillAimRemaining;
        if (p.shoveAimRemaining > 0) --p.shoveAimRemaining;
        if (p.throwAimRemaining > 0 && released == 0) --p.throwAimRemaining;
        return ctx;
    }

    struct PostInput
    {
        std::uint32_t features = 0u;
        bool interaction = false;
        std::uintptr_t owner = 0u;
        std::uintptr_t weaponTagBefore = 0u;
        std::uintptr_t weaponTagAfter = 0u;
        int weaponIdAfter = 0;
        bool liveSurvivor = false;
        bool incapacitated = false;
        bool controlled = false;
        bool meleeBackend = false;         // TestMeleeSwingCollision path resolved
        bool dropBackend = false;          // Weapon_Drop resolved
        bool deployBackend = false;        // next-attack netprops + curtime resolved
        std::uint64_t nowMs = 0u;
        float tickSeconds = 1.0f / 30.0f;
    };

    struct PostActions
    {
        MeleeSweepState::Step melee{};
        bool drop = false;
        PendingDrop dropRequest{};
        bool instantDeploy = false;
    };

    // After the original PlayerRunCommand.
    inline PostActions PostExecute(PlayerVRState& p, const CommandPose* pose, const ExecContext& ctx,
        const PostInput& in, const ServerConfig& c)
    {
        PostActions a{};
        if (p.pendingThrow.valid && ++p.pendingThrow.executedSinceRelease > 48)
            p.pendingThrow = {};
        if (!ctx.vr || !pose)
            return a;

        // Physical melee sweep on the newest pose of this command.
        if (in.meleeBackend && (in.features & kFeatureMeleeSweepV2) != 0u)
        {
            const bool eligible = in.liveSurvivor && in.weaponIdAfter == weapon::kMelee && in.weaponTagAfter != 0u;
            const bool blocked = (c.meleeBlockWhileIncapacitated && in.incapacitated) ||
                (c.meleeBlockWhileControlled && in.controlled);
            a.melee = p.melee.Process(in.owner, in.weaponTagAfter, ctx.command, eligible, pose->meleeSwing, blocked,
                ctx.position, ctx.aimAngles, in.nowMs, c);
        }

        // Drops run after the command, through the game's own Weapon_Drop.
        const bool canDrop = in.dropBackend && in.liveSurvivor && !in.incapacitated && !in.controlled &&
            in.weaponTagAfter != 0u && in.weaponTagAfter == in.weaponTagBefore;
        if (canDrop && pose->intent == Intent::HolsterDrop && (in.features & kFeatureHolsterDrop) != 0u &&
            p.holsterLimiter.Allow(in.nowMs, c.holsterDropMinIntervalMs) &&
            p.intentBudget.Consume(in.nowMs, c.intentBudgetPerSecond))
        {
            Vec3 velocity{}, angular{};
            p.throwRing.EstimateReleaseVelocity(pose->tick, c.throwVelocityWindowTicks, c.throwPeakVelocityBlend,
                in.tickSeconds, velocity, angular);
            ClampMagnitude(velocity, c.dropMaxSpeed);
            a.drop = true;
            a.dropRequest = { true, true, false, in.weaponIdAfter, ctx.command, velocity };
        }
        else if (canDrop && pose->inventoryRelease && in.interaction && (in.features & kFeatureHolsterDrop) != 0u &&
            pose->carryWeaponId == in.weaponIdAfter && pose->dropHand == l4d2vr_dual::Hand::None &&
            p.holsterLimiter.Allow(in.nowMs, c.holsterDropMinIntervalMs))
        {
            const bool grip = pose->intent == Intent::GripRelease;
            ThrowRelease release = ComputeThrowRelease(p.throwRing, pose->tick, ctx.position, ctx.aimAngles,
                in.tickSeconds, c);
            a.drop = true;
            a.dropRequest = { true, false, grip, in.weaponIdAfter, ctx.command, grip ? Vec3{} : release.velocity };
        }

        // Instant deploy after a weapon switch made by this VR command.
        a.instantDeploy = in.deployBackend && (in.features & kFeatureInstantDeploy) != 0u && in.liveSurvivor &&
            in.weaponTagAfter != 0u && in.weaponTagAfter != in.weaponTagBefore;
        return a;
    }
}

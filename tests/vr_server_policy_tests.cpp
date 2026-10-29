// Limits, throw velocity, melee planner and execution policy of the
// dedicated-server core.
#include "../L4D2VR/server_core/vr_server_exec.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

static int g_Checks = 0;
static void CheckImpl(bool ok, const char* what, int line)
{
    ++g_Checks;
    if (!ok) { std::fprintf(stderr, "server policy check failed at line %d: %s\n", line, what); std::abort(); }
}
#define Check(x) CheckImpl((x), #x, __LINE__)

using namespace l4d2vr_server;

namespace
{
    bool Near(float a, float b, float t) { return std::fabs(a - b) <= t; }

    // The blade direction is the controller forward pitched 50 degrees about its
    // right axis, so a pure yaw swing of `yaw` degrees sweeps
    // acos(cos^2(50) cos(yaw) + sin^2(50)); one trace per 10 degrees.
    int SweepSamplesForYaw(float yaw)
    {
        const float c50 = std::cos(50.0f * kDegToRad), s50 = std::sin(50.0f * kDegToRad);
        const float angle = std::acos(c50 * c50 * std::cos(yaw * kDegToRad) + s50 * s50) * kRadToDeg;
        return static_cast<int>(std::ceil(angle / 10.0f - 0.00001f));
    }

    void TestConfig()
    {
        ServerConfig c{};
        Check(ApplyConfigLine(c, "  # comment only"));
        Check(ApplyConfigLine(c, ""));
        Check(ApplyConfigLine(c, "FeatureMask=0x2f"));
        Check(c.featureMask == 0x2Fu);
        Check(ApplyConfigLine(c, "MeleeSwingCooldownSeconds = 0.5 # trailing"));
        Check(Near(c.meleeSwingCooldownSeconds, 0.5f, 1e-6f));
        Check(ApplyConfigLine(c, "MaxHandReachMeters=3.0"));
        Check(c.maxHandReachMeters <= 1.2f);                  // spec ceiling
        Check(ApplyConfigLine(c, "ThrowMaxSpeed=99999"));
        Check(c.throwMaxSpeed == 2500.0f);
        Check(!ApplyConfigLine(c, "NoSuchKey=1"));
        Check(!ApplyConfigLine(c, "MeleeSwingBurstMax=abc"));
        Check(!ApplyConfigLine(c, "Verbose"));
        Check(ApplyConfigLine(c, "verbose=on") && c.verbose);
        Check(Near(ServerConfig{}.MaxHandReachUnits(), 1.2f * 43.2f, 0.01f));
    }

    void TestReachAndLimiters()
    {
        Vec3 hand(100.0f, 0.0f, 0.0f);
        Check(ClampHandReach(Vec3(0, 0, 0), hand, 51.84f));
        Check(Near(hand.Length(), 51.84f, 0.01f) && Near(hand.x, 51.84f, 0.01f));
        Vec3 inside(10.0f, 10.0f, 10.0f);
        Check(!ClampHandReach(Vec3(0, 0, 0), inside, 51.84f) && inside.x == 10.0f);
        Vec3 bad(NAN, 0.0f, 0.0f);
        Check(ClampHandReach(Vec3(1, 2, 3), bad, 51.84f) && bad == Vec3(1, 2, 3));

        MinIntervalLimiter limiter;
        Check(limiter.Allow(1000, 400));
        Check(!limiter.Allow(1399, 400));
        Check(limiter.Allow(1400, 400));
        IntentBudget budget;
        for (int i = 0; i < 3; ++i) Check(budget.Consume(5000 + i, 3));
        Check(!budget.Consume(5500, 3));
        Check(budget.Consume(6000, 3));

        ServerConfig c{};
        c.meleeSwingCooldownSeconds = 0.35f;
        c.meleeSwingBurstWindowSeconds = 2.0f;
        c.meleeSwingBurstMax = 4;
        MeleeSwingGate gate;
        Check(gate.TryStart(1000, c) == MeleeSwingGate::Result::Allowed);
        Check(gate.TryStart(1200, c) == MeleeSwingGate::Result::Cooldown);
        Check(gate.TryStart(1400, c) == MeleeSwingGate::Result::Allowed);
        Check(gate.TryStart(1800, c) == MeleeSwingGate::Result::Allowed);
        Check(gate.TryStart(2200, c) == MeleeSwingGate::Result::Allowed);
        Check(gate.TryStart(2600, c) == MeleeSwingGate::Result::Burst);   // 4 swings within 2 s
        Check(gate.TryStart(3500, c) == MeleeSwingGate::Result::Allowed);
    }

    void TestThrowRing()
    {
        ThrowPoseRing ring;
        // Hand moving +X at 3 units per tick relative to the player, while the
        // player itself runs at 10 units per tick: locomotion must not count.
        for (int tick = 1; tick <= 10; ++tick)
        {
            const Vec3 origin(10.0f * tick, 0.0f, 0.0f);
            const Vec3 relative(3.0f * tick, 0.0f, 40.0f);
            ring.Record(tick, origin + relative, relative, true, Angles(0, 0, 0));
        }
        Vec3 v, w;
        Check(ring.EstimateReleaseVelocity(10, 5, 0.5f, 1.0f / 30.0f, v, w));
        Check(Near(v.x, 90.0f, 0.01f) && Near(v.y, 0.0f, 0.01f) && Near(v.z, 0.0f, 0.01f));
        // Duplicate tick updates in place; a large tick jump clears the ring.
        ring.Record(10, Vec3(1, 1, 1), Vec3(1, 1, 1), true, Angles(0, 0, 0));
        Check(ring.Count() == 10);
        ring.Record(100, Vec3(), Vec3(), true, Angles());
        Check(ring.Count() == 1);
        Check(!ring.EstimateReleaseVelocity(100, 5, 0.5f, 1.0f / 30.0f, v, w));

        ServerConfig c{};
        ThrowPoseRing fast;
        for (int tick = 1; tick <= 6; ++tick)
            fast.Record(tick, Vec3(60.0f * tick, 0, 0), Vec3(60.0f * tick, 0, 0), true, Angles(0, 0, 0));
        const ThrowRelease r = ComputeThrowRelease(fast, 6, Vec3(360, 0, 0), Angles(0, 0, 0), 1.0f / 30.0f, c);
        Check(r.valid && !r.belowMinimum);
        Check(Near(r.velocity.Length(), c.throwMaxSpeed, 0.5f));        // 1800*4 clamped to 1400
        Check(Near(r.origin.x, 365.0f, 0.01f));                          // 5 units along the throw
        ThrowPoseRing still;
        still.Record(1, Vec3(), Vec3(), true, Angles(0, 90, 0));
        const ThrowRelease s = ComputeThrowRelease(still, 1, Vec3(0, 0, 0), Angles(0, 90, 0), 1.0f / 30.0f, c);
        Check(s.valid && s.belowMinimum && Near(s.origin.y, 5.0f, 0.01f));

        ThrowableReleaseTracker tracker;
        Check(tracker.Update(0x1000, weapon::kMolotov, true, true) == 0);
        Check(tracker.Update(0x1000, weapon::kMolotov, true, true) == 0);
        Check(tracker.Update(0x1000, weapon::kMolotov, false, true) == weapon::kMolotov);
        Check(tracker.Update(0x1000, weapon::kMolotov, false, true) == 0);
        Check(tracker.Update(0x1000, weapon::kMolotov, true, true) == 0);
        Check(tracker.Update(0x2000, weapon::kPistol, false, false) == 0);   // switch is not a release
    }

    void TestMeleePlanner()
    {
        // No previous pose: nothing to sweep.
        MeleeSweepPlan none = PlanMeleeSweep(false, Vec3(), Angles(), Vec3(1, 0, 0), Angles(0, 30, 0), 43.2f);
        Check(none.count == 0);
        // 60 degree yaw swing in place: angle-driven samples (6), endpoints match.
        MeleeSweepPlan yaw = PlanMeleeSweep(true, Vec3(0, 0, 60), Angles(0, 0, 0), Vec3(0, 0, 60), Angles(0, 60, 0), 43.2f);
        Check(yaw.count == SweepSamplesForYaw(60.0f) && yaw.count == 4);
        Vec3 f, r;
        AngleVectors(Angles(0, 60, 0), &f, &r, nullptr);
        Vec3 expected = RotateAroundAxis(f, r, 50.0f);
        Normalize(expected);
        const Vec3 last = yaw.traces[static_cast<std::size_t>(yaw.count - 1)].direction;
        Check(Near(Dot(last, expected), 1.0f, 1e-4f));
        Check(yaw.traces[0].origin == Vec3(0, 0, 60));
        // Straight 20 cm thrust without rotation: distance-driven samples (10).
        MeleeSweepPlan thrust = PlanMeleeSweep(true, Vec3(0, 0, 0), Angles(0, 0, 0), Vec3(8.64f, 0, 0), Angles(0, 0, 0), 43.2f);
        Check(thrust.count == 10);
        Check(Near(thrust.traces[9].origin.x, 8.64f, 1e-3f) && Near(thrust.traces[0].origin.x, 0.864f, 1e-3f));
        // Teleport-sized jumps are not swept.
        MeleeSweepPlan jump = PlanMeleeSweep(true, Vec3(0, 0, 0), Angles(), Vec3(500, 0, 0), Angles(), 43.2f);
        Check(jump.count == 0);

        ServerConfig c{};
        MeleeSweepState state;
        const std::uintptr_t owner = 0x100, weapon = 0x200;
        auto step = state.Process(owner, weapon, 10, true, false, false, Vec3(), Angles(0, 0, 0), 1000, c);
        Check(!step.sweep);
        step = state.Process(owner, weapon, 11, true, true, false, Vec3(), Angles(0, 40, 0), 1033, c);
        Check(step.sweep && step.newSwing && step.plan.count == SweepSamplesForYaw(40.0f));
        step = state.Process(owner, weapon, 12, true, true, false, Vec3(), Angles(0, 80, 0), 1066, c);
        Check(step.sweep && !step.newSwing);
        step = state.Process(owner, weapon, 12, true, true, false, Vec3(), Angles(0, 120, 0), 1070, c);
        Check(!step.sweep);                                               // backup re-run ignored
        step = state.Process(owner, weapon, 13, true, false, false, Vec3(), Angles(0, 120, 0), 1100, c);
        step = state.Process(owner, weapon, 14, true, true, false, Vec3(), Angles(0, 160, 0), 1133, c);
        Check(step.rejected && !step.sweep);                              // cooldown 0.35 s
        step = state.Process(owner, weapon, 15, true, true, true, Vec3(), Angles(0, 170, 0), 2000, c);
        Check(!step.sweep);                                               // blocked (pinned)
    }

    ExecInput Input(int weaponId, std::uint32_t features)
    {
        ExecInput in{};
        in.features = features;
        in.interaction = true;
        in.eyeHooks = true;
        in.owner = 0x1000;
        in.weaponTag = 0x2000;
        in.weaponId = weaponId;
        in.liveSurvivor = true;
        in.haveEye = true;
        in.eye = Vec3(0, 0, 64);
        in.nowMs = 10000;
        return in;
    }

    CommandPose Pose(int command, Intent intent = Intent::None)
    {
        CommandPose p{};
        p.command = command;
        p.tick = command;
        p.poseValid = true;
        p.position = Vec3(20, -10, 50);
        p.angles = Angles(10, 370, 15);
        p.intent = intent;
        return p;
    }

    void TestExecution()
    {
        ServerConfig c{};
        PlayerVRState p{};
        p.isUsingVR = true;

        // Plain VR command: pose normalized, no override, reach not clamped.
        CommandPose pose = Pose(100);
        ExecContext ctx = PreExecute(p, &pose, 100, 0, Input(1, kFeatureAllMask), c);
        Check(ctx.vr && ctx.poseValid && ctx.aim == AimReason::None && !ctx.reachClamped);
        Check(Near(ctx.aimAngles.y, 10.0f, 1e-3f) && ctx.aimAngles.z == 0.0f);

        // Forged far hand is clamped to 1.2 m from the eye.
        CommandPose far = Pose(101);
        far.position = Vec3(1000, 0, 64);
        ctx = PreExecute(p, &far, 101, 0, Input(1, kFeatureAllMask), c);
        Check(ctx.reachClamped && Near((ctx.position - Vec3(0, 0, 64)).Length(), c.MaxHandReachUnits(), 0.01f));

        // Shove: adds IN_ATTACK2 and aims the following commands with the hand.
        CommandPose shove = Pose(102, Intent::Shove);
        ctx = PreExecute(p, &shove, 102, 0, Input(1, kFeatureAllMask), c);
        Check(ctx.shoveStarted && (ctx.addButtons & kInAttack2) && ctx.aim == AimReason::Shove);
        CommandPose next = Pose(103);
        ctx = PreExecute(p, &next, 103, 0, Input(1, kFeatureAllMask), c);
        Check(ctx.aim == AimReason::Shove && ctx.addButtons == 0);
        CommandPose shoveAgain = Pose(104, Intent::Shove);
        ctx = PreExecute(p, &shoveAgain, 104, 0, Input(1, kFeatureAllMask), c);
        Check(!ctx.shoveStarted);                                    // 250 ms interval
        CommandPose shoveDisabled = Pose(105, Intent::Shove);
        PlayerVRState q{};
        q.isUsingVR = true;
        ctx = PreExecute(q, &shoveDisabled, 105, 0, Input(1, kFeatureAllMask & ~kFeatureShoveAim), c);
        Check(!ctx.shoveStarted && ctx.addButtons == 0 && ctx.aim == AimReason::None);

        // Pill give only with pills/adrenaline.
        PlayerVRState pills{};
        pills.isUsingVR = true;
        CommandPose give = Pose(200, Intent::PillGive);
        ctx = PreExecute(pills, &give, 200, 0, Input(weapon::kPistol, kFeatureAllMask), c);
        Check(!ctx.pillGiveStarted);
        ctx = PreExecute(pills, &give, 201, 0, Input(weapon::kPainPills, kFeatureAllMask), c);
        Check(ctx.pillGiveStarted && (ctx.addButtons & kInAttack2) && ctx.aim == AimReason::PillGive);
        // With the real eye known, the give traces from the eye through the hand.
        PlayerVRState handover{};
        handover.isUsingVR = true;
        ExecInput withEye = Input(weapon::kPainPills, kFeatureAllMask);
        withEye.haveEye = true;
        withEye.eye = Vec3(0, -10, 50);
        ctx = PreExecute(handover, &give, 202, 0, withEye, c);
        Check(ctx.aim == AimReason::PillGive && ctx.position.x == 0.0f && ctx.position.z == 50.0f);
        Check(std::fabs(ctx.aimAngles.x) < 0.01f && std::fabs(ctx.aimAngles.y) < 0.01f);  // straight ahead (+x)

        // Grip pickup aims the use trace; mounted guns win over everything.
        PlayerVRState use{};
        use.isUsingVR = true;
        CommandPose grip = Pose(300, Intent::GripPickup);
        grip.useDown = true;
        ctx = PreExecute(use, &grip, 300, kInUse, Input(1, kFeatureAllMask), c);
        Check(ctx.aim == AimReason::Use);
        ExecInput mounted = Input(1, kFeatureAllMask);
        mounted.usingMounted = true;
        ctx = PreExecute(use, &grip, 301, kInUse, mounted, c);
        Check(ctx.aim == AimReason::Mounted);
        ExecInput noEye = Input(1, kFeatureAllMask);
        noEye.eyeHooks = false;
        ctx = PreExecute(use, &grip, 302, kInUse, noEye, c);
        Check(ctx.aim == AimReason::None);

        // Grenade release: pending throw + 48-command aim grace.
        PlayerVRState thrower{};
        thrower.isUsingVR = true;
        for (int tick = 1; tick <= 6; ++tick)
            thrower.throwRing.Record(tick, Vec3(20.0f * tick, 0, 50), Vec3(20.0f * tick, 0, 50), true, Angles(0, 0, 0));
        CommandPose hold = Pose(5);
        hold.tick = 5;
        hold.position = Vec3(10, 0, 50);
        ctx = PreExecute(thrower, &hold, 5, kInAttack, Input(weapon::kPipeBomb, kFeatureAllMask), c);
        Check(!ctx.throwPrepared);
        CommandPose release = hold;
        release.command = 6;
        release.tick = 6;
        ctx = PreExecute(thrower, &release, 6, 0, Input(weapon::kPipeBomb, kFeatureAllMask), c);
        Check(ctx.throwPrepared && thrower.pendingThrow.valid && thrower.pendingThrow.weaponId == weapon::kPipeBomb);
        Check(ctx.aim == AimReason::Throw && thrower.throwAimRemaining == c.throwAimCommands);
        for (int i = 0; i < 60; ++i)
        {
            CommandPose after = Pose(7 + i);
            ctx = PreExecute(thrower, &after, 7 + i, 0, Input(weapon::kPistol, kFeatureAllMask), c);
            PostInput post{};
            PostExecute(thrower, &after, ctx, post, c);
        }
        Check(thrower.throwAimRemaining == 0 && ctx.aim == AimReason::None && !thrower.pendingThrow.valid);
        PlayerVRState noVelocity{};
        noVelocity.isUsingVR = true;
        PreExecute(noVelocity, &hold, 5, kInAttack, Input(weapon::kMolotov, kFeatureAllMask & ~kFeatureThrowVelocity), c);
        PreExecute(noVelocity, &release, 6, 0, Input(weapon::kMolotov, kFeatureAllMask & ~kFeatureThrowVelocity), c);
        Check(!noVelocity.pendingThrow.valid && noVelocity.throwAimRemaining == c.throwAimCommands);

        // Hand throw (impulse 244): the projectile leaves in the release command
        // itself, at the reach-clamped hand, hand velocity * scale + body velocity.
        {
            auto handPose = [](int command, l4d2vr_hand_throw::Kind kind, float vx, float vz)
            {
                CommandPose h = Pose(command, Intent::HandThrow);
                h.handThrow.valid = true;
                h.handThrow.kind = kind;
                h.handThrow.velocity = l4d2vr_hand_throw::V3(vx, 0.0f, vz);
                h.decodedAtMs = 9990u;
                return h;
            };
            auto handInput = [](int weaponId, std::uint32_t features)
            {
                ExecInput in = Input(weaponId, features);
                in.handThrowBackend = true;
                in.havePlayerVelocity = true;
                in.playerVelocity = Vec3(100.0f, 0.0f, 0.0f);
                return in;
            };
            PlayerVRState hand{};
            hand.isUsingVR = true;
            CommandPose toss = handPose(500, l4d2vr_hand_throw::Kind::PipeBomb, 150.0f, 60.0f);
            ctx = PreExecute(hand, &toss, 500, 0, handInput(weapon::kPipeBomb, kFeatureAllMask), c);
            Check(ctx.handThrow && hand.pendingThrow.valid && hand.pendingThrow.hand && hand.pendingThrow.releaseCommand == 500);
            Check(Near(hand.pendingThrow.release.velocity.x, 150.0f * c.throwHandVelocityScale + 100.0f, 0.01f));
            Check(Near(hand.pendingThrow.release.velocity.z, 60.0f * c.throwHandVelocityScale, 0.01f));
            Check(hand.pendingThrow.release.origin.x == ctx.position.x && hand.pendingThrow.decodedAtMs == 9990u);
            Check(ctx.aim == AimReason::None && hand.throwAimRemaining == 0);   // no eye override needed

            // A drop: no hand motion keeps only the body's velocity.
            PlayerVRState drop{};
            drop.isUsingVR = true;
            CommandPose still = handPose(510, l4d2vr_hand_throw::Kind::Molotov, 0.0f, 0.0f);
            ExecInput standing = handInput(weapon::kMolotov, kFeatureAllMask);
            standing.playerVelocity = Vec3();
            ctx = PreExecute(drop, &still, 510, 0, standing, c);
            Check(ctx.handThrow && drop.pendingThrow.release.velocity.Length() == 0.0f);

            // Anti-abuse: the speed clamp, a far hand, the wrong item, missing
            // feature/backend, incapacitated or pinned players, rate limit.
            PlayerVRState abuse{};
            abuse.isUsingVR = true;
            CommandPose fast = handPose(520, l4d2vr_hand_throw::Kind::VomitJar, 2047.0f, 1023.0f);
            fast.position = Vec3(1000.0f, 0.0f, 64.0f);
            ctx = PreExecute(abuse, &fast, 520, 0, handInput(weapon::kVomitJar, kFeatureAllMask), c);
            Check(ctx.handThrow && abuse.pendingThrow.release.velocity.Length() <= c.throwMaxSpeed + 0.01f);
            Check(Near((abuse.pendingThrow.release.origin - Vec3(0, 0, 64)).Length(), c.MaxHandReachUnits(), 0.01f));
            CommandPose again = handPose(521, l4d2vr_hand_throw::Kind::VomitJar, 10.0f, 0.0f);
            abuse.pendingThrow = {};
            ctx = PreExecute(abuse, &again, 521, 0, handInput(weapon::kVomitJar, kFeatureAllMask), c);
            Check(!ctx.handThrow && !abuse.pendingThrow.valid);                  // 250 ms interval

            PlayerVRState gate{};
            gate.isUsingVR = true;
            CommandPose pipe = handPose(530, l4d2vr_hand_throw::Kind::PipeBomb, 10.0f, 0.0f);
            ctx = PreExecute(gate, &pipe, 530, 0, handInput(weapon::kMolotov, kFeatureAllMask), c);
            Check(!ctx.handThrow && !gate.pendingThrow.valid);                   // item mismatch
            ctx = PreExecute(gate, &pipe, 531, 0, handInput(weapon::kPistol, kFeatureAllMask), c);
            Check(!ctx.handThrow);
            ctx = PreExecute(gate, &pipe, 532, 0, handInput(weapon::kPipeBomb, kFeatureAllMask & ~kFeatureHandThrow), c);
            Check(!ctx.handThrow);
            ExecInput noBackend = handInput(weapon::kPipeBomb, kFeatureAllMask);
            noBackend.handThrowBackend = false;
            ctx = PreExecute(gate, &pipe, 533, 0, noBackend, c);
            Check(!ctx.handThrow);
            ExecInput incap = handInput(weapon::kPipeBomb, kFeatureAllMask);
            incap.incapacitated = true;
            ctx = PreExecute(gate, &pipe, 534, 0, incap, c);
            Check(!ctx.handThrow);
            ExecInput pinned = handInput(weapon::kPipeBomb, kFeatureAllMask);
            pinned.controlled = true;
            ctx = PreExecute(gate, &pipe, 535, 0, pinned, c);
            Check(!ctx.handThrow);
            ExecInput dead = handInput(weapon::kPipeBomb, kFeatureAllMask);
            dead.liveSurvivor = false;
            ctx = PreExecute(gate, &pipe, 536, 0, dead, c);
            Check(!ctx.handThrow && !gate.pendingThrow.valid);
            CommandPose invalid = pipe;
            invalid.handThrow.valid = false;
            ctx = PreExecute(gate, &invalid, 537, 0, handInput(weapon::kPipeBomb, kFeatureAllMask), c);
            Check(!ctx.handThrow);
            ctx = PreExecute(gate, &pipe, 538, 0, handInput(weapon::kPipeBomb, kFeatureAllMask), c);
            Check(ctx.handThrow && gate.pendingThrow.valid);

            // A hand-throw command never starts the old trigger-release path.
            PlayerVRState legacy{};
            legacy.isUsingVR = true;
            CommandPose ht = handPose(540, l4d2vr_hand_throw::Kind::PipeBomb, 50.0f, 0.0f);
            ctx = PreExecute(legacy, &ht, 540, 0, handInput(weapon::kPipeBomb, kFeatureAllMask), c);
            Check(!ctx.throwPrepared && ctx.handThrow);

            Check(ApplyConfigLine(c, "ThrowHandVelocityScale=2.5") && Near(c.throwHandVelocityScale, 2.5f, 1e-6f));
            Check(ApplyConfigLine(c, "ThrowHandVelocityScale=50") && c.throwHandVelocityScale == 10.0f);
            Check(ApplyConfigLine(c, "HandThrowMinIntervalMs=10") && c.handThrowMinIntervalMs == 50);
            c = ServerConfig{};
        }

        // Reload gate: impulse 236 blocks the native reload of that firearm.
        PlayerVRState reload{};
        reload.isUsingVR = true;
        CommandPose block = Pose(400, Intent::BlockNativeReload);
        block.reload = l4d2vr_server_pistol::ReloadPolicy::Block;
        PreExecute(reload, &block, 400, 0, Input(weapon::kPistol, kFeatureAllMask), c);
        Check(reload.reloadLedger.Blocks(0x2000));
        CommandPose native = Pose(401);
        PreExecute(reload, &native, 401, 0, Input(weapon::kPistol, kFeatureAllMask), c);
        Check(!reload.reloadLedger.Blocks(0x2000));

        // Non-VR or unknown commands are inert.
        PlayerVRState desktop{};
        ctx = PreExecute(desktop, &pose, 100, kInAttack, Input(1, kFeatureAllMask), c);
        Check(!ctx.vr && ctx.addButtons == 0 && ctx.aim == AimReason::None);
        ctx = PreExecute(p, nullptr, 999, 0, Input(1, kFeatureAllMask), c);
        Check(!ctx.vr);
    }

    PostInput Post(int weaponId, std::uintptr_t before, std::uintptr_t after)
    {
        PostInput in{};
        in.features = kFeatureAllMask;
        in.interaction = true;
        in.owner = 0x1000;
        in.weaponTagBefore = before;
        in.weaponTagAfter = after;
        in.weaponIdAfter = weaponId;
        in.liveSurvivor = true;
        in.meleeBackend = in.dropBackend = in.deployBackend = true;
        in.nowMs = 20000;
        return in;
    }

    void TestPostExecution()
    {
        ServerConfig c{};
        PlayerVRState p{};
        p.isUsingVR = true;
        CommandPose drop = Pose(500, Intent::HolsterDrop);
        ExecContext ctx = PreExecute(p, &drop, 500, 0, Input(weapon::kMelee, kFeatureAllMask), c);
        PostActions a = PostExecute(p, &drop, ctx, Post(weapon::kMelee, 0x2000, 0x2000), c);
        Check(a.drop && a.dropRequest.holster && a.dropRequest.weaponId == weapon::kMelee);
        CommandPose drop2 = Pose(501, Intent::HolsterDrop);
        ctx = PreExecute(p, &drop2, 501, 0, Input(weapon::kMelee, kFeatureAllMask), c);
        a = PostExecute(p, &drop2, ctx, Post(weapon::kMelee, 0x2000, 0x2000), c);
        Check(!a.drop);                                                  // 400 ms interval
        PostInput incap = Post(weapon::kMelee, 0x2000, 0x2000);
        incap.incapacitated = true;
        incap.nowMs = 30000;
        CommandPose drop3 = Pose(502, Intent::HolsterDrop);
        ctx = PreExecute(p, &drop3, 502, 0, Input(weapon::kMelee, kFeatureAllMask), c);
        Check(!PostExecute(p, &drop3, ctx, incap, c).drop);
        PostInput disabled = Post(weapon::kMelee, 0x2000, 0x2000);
        disabled.features &= ~kFeatureHolsterDrop;
        disabled.nowMs = 40000;
        CommandPose drop4 = Pose(503, Intent::HolsterDrop);
        ctx = PreExecute(p, &drop4, 503, 0, Input(weapon::kMelee, kFeatureAllMask), c);
        Check(!PostExecute(p, &drop4, ctx, disabled, c).drop);

        // Grip release of an inventory item: zero velocity.
        PlayerVRState grip{};
        grip.isUsingVR = true;
        CommandPose release = Pose(600, Intent::GripRelease);
        release.inventoryRelease = true;
        release.carryWeaponId = weapon::kPistol;
        ctx = PreExecute(grip, &release, 600, 0, Input(weapon::kPistol, kFeatureAllMask), c);
        a = PostExecute(grip, &release, ctx, Post(weapon::kPistol, 0x2000, 0x2000), c);
        Check(a.drop && a.dropRequest.gripRelease && a.dropRequest.velocity == Vec3());
        CommandPose mismatch = release;
        mismatch.command = 601;
        mismatch.carryWeaponId = weapon::kMelee;
        PostInput later = Post(weapon::kPistol, 0x2000, 0x2000);
        later.nowMs = 50000;
        ctx = PreExecute(grip, &mismatch, 601, 0, Input(weapon::kPistol, kFeatureAllMask), c);
        Check(!PostExecute(grip, &mismatch, ctx, later, c).drop);       // released item is not the active one

        // Instant deploy after a VR weapon switch only.
        PlayerVRState deploy{};
        deploy.isUsingVR = true;
        CommandPose sw = Pose(700);
        ctx = PreExecute(deploy, &sw, 700, 0, Input(weapon::kPistol, kFeatureAllMask), c);
        Check(PostExecute(deploy, &sw, ctx, Post(weapon::kMelee, 0x2000, 0x3000), c).instantDeploy);
        Check(!PostExecute(deploy, &sw, ctx, Post(weapon::kMelee, 0x3000, 0x3000), c).instantDeploy);
        PostInput off = Post(weapon::kMelee, 0x2000, 0x3000);
        off.features &= ~kFeatureInstantDeploy;
        Check(!PostExecute(deploy, &sw, ctx, off, c).instantDeploy);

        // A switch made between commands (holster draw "use" command): the
        // next VR command clears the deploy delay before it runs.
        PlayerVRState between{};
        between.isUsingVR = true;
        ExecInput first = Input(weapon::kPistol, kFeatureAllMask);
        first.deployBackend = true;
        CommandPose c1 = Pose(800);
        ctx = PreExecute(between, &c1, 800, 0, first, c);
        Check(!ctx.instantDeployBefore);                                 // nothing known yet
        PostExecute(between, &c1, ctx, Post(weapon::kPistol, 0x2000, 0x2000), c);
        CommandPose c2 = Pose(801);
        ctx = PreExecute(between, &c2, 801, 0, first, c);
        Check(!ctx.instantDeployBefore);                                 // same item
        PostExecute(between, &c2, ctx, Post(weapon::kPistol, 0x2000, 0x2000), c);
        ExecInput drawn = first;
        drawn.weaponTag = 0x4000;
        CommandPose c3 = Pose(802);
        ctx = PreExecute(between, &c3, 802, 0, drawn, c);
        Check(ctx.instantDeployBefore);                                  // switched between commands
        PostExecute(between, &c3, ctx, Post(weapon::kMelee, 0x4000, 0x4000), c);
        CommandPose c4 = Pose(803);
        ctx = PreExecute(between, &c4, 803, 0, drawn, c);
        Check(!ctx.instantDeployBefore);                                 // once
        ExecInput noBackend = drawn;
        noBackend.weaponTag = 0x5000;
        noBackend.deployBackend = false;
        ctx = PreExecute(between, &c4, 803, 0, noBackend, c);
        Check(!ctx.instantDeployBefore);
        ExecInput dead = drawn;
        dead.weaponTag = 0x5000;
        dead.liveSurvivor = false;
        ctx = PreExecute(between, &c4, 803, 0, dead, c);
        Check(!ctx.instantDeployBefore);
        PlayerVRState desktop{};                                         // not a VR player
        desktop.lastActiveWeapon = 0x2000;
        ctx = PreExecute(desktop, &c4, 803, 0, drawn, c);
        Check(!ctx.instantDeployBefore);

        // Melee sweep through PostExecute.
        PlayerVRState melee{};
        melee.isUsingVR = true;
        CommandPose m1 = Pose(800);
        m1.angles = Angles(0, 0, 0);
        ctx = PreExecute(melee, &m1, 800, 0, Input(weapon::kMelee, kFeatureAllMask), c);
        a = PostExecute(melee, &m1, ctx, Post(weapon::kMelee, 0x2000, 0x2000), c);
        Check(!a.melee.sweep);
        CommandPose m2 = Pose(801);
        m2.angles = Angles(0, 90, 0);
        m2.meleeSwing = true;
        ctx = PreExecute(melee, &m2, 801, 0, Input(weapon::kMelee, kFeatureAllMask), c);
        a = PostExecute(melee, &m2, ctx, Post(weapon::kMelee, 0x2000, 0x2000), c);
        Check(a.melee.sweep && a.melee.newSwing && a.melee.plan.count == SweepSamplesForYaw(90.0f));
    }
}

int main()
{
    TestConfig();
    TestReachAndLimiters();
    TestThrowRing();
    TestMeleePlanner();
    TestExecution();
    TestPostExecution();
    std::printf("vr_server_policy_tests: %d checks passed\n", g_Checks);
    return 0;
}

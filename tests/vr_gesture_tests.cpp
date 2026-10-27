#ifdef _MSC_VER
#pragma warning(push, 0)
#endif
#include "../L4D2VR/sdk/vector.h"
#ifdef _MSC_VER
#pragma warning(pop)
#endif
#include "../L4D2VR/vr_gesture.h"
#include <cstdio>
#include <cstdlib>
#include <limits>

static void CheckImpl(bool result, int line)
{
    if (!result) { std::fprintf(stderr, "Check failed at line %d\n", line); std::abort(); }
}
#define Check(result) CheckImpl((result), __LINE__)

using namespace l4d2vr_gesture;

static const float kDt = 1.0f / 90.0f;

// Synthetic rig: integrates hand/HMD motion at 90 Hz.
struct Rig
{
    Sample s{};
    Rig()
    {
        s.hmdPosition = Vector(0.0f, 0.0f, 1.7f);
        s.hmdForward = Vector(1.0f, 0.0f, 0.0f);
        s.hmdUp = Vector(0.0f, 0.0f, 1.0f);
        s.handPosition = Vector(0.3f, -0.2f, 1.2f);
        s.handForward = Vector(0.0f, 0.0f, 1.0f);
    }
    // Sets velocities, then advances positions/orientation by one frame.
    void Move(const Vector& handVelocity, const Vector& angularDeg, const Vector& hmdVelocity, bool reportHmd)
    {
        s.time += kDt;
        s.handVelocity = handVelocity;
        s.handAngularVelocityDeg = angularDeg;
        s.handPosition = s.handPosition + handVelocity * kDt;
        s.handForward = s.handForward + CrossProduct(angularDeg * kDegToRad, s.handForward) * kDt;
        Normalize(s.handForward, s.handForward);
        s.hmdPosition = s.hmdPosition + hmdVelocity * kDt;
        s.hmdVelocity = hmdVelocity;
        s.hasHmdVelocity = reportHmd;
    }
    void Still() { Move(Vector(0.0f, 0.0f, 0.0f), Vector(0.0f, 0.0f, 0.0f), Vector(0.0f, 0.0f, 0.0f), true); }
};

static const Vector kZero(0.0f, 0.0f, 0.0f);

// Runs `frames` frames, returning how many swing starts were reported.
template <typename Step>
static int CountSwings(SwingDetector& detector, Rig& rig, int frames, Step step)
{
    int starts = 0;
    for (int i = 0; i < frames; ++i)
    {
        step(rig);
        const SwingEvent e = detector.Update(rig.s);
        if (e.started) ++starts;
    }
    return starts;
}

static void TestSwing()
{
    SwingDetector swing;
    Rig rig;
    // Wrist flick: blade up, pitching forward at 400 deg/s. Tip ~3.5 m/s.
    const Vector pitch(0.0f, 400.0f, 0.0f);
    auto flick = [&](Rig& r) { r.Move(kZero, pitch, kZero, true); };
    auto still = [&](Rig& r) { r.Still(); };

    // Weapon adopted mid-motion: no swing until it has rested.
    Check(CountSwings(swing, rig, 10, flick) == 0);
    Check(CountSwings(swing, rig, 5, still) == 0 && !swing.Armed()); // 0.055 s < 0.08 s rest
    Check(CountSwings(swing, rig, 5, still) == 0 && swing.Armed());

    // One swing fires exactly once, reporting direction while it lasts.
    rig.s.handForward = Vector(0.0f, 0.0f, 1.0f);
    int starts = 0;
    for (int i = 0; i < 20; ++i)
    {
        flick(rig);
        const SwingEvent e = swing.Update(rig.s);
        if (e.started)
        {
            ++starts;
            Check(e.direction.x > 0.9f && e.tipSpeed > 1.6f);
        }
        Check(e.swinging);
    }
    Check(starts == 1);
    // Stops, but without the 0.08 s rest a renewed motion does not re-trigger.
    Check(CountSwings(swing, rig, 3, still) == 0 && !swing.Swinging());
    Check(CountSwings(swing, rig, 10, flick) == 0);
    // Rest re-arms, then the next swing fires.
    Check(CountSwings(swing, rig, 9, still) == 0 && swing.Armed());
    Check(CountSwings(swing, rig, 10, flick) == 1);

    // Speed between release and start keeps the swing alive (hysteresis).
    SwingDetector hysteresis;
    Rig h;
    CountSwings(hysteresis, h, 10, still);
    Check(CountSwings(hysteresis, h, 3, flick) == 1);
    Check(CountSwings(hysteresis, h, 10, [&](Rig& r) { r.Move(kZero, Vector(0.0f, 100.0f, 0.0f), kZero, true); }) == 0);
    Check(hysteresis.Swinging()); // 100 deg/s * 0.5 m = 0.87 m/s > 0.6 release

    // Cooldown: even after resting, a new swing within cooldown of the last start is ignored.
    SwingParams cooldownParams{};
    cooldownParams.cooldown = 1.0f;
    SwingDetector cooled(cooldownParams);
    Rig c;
    CountSwings(cooled, c, 10, still);
    Check(CountSwings(cooled, c, 3, flick) == 1);
    CountSwings(cooled, c, 10, still);
    Check(!cooled.Armed());
    Check(CountSwings(cooled, c, 3, flick) == 0);
    CountSwings(cooled, c, 90, still);
    Check(CountSwings(cooled, c, 3, flick) == 1);
}

static void TestSwingIgnoresWalking()
{
    const Vector walk(4.0f, 1.0f, 0.0f);
    // Hand and HMD translate together (thumbstick locomotion or physical walking).
    for (bool reportHmd : { true, false })
    {
        SwingDetector swing;
        Rig rig;
        Check(CountSwings(swing, rig, 10, [](Rig& r) { r.Still(); }) == 0);
        Check(CountSwings(swing, rig, 90, [&](Rig& r) { r.Move(walk, kZero, walk, reportHmd); }) == 0);
        // Head bob and slight sway on top of walking are below the threshold.
        Check(CountSwings(swing, rig, 30, [&](Rig& r) {
            r.Move(walk + Vector(0.0f, 0.0f, 0.3f), Vector(20.0f, 30.0f, 0.0f), walk, reportHmd); }) == 0);
        // A real swing while walking still fires.
        Check(CountSwings(swing, rig, 10, [&](Rig& r) { r.Move(walk, Vector(0.0f, 400.0f, 0.0f), walk, reportHmd); }) == 1);
    }
    // Control: the same hand velocity without HMD motion is a swing.
    SwingDetector swing;
    Rig rig;
    CountSwings(swing, rig, 10, [](Rig& r) { r.Still(); });
    Check(CountSwings(swing, rig, 10, [&](Rig& r) { r.Move(walk, kZero, kZero, true); }) == 1);
}

static void TestSwingValidation()
{
    SwingDetector swing;
    Rig rig;
    CountSwings(swing, rig, 10, [](Rig& r) { r.Still(); });
    Check(swing.Armed());
    Sample bad = rig.s;
    bad.time += kDt;
    bad.handVelocity.x = std::numeric_limits<float>::quiet_NaN();
    Check(!swing.Update(bad).started && swing.Armed());
    bad = rig.s;
    bad.time += kDt;
    bad.handForward = kZero;
    Check(!swing.Update(bad).started);
    // Duplicate timestamps are ignored.
    Check(!swing.Update(rig.s).started);
    swing.Reset();
    Check(!swing.Armed());
}

template <typename Detector, typename Step>
static int CountFires(Detector& detector, Rig& rig, int frames, Step step)
{
    int fires = 0;
    for (int i = 0; i < frames; ++i)
    {
        step(rig);
        if (detector.Update(rig.s).fired) ++fires;
    }
    return fires;
}

static void TestThrust()
{
    const Vector jab(2.5f, 0.0f, 0.0f);
    auto still = [](Rig& r) { r.Still(); };
    auto thrust = [&](Rig& r) { r.Move(jab, kZero, kZero, true); };
    {
        ThrustDetector detector;
        SwingDetector swing;
        Rig rig;
        rig.s.handForward = Vector(1.0f, 0.0f, 0.0f);
        int thrusts = 0, swings = 0;
        for (int i = 0; i < 10; ++i) { still(rig); detector.Update(rig.s); swing.Update(rig.s); }
        for (int i = 0; i < 14; ++i) // 0.155 s at 2.5 m/s = 0.39 m
        {
            thrust(rig);
            const ThrustEvent e = detector.Update(rig.s);
            if (e.fired)
            {
                ++thrusts;
                Check(e.direction.x > 0.99f && e.travel >= 0.15f && e.forwardSpeed >= 1.8f);
            }
            if (swing.Update(rig.s).started) ++swings;
        }
        Check(thrusts == 1);
        Check(swings == 0); // tip velocity along the blade axis is a thrust, not a swing
        // Holding the speed does not re-fire; after release and cooldown it does.
        Check(CountFires(detector, rig, 10, thrust) == 0);
        Check(CountFires(detector, rig, 40, still) == 0);
        Check(CountFires(detector, rig, 14, thrust) == 1);
    }
    {
        // A jab with a big wrist rotation is a swing, not a thrust.
        ThrustDetector detector;
        Rig rig;
        rig.s.handForward = Vector(1.0f, 0.0f, 0.0f);
        CountFires(detector, rig, 10, still);
        Check(CountFires(detector, rig, 14, [&](Rig& r) { r.Move(jab, Vector(0.0f, 0.0f, 400.0f), kZero, true); }) == 0);
    }
    {
        // A short jab (0.09 m) is not enough travel.
        ThrustDetector detector;
        Rig rig;
        rig.s.handForward = Vector(1.0f, 0.0f, 0.0f);
        CountFires(detector, rig, 10, still);
        Check(CountFires(detector, rig, 3, thrust) == 0);
        Check(CountFires(detector, rig, 10, still) == 0);
    }
    {
        // Walking forward with the weapon is not a thrust (finite-difference HMD velocity).
        ThrustDetector detector;
        Rig rig;
        rig.s.handForward = Vector(1.0f, 0.0f, 0.0f);
        CountFires(detector, rig, 10, still);
        Check(CountFires(detector, rig, 60, [&](Rig& r) { r.Move(jab, kZero, jab, false); }) == 0);
    }
    {
        // Moving sideways (perpendicular to the weapon) is not a thrust.
        ThrustDetector detector;
        Rig rig;
        rig.s.handForward = Vector(1.0f, 0.0f, 0.0f);
        CountFires(detector, rig, 10, still);
        Check(CountFires(detector, rig, 14, [&](Rig& r) { r.Move(Vector(0.0f, 2.5f, 0.0f), kZero, kZero, true); }) == 0);
    }
    {
        // Gun bash preset needs 2.0 m/s.
        ThrustDetector detector(ThrustParams::GunBash());
        Rig rig;
        rig.s.handForward = Vector(1.0f, 0.0f, 0.0f);
        CountFires(detector, rig, 10, still);
        Check(CountFires(detector, rig, 20, [&](Rig& r) { r.Move(Vector(1.9f, 0.0f, 0.0f), kZero, kZero, true); }) == 0);
        CountFires(detector, rig, 10, still);
        Check(CountFires(detector, rig, 14, thrust) == 1);
    }
}

static void TestGunBash()
{
    auto still = [](Rig& r) { r.Still(); };
    auto run = [&](const Vector& velocity, int frames) {
        GunBashDetector detector;
        Rig rig;
        rig.s.handForward = Vector(1.0f, 0.0f, 0.0f);
        CountFires(detector, rig, 5, still);
        return CountFires(detector, rig, frames, [&](Rig& r) { r.Move(velocity, kZero, kZero, true); });
    };
    Check(run(Vector(3.0f, 0.0f, 0.0f), 10) == 1);   // forward shove fires once
    Check(run(Vector(0.0f, 3.0f, 0.0f), 10) == 1);   // lateral sweep
    Check(run(Vector(2.5f, 0.0f, -1.5f), 10) == 1);  // 31 degrees down is still a bash
    Check(run(Vector(0.5f, 0.0f, -3.0f), 10) == 0);  // downward reload motion (80 degrees down)
    Check(run(Vector(0.0f, 0.0f, -3.0f), 10) == 0);  // straight down
    Check(run(Vector(-3.0f, 0.0f, 0.0f), 10) == 0);  // pulling the gun back toward the face
    Check(run(Vector(1.5f, 0.0f, 0.0f), 10) == 0);   // too slow
    {
        // Walking with the gun raised does not bash.
        GunBashDetector detector;
        Rig rig;
        rig.s.handForward = Vector(1.0f, 0.0f, 0.0f);
        CountFires(detector, rig, 5, still);
        const Vector walk(3.5f, 0.0f, 0.0f);
        Check(CountFires(detector, rig, 60, [&](Rig& r) { r.Move(walk, kZero, walk, false); }) == 0);
    }
    {
        // Re-arms only after slowing down and the cooldown.
        GunBashDetector detector;
        Rig rig;
        rig.s.handForward = Vector(1.0f, 0.0f, 0.0f);
        auto shove = [](Rig& r) { r.Move(Vector(3.0f, 0.0f, 0.0f), kZero, kZero, true); };
        Check(CountFires(detector, rig, 5, shove) == 1);
        Check(CountFires(detector, rig, 5, still) == 0);
        Check(CountFires(detector, rig, 5, shove) == 0); // within 0.5 s cooldown
        Check(CountFires(detector, rig, 50, still) == 0);
        Check(CountFires(detector, rig, 5, shove) == 1);
    }
}

static void TestPills()
{
    const PillsParams params{};
    Rig base;
    Vector mouth{};
    Check(MouthPoint(base.s.hmdPosition, base.s.hmdForward, base.s.hmdUp, params, mouth));
    Check((mouth - Vector(0.07f, 0.0f, 1.62f)).Length() < 1e-5f);
    auto at = [](const Vector& p) { return [p](Rig& r) { r.Still(); r.s.handPosition = p; }; };
    const Vector away(0.3f, -0.2f, 1.2f);
    {
        PillsMouthDetector pills;
        Rig rig;
        Check(CountFires(pills, rig, 5, at(away)) == 0);
        // Held at the mouth: fires once after 0.15 s, not before.
        int fires = 0, firedFrame = -1;
        for (int i = 0; i < 30; ++i)
        {
            at(mouth + Vector(0.02f, 0.03f, 0.0f))(rig);
            const PillsEvent e = pills.Update(rig.s);
            Check(e.inside);
            if (e.fired) { ++fires; firedFrame = i; }
        }
        Check(fires == 1 && firedFrame >= 13 && firedFrame <= 14);
        // Not re-armed by leaving to 1.2x radius only.
        Check(CountFires(pills, rig, 5, at(mouth + Vector(0.0f, 0.13f, 0.0f))) == 0);
        Check(CountFires(pills, rig, 30, at(mouth)) == 0);
        // Leaving past 1.5x radius re-arms.
        Check(CountFires(pills, rig, 2, at(mouth + Vector(0.0f, 0.17f, 0.0f))) == 0);
        Check(CountFires(pills, rig, 20, at(mouth)) == 1);
    }
    {
        // Too short a dwell does not fire.
        PillsMouthDetector pills;
        Rig rig;
        CountFires(pills, rig, 5, at(away));
        Check(CountFires(pills, rig, 10, at(mouth)) == 0); // 0.1 s
        Check(CountFires(pills, rig, 5, at(away)) == 0);
        Check(CountFires(pills, rig, 10, at(mouth)) == 0); // dwell restarts on re-entry
    }
    {
        // Sweeping the hand past the mouth at 2 m/s spends ~0.1 s inside: no fire.
        PillsMouthDetector pills;
        Rig rig;
        rig.s.handPosition = mouth + Vector(0.0f, -0.5f, 0.0f);
        Check(CountFires(pills, rig, 45, [](Rig& r) { r.Move(Vector(0.0f, 2.0f, 0.0f), kZero, kZero, true); }) == 0);
    }
    {
        // The mouth follows the head (looking left).
        PillsMouthDetector pills;
        Rig rig;
        rig.s.hmdForward = Vector(0.0f, 1.0f, 0.0f);
        Vector turned{};
        Check(MouthPoint(rig.s.hmdPosition, rig.s.hmdForward, rig.s.hmdUp, params, turned));
        // In front of the original face direction, but 0.165 m from the turned mouth.
        const Vector straightAhead = rig.s.hmdPosition + Vector(0.15f, 0.0f, -0.08f);
        Check((straightAhead - mouth).Length() < params.radius && (straightAhead - turned).Length() > params.radius);
        Check(CountFires(pills, rig, 20, at(straightAhead)) == 0);
        Check(CountFires(pills, rig, 20, at(turned)) == 1);
    }
}

static void TestAdrenaline()
{
    const Vector chest(0.15f, 0.0f, 1.3f);
    Vector arm{};
    Check(ArmTargetPoint(Vector(0.3f, 0.3f, 1.2f), Vector(1.0f, 0.0f, 0.0f), 0.15f, arm));
    Check((arm - Vector(0.15f, 0.3f, 1.2f)).Length() < 1e-5f);
    StabTargets targets{};
    targets.chest = chest;
    targets.chestValid = true;
    targets.arm = arm;
    targets.armValid = true;

    // Approach `target` along -x from 0.4 m at `speed`, stopping with the tip `stop` metres short.
    auto stab = [&](const Vector& target, float speed, float stop, StabEvent& last) {
        AdrenalineStabDetector detector;
        Rig rig;
        rig.s.handForward = Vector(-1.0f, 0.0f, 0.0f);
        rig.s.handPosition = target + Vector(0.4f + 0.10f, 0.0f, 0.0f);
        int fires = 0;
        for (int i = 0; i < 200; ++i)
        {
            const float tipDistance = (rig.s.handPosition - Vector(0.10f, 0.0f, 0.0f) - target).Length();
            if (tipDistance > stop + speed * kDt) rig.Move(Vector(-speed, 0.0f, 0.0f), kZero, kZero, true);
            else
            {
                rig.Still();
                rig.s.handPosition = target + Vector(0.10f + stop, 0.0f, 0.0f);
            }
            const StabEvent e = detector.Update(rig.s, targets);
            if (e.fired) { ++fires; last = e; }
        }
        return fires;
    };
    StabEvent e{};
    Check(stab(chest, 2.0f, 0.03f, e) == 1 && e.target == StabTarget::Chest && e.approachSpeed >= 1.2f);
    Check(stab(arm, 2.0f, 0.03f, e) == 1 && e.target == StabTarget::Arm);
    Check(stab(chest, 0.5f, 0.03f, e) == 0); // slow approach
    Check(stab(chest, 2.0f, 0.2f, e) == 0);  // fast, but stops short of the target

    {
        // Fast approach that stops short, then a slow drift in later, does not inject.
        AdrenalineStabDetector detector;
        Rig rig;
        rig.s.handForward = Vector(-1.0f, 0.0f, 0.0f);
        rig.s.handPosition = chest + Vector(0.5f, 0.0f, 0.0f);
        int fires = 0;
        for (int i = 0; i < 10; ++i)
        {
            rig.Move(Vector(-2.0f, 0.0f, 0.0f), kZero, kZero, true); // tip stops 0.18 m out
            if (detector.Update(rig.s, targets).fired) ++fires;
        }
        for (int i = 0; i < 40; ++i)
        {
            rig.Move(Vector(-0.4f, 0.0f, 0.0f), kZero, kZero, true);
            if (detector.Update(rig.s, targets).fired) ++fires;
        }
        Check(fires == 0);
    }
    {
        // Walking with the tip resting near the chest: target and tip move together.
        AdrenalineStabDetector detector;
        Rig rig;
        rig.s.handForward = Vector(-1.0f, 0.0f, 0.0f);
        rig.s.handPosition = chest + Vector(0.15f, 0.0f, 0.0f);
        StabTargets moving = targets;
        moving.armValid = false;
        int fires = 0;
        const Vector walk(3.0f, 0.0f, 0.0f);
        for (int i = 0; i < 60; ++i)
        {
            rig.Move(walk, kZero, walk, true);
            moving.chest = moving.chest + walk * kDt;
            if (detector.Update(rig.s, moving).fired) ++fires;
        }
        Check(fires == 0);
    }
    {
        // Missing targets never fire; invalid samples are ignored.
        AdrenalineStabDetector detector;
        Rig rig;
        StabTargets none{};
        rig.s.handForward = Vector(-1.0f, 0.0f, 0.0f);
        rig.s.handPosition = chest + Vector(0.5f, 0.0f, 0.0f);
        int fires = 0;
        for (int i = 0; i < 30; ++i)
        {
            rig.Move(Vector(-2.0f, 0.0f, 0.0f), kZero, kZero, true);
            if (detector.Update(rig.s, none).fired) ++fires;
        }
        Check(fires == 0);
        Sample bad = rig.s;
        bad.time += kDt;
        bad.handPosition.x = std::numeric_limits<float>::infinity();
        Check(!detector.Update(bad, targets).fired);
    }
}

static void TestSampleGapRestartsTiming()
{
    // A hitch longer than kMaxSampleGap restarts the pills dwell.
    PillsMouthDetector pills;
    Rig rig;
    Vector mouth{};
    MouthPoint(rig.s.hmdPosition, rig.s.hmdForward, rig.s.hmdUp, PillsParams{}, mouth);
    rig.s.handPosition = mouth;
    for (int i = 0; i < 8; ++i) { rig.Still(); rig.s.handPosition = mouth; Check(!pills.Update(rig.s).fired); }
    rig.s.time += 1.0f;
    Check(!pills.Update(rig.s).fired);
    for (int i = 0; i < 8; ++i) { rig.Still(); rig.s.handPosition = mouth; Check(!pills.Update(rig.s).fired); }
    // Time going backwards is treated as a restart rather than wedging the detector.
    rig.s.time = 0.0f;
    int fires = 0;
    for (int i = 0; i < 20; ++i) { rig.Still(); rig.s.handPosition = mouth; if (pills.Update(rig.s).fired) ++fires; }
    Check(fires == 1);
}

int main()
{
    TestSwing();
    TestSwingIgnoresWalking();
    TestSwingValidation();
    TestThrust();
    TestGunBash();
    TestPills();
    TestAdrenaline();
    TestSampleGapRestartsTiming();
    std::printf("vr_gesture tests passed\n");
    return 0;
}

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
    Check(run(Vector(2.5f, 0.0f, -1.5f), 10) == 1);  // 31 degrees down is still a bash
    Check(run(Vector(0.0f, 3.0f, 0.0f), 10) == 0);   // a sideways sweep is not a push
    Check(run(Vector(0.5f, 0.0f, -3.0f), 10) == 0);  // downward reload motion (80 degrees down)
    Check(run(Vector(0.0f, 0.0f, -3.0f), 10) == 0);  // straight down
    Check(run(Vector(0.8f, 0.0f, 3.0f), 10) == 0);   // up (toward the shoulder)
    Check(run(Vector(-3.0f, 0.0f, 0.0f), 10) == 0);  // pulling the gun back toward the face
    Check(run(Vector(1.5f, 0.0f, 0.0f), 10) == 0);   // too slow
    {
        // Pushing forward with the barrel pointing sideways is not a bash.
        GunBashDetector detector;
        Rig rig;
        rig.s.handForward = Vector(0.0f, 1.0f, 0.0f);
        CountFires(detector, rig, 5, still);
        Check(CountFires(detector, rig, 10, [&](Rig& r) { r.Move(Vector(3.0f, 0.0f, 0.0f), kZero, kZero, true); }) == 0);
    }
    {
        // A fast wrist flick swings the muzzle at > 3 m/s but moves the hand
        // little: not a bash.
        GunBashDetector detector;
        Rig rig;
        rig.s.handForward = Vector(1.0f, 0.0f, 0.0f);
        CountFires(detector, rig, 5, still);
        Check(CountFires(detector, rig, 10, [&](Rig& r) { r.Move(Vector(0.3f, 0.0f, 0.0f), Vector(0.0f, 600.0f, 0.0f), kZero, true); }) == 0);
    }
    {
        // Not while the hand is at a holster (allowed=false).
        GunBashDetector detector;
        Rig rig;
        rig.s.handForward = Vector(1.0f, 0.0f, 0.0f);
        CountFires(detector, rig, 5, still);
        int fired = 0;
        for (int i = 0; i < 10; ++i)
        {
            rig.Move(Vector(3.0f, 0.0f, 0.0f), kZero, kZero, true);
            if (detector.Update(rig.s, false).fired) ++fired;
        }
        Check(fired == 0);
    }
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
        // A gun adopted mid-motion (e.g. just drawn) waits for the hand to settle.
        GunBashDetector detector;
        Rig rig;
        rig.s.handForward = Vector(1.0f, 0.0f, 0.0f);
        Check(CountFires(detector, rig, 10, [](Rig& r) { r.Move(Vector(3.0f, 0.0f, 0.0f), kZero, kZero, true); }) == 0);
    }
    {
        // Re-arms only after slowing down and the cooldown.
        GunBashDetector detector;
        Rig rig;
        rig.s.handForward = Vector(1.0f, 0.0f, 0.0f);
        auto shove = [](Rig& r) { r.Move(Vector(3.0f, 0.0f, 0.0f), kZero, kZero, true); };
        CountFires(detector, rig, 5, still);
        Check(CountFires(detector, rig, 5, shove) == 1);
        Check(CountFires(detector, rig, 5, still) == 0);
        Check(CountFires(detector, rig, 5, shove) == 0); // within 0.5 s cooldown
        Check(CountFires(detector, rig, 50, still) == 0);
        Check(CountFires(detector, rig, 5, shove) == 1);
    }
}

// Realistic reaches from a standing body model (eyes 1.65 m, metres, x
// forward, y RIGHT, z up; converted to Source's +y-left world here). The hand
// follows a minimum-jerk path at 90 Hz with tracking jitter, the barrel turns
// on the way, and the head bobs and turns a little. The gun-hand policy of the
// game is applied: no bash while the hand is at a holster or within 0.7 s of
// a squeeze (0.3 s of a release).
namespace realistic
{
    struct Pose { Vector hand; Vector barrel; };
    static Vector Body(float x, float yRight, float z) { return Vector(x, -yRight, 1.65f + z); }
    static float MinJerk(float u) { u = std::min(1.0f, std::max(0.0f, u)); return u * u * u * (10.0f + u * (-15.0f + 6.0f * u)); }
    static Vector Lerp(const Vector& a, const Vector& b, float t) { return a + (b - a) * t; }
    static unsigned g_seed = 12345u;
    static float Jitter() { g_seed = g_seed * 1664525u + 1013904223u; return ((g_seed >> 8) / 16777216.0f - 0.5f) * 0.002f; }

    struct Sim
    {
        GunBashDetector detector;
        Sample s{};
        Vector hand{}, barrel{ 1.0f, 0.0f, 0.0f }, prevHand{};
        float headYaw = 0.0f;
        float blockUntil = -1.0f;
        int fired = 0;
        bool started = false;
        Sim(const Vector& start) : hand(start), prevHand(start) { s.hmdPosition = Body(0, 0, 0); s.hmdUp = Vector(0, 0, 1); }
        void Frame(const Vector& newHand, const Vector& newBarrel, bool atHolster)
        {
            s.time += kDt;
            Vector h = newHand + Vector(Jitter(), Jitter(), Jitter());
            Vector b{};
            Normalize(newBarrel, b);
            // Angular velocity from the barrel's turn (deg/s).
            Vector axis = CrossProduct(barrel, b);
            const float sine = axis.Length();
            const Vector angular = sine > 1e-6f ? axis * (std::asin(std::min(1.0f, sine)) / kDt / kDegToRad / sine) : Vector(0, 0, 0);
            s.handVelocity = started ? (h - prevHand) * (1.0f / kDt) : Vector(0, 0, 0);
            s.handPosition = h;
            s.handForward = b;
            s.handAngularVelocityDeg = angular;
            const float bob = 0.01f * std::sin(s.time * 6.0f);
            s.hmdPosition = Body(0.0f, 0.0f, bob);
            const float yaw = headYaw * kDegToRad;
            s.hmdForward = Vector(std::cos(yaw), std::sin(yaw), 0.0f);
            s.hasHmdVelocity = false;
            const bool allowed = !atHolster && s.time >= blockUntil;
            if (detector.Update(s, allowed).fired) ++fired;
            prevHand = h; barrel = b; hand = newHand; started = true;
        }
        void Hold(float seconds, bool atHolster = false) { for (float t = 0; t < seconds; t += kDt) Frame(hand, barrel, atHolster); }
        // Moves to `target` at `speed` m/s average; the hand counts as at the
        // holster within `holsterRadius` of `holster`.
        void Move(const Vector& target, const Vector& targetBarrel, float speed, const Vector& holster, float holsterRadius,
            float headTurnDeg = 0.0f)
        {
            const Vector from = hand, fromBarrel = barrel;
            const float duration = std::max(0.15f, (target - from).Length() / speed);
            const float yaw0 = headYaw;
            for (float t = kDt; t <= duration + 1e-4f; t += kDt)
            {
                const float u = MinJerk(t / duration);
                headYaw = yaw0 + headTurnDeg * u;
                const Vector p = Lerp(from, target, u);
                Frame(p, Lerp(fromBarrel, targetBarrel, u), (p - holster).Length() <= holsterRadius);
            }
        }
        void Squeeze() { blockUntil = s.time + 0.7f; }
        void Release() { blockUntil = std::max(blockUntil, s.time + 0.3f); }
    };
}

static void TestGunBashRealisticReaches()
{
    using namespace realistic;
    const Vector ready = Body(0.40f, 0.15f, -0.35f);
    const Vector forward(1.0f, 0.0f, 0.0f);
    struct Target { const char* name; Vector point; Vector barrel; float headTurn; };
    const Target targets[] = {
        { "right hip", Body(-0.05f, 0.22f, -0.80f), Vector(0.2f, 0.0f, -1.0f), -25.0f },
        { "over the right shoulder", Body(-0.10f, 0.15f, -0.05f), Vector(-0.3f, 0.0f, 0.95f), -15.0f },
        { "right chest", Body(0.12f, 0.10f, -0.36f), Vector(0.3f, 0.9f, -0.3f), 0.0f },
        { "left chest", Body(0.12f, -0.10f, -0.36f), Vector(0.2f, 0.9f, -0.4f), 10.0f },
        { "over the left shoulder", Body(-0.10f, -0.14f, -0.06f), Vector(-0.4f, 0.3f, 0.85f), 20.0f },
        { "lower back", Body(-0.15f, 0.10f, -0.45f), Vector(0.0f, 0.3f, -0.95f), -30.0f },
    };
    for (const float speed : { 0.5f, 1.0f, 1.5f })
    {
        for (const Target& target : targets)
        {
            Sim sim(ready);
            sim.Hold(0.5f);
            // Reach, squeeze at the holster, come back out (drawing), let go.
            sim.Move(target.point, target.barrel, speed, target.point, 0.20f, target.headTurn);
            sim.Squeeze();
            sim.Hold(0.15f, true);
            sim.Move(ready, forward, speed, target.point, 0.20f, -target.headTurn);
            sim.Release();
            sim.Hold(0.5f);
            // An empty-handed reach that changes its mind halfway (no squeeze).
            sim.Move(Lerp(ready, target.point, 0.6f), Lerp(forward, target.barrel, 0.6f), speed, target.point, 0.20f);
            sim.Move(ready, forward, speed, target.point, 0.20f);
            sim.Hold(0.5f);
            if (sim.fired != 0)
                std::fprintf(stderr, "gun bash fired %d time(s) reaching for the %s at %.1f m/s\n", sim.fired, target.name, speed);
            Check(sim.fired == 0);
        }
    }
    // A real bash still works: a 0.30 m push along the barrel in 0.15-0.25 s.
    for (const float duration : { 0.15f, 0.2f, 0.25f })
    {
        Sim sim(ready);
        sim.Hold(0.5f);
        const Vector end = Body(0.70f, 0.15f, -0.30f);
        sim.Move(end, forward, (end - ready).Length() / duration, Body(-5, 0, 0), 0.0f);
        sim.Hold(0.3f);
        sim.Move(ready, forward, 0.6f, Body(-5, 0, 0), 0.0f);
        sim.Hold(0.6f);
        sim.Move(end, forward, (end - ready).Length() / duration, Body(-5, 0, 0), 0.0f);
        if (sim.fired != 2)
            std::fprintf(stderr, "a %.2f s push fired %d bash(es), expected 2\n", duration, sim.fired);
        Check(sim.fired == 2);
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

static void TestHandover()
{
    using namespace l4d2vr_gesture;
    HandoverDetector detector;
    HandoverTarget teammate{ 7, Vector(1.0f, 0.0f, 1.1f) };
    Sample s{};
    s.hmdPosition = Vector(0.0f, 0.0f, 1.6f);
    s.handForward = Vector(1.0f, 0.0f, 0.0f);
    // Item point (hand + 8 cm) held 10 cm from the teammate's chest for 0.3 s: one give.
    int fired = 0, target = 0;
    for (int i = 0; i <= 30; ++i)
    {
        s.time = i * 0.01f;
        s.handPosition = Vector(0.82f, 0.0f, 1.1f);
        const HandoverEvent e = detector.Update(s, &teammate, 1);
        if (e.fired) { ++fired; target = e.target; }
    }
    Check(fired == 1 && target == 7);
    // Staying there does not give again; leaving and coming back re-arms.
    for (int i = 31; i <= 60; ++i)
    {
        s.time = i * 0.01f;
        if (detector.Update(s, &teammate, 1).fired) ++fired;
    }
    Check(fired == 1);
    s.time = 0.61f;
    s.handPosition = Vector(0.2f, 0.0f, 1.1f);
    detector.Update(s, &teammate, 1);
    for (int i = 62; i <= 95; ++i)
    {
        s.time = i * 0.01f;
        s.handPosition = Vector(0.85f, 0.0f, 1.1f);
        if (detector.Update(s, &teammate, 1).fired) ++fired;
    }
    Check(fired == 2);
    // A brief touch is not a give.
    HandoverDetector brief;
    int briefFired = 0;
    for (int i = 0; i <= 10; ++i)
    {
        s.time = 2.0f + i * 0.01f;
        s.handPosition = Vector(0.85f, 0.0f, 1.1f);
        if (brief.Update(s, &teammate, 1).fired) ++briefFired;
    }
    Check(briefFired == 0);
    // Never near your own head.
    HandoverDetector nearHead;
    HandoverTarget atHead{ 3, Vector(0.1f, 0.0f, 1.55f) };
    int headFired = 0;
    for (int i = 0; i <= 40; ++i)
    {
        s.time = 3.0f + i * 0.01f;
        s.handPosition = Vector(0.02f, 0.0f, 1.55f);
        if (nearHead.Update(s, &atHead, 1).fired) ++headFired;
    }
    Check(headFired == 0);
}

int main()
{
    TestHandover();
    TestSwing();
    TestSwingIgnoresWalking();
    TestSwingValidation();
    TestThrust();
    TestGunBash();
    TestGunBashRealisticReaches();
    TestPills();
    TestAdrenaline();
    TestSampleGapRestartsTiming();
    std::printf("vr_gesture tests passed\n");
    return 0;
}

#ifdef _MSC_VER
#pragma warning(push, 0)
#endif
#include "../L4D2VR/sdk/vector.h"
#ifdef _MSC_VER
#pragma warning(pop)
#endif
#include "../L4D2VR/vr_gesture.h"
#include "../L4D2VR/vr_melee_inertia.h"
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>

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

// Blade along +z (up), hand moving sideways: the tip speed is the hand speed
// plus the wrist's contribution (0.62 m * angular speed).
static void TestSwing()
{
    const Vector slash(0.0f, -3.5f, 0.0f);       // hand 3.5 m/s to the right
    const Vector roll(600.0f, 0.0f, 0.0f);      // blade tipping the same way, 600 deg/s
    auto still = [&](Rig& r) { r.Still(); };
    auto swingStep = [&](Rig& r) { r.Move(slash, roll, kZero, true); };
    auto backStep = [&](Rig& r) { r.Move(slash * -1.0f, roll * -1.0f, kZero, true); };

    {
        // Weapon adopted mid-motion: no swing until it has rested.
        SwingDetector adopted;
        Rig a;
        Check(CountSwings(adopted, a, 12, swingStep) == 0 && !adopted.Armed());
        Check(CountSwings(adopted, a, 10, still) == 0 && adopted.Armed());
    }
    SwingDetector swing;
    Rig rig;
    Check(CountSwings(swing, rig, 10, still) == 0 && swing.Armed());

    // One swing fires exactly once, with its direction, once speed and travel are there.
    int starts = 0, startFrame = -1;
    for (int i = 0; i < 20; ++i)
    {
        swingStep(rig);
        const SwingEvent e = swing.Update(rig.s);
        if (e.started)
        {
            ++starts;
            startFrame = i;
            Check(e.direction.y < -0.9f && e.tipSpeed >= 4.5f && e.handleSpeed >= 2.5f);
            Check(e.tipTravel >= 0.43f && e.handleTravel >= 0.13f);
            Check(SwingQuality(e.tipSpeed, e.tipTravel, e.handleTravel) > 0.0f);
        }
        if (startFrame >= 0) Check(e.swinging);
    }
    Check(starts == 1);
    // Not on the first fast frame: the tip (~10 m/s) needs ~45 ms to cover 0.43 m.
    Check(startFrame >= 3 && startFrame <= 5);
    // Stopping ends the hit window.
    Check(CountSwings(swing, rig, 3, still) == 0 && !swing.Swinging());
    // A backhand straight out of a forehand counts too: reversing through a
    // standstill within 0.1 s is rest enough (Ancient Dungeon has no rest rule).
    {
        SwingDetector combo;
        Rig c;
        CountSwings(combo, c, 10, still);
        Check(CountSwings(combo, c, 30, swingStep) == 1);
        int frame = 0;
        Check(CountSwings(combo, c, 9, [&](Rig& r) {
            const float k = 1.0f - 2.0f * static_cast<float>(++frame) / 9.0f; // +1 -> -1 in 0.1 s
            r.Move(slash * k, roll * k, kZero, true); }) == 0);
        Check(CountSwings(combo, c, 20, backStep) == 1);
    }
    // Within the cooldown nothing new starts, however fast.
    {
        SwingParams p{};
        p.cooldown = 1.0f;
        SwingDetector cooled(p);
        Rig c;
        CountSwings(cooled, c, 10, still);
        Check(CountSwings(cooled, c, 20, swingStep) == 1);
        CountSwings(cooled, c, 5, still);
        Check(CountSwings(cooled, c, 20, backStep) == 0);
        CountSwings(cooled, c, 90, still);
        Check(CountSwings(cooled, c, 20, swingStep) == 1);
    }

    // The hit window lasts while blade and hand stay fast: a slower follow-through ends it.
    {
        SwingDetector window;
        Rig w;
        CountSwings(window, w, 10, still);
        Check(CountSwings(window, w, 8, swingStep) == 1 && window.Swinging());
        // Hand at 2 m/s, blade 2 + 0.62 * 2.6 = 3.6 m/s: still above both releases.
        CountSwings(window, w, 10, [&](Rig& r) { r.Move(Vector(0.0f, -2.0f, 0.0f), Vector(150.0f, 0.0f, 0.0f), kZero, true); });
        Check(window.Swinging());
        // The hand slows to 1 m/s (below 1.25) while the wrist still turns: the window closes.
        CountSwings(window, w, 10, [&](Rig& r) { r.Move(Vector(0.0f, -1.0f, 0.0f), Vector(300.0f, 0.0f, 0.0f), kZero, true); });
        Check(!window.Swinging());
    }
}

// Each gate rejects its own kind of motion.
static void TestSwingGates()
{
    auto still = [](Rig& r) { r.Still(); };
    auto run = [&](SwingParams params, const Vector& hand, const Vector& angular, int frames) {
        SwingDetector d(params);
        Rig r;
        CountSwings(d, r, 10, still);
        Check(d.Armed());
        return CountSwings(d, r, frames, [&](Rig& q) { q.Move(hand, angular, kZero, true); });
    };
    const SwingParams longWeapon{};
    SwingParams shortWeapon{};
    shortWeapon.minHandleSpeed = 3.0f;
    shortWeapon.bladeLength = 0.5f;

    // Wrist flick: the tip at 0.62 * 1000 deg/s = 10.8 m/s, but the hand is still.
    Check(run(longWeapon, kZero, Vector(1000.0f, 0.0f, 0.0f), 30) == 0);
    // Wrist flick with a little hand motion (1 m/s): still not a swing.
    Check(run(longWeapon, Vector(0.0f, -1.0f, 0.0f), Vector(1000.0f, 0.0f, 0.0f), 30) == 0);
    // Slow swing: hand 1.8 m/s, blade 1.8 + 0.62 * 3.5 = 4.0 m/s.
    Check(run(longWeapon, Vector(0.0f, -1.8f, 0.0f), Vector(200.0f, 0.0f, 0.0f), 60) == 0);
    // Fast enough but too short (a jab of 0.11 m at the hand, 0.30 m at the tip): no swing ...
    Check(run(longWeapon, Vector(0.0f, -3.5f, 0.0f), Vector(400.0f, 0.0f, 0.0f), 3) == 0);
    // ... the same motion carried on is one.
    Check(run(longWeapon, Vector(0.0f, -3.5f, 0.0f), Vector(400.0f, 0.0f, 0.0f), 20) == 1);
    // Hand at 2.7 m/s: enough for a long weapon, not for a short one.
    Check(run(longWeapon, Vector(0.0f, -2.7f, 0.0f), Vector(450.0f, 0.0f, 0.0f), 30) == 1);
    Check(run(shortWeapon, Vector(0.0f, -2.7f, 0.0f), Vector(450.0f, 0.0f, 0.0f), 30) == 0);
    Check(run(shortWeapon, Vector(0.0f, -3.4f, 0.0f), Vector(450.0f, 0.0f, 0.0f), 30) == 1);
    // A blade-first thrust (hand driven along the blade) is a shove, not a swing.
    Check(run(longWeapon, Vector(0.0f, 0.0f, 5.0f), kZero, 30) == 0);
    // Config values move the gates.
    SwingParams strict{};
    strict.startSpeed = 8.0f;
    Check(run(strict, Vector(0.0f, -3.5f, 0.0f), Vector(300.0f, 0.0f, 0.0f), 30) == 0);
}

static void TestSwingIgnoresWalking()
{
    const Vector walk(4.0f, 1.0f, 0.0f);
    const Vector slash(0.0f, -3.5f, 0.0f), roll(600.0f, 0.0f, 0.0f);
    // Hand and HMD translate together (thumbstick locomotion or physical walking).
    for (bool reportHmd : { true, false })
    {
        SwingDetector swing;
        Rig rig;
        Check(CountSwings(swing, rig, 10, [](Rig& r) { r.Still(); }) == 0);
        Check(CountSwings(swing, rig, 90, [&](Rig& r) { r.Move(walk, kZero, walk, reportHmd); }) == 0);
        // Head bob and sway on top of walking are far below the gates.
        Check(CountSwings(swing, rig, 30, [&](Rig& r) {
            r.Move(walk + Vector(0.0f, 0.0f, 0.3f), Vector(20.0f, 30.0f, 0.0f), walk, reportHmd); }) == 0);
        // A real swing while walking still fires.
        Check(CountSwings(swing, rig, 20, [&](Rig& r) { r.Move(walk + slash, roll, walk, reportHmd); }) == 1);
    }
    // Control: the walk's hand motion (a little faster, 5.2 m/s) without the HMD moving along is a swing.
    SwingDetector swing;
    Rig rig;
    CountSwings(swing, rig, 10, [](Rig& r) { r.Still(); });
    Check(CountSwings(swing, rig, 20, [&](Rig& r) { r.Move(walk * 1.25f, kZero, kZero, true); }) == 1);
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
    Check(run(Vector(0.0f, 3.0f, 0.0f), 10) == 1);   // a fast sideways sweep in front of the body
    Check(run(Vector(0.0f, -3.0f, 0.0f), 10) == 1);  // ... either way
    Check(run(Vector(0.0f, 2.0f, 0.0f), 10) == 0);   // a slower sideways move is aiming, not a sweep
    Check(run(Vector(0.0f, 2.6f, -2.6f), 10) == 0);  // 45 degrees down is not level (a reload or hip reach)
    Check(run(Vector(-1.5f, 3.0f, 0.0f), 10) == 0);  // swinging in toward the body (a chest reach)
    Check(run(Vector(0.5f, 0.0f, -3.0f), 10) == 0);  // downward reload motion (80 degrees down)
    Check(run(Vector(0.0f, 0.0f, -3.0f), 10) == 0);  // straight down
    Check(run(Vector(0.8f, 0.0f, 3.0f), 10) == 0);   // up (toward the shoulder)
    Check(run(Vector(-3.0f, 0.0f, 0.0f), 10) == 0);  // pulling the gun back toward the face
    Check(run(Vector(1.4f, 0.0f, 0.0f), 10) == 0);   // too slow
    {
        // The sweep is reported as one and shoves toward the gun, level.
        GunBashDetector detector;
        Rig rig;
        rig.s.handForward = Vector(1.0f, 0.0f, 0.0f);
        CountFires(detector, rig, 5, still);
        GunBashEvent fired{};
        for (int i = 0; i < 10 && !fired.fired; ++i)
        {
            rig.Move(Vector(0.0f, 3.0f, 0.0f), kZero, kZero, true);
            fired = detector.Update(rig.s);
        }
        Check(fired.fired && fired.sweep);
        Check(std::fabs(fired.direction.z) < 1e-5f && fired.direction.x > 0.5f);
    }
    {
        // Sweeping low at the hip or above the head is not a sweep.
        for (const float z : { 0.85f, 1.9f })
        {
            GunBashDetector detector;
            Rig rig;
            rig.s.handForward = Vector(1.0f, 0.0f, 0.0f);
            rig.s.handPosition.z = z;
            CountFires(detector, rig, 5, still);
            Check(CountFires(detector, rig, 10, [&](Rig& r) { r.Move(Vector(0.0f, 3.0f, 0.0f), kZero, kZero, true); }) == 0);
        }
    }
    {
        // Sweeps can be turned off.
        GunBashParams params{};
        params.sweepEnabled = false;
        GunBashDetector detector(params);
        Rig rig;
        rig.s.handForward = Vector(1.0f, 0.0f, 0.0f);
        CountFires(detector, rig, 5, still);
        Check(CountFires(detector, rig, 10, [&](Rig& r) { r.Move(Vector(0.0f, 3.0f, 0.0f), kZero, kZero, true); }) == 0);
    }
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

// Strikes and gun handling from the standing body model, at human speeds.
static void TestGunBashRealisticStrikes()
{
    using namespace realistic;
    const Vector ready = Body(0.40f, 0.15f, -0.35f);
    const Vector forward(1.0f, 0.0f, 0.0f);
    const Vector nowhere = Body(-5, 0, 0);
    // Pushes along the barrel: 0.20 m in up to 0.2 s, 0.30 m in up to 0.3 s
    // (peak hand speed 1.9-3.75 m/s).
    for (const float distance : { 0.20f, 0.30f })
    {
        for (const float duration : { 0.12f, 0.15f, 0.2f, 0.25f, 0.3f })
        {
            if (duration > distance + 1e-3f)
                continue;
            Sim sim(ready);
            sim.Hold(0.5f);
            const Vector end = ready + Vector(distance, 0.0f, 0.03f);
            sim.Move(end, forward, distance / duration, nowhere, 0.0f);
            sim.Hold(0.3f);
            sim.Move(ready, forward, 0.6f, nowhere, 0.0f);
            sim.Hold(0.4f);
            if (sim.fired != 1)
                std::fprintf(stderr, "a %.2f m push in %.2f s fired %d bash(es), expected 1\n", distance, duration, sim.fired);
            Check(sim.fired == 1);
        }
    }
    // Sideways sweeps across the front of the body, both ways, level or a
    // little downward, the barrel turning with the swing (0.45 m in 0.15-0.3 s).
    struct Sweep { const char* name; Vector from; Vector to; Vector fromBarrel; Vector toBarrel; };
    const Sweep sweeps[] = {
        { "right to left", Body(0.38f, 0.30f, -0.30f), Body(0.42f, -0.15f, -0.32f), Vector(0.8f, -0.6f, 0.0f), Vector(0.8f, 0.6f, 0.0f) },
        { "left to right", Body(0.40f, -0.10f, -0.30f), Body(0.38f, 0.35f, -0.35f), Vector(0.8f, 0.6f, 0.0f), Vector(0.8f, -0.6f, 0.0f) },
        { "down and left", Body(0.36f, 0.30f, -0.20f), Body(0.44f, -0.12f, -0.45f), Vector(0.7f, -0.5f, 0.3f), Vector(0.7f, 0.5f, -0.4f) },
    };
    for (const Sweep& sweep : sweeps)
    {
        for (const float duration : { 0.15f, 0.2f, 0.3f })
        {
            Sim sim(sweep.from);
            sim.barrel = sweep.fromBarrel;
            sim.Hold(0.5f);
            sim.Move(sweep.to, sweep.toBarrel, (sweep.to - sweep.from).Length() / duration, nowhere, 0.0f);
            sim.Hold(0.4f);
            if (sim.fired != 1)
                std::fprintf(stderr, "a %s sweep in %.2f s fired %d bash(es), expected 1\n", sweep.name, duration, sim.fired);
            Check(sim.fired == 1);
        }
    }

    // Gun handling that is not a strike.
    auto expectNone = [](const char* name, Sim& sim) {
        if (sim.fired != 0)
            std::fprintf(stderr, "gun bash fired %d time(s) while %s\n", sim.fired, name);
        Check(sim.fired == 0);
    };
    {
        // Aim sweeps: slow tracking, and a quick re-aim across 0.30 m in 0.3 s.
        Sim sim(ready);
        sim.Hold(0.5f);
        for (int i = 0; i < 3; ++i)
        {
            sim.Move(Body(0.42f, -0.10f, -0.33f), Vector(0.9f, 0.4f, 0.0f), 0.6f, nowhere, 0.0f, 20.0f);
            sim.Move(ready, forward, 0.6f, nowhere, 0.0f, -20.0f);
            sim.Move(Body(0.42f, 0.45f, -0.33f), Vector(0.9f, -0.45f, 0.0f), 1.0f, nowhere, 0.0f, -30.0f);
            sim.Move(ready, forward, 1.0f, nowhere, 0.0f, 30.0f);
        }
        sim.Hold(0.3f);
        sim.Move(Body(0.42f, -0.15f, -0.33f), Vector(0.9f, 0.4f, 0.0f), 0.30f / 0.3f, nowhere, 0.0f);
        sim.Hold(0.3f);
        sim.Move(Body(0.40f, 0.20f, -0.33f), Vector(0.9f, -0.3f, 0.0f), 0.35f / 0.3f, nowhere, 0.0f);
        sim.Hold(0.3f);
        expectNone("aiming", sim);
    }
    {
        // Turning the whole body 90 degrees with the gun raised: the arm swings
        // round with the head (2.4 m/s at the hand), but nothing struck.
        for (const float sign : { 1.0f, -1.0f })
        {
            Sim sim(ready);
            sim.Hold(0.5f);
            const float duration = 0.35f;
            const Vector offset = ready - Body(0, 0, 0);
            for (float t = kDt; t <= duration + 1e-4f; t += kDt)
            {
                const float yaw = sign * 90.0f * MinJerk(t / duration);
                sim.headYaw = yaw;
                const Vector p = Body(0, 0, 0) + RotateYawDeg(offset, yaw);
                sim.Frame(p, RotateYawDeg(forward, yaw), false);
            }
            sim.Hold(0.5f);
            expectNone("turning the body with the gun raised", sim);
        }
    }
    {
        // Two-handed aim with the support hand steadying: small, slow motion.
        Sim sim(ready);
        for (int i = 0; i < 270; ++i)
        {
            const float t = i * kDt;
            sim.Frame(ready + Vector(0.01f * std::sin(t * 7.0f), 0.015f * std::sin(t * 5.0f), 0.01f * std::sin(t * 9.0f)),
                forward, false);
        }
        expectNone("aiming two-handed", sim);
    }
    {
        // A reload: the gun comes in toward the chest and tilts, the off hand
        // slaps the magazine home (the gun hand is bumped up), the gun goes back out.
        Sim sim(ready);
        sim.Hold(0.5f);
        const Vector reload = Body(0.28f, 0.02f, -0.42f);
        for (const float speed : { 0.8f, 1.5f })
        {
            sim.Move(reload, Vector(0.6f, 0.6f, 0.5f), speed, nowhere, 0.0f, 10.0f);
            sim.Hold(0.4f);
            sim.Move(reload + Vector(0.0f, 0.0f, 0.03f), Vector(0.6f, 0.6f, 0.5f), 1.0f, nowhere, 0.0f);
            sim.Move(reload, Vector(0.6f, 0.6f, 0.5f), 1.0f, nowhere, 0.0f);
            sim.Hold(0.3f);
            // Rolling the gun to look at the ejection port, then racking it.
            sim.Move(reload, Vector(0.5f, -0.5f, 0.7f), 0.5f, nowhere, 0.0f);
            sim.Move(reload, Vector(0.6f, 0.6f, 0.5f), 0.5f, nowhere, 0.0f);
            sim.Move(ready, forward, speed, nowhere, 0.0f, -10.0f);
            sim.Hold(0.5f);
        }
        expectNone("reloading", sim);
    }
}

// The whole client path: the game places the hands in the world from tracking
// poses (the formula of VR::UpdateTracking: pivot about the head by the stick
// yaw, offset by the camera anchor), and the gesture code maps them back with
// a TrackingFrame. Artificial locomotion must never bash; real strikes during
// it still do.
namespace locomotion
{
    constexpr float kScale = 43.2f; // world units per metre (Source body scale)

    struct Game
    {
        // Physical state, tracking space, metres.
        // Standing away from the room centre; the game.s accumulated head offset
        // (hmdCorrected) drifts further over a session.
        Vector hmdLocal{ 3.0f, -2.0f, 1.65f };
        float headYawLocal = 30.0f;
        Vector handLocal{};                      // set from the body model below
        Vector handForwardLocal{ 1.0f, 0.0f, 0.0f };
        // Game state.
        Vector anchor{ 6300.0f, -640.0f, 900.0f }; // world units, includes the 64-unit eye height
        float yaw = 120.0f;
        Vector hmdCorrected{};
        Vector hmdLocalPrev{};
        bool started = false;

        // The game's own placement (vr_tracking.inl), in world units.
        void Place(Vector& hmdWorld, Vector& handWorld, Vector& handForwardWorld, Vector& hmdForwardWorld)
        {
            if (!started)
            {
                hmdCorrected = hmdLocal;
                hmdLocalPrev = hmdLocal;
                started = true;
            }
            Vector corrected = hmdCorrected + (hmdLocal - hmdLocalPrev);
            VectorPivotXY(corrected, hmdCorrected, yaw);
            hmdCorrected = corrected;
            hmdLocalPrev = hmdLocal;
            const Vector base = anchor - Vector(0.0f, 0.0f, 64.0f);
            hmdWorld = base + hmdCorrected * kScale;
            Vector hand = hmdCorrected + (handLocal - hmdLocal);
            VectorPivotXY(hand, hmdCorrected, yaw);
            handWorld = base + hand * kScale;
            const float handYaw = std::atan2(handForwardLocal.y, handForwardLocal.x) / kDegToRad + yaw;
            const float handPitch = std::asin(std::max(-1.0f, std::min(1.0f, handForwardLocal.z)));
            handForwardWorld = Vector(std::cos(handPitch) * std::cos(handYaw * kDegToRad),
                std::cos(handPitch) * std::sin(handYaw * kDegToRad), std::sin(handPitch));
            const float headYaw = (headYawLocal + yaw) * kDegToRad;
            hmdForwardWorld = Vector(std::cos(headYaw), std::sin(headYaw), 0.0f);
        }
        TrackingFrame Frame() const
        {
            TrackingFrame frame{};
            frame.origin = anchor - Vector(0.0f, 0.0f, 64.0f);
            frame.yawDeg = yaw;
            frame.hmdCorrected = hmdCorrected;
            frame.hmdLocal = hmdLocal;
            frame.scale = kScale;
            return frame;
        }
    };

    // Body-model hand point (x forward, y right, z up from the eyes) for a head yaw.
    static Vector HandAt(const Game& g, float x, float yRight, float z)
    {
        return g.hmdLocal + RotateYawDeg(Vector(x, -yRight, z), g.headYawLocal);
    }

    struct Pipeline
    {
        Game game;
        TrackingSampler sampler;
        GunBashDetector detector;
        float time = 1.0f;
        int frame = 0;
        int fired = 0, sweeps = 0;
        // The pre-fix client: anchor-pivot body frame, float time since boot, head velocity
        // differentiated by the detector over the sample clock.
        GunBashDetector legacy;
        double legacyClock = 274380.0; // three days of uptime
        Vector legacyPrevHand{};
        bool legacyHas = false;
        int legacyFired = 0;

        void Step(float dt)
        {
            ++frame;
            time += dt;
            Vector hmdWorld{}, handWorld{}, handForwardWorld{}, hmdForwardWorld{};
            game.Place(hmdWorld, handWorld, handForwardWorld, hmdForwardWorld);
            const TrackingFrame f = game.Frame();
            const Sample s = sampler.Build(time, f.ToTrackingPoint(handWorld), f.ToTrackingDirection(handForwardWorld),
                kZero, f.ToTrackingPoint(hmdWorld), f.ToTrackingDirection(hmdForwardWorld), Vector(0.0f, 0.0f, 1.0f));
            const GunBashEvent e = detector.Update(s);
            if (e.fired) { ++fired; if (e.sweep) ++sweeps; }

            legacyClock += dt;
            Sample l{};
            l.time = static_cast<float>(legacyClock);
            auto toBody = [&](const Vector& p) { return RotateYawDeg(p - game.anchor, -game.yaw) * (1.0f / kScale); };
            l.handPosition = toBody(handWorld);
            l.handForward = RotateYawDeg(handForwardWorld, -game.yaw);
            if (legacyHas) l.handVelocity = (l.handPosition - legacyPrevHand) * (1.0f / dt);
            legacyPrevHand = l.handPosition;
            legacyHas = true;
            l.hmdPosition = toBody(hmdWorld);
            l.hmdForward = RotateYawDeg(hmdForwardWorld, -game.yaw);
            l.hmdUp = Vector(0.0f, 0.0f, 1.0f);
            if (legacy.Update(l).fired) ++legacyFired;
        }
    };

    // Frame times of a 90 Hz headset with jitter.
    static float FrameDt(int frame) { return kDt * (1.0f + 0.12f * std::sin(frame * 1.7f)); }
}

static void TestLocomotionNeverBashes()
{
    using namespace locomotion;
    auto ready = [](Pipeline& p) {
        p.game.handLocal = HandAt(p.game, 0.40f, 0.15f, -0.35f);
        p.game.handForwardLocal = RotateYawDeg(Vector(1.0f, 0.0f, 0.0f), p.game.headYawLocal);
    };
    auto run = [&](Pipeline& p, float seconds, const auto& drive) {
        for (float t = 0.0f; t < seconds;)
        {
            const float dt = FrameDt(p.frame);
            drive(p, t, dt);
            p.Step(dt);
            t += dt;
        }
    };
    auto settle = [&](Pipeline& p) { run(p, 0.5f, [&](Pipeline& q, float, float) { ready(q); }); };

    struct Scene { const char* name; void (*drive)(Pipeline&, float, float); float seconds; };
    const Scene scenes[] = {
        { "smooth turning at full stick (300 deg/s), both ways", [](Pipeline& p, float t, float dt) {
            p.game.yaw += (t < 3.0f ? -300.0f : 300.0f) * dt; }, 6.0f },
        { "snap turning 45 degrees", [](Pipeline& p, float t, float dt) {
            const int step = static_cast<int>(t / 0.3f), prev = static_cast<int>((t - dt) / 0.3f);
            if (t > 0.0f && step != prev) p.game.yaw += (step % 8 < 4) ? 45.0f : -45.0f; }, 4.0f },
        { "walking at full speed while turning", [](Pipeline& p, float t, float dt) {
            const float heading = (p.game.yaw + p.game.headYawLocal) * kDegToRad;
            p.game.anchor = p.game.anchor + Vector(std::cos(heading), std::sin(heading), 0.0f) * (220.0f * dt);
            p.game.yaw += 150.0f * std::sin(t * 2.0f) * dt; }, 5.0f },
        { "teleports, ladders and being pushed", [](Pipeline& p, float t, float dt) {
            const int step = static_cast<int>(t / 0.7f), prev = static_cast<int>((t - dt) / 0.7f);
            if (t > 0.0f && step != prev) p.game.anchor = p.game.anchor + Vector(300.0f, -120.0f, 40.0f); // teleport
            if (t > 1.0f && t < 2.5f) p.game.anchor.z += 110.0f * dt;                                        // ladder
            if (t > 3.0f && t < 3.12f) p.game.anchor = p.game.anchor + Vector(-350.0f * dt, 200.0f * dt, 0.0f); // charger hit
            }, 4.0f },
        { "looking around while turning", [](Pipeline& p, float t, float dt) {
            p.game.headYawLocal = 30.0f + 50.0f * std::sin(t * 3.0f);
            p.game.yaw -= 200.0f * dt; }, 4.0f },
        { "walking around the room (head and hand together)", [](Pipeline& p, float t, float dt) {
            const Vector step = Vector(std::cos(t), std::sin(t * 0.7f), 0.0f) * (1.2f * dt);
            p.game.hmdLocal = p.game.hmdLocal + step;
            p.game.handLocal = p.game.handLocal + step;
            p.game.yaw += 100.0f * dt; }, 4.0f },
    };
    for (const Scene& scene : scenes)
    {
        Pipeline p;
        settle(p);
        run(p, scene.seconds, [&](Pipeline& q, float t, float dt) {
            if (std::string(scene.name).find("room") == std::string::npos) ready(q);
            scene.drive(q, t, dt);
        });
        if (p.fired != 0)
            std::fprintf(stderr, "gun bash fired %d time(s) while %s\n", p.fired, scene.name);
        Check(p.fired == 0);
        if (std::string(scene.name).find("smooth turning") != std::string::npos)
        {
            // The scene is not vacuous: the pre-fix client fired here.
            std::printf("  legacy client bashed %d time(s) while %s\n", p.legacyFired, scene.name);
            Check(p.legacyFired > 0);
        }
    }

    // Real strikes during artificial locomotion still register, once each.
    for (int mode = 0; mode < 3; ++mode)
    {
        Pipeline p;
        settle(p);
        auto locomote = [mode](Pipeline& q, float t, float dt) {
            if (mode == 0) q.game.yaw -= 300.0f * dt;
            if (mode == 1)
            {
                const int step = static_cast<int>(t / 0.25f), prev = static_cast<int>((t - dt) / 0.25f);
                if (t > 0.0f && step != prev) q.game.yaw += 45.0f;
            }
            if (mode == 2) q.game.anchor = q.game.anchor + Vector(220.0f * dt, 0.0f, 0.0f);
        };
        auto strike = [&](bool sweep) {
            const Vector from = sweep ? HandAt(p.game, 0.38f, 0.30f, -0.30f) : HandAt(p.game, 0.40f, 0.15f, -0.35f);
            const Vector to = sweep ? HandAt(p.game, 0.42f, -0.15f, -0.32f) : HandAt(p.game, 0.68f, 0.15f, -0.32f);
            run(p, 0.4f, [&](Pipeline& q, float t, float dt) { q.game.handLocal = from; locomote(q, t, dt); });
            run(p, 0.2f, [&](Pipeline& q, float t, float dt) {
                q.game.handLocal = from + (to - from) * realistic::MinJerk(t / 0.2f);
                locomote(q, t, dt); });
            run(p, 0.4f, [&](Pipeline& q, float t, float dt) { q.game.handLocal = to; locomote(q, t, dt); });
            run(p, 0.5f, [&](Pipeline& q, float t, float dt) {
                q.game.handLocal = to + (HandAt(q.game, 0.40f, 0.15f, -0.35f) - to) * realistic::MinJerk(t / 0.5f);
                locomote(q, t, dt); });
            run(p, 0.3f, [&](Pipeline& q, float t, float dt) { ready(q); locomote(q, t, dt); });
        };
        strike(false);
        strike(true);
        if (p.fired != 2 || p.sweeps != 1)
            std::fprintf(stderr, "strikes during locomotion mode %d: %d fired (%d sweeps), expected 2 (1)\n", mode, p.fired, p.sweeps);
        Check(p.fired == 2 && p.sweeps == 1);
    }
}

// The melee path of VR::UpdateMeleeMotion: tracking-space hand pose -> lag
// spring (welded grip, lagging rotation) -> sample from the lagged blade ->
// swing detector with the weapon's gates. Locomotion must never swing; real
// slashes during it count once each; wrist flicks and slow swings never do.
namespace meleepath
{
    using namespace locomotion;
    namespace mi = l4d2vr_melee_inertia;

    struct MeleePipeline
    {
        Game game;
        TrackingSampler sampler;
        mi::Spring spring;
        SwingDetector detector;
        float time = 1.0f;
        int frame = 0, swings = 0;
        bool swinging = false;
        float peakLag = 0.0f;
        float slashStart = 0.0f, slashSeconds = 1.0f, startFraction = -1.0f; // when in the slash it counted
        explicit MeleePipeline(const char* weapon)
        {
            const mi::Params p = mi::ParamsForMeleeScriptName(weapon);
            spring.SetParams(p);
            SwingParams sp{};
            sp.bladeLength = mi::TipSampleDistance(p);
            sp.minHandleSpeed = mi::ShortWeapon(p) ? 3.0f : 2.5f;
            detector.SetParams(sp);
        }
        void Step(float dt)
        {
            ++frame;
            time += dt;
            Vector hmdWorld{}, handWorld{}, handForwardWorld{}, hmdForwardWorld{};
            game.Place(hmdWorld, handWorld, handForwardWorld, hmdForwardWorld);
            const TrackingFrame f = game.Frame();
            const Vector hand = f.ToTrackingPoint(handWorld), forward = f.ToTrackingDirection(handForwardWorld);
            const Vector up(0.0f, 0.0f, 1.0f);
            mi::Pose target{};
            Check(mi::PoseFromAxes(hand, forward, CrossProduct(forward, up), up, target));
            Check(spring.Step(target, dt));
            Check((spring.GetState().position - hand).Length() < 1e-5f); // welded
            peakLag = std::max(peakLag, spring.LagDeg());
            Vector laggedForward{}, right{}, laggedUp{};
            mi::QuatToAxes(spring.GetState().orientation, laggedForward, right, laggedUp);
            const Sample sample = sampler.Build(time, spring.GetState().position, laggedForward,
                spring.GetState().angularVelocity * (1.0f / kDegToRad), f.ToTrackingPoint(hmdWorld),
                f.ToTrackingDirection(hmdForwardWorld), up);
            const SwingEvent e = detector.Update(sample);
            if (e.started)
            {
                ++swings;
                startFraction = (time - slashStart) / slashSeconds;
            }
            swinging = e.swinging;
        }
    };

    static Vector Yawed(const Game& g, float yawDeg, float pitchDeg = 0.0f)
    {
        const float yaw = (g.headYawLocal + yawDeg) * kDegToRad, pitch = pitchDeg * kDegToRad;
        return Vector(std::cos(pitch) * std::cos(yaw), std::cos(pitch) * std::sin(yaw), std::sin(pitch));
    }
}

static void TestMeleeSwingPath()
{
    using namespace meleepath;
    auto run = [](MeleePipeline& p, float seconds, const auto& drive) {
        for (float t = 0.0f; t < seconds;)
        {
            const float dt = FrameDt(p.frame);
            drive(p, t, dt);
            p.Step(dt);
            t += dt;
        }
    };
    auto ready = [](MeleePipeline& p) {
        p.game.handLocal = HandAt(p.game, 0.35f, 0.25f, -0.30f);
        p.game.handForwardLocal = Yawed(p.game, -20.0f, 40.0f); // blade up and forward
    };
    // A forehand slash from the right shoulder across the body (hand ~0.75 m,
    // blade ~110 degrees) in `seconds`, then back to ready slowly.
    auto slash = [&](MeleePipeline& p, float seconds, const auto& locomote) {
        const Vector from = HandAt(p.game, 0.30f, 0.40f, -0.15f), to = HandAt(p.game, 0.50f, -0.30f, -0.40f);
        // Wind up slowly (0.6 s), then hold.
        const Vector start = p.game.handLocal, startForward = p.game.handForwardLocal;
        run(p, 0.6f, [&](MeleePipeline& q, float t, float dt) {
            const float u = realistic::MinJerk(t / 0.6f);
            q.game.handLocal = start + (from - start) * u;
            Vector f = startForward + (Yawed(q.game, -70.0f, 30.0f) - startForward) * u;
            Normalize(f, q.game.handForwardLocal);
            locomote(q, t, dt); });
        run(p, 0.1f, [&](MeleePipeline& q, float t, float dt) { locomote(q, t, dt); });
        p.slashStart = p.time;
        p.slashSeconds = seconds;
        run(p, seconds, [&](MeleePipeline& q, float t, float dt) {
            const float u = realistic::MinJerk(t / seconds);
            q.game.handLocal = from + (to - from) * u;
            q.game.handForwardLocal = Yawed(q.game, -70.0f + 110.0f * u, 30.0f - 40.0f * u);
            locomote(q, t, dt); });
        run(p, 0.3f, [&](MeleePipeline& q, float t, float dt) { q.game.handLocal = to; locomote(q, t, dt); });
        run(p, 0.8f, [&](MeleePipeline& q, float t, float dt) {
            const float u = realistic::MinJerk(t / 0.8f);
            q.game.handLocal = to + (from - to) * u;
            q.game.handForwardLocal = Yawed(q.game, 40.0f - 110.0f * u, -10.0f + 40.0f * u);
            locomote(q, t, dt); });
        run(p, 0.3f, [&](MeleePipeline& q, float t, float dt) { locomote(q, t, dt); });
    };
    auto none = [](MeleePipeline&, float, float) {};

    for (const char* weapon : { "katana", "knife", "fireaxe", "machete" })
    {
        // Slashes at three speeds: 0.75 s (slow: hand ~2 m/s) never counts,
        // 0.25 s and 0.15 s count once each, return strokes never.
        int counted[3] = {};
        const float durations[3] = { 0.75f, 0.25f, 0.15f };
        for (int i = 0; i < 3; ++i)
        {
            MeleePipeline p(weapon);
            run(p, 0.5f, [&](MeleePipeline& q, float, float) { ready(q); });
            slash(p, durations[i], none);
            counted[i] = p.swings;
            if (i > 0)
            {
                // Like Ancient Dungeon's travel gate, the first part of the arc
                // (until the tip is 0.43 m from where it was) does not count; the
                // middle and the end of it do.
                std::printf("  %s %.2f s slash: counts %.0f %% into it, peak lag %.1f deg\n", weapon, durations[i],
                    p.startFraction * 100.0f, p.peakLag);
                Check(p.startFraction > 0.2f && p.startFraction < 0.6f);
            }
            if (i == 2) Check(p.peakLag > 12.0f); // a fast slash trails visibly
        }
        if (counted[0] != 0 || counted[1] != 1 || counted[2] != 1)
            std::fprintf(stderr, "%s: slashes counted %d %d %d (want 0 1 1)\n", weapon, counted[0], counted[1], counted[2]);
        Check(counted[0] == 0 && counted[1] == 1 && counted[2] == 1);

        // Wrist flicks: the hand still, the blade whipped 120 degrees in 0.12 s (peak 1900 deg/s).
        MeleePipeline flick(weapon);
        run(flick, 0.5f, [&](MeleePipeline& q, float, float) { ready(q); });
        for (int i = 0; i < 4; ++i)
        {
            run(flick, 0.12f, [&](MeleePipeline& q, float t, float) {
                const float u = realistic::MinJerk(t / 0.12f);
                q.game.handForwardLocal = Yawed(q.game, (i % 2 ? 50.0f : -70.0f) + (i % 2 ? -120.0f : 120.0f) * u, 20.0f); });
            run(flick, 0.3f, none);
        }
        Check(flick.swings == 0);
    }

    // Locomotion with the weapon held ready (or swept slowly while aiming) never swings.
    struct Scene { const char* name; void (*drive)(meleepath::MeleePipeline&, float, float); float seconds; };
    const Scene scenes[] = {
        { "smooth turning at full stick, both ways", [](meleepath::MeleePipeline& p, float t, float dt) {
            p.game.yaw += (t < 3.0f ? -300.0f : 300.0f) * dt; }, 6.0f },
        { "snap turning 45 degrees", [](meleepath::MeleePipeline& p, float t, float dt) {
            const int step = static_cast<int>(t / 0.3f), prev = static_cast<int>((t - dt) / 0.3f);
            if (t > 0.0f && step != prev) p.game.yaw += (step % 8 < 4) ? 45.0f : -45.0f; }, 4.0f },
        { "walking at full speed while turning", [](meleepath::MeleePipeline& p, float t, float dt) {
            const float heading = (p.game.yaw + p.game.headYawLocal) * kDegToRad;
            p.game.anchor = p.game.anchor + Vector(std::cos(heading), std::sin(heading), 0.0f) * (220.0f * dt);
            p.game.yaw += 150.0f * std::sin(t * 2.0f) * dt; }, 5.0f },
        { "teleports, ladders and being pushed", [](meleepath::MeleePipeline& p, float t, float dt) {
            const int step = static_cast<int>(t / 0.7f), prev = static_cast<int>((t - dt) / 0.7f);
            if (t > 0.0f && step != prev) p.game.anchor = p.game.anchor + Vector(300.0f, -120.0f, 40.0f);
            if (t > 1.0f && t < 2.5f) p.game.anchor.z += 110.0f * dt;
            if (t > 3.0f && t < 3.12f) p.game.anchor = p.game.anchor + Vector(-350.0f * dt, 200.0f * dt, 0.0f); }, 4.0f },
        { "walking around the room with the weapon raised", [](meleepath::MeleePipeline& p, float t, float dt) {
            const Vector step = Vector(std::cos(t), std::sin(t * 0.7f), 0.0f) * (1.2f * dt);
            p.game.hmdLocal = p.game.hmdLocal + step;
            p.game.handLocal = p.game.handLocal + step;
            p.game.yaw += 100.0f * dt; }, 4.0f },
    };
    for (const Scene& scene : scenes)
    {
        for (const char* weapon : { "katana", "knife" })
        {
            meleepath::MeleePipeline p(weapon);
            run(p, 0.5f, [&](meleepath::MeleePipeline& q, float, float) { ready(q); });
            const bool room = std::string(scene.name).find("room") != std::string::npos;
            run(p, scene.seconds, [&](meleepath::MeleePipeline& q, float t, float dt) {
                if (!room) ready(q);
                // A slow aim sweep of the blade on top (60 deg/s).
                q.game.handForwardLocal = meleepath::Yawed(q.game, -20.0f + 30.0f * std::sin(t * 2.0f), 40.0f);
                scene.drive(q, t, dt); });
            if (p.swings != 0)
                std::fprintf(stderr, "%s swung %d time(s) while %s\n", weapon, p.swings, scene.name);
            Check(p.swings == 0);
        }
    }
    // Real slashes during stick turning, snap turning and walking count once each.
    for (int mode = 0; mode < 3; ++mode)
    {
        meleepath::MeleePipeline p("katana");
        run(p, 0.5f, [&](meleepath::MeleePipeline& q, float, float) { ready(q); });
        auto locomote = [mode](meleepath::MeleePipeline& q, float t, float dt) {
            if (mode == 0) q.game.yaw -= 300.0f * dt;
            if (mode == 1)
            {
                const int step = static_cast<int>(t / 0.25f), prev = static_cast<int>((t - dt) / 0.25f);
                if (t > 0.0f && step != prev) q.game.yaw += 45.0f;
            }
            if (mode == 2) q.game.anchor = q.game.anchor + Vector(220.0f * dt, 0.0f, 0.0f);
        };
        slash(p, 0.2f, locomote);
        slash(p, 0.2f, locomote);
        if (p.swings != 2) std::fprintf(stderr, "slashes during locomotion mode %d: %d swings, expected 2\n", mode, p.swings);
        Check(p.swings == 2);
    }
}

static void TestTrackingFrameAndClock()
{
    // The tracking frame inverts the game's placement exactly, whatever the turn.
    locomotion::Game game;
    game.handLocal = locomotion::HandAt(game, 0.4f, 0.15f, -0.35f);
    for (const float yaw : { 0.0f, 45.0f, 137.0f, 271.0f, 359.0f })
    {
        game.yaw = yaw;
        Vector hmdWorld{}, handWorld{}, handForwardWorld{}, hmdForwardWorld{};
        game.Place(hmdWorld, handWorld, handForwardWorld, hmdForwardWorld);
        const TrackingFrame f = game.Frame();
        Check(f.Valid());
        Check((f.ToTrackingPoint(handWorld) - game.handLocal).Length() < 1e-3f);
        Check((f.ToTrackingPoint(hmdWorld) - game.hmdLocal).Length() < 1e-3f);
        Check((f.ToWorldPoint(game.handLocal) - handWorld).Length() < 0.05f); // world units
        Check((f.ToTrackingDirection(handForwardWorld) - game.handForwardLocal).Length() < 1e-4f);
    }
    TrackingFrame bad{};
    bad.scale = 0.0f;
    Check(!bad.Valid());

    // Session time keeps 11 ms frames distinct after days of uptime.
    SessionClock clock;
    float a = 0.0f, b = 0.0f;
    Check(clock.Seconds(274380.0, a));
    Check(clock.Seconds(274380.011, b));
    Check(b - a > 0.0105f && b - a < 0.0115f);
    Check(clock.Seconds(274380.0 + 1700.0, b) && b > 1700.0f);
    Check(!clock.Seconds(274380.0 + 1900.0, b) && b == SessionClock::kStart); // rebased
    Check(!clock.Seconds(1000.0, b));                                         // clock went backwards
    Check(static_cast<float>(274380.011) == static_cast<float>(274380.0) ||
        static_cast<float>(274380.022) - static_cast<float>(274380.0) != 0.022f); // why: raw float time is coarse

    // The sampler differentiates hand and head over the same interval.
    TrackingSampler sampler;
    Sample s = sampler.Build(1.0f, Vector(0, 0, 0), Vector(1, 0, 0), kZero, Vector(0, 0, 1), Vector(1, 0, 0), Vector(0, 0, 1));
    Check(s.hasHmdVelocity && s.handVelocity.Length() == 0.0f);
    s = sampler.Build(1.01f, Vector(0.03f, 0, 0), Vector(1, 0, 0), kZero, Vector(0.01f, 0, 1), Vector(1, 0, 0), Vector(0, 0, 1));
    Check(std::fabs(s.handVelocity.x - 3.0f) < 0.01f && std::fabs(s.hmdVelocity.x - 1.0f) < 0.01f);
    s = sampler.Build(1.5f, Vector(0.5f, 0, 0), Vector(1, 0, 0), kZero, Vector(0.01f, 0, 1), Vector(1, 0, 0), Vector(0, 0, 1));
    Check(s.handVelocity.Length() == 0.0f); // a hitch restarts
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
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    TestHandover();
    TestSwing();
    TestSwingGates();
    TestSwingIgnoresWalking();
    TestSwingValidation();
    TestThrust();
    TestGunBash();
    TestGunBashRealisticReaches();
    TestGunBashRealisticStrikes();
    TestTrackingFrameAndClock();
    TestLocomotionNeverBashes();
    TestMeleeSwingPath();
    TestPills();
    TestAdrenaline();
    TestSampleGapRestartsTiming();
    std::printf("vr_gesture tests passed\n");
    return 0;
}

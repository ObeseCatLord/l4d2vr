#ifdef _MSC_VER
#pragma warning(push, 0)
#endif
#include "../L4D2VR/sdk/vector.h"
#ifdef _MSC_VER
#pragma warning(pop)
#endif
#include "../L4D2VR/vr_melee_inertia.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

static void CheckImpl(bool result, int line)
{
    if (!result) { std::fprintf(stderr, "Check failed at line %d\n", line); std::abort(); }
}
#define Check(result) CheckImpl((result), __LINE__)

using namespace l4d2vr_melee_inertia;

static bool Near(const Vector& a, const Vector& b, float tolerance = 1e-4f) { return (a - b).Length() < tolerance; }
static bool NearQuat(const Quat& a, const Quat& b, float toleranceRadians = 1e-3f)
{
    return QuatAngleBetween(a, b) < toleranceRadians;
}

static void TestQuaternionUtilities()
{
    const Vector f(1.0f, 0.0f, 0.0f), r(0.0f, -1.0f, 0.0f), u(0.0f, 0.0f, 1.0f);
    Quat q{};
    Check(QuatFromAxes(f, r, u, q) && NearQuat(q, Quat{}));
    // Yaw 90 degrees left: forward -> +y, right -> +x.
    Check(QuatFromAxes(Vector(0.0f, 1.0f, 0.0f), Vector(1.0f, 0.0f, 0.0f), u, q));
    Vector f2{}, r2{}, u2{};
    QuatToAxes(q, f2, r2, u2);
    Check(Near(f2, Vector(0.0f, 1.0f, 0.0f)) && Near(r2, Vector(1.0f, 0.0f, 0.0f)) && Near(u2, u));
    Check(NearQuat(q, QuatFromAxisAngle(u, kPi * 0.5f)));
    // Arbitrary basis round trips through all quaternion branches.
    const Quat samples[] = { QuatFromAxisAngle(Vector(1.0f, 2.0f, 3.0f), 2.9f),
        QuatFromAxisAngle(Vector(1.0f, 0.0f, 0.0f), 3.1f), QuatFromAxisAngle(Vector(0.0f, 1.0f, 0.0f), 3.1f),
        QuatFromAxisAngle(Vector(0.0f, 0.0f, 1.0f), 3.1f), QuatFromAxisAngle(Vector(-1.0f, 0.5f, 0.2f), 0.7f) };
    for (const Quat& sample : samples)
    {
        QuatToAxes(sample, f2, r2, u2);
        Quat back{};
        Check(QuatFromAxes(f2, r2, u2, back) && NearQuat(back, sample));
        Check(NearQuat(QuatExp(QuatLog(sample)), sample));
        Check(NearQuat(QuatMultiply(sample, QuatConjugate(sample)), Quat{}));
    }
    // Degenerate up falls back to right.
    Check(QuatFromAxes(f, r, f, q) && NearQuat(q, Quat{}));
    Check(!QuatFromAxes(Vector(0.0f, 0.0f, 0.0f), r, u, q));
    const float nan = std::numeric_limits<float>::quiet_NaN();
    Check(!QuatFromAxes(Vector(nan, 0.0f, 0.0f), r, u, q));
    // Slerp halfway between identity and 90-degree yaw is 45 degrees.
    const Quat yaw90 = QuatFromAxisAngle(u, kPi * 0.5f);
    Check(NearQuat(QuatSlerp(Quat{}, yaw90, 0.5f), QuatFromAxisAngle(u, kPi * 0.25f)));
    Check(NearQuat(QuatSlerp(Quat{}, yaw90, 0.0f), Quat{}) && NearQuat(QuatSlerp(Quat{}, yaw90, 1.0f), yaw90));
    // Log takes the shortest path even for the negated quaternion.
    const Quat negated{ -yaw90.w, -yaw90.x, -yaw90.y, -yaw90.z };
    Check(Near(QuatLog(negated), Vector(0.0f, 0.0f, kPi * 0.5f), 1e-4f));
}

static void TestPresets()
{
    std::size_t count = 0;
    const MeleeProfile* const profiles = MeleeProfiles(count);
    Check(count == 16);
    for (std::size_t i = 0; i < count; ++i)
    {
        const Params p = ParamsForMeleeScriptName(profiles[i].name);
        Check(ValidParams(p) && p.positionLocked);
        Check(p.rotationHz >= 7.0f && p.rotationHz <= 17.0f);
        Check(p.maxLagDeg >= 15.0f && p.maxLagDeg <= 25.0f);
        // damping / stiffness ~ 23 ms for the swung weapons (zeta ~ 0.072 x Hz).
        if (std::strcmp(profiles[i].name, "riotshield") != 0 && std::strcmp(profiles[i].name, "chainsaw") != 0)
            Check(std::fabs(p.rotationZeta - 0.072f * p.rotationHz) < 0.03f);
        Check(p.motionMelee == (std::strcmp(profiles[i].name, "chainsaw") != 0));
    }
    // The two reference weapons.
    const Params katana = ParamsForMeleeScriptName("katana"), knife = ParamsForMeleeScriptName("knife");
    Check(katana.rotationHz == 11.3f && katana.rotationZeta == 0.81f && katana.maxLagDeg == 20.0f && katana.reach == 1.0f);
    Check(knife.rotationHz == 17.0f && knife.rotationZeta == 1.2f && knife.maxLagDeg == 15.0f && knife.reach == 0.3f);
    // Long = sword-like, short = dagger-like.
    for (const char* name : { "knife", "tonfa", "machete", "frying_pan", "riotshield" })
        Check(ShortWeapon(ParamsForMeleeScriptName(name)));
    for (const char* name : { "crowbar", "katana", "baseball_bat", "cricket_bat", "golfclub", "electric_guitar",
             "fireaxe", "shovel", "sledgehammer", "pitchfork", "unknown_mod_weapon", "" })
        Check(!ShortWeapon(ParamsForMeleeScriptName(name)));
    // Where the swing speed is measured.
    Check(std::fabs(TipSampleDistance(katana) - 0.73f) < 1e-5f && TipSampleDistance(knife) == 0.5f);
    Check(std::fabs(TipSampleDistance(ParamsForMeleeScriptName("pitchfork")) - 1.022f) < 1e-4f);
    // Names: prefix and case ignored, unknown -> the default (a medium sword-like weapon).
    Check(ParamsForMeleeScriptName("weapon_Katana").rotationHz == 11.3f);
    Check(ParamsForMeleeScriptName("FireAxe").rotationHz == 9.0f);
    Check(ParamsForMeleeScriptName("fireaxe2").rotationHz == DefaultParams().rotationHz);
    Check(ParamsForMeleeScriptName(nullptr).rotationHz == DefaultParams().rotationHz);
    Check(ParamsForMeleeScriptName("weapon_").rotationHz == DefaultParams().rotationHz);
    Check(!ParamsForMeleeScriptName("weapon_chainsaw").motionMelee && ParamsForMeleeScriptName("unknown").motionMelee);
    // Invalid params fall back to defaults instead of producing NaN.
    Params bad = katana;
    bad.rotationZeta = -1.0f;
    Spring spring(bad);
    Check(ValidParams(spring.GetParams()) && spring.GetParams().rotationZeta == DefaultParams().rotationZeta);
}

// ---------------------------------------------------------------------------
// Reference: Ancient Dungeon VR's weapon joint as a 1-DOF model (drive spring k,
// damper c on the absolute angular velocity, soft limit at 20 deg with spring
// lk and damper lc), integrated finely. MELEE.md section 1.
struct AdJoint
{
    float inertia;
    float Peak(float sweepDeg, float seconds, float& overshootDeg) const
    {
        const double k = 3500.0, c = 80.0, lk = 3000.0, lc = 80.0, limit = 20.0 * kDegToRad;
        double th = 0.0, w = 0.0, t = 0.0, peak = 0.0, over = 0.0;
        const double h = 1.0 / 20000.0;
        while (t < seconds + 0.4)
        {
            t += h;
            const double s = std::min(t / seconds, 1.0);
            const double target = sweepDeg * kDegToRad * (10 * s * s * s - 15 * s * s * s * s + 6 * s * s * s * s * s);
            const double e = th - target;
            double tau = -k * e - c * w;
            if (std::fabs(e) > limit) tau += -lk * (e - std::copysign(limit, e)) - lc * w;
            w += tau / inertia * h;
            th += w * h;
            const double lag = (target - th) * kRadToDeg;
            if (t <= seconds) peak = std::max(peak, std::fabs(lag));
            else over = std::max(over, -lag);
        }
        overshootDeg = static_cast<float>(over);
        return static_cast<float>(peak);
    }
};

static float MinJerk(float u)
{
    u = std::clamp(u, 0.0f, 1.0f);
    return u * u * u * (10.0f + u * (-15.0f + 6.0f * u));
}

// Our spring following the same swing about one axis at a frame rate.
static float SpringPeak(const Params& p, float sweepDeg, float seconds, float frameHz, float& overshootDeg,
    float* peakSpeedDeg = nullptr)
{
    Spring spring(p);
    const Vector axis(0.0f, 0.0f, 1.0f);
    const Vector grip(0.2f, -0.3f, 1.2f);
    spring.Reset(Pose{ grip, Quat{} });
    float peak = 0.0f, over = 0.0f, speed = 0.0f;
    const float dt = 1.0f / frameHz;
    for (float t = dt; t < seconds + 0.4f; t += dt)
    {
        const float target = sweepDeg * kDegToRad * MinJerk(t / seconds);
        const Quat goal = QuatFromAxisAngle(axis, target);
        Check(spring.Step(Pose{ grip, goal }, dt));
        // Signed lag about the swing axis (positive = behind).
        const Vector lag = QuatLog(QuatMultiply(goal, QuatConjugate(spring.GetState().orientation)));
        const float lagDeg = lag.z * kRadToDeg;
        Check(std::fabs(lag.x) < 1e-3f && std::fabs(lag.y) < 1e-3f); // stays in the swing plane
        Check(std::fabs(std::fabs(lagDeg) - spring.LagDeg()) < 0.05f);
        if (t <= seconds) peak = std::max(peak, lagDeg);
        else over = std::max(over, -lagDeg);
        speed = std::max(speed, spring.AngularSpeedDegPerSec());
        // The grip never leaves the hand.
        Check(Near(spring.GetState().position, grip, 1e-6f));
    }
    overshootDeg = over;
    if (peakSpeedDeg) *peakSpeedDeg = speed;
    return peak;
}

// Constant angular speed for 0.6 s: the settled lag.
static float SteadyLag(const Params& p, float degPerSec)
{
    Spring spring(p);
    spring.Reset(Pose{});
    const float dt = 1.0f / 90.0f;
    for (float t = dt; t < 0.6f; t += dt)
        spring.Step(Pose{ Vector(0.0f, 0.0f, 0.0f), QuatFromAxisAngle(Vector(1.0f, 0.0f, 0.0f), degPerSec * kDegToRad * t) }, dt);
    return spring.LagDeg();
}

static void TestLagCurve()
{
    const Params katana = ParamsForMeleeScriptName("katana"), knife = ParamsForMeleeScriptName("knife");
    // Ancient Dungeon's two weapons as our parameters (sqrt(3500 / I), 80 / (2 sqrt(3500 I)), 20 deg).
    Params sword = katana, dagger = knife;
    dagger.rotationHz = 16.9f;
    dagger.rotationZeta = 1.21f;
    dagger.maxLagDeg = 20.0f;
    const AdJoint adSword{ 0.69f }, adDagger{ 0.31f };

    // Below the soft limit the lag is Ancient Dungeon's: speed-proportional,
    // 2 zeta / omega = 22.9 ms of angular speed.
    Check(std::fabs(SteadyLagDeg(katana, 500.0f) - 11.4f) < 0.2f && std::fabs(SteadyLagDeg(knife, 500.0f) - 11.2f) < 0.2f);
    for (const Params& p : { katana, knife })
    {
        const float lag = SteadyLag(p, 500.0f);
        Check(std::fabs(lag - SteadyLagDeg(p, 500.0f)) < 0.6f); // ~11 deg at 500 deg/s
        Check(lag > 10.5f && lag < 12.5f);
    }
    // A 90 degree swing in 0.3 s (peak 560 deg/s): within 5 % of the reference joint.
    float over = 0.0f, adOver = 0.0f;
    const float swordPeak = SpringPeak(sword, 90.0f, 0.3f, 90.0f, over), adSwordPeak = adSword.Peak(90.0f, 0.3f, adOver);
    const float daggerPeak = SpringPeak(dagger, 90.0f, 0.3f, 90.0f, over), adDaggerPeak = adDagger.Peak(90.0f, 0.3f, adOver);
    std::printf("  90 deg / 0.3 s: sword %.1f deg (reference %.1f), dagger %.1f deg (reference %.1f)\n",
        swordPeak, adSwordPeak, daggerPeak, adDaggerPeak);
    Check(std::fabs(swordPeak - adSwordPeak) < 0.05f * adSwordPeak);
    Check(std::fabs(daggerPeak - adDaggerPeak) < 0.05f * adDaggerPeak);
    Check(swordPeak > 10.0f && swordPeak < 14.0f && daggerPeak > 10.0f && daggerPeak < 14.0f);

    // Faster swings: the soft limit holds the lag near it (MELEE.md: katana
    // 20-25 deg at 900 deg/s, the knife at most ~17-18), while the reference
    // joint's limit damper on the absolute speed drags the blade further behind.
    struct Swing { float sweep, seconds; };
    const Swing fast[] = { { 120.0f, 0.25f }, { 150.0f, 0.2f } };
    for (const Swing& sw : fast)
    {
        const float ks = SpringPeak(katana, sw.sweep, sw.seconds, 90.0f, over);
        const float kn = SpringPeak(knife, sw.sweep, sw.seconds, 90.0f, over);
        const float ref = adSword.Peak(sw.sweep, sw.seconds, adOver);
        std::printf("  %.0f deg / %.2f s (peak %.0f deg/s): katana %.1f deg, knife %.1f deg (reference sword %.1f)\n",
            sw.sweep, sw.seconds, 1.875f * sw.sweep / sw.seconds, ks, kn, ref);
        Check(ks < ref && ks >= katana.maxLagDeg - 1.0f && ks <= katana.maxLagDeg + 7.0f);
        Check(kn < ks && kn <= knife.maxLagDeg + 8.0f);
    }
    const float katana900 = SteadyLag(katana, 900.0f), knife900 = SteadyLag(knife, 900.0f);
    std::printf("  steady 900 deg/s: katana %.1f deg, knife %.1f deg\n", katana900, knife900);
    Check(katana900 >= 19.5f && katana900 <= 25.0f);
    Check(knife900 >= 15.0f && knife900 <= 18.5f);

    // Follow-through: the long weapon swings a little past (zeta 0.81), the knife never does.
    float katanaOver = 0.0f, knifeOver = 0.0f;
    SpringPeak(katana, 90.0f, 0.3f, 90.0f, katanaOver);
    SpringPeak(knife, 90.0f, 0.3f, 90.0f, knifeOver);
    Check(katanaOver > 0.01f && katanaOver < 1.0f);
    Check(knifeOver < 1e-3f);
    // Heavier weapons ring more after the swing.
    float sledgeOver = 0.0f;
    SpringPeak(ParamsForMeleeScriptName("sledgehammer"), 90.0f, 0.3f, 90.0f, sledgeOver);
    Check(sledgeOver > katanaOver);

    // The same swing at 72-144 Hz gives the same lag (the goal moves within the frame).
    const float at90 = SpringPeak(katana, 120.0f, 0.25f, 90.0f, over);
    for (float hz : { 72.0f, 120.0f, 144.0f })
        Check(std::fabs(SpringPeak(katana, 120.0f, 0.25f, hz, over) - at90) < 0.04f * at90);
}

static void TestLagLimit()
{
    const Params katana = ParamsForMeleeScriptName("katana");
    // No angular speed cap: a 1400 deg/s swing is followed at full speed
    // (the old presets capped the weapon at 420-700 deg/s).
    float over = 0.0f, speed = 0.0f;
    SpringPeak(katana, 150.0f, 0.2f, 90.0f, over, &speed);
    Check(speed > 1300.0f);
    // The soft limit: without it the lag keeps growing with speed.
    Params unlimited = katana;
    unlimited.maxLagDeg = 0.0f;
    for (float degPerSec : { 1400.0f, 2000.0f })
    {
        const float limited = SteadyLag(katana, degPerSec), free = SteadyLag(unlimited, degPerSec);
        std::printf("  steady %.0f deg/s: katana %.1f deg (no limit %.1f)\n", degPerSec, limited, free);
        Check(free > SteadyLagDeg(katana, degPerSec) - 1.5f);
        Check(limited < free - 4.0f && limited > katana.maxLagDeg);
        Check(limited < 36.0f);
    }
    Check(SteadyLag(katana, 1400.0f) <= 28.0f);
    // A glitch that turns the controller 170 degrees in a frame snaps instead of spinning the weapon round.
    Spring spring(katana);
    spring.Reset(Pose{});
    const Pose flipped{ Vector(0.0f, 0.0f, 0.0f), QuatFromAxisAngle(Vector(0.0f, 1.0f, 0.0f), 170.0f * kDegToRad) };
    Check(spring.Step(flipped, 1.0f / 90.0f));
    Check(NearQuat(spring.GetState().orientation, flipped.orientation) && spring.AngularSpeedDegPerSec() == 0.0f);
    // A 60 degree jump does not snap: the weapon swings after it and settles.
    spring.Reset(Pose{});
    const Pose turned{ Vector(0.0f, 0.0f, 0.0f), QuatFromAxisAngle(Vector(0.0f, 1.0f, 0.0f), 60.0f * kDegToRad) };
    spring.Step(turned, 1.0f / 90.0f);
    Check(spring.LagDeg() > 30.0f);
    for (int frame = 0; frame < 90; ++frame) spring.Step(turned, 1.0f / 90.0f);
    Check(spring.LagDeg() < 0.1f);
}

// The grip stays welded to the hand however the hand moves; only the
// rotation lags, pivoting about the grip.
static void TestWeldedGrip()
{
    for (const char* name : { "katana", "knife", "sledgehammer", "chainsaw", "unknown" })
    {
        Spring spring(ParamsForMeleeScriptName(name));
        Pose hand{ Vector(0.3f, -0.2f, 1.2f), QuatFromAxisAngle(Vector(0.0f, 0.0f, 1.0f), 0.3f) };
        spring.Reset(hand);
        float maxLag = 0.0f;
        for (int frame = 0; frame < 60; ++frame)
        {
            // A fast slash: 4-6 m/s at the hand and ~900 deg/s at the wrist.
            const float t = frame / 90.0f;
            hand.position = hand.position + Vector(0.05f * std::cos(t * 9.0f), -0.06f, 0.03f * std::sin(t * 7.0f));
            hand.orientation = QuatMultiply(QuatFromAxisAngle(Vector(0.2f, 1.0f, 0.1f), 0.17f), hand.orientation);
            Check(spring.Step(hand, 1.0f / 90.0f));
            Check(Near(spring.GetState().position, hand.position, 1e-6f));
            maxLag = std::max(maxLag, spring.LagDeg());
        }
        Check(maxLag > 10.0f);
        // The viewmodel origin (behind the grip) swings about the grip.
        const Vector gripOffsetLocal(0.12f, 0.02f, -0.05f); // grip in the viewmodel frame
        const Pose viewmodel{ hand.position - QuatRotate(hand.orientation, gripOffsetLocal), hand.orientation };
        const Quat rotation = QuatMultiply(spring.GetState().orientation, QuatConjugate(hand.orientation));
        const Pose lagged = PivotAboutGrip(viewmodel, hand.position, rotation);
        Check(Near(lagged.position + QuatRotate(lagged.orientation, gripOffsetLocal), hand.position, 1e-5f));
        Check(NearQuat(lagged.orientation, spring.GetState().orientation, 1e-4f));
        Check((lagged.position - viewmodel.position).Length() > 0.01f); // the origin did move
        // The render thread re-applies the delta to a fresher pose: the grip stays on the fresh hand.
        const Pose delta = LagDelta(viewmodel, lagged);
        const Pose motion{ Vector(0.04f, -0.03f, 0.02f), QuatFromAxisAngle(Vector(0.0f, 1.0f, 0.0f), 0.2f) };
        const Pose fresher = PoseCompose(viewmodel, motion);
        const Pose reapplied = ApplyLagDelta(fresher, delta);
        const Vector freshGrip = fresher.position + QuatRotate(fresher.orientation, gripOffsetLocal);
        Check(Near(reapplied.position + QuatRotate(reapplied.orientation, gripOffsetLocal), freshGrip, 1e-5f));
        Check(std::fabs(QuatAngleBetween(fresher.orientation, reapplied.orientation) -
            QuatAngleBetween(viewmodel.orientation, lagged.orientation)) < 1e-3f);
    }
    // Hand jumps (teleport, respawn) snap; the legacy position spring still works when unlocked.
    Spring spring(ParamsForMeleeScriptName("katana"));
    spring.Reset(Pose{});
    const Pose far{ Vector(5.0f, 0.0f, 0.0f), QuatFromAxisAngle(Vector(0.0f, 0.0f, 1.0f), 0.5f) };
    Check(spring.Step(far, 1.0f / 90.0f) && NearQuat(spring.GetState().orientation, far.orientation));
    Params legacy = ParamsForMeleeScriptName("katana");
    legacy.positionLocked = false;
    Spring trailing(legacy);
    trailing.Reset(Pose{});
    trailing.Step(Pose{ Vector(0.3f, 0.0f, 0.0f), Quat{} }, 1.0f / 90.0f);
    Check(trailing.GetState().position.x > 0.0f && trailing.GetState().position.x < 0.3f);
}

static void TestResetAndValidation()
{
    Spring spring(ParamsForMeleeScriptName("crowbar"));
    const Pose a{ Vector(1.0f, 2.0f, 3.0f), QuatFromAxisAngle(Vector(0.0f, 0.0f, 1.0f), 0.5f) };
    Check(!spring.Initialized() && spring.LagDeg() == 0.0f);
    // First step snaps.
    Check(spring.Step(a, 1.0f / 90.0f) && spring.Initialized());
    Check(Near(spring.GetState().position, a.position) && NearQuat(spring.GetState().orientation, a.orientation));
    const Pose b{ Vector(1.03f, 2.0f, 3.0f), QuatFromAxisAngle(Vector(1.0f, 0.0f, 0.0f), 1.0f) };
    for (int i = 0; i < 3; ++i) spring.Step(b, 1.0f / 90.0f);
    Check(Near(spring.GetState().position, b.position) && spring.AngularSpeedDegPerSec() > 1.0f);
    Check(spring.Reset(b));
    Check(Near(spring.GetState().position, b.position) && NearQuat(spring.GetState().orientation, b.orientation));
    Check(spring.GetState().velocity.Length() == 0.0f && spring.AngularSpeedDegPerSec() == 0.0f);
    // Non-finite input is rejected and leaves the state untouched.
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const State before = spring.GetState();
    Check(!spring.Step(Pose{ Vector(nan, 0.0f, 0.0f), Quat{} }, 1.0f / 90.0f));
    Check(!spring.Step(b, nan));
    Check(!spring.Reset(Pose{ Vector(0.0f, 0.0f, 0.0f), Quat{ nan, 0.0f, 0.0f, 0.0f } }));
    Check(Near(spring.GetState().position, before.position));
    // Zero / negative dt is a no-op.
    Check(spring.Step(Pose{ Vector(1.1f, 2.0f, 3.0f), Quat{} }, 0.0f) && Near(spring.GetState().position, before.position));
    Check(spring.Step(Pose{ Vector(1.1f, 2.0f, 3.0f), Quat{} }, -1.0f) && Near(spring.GetState().position, before.position));
    spring.Clear();
    Check(!spring.Initialized());
}

static void TestLargeDtStable()
{
    std::size_t count = 0;
    const MeleeProfile* const profiles = MeleeProfiles(count);
    for (std::size_t i = 0; i < count; ++i)
    {
        Spring spring(ParamsForMeleeScriptName(profiles[i].name));
        spring.Reset(Pose{});
        const Pose target{ Vector(0.5f, -0.3f, 0.2f), QuatFromAxisAngle(Vector(1.0f, 1.0f, 0.0f), 1.2f) };
        for (int frame = 0; frame < 200; ++frame)
        {
            // Hitches (0.5 s, 10 s) are clamped and substepped.
            Check(spring.Step(target, frame % 7 == 0 ? 10.0f : 0.5f));
            const State& s = spring.GetState();
            Check(Finite(s.position) && QuatFinite(s.orientation) && Finite(s.velocity) && Finite(s.angularVelocity));
            Check(spring.LagDeg() < 90.0f);
        }
        Check(QuatAngleBetween(spring.GetState().orientation, target.orientation) < 2e-3f);
    }
    // One clamped 10 s step is identical to one 0.05 s step.
    Spring a(ParamsForMeleeScriptName("katana")), b(ParamsForMeleeScriptName("katana"));
    a.Reset(Pose{});
    b.Reset(Pose{});
    const Pose target{ Vector(0.0f, 0.0f, 0.0f), QuatFromAxisAngle(Vector(0.0f, 0.0f, 1.0f), 0.8f) };
    a.Step(target, 10.0f);
    b.Step(target, kMaxStepDt);
    Check(NearQuat(a.GetState().orientation, b.GetState().orientation, 1e-6f));
}

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    TestQuaternionUtilities();
    TestPresets();
    TestLagCurve();
    TestLagLimit();
    TestWeldedGrip();
    TestResetAndValidation();
    TestLargeDtStable();
    std::printf("vr_melee_inertia tests passed\n");
    return 0;
}

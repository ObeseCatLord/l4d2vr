#include <limits>
#if defined(VECTOR_PARANOIA) && !defined(VEC_T_NAN)
#define VEC_T_NAN std::numeric_limits<float>::quiet_NaN()
#endif
#ifdef _MSC_VER
#pragma warning(push, 0)
#endif
#include "../L4D2VR/sdk/vector.h"
#ifdef _MSC_VER
#pragma warning(pop)
#endif
#include "../L4D2VR/vr_throw_ballistics.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#define CHECK(v) do { if (!(v)) { std::fprintf(stderr, "Throw ballistics failed at %d\n", __LINE__); std::abort(); } } while(false)
static bool Near(float a, float b, float eps) { return std::fabs(a - b) <= eps; }
using namespace l4d2vr_ballistics;
static float Horizontal(const Vector& a, const Vector& b)
{ return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y)); }
// Launch along a solution and check it passes through the target (within 1% of
// range) at the solved flight time. A target above the thrower may be hit on
// the way up, so the descending landing is only checked for targets below.
static bool Hits(const Vector& origin, const Vector& target, const LaunchSolution& s, float gravity, float drag)
{
    if (!s.valid) return false;
    const float range = (std::max)(Horizontal(origin, target), 1.0f);
    const Vector at = PositionAt(origin, s.velocity, s.time, gravity, drag);
    if (!(VectorLength(at - target) <= 0.01f * range)) return false;
    if (target.z > origin.z) return true;
    const Landing landing = PredictLanding(origin, s.velocity, target.z, gravity, drag);
    return landing.valid && VectorLength(landing.point - target) <= 0.01f * range;
}
int main()
{
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float g = kDefaultGravity;

    // Landing prediction.
    Landing drop = PredictLanding(Vector(0.0f, 0.0f, 100.0f), Vector(0.0f, 0.0f, 0.0f), 0.0f);
    CHECK(drop.valid && Near(drop.time, 0.5f, 0.0001f) && Near(drop.velocity.z, -400.0f, 0.01f));
    Landing lob = PredictLanding(Vector(0.0f, 0.0f, 0.0f), Vector(400.0f, 0.0f, 400.0f), 0.0f);
    CHECK(lob.valid && Near(lob.time, 1.0f, 0.0001f) && Near(lob.point.x, 400.0f, 0.01f));
    // Landing on a ledge above the origin uses the descending crossing.
    lob = PredictLanding(Vector(0.0f, 0.0f, 0.0f), Vector(400.0f, 0.0f, 400.0f), 75.0f);
    CHECK(lob.valid && Near(lob.time, 0.75f, 0.0001f) && Near(lob.point.z, 75.0f, 0.0001f));
    CHECK(!PredictLanding(Vector(0.0f, 0.0f, 0.0f), Vector(400.0f, 0.0f, 400.0f), 101.0f).valid); // above the apex
    CHECK(!PredictLanding(Vector(nan, 0.0f, 0.0f), Vector(1.0f, 0.0f, 0.0f), 0.0f).valid);
    CHECK(!PredictLanding(Vector(0.0f, 0.0f, 0.0f), Vector(1.0f, 0.0f, 0.0f), 0.0f, 0.0f).valid);
    CHECK(!PredictLanding(Vector(0.0f, 0.0f, 0.0f), Vector(1.0f, 0.0f, 0.0f), 0.0f, g, -1.0f).valid);
    // Linear drag: tiny drag matches the parabola; real drag shortens the throw.
    const Landing nearlyFree = PredictLanding(Vector(0.0f, 0.0f, 0.0f), Vector(400.0f, 0.0f, 400.0f), 0.0f, g, 1.0e-5f);
    CHECK(nearlyFree.valid && Near(nearlyFree.point.x, 400.0f, 0.1f) && Near(nearlyFree.time, 1.0f, 0.001f));
    const Landing dragged = PredictLanding(Vector(0.0f, 0.0f, 0.0f), Vector(400.0f, 0.0f, 400.0f), 0.0f, g, 0.5f);
    CHECK(dragged.valid && dragged.point.x < 350.0f && dragged.point.x > 200.0f);
    CHECK(Near(PositionAt(Vector(0.0f, 0.0f, 0.0f), Vector(400.0f, 0.0f, 400.0f), dragged.time, g, 0.5f).z, 0.0f, 0.05f));

    // Round trip: solve then fly, lands within 1%.
    const Vector origin(10.0f, -20.0f, 64.0f);
    const Vector target(400.0f, 250.0f, 0.0f);
    const LaunchSolution low = SolveLaunchPitch(origin, target, 700.0f, g, true);
    const LaunchSolution high = SolveLaunchPitch(origin, target, 700.0f, g, false);
    CHECK(Hits(origin, target, low, g, 0.0f) && Hits(origin, target, high, g, 0.0f));
    CHECK(Near(low.yawDeg, std::atan2(270.0f, 390.0f) * 180.0f / kPi, 0.01f) && Near(high.yawDeg, low.yawDeg, 0.0001f));
    // Low vs high arc: high is steeper (more negative pitch = more up) and slower to arrive.
    CHECK(high.pitchDeg < low.pitchDeg && high.time > low.time && high.pitchDeg < -45.0f);
    CHECK(Near(VectorLength(low.velocity), 700.0f, 0.05f));
    // Target above the thrower.
    const Vector roof(300.0f, 0.0f, 200.0f);
    CHECK(Hits(origin, roof, SolveLaunchPitch(origin, roof, 700.0f, g, true), g, 0.0f));
    CHECK(Hits(origin, roof, SolveLaunchPitch(origin, roof, 700.0f, g, false), g, 0.0f));
    // Straight up / down.
    const LaunchSolution up = SolveLaunchPitch(origin, origin + Vector(0.0f, 0.0f, 200.0f), 700.0f, g, true);
    CHECK(up.valid && Near(up.pitchDeg, -90.0f, 0.001f));
    const LaunchSolution down = SolveLaunchPitch(origin, origin - Vector(0.0f, 0.0f, 64.0f), 700.0f, g, true);
    CHECK(down.valid && Near(down.pitchDeg, 90.0f, 0.001f));
    CHECK(!SolveLaunchPitch(origin, origin + Vector(0.0f, 0.0f, 400.0f), 700.0f, g, true).valid);

    // Unreachable targets are reported (max flat range at 700 u/s is ~612 u).
    CHECK(!SolveLaunchPitch(origin, Vector(2000.0f, 0.0f, 64.0f), 700.0f, g, true).valid);
    CHECK(!SolveLaunchPitch(origin, Vector(2000.0f, 0.0f, 64.0f), 700.0f, g, false).valid);
    CHECK(!SolveLaunchPitch(origin, Vector(100.0f, 0.0f, 900.0f), 700.0f, g, true).valid);
    CHECK(!SolveLaunchPitch(origin, target, 0.0f, g, true).valid);
    CHECK(!SolveLaunchPitch(origin, target, nan, g, true).valid);
    CHECK(!SolveLaunchPitch(origin, Vector(nan, 0.0f, 0.0f), 700.0f, g, true).valid);

    // Linear drag solve round-trips and agrees with the closed form at tiny drag.
    const LaunchSolution dragLow = SolveLaunchPitch(origin, target, 700.0f, g, true, 0.3f);
    const LaunchSolution dragHigh = SolveLaunchPitch(origin, target, 700.0f, g, false, 0.3f);
    CHECK(Hits(origin, target, dragLow, g, 0.3f) && Hits(origin, target, dragHigh, g, 0.3f));
    CHECK(dragHigh.pitchDeg < dragLow.pitchDeg);
    CHECK(dragLow.pitchDeg < low.pitchDeg); // drag needs a higher aim
    const LaunchSolution tinyDrag = SolveLaunchPitch(origin, target, 700.0f, g, true, 1.0e-5f);
    CHECK(tinyDrag.valid && Near(tinyDrag.pitchDeg, low.pitchDeg, 0.1f));
    CHECK(!SolveLaunchPitch(origin, Vector(1500.0f, 0.0f, 0.0f), 700.0f, g, true, 0.3f).valid);

    // Native throw models: shared placeholder defaults for every throwable.
    for (Throwable t : {Throwable::Molotov, Throwable::PipeBomb, Throwable::VomitJar})
    {
        const NativeThrowModel m = DefaultNativeThrowModel(t);
        CHECK(Valid(m) && m.speed == 700.0f && m.upwardBiasDeg == 0.0f && m.gravityScale == 1.0f);
    }
    NativeThrowModel model = DefaultNativeThrowModel(Throwable::Molotov);
    CHECK(Near(LaunchPitchForAimPitch(AimPitchForLaunchPitch(-20.0f, model), model), -20.0f, 0.0001f));

    // Hand release -> native aim lands where the hand throw would.
    const Vector hand(0.0f, 0.0f, 50.0f), eye(0.0f, 0.0f, 64.0f);
    const Vector handVelocity(450.0f, 120.0f, 220.0f);
    const Landing handLanding = PredictLanding(hand, handVelocity, 0.0f);
    CHECK(handLanding.valid);
    auto nativeLands = [&](const NativeThrowAim& aim, const NativeThrowModel& m) {
        const Vector v = VelocityFromAngles(LaunchPitchForAimPitch(aim.pitchDeg, m), aim.yawDeg, m.speed);
        const Landing l = PredictLanding(eye, v, 0.0f, g * m.gravityScale, m.drag);
        return l.valid && VectorLength(l.point - handLanding.point) <= 0.01f * Horizontal(eye, handLanding.point);
    };
    NativeThrowAim aim = MapHandThrowToNative(hand, handVelocity, eye, 0.0f, model);
    CHECK(aim.solved && !aim.fallback && nativeLands(aim, model));
    CHECK(VectorLength(aim.landing - handLanding.point) < 0.01f);
    // Bias: the game adds an upward kick, so the aim we hand it is lower by that much.
    NativeThrowModel biased = model;
    biased.upwardBiasDeg = 6.0f;
    const NativeThrowAim biasedAim = MapHandThrowToNative(hand, handVelocity, eye, 0.0f, biased);
    CHECK(biasedAim.solved && nativeLands(biasedAim, biased));
    CHECK(Near(biasedAim.pitchDeg - aim.pitchDeg, 6.0f, 0.001f));
    // A bias that would push the aim past the 89 degree view clamp falls back.
    NativeThrowModel extreme = model;
    extreme.upwardBiasDeg = -60.0f;
    const NativeThrowAim clamped = MapHandThrowToNative(hand, Vector(60.0f, 0.0f, 600.0f), eye, 0.0f, extreme,
        g, ArcChoice::High);
    CHECK(!clamped.solved && clamped.fallback && clamped.pitchDeg >= -kMaxPitchDeg);
    // Gravity scale is applied to both the hand prediction and the native solve.
    NativeThrowModel floaty = model;
    floaty.gravityScale = 0.6f;
    const NativeThrowAim floatyAim = MapHandThrowToNative(hand, handVelocity, eye, 0.0f, floaty);
    CHECK(floatyAim.solved);

    // Arc matching: a steep lob picks the high arc, a flat throw the low one.
    const Vector lobVelocity(90.0f, 0.0f, 600.0f);
    const NativeThrowAim lobAim = MapHandThrowToNative(hand, lobVelocity, eye, 0.0f, model);
    const NativeThrowAim lobLow = MapHandThrowToNative(hand, lobVelocity, eye, 0.0f, model, g, ArcChoice::Low);
    CHECK(lobAim.solved && lobLow.solved && lobAim.pitchDeg < -60.0f && lobLow.pitchDeg > lobAim.pitchDeg);
    const NativeThrowAim flatHigh = MapHandThrowToNative(hand, handVelocity, eye, 0.0f, model, g, ArcChoice::High);
    CHECK(flatHigh.solved && flatHigh.pitchDeg < aim.pitchDeg && nativeLands(flatHigh, model));

    // Out of native range: raw hand direction with fallback flagged.
    const Vector fastVelocity(2500.0f, 0.0f, 500.0f);
    const NativeThrowAim far = MapHandThrowToNative(hand, fastVelocity, eye, 0.0f, model);
    float rawPitch = 0.0f, rawYaw = 0.0f;
    CHECK(DirectionAngles(fastVelocity, rawPitch, rawYaw));
    CHECK(!far.solved && far.fallback && Near(far.pitchDeg, rawPitch, 0.0001f) && Near(far.yawDeg, rawYaw, 0.0001f));
    CHECK(rawPitch < 0.0f); // upward throw has negative Source pitch
    // Nothing usable at all.
    const NativeThrowAim none = MapHandThrowToNative(hand, Vector(0.0f, 0.0f, 0.0f), eye, 0.0f, model);
    CHECK(!none.solved && !none.fallback);
    NativeThrowModel broken = model;
    broken.speed = nan;
    CHECK(!MapHandThrowToNative(hand, handVelocity, eye, 0.0f, broken).solved);
    std::puts("vr_throw_ballistics tests passed");
    return 0;
}

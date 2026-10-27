#ifdef _MSC_VER
#pragma warning(push, 0)
#endif
#include "../L4D2VR/sdk/vector.h"
#ifdef _MSC_VER
#pragma warning(pop)
#endif
#include "../L4D2VR/vr_melee_inertia.h"
#include <cstdio>
#include <cstdlib>
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
    const char* heavy[] = { "fireaxe", "sledgehammer", "electric_guitar", "pitchfork", "shovel", "FireAxe", "weapon_shovel" };
    const char* medium[] = { "baseball_bat", "cricket_bat", "crowbar", "katana", "machete", "golfclub", "frying_pan",
        "tonfa", "unknown_mod_weapon", "", "weapon_" };
    for (const char* name : heavy) Check(ClassForMeleeScriptName(name) == WeightClass::Heavy);
    for (const char* name : medium) Check(ClassForMeleeScriptName(name) == WeightClass::Medium);
    Check(ClassForMeleeScriptName(nullptr) == WeightClass::Medium);
    Check(ClassForMeleeScriptName("knife") == WeightClass::Light);
    Check(ClassForMeleeScriptName("chainsaw") == WeightClass::Chainsaw);
    Check(ClassForMeleeScriptName("weapon_chainsaw") == WeightClass::Chainsaw);
    Check(ClassForMeleeScriptName("fireaxe2") == WeightClass::Medium);

    const Params h = ParamsForClass(WeightClass::Heavy), m = ParamsForClass(WeightClass::Medium),
        l = ParamsForClass(WeightClass::Light), c = ParamsForMeleeScriptName("chainsaw");
    Check(h.rotationHz == 7.0f && h.maxAngularSpeedDegPerSec == 420.0f && h.motionMelee);
    Check(m.rotationHz == 10.0f && m.maxAngularSpeedDegPerSec == 520.0f && m.motionMelee);
    Check(l.rotationHz == 14.0f && l.maxAngularSpeedDegPerSec == 700.0f && l.motionMelee);
    Check(!c.motionMelee && ValidParams(c));
    Check(h.positionHz < m.positionHz && m.positionHz < l.positionHz);
    Check(ParamsForMeleeScriptName("katana").rotationHz == m.rotationHz);
    // Invalid params fall back to defaults instead of producing NaN.
    Params bad = m;
    bad.rotationHz = -1.0f;
    Spring spring(bad);
    Check(ValidParams(spring.GetParams()));
}

static void TestCriticallyDampedStepResponse()
{
    for (WeightClass weight : { WeightClass::Heavy, WeightClass::Medium, WeightClass::Light })
    {
        Params p = ParamsForClass(weight);
        p.maxLinearSpeed = 100.0f;
        p.maxAngularSpeedDegPerSec = 100000.0f; // isolate the linear spring behaviour
        Spring spring(p);
        Check(spring.Reset(Pose{}));
        const Quat goal = QuatFromAxisAngle(Vector(0.3f, 1.0f, 0.2f), 1.2f);
        const Pose target{ Vector(0.4f, 0.0f, 0.0f), goal };
        float previousDistance = 0.4f, previousAngle = QuatAngleBetween(Quat{}, goal);
        for (int frame = 0; frame < 180; ++frame) // 2 s at 90 Hz
        {
            Check(spring.Step(target, 1.0f / 90.0f));
            const State& s = spring.GetState();
            // No overshoot: position never passes the target and error decays monotonically.
            Check(s.position.x <= 0.4f + 1e-6f);
            const float distance = (s.position - target.position).Length();
            const float angle = QuatAngleBetween(s.orientation, goal);
            Check(distance <= previousDistance + 1e-6f);
            Check(angle <= previousAngle + 1e-5f);
            // Rotation only moves about the error axis (no wobble off-axis).
            previousDistance = distance;
            previousAngle = angle;
        }
        Check(previousDistance < 1e-3f);
        Check(previousAngle < 2e-3f);
        // The lag is noticeable early on: after one 90 Hz frame the weapon has not caught up.
        Spring early(p);
        early.Reset(Pose{});
        early.Step(target, 1.0f / 90.0f);
        Check(early.GetState().position.x < 0.3f);
        Check(QuatAngleBetween(early.GetState().orientation, goal) > 0.2f);
    }
    // Heavier weapons lag more for the same input.
    Spring heavy(ParamsForClass(WeightClass::Heavy)), light(ParamsForClass(WeightClass::Light));
    heavy.Reset(Pose{});
    light.Reset(Pose{});
    const Pose target{ Vector(0.0f, 0.0f, 0.0f), QuatFromAxisAngle(Vector(0.0f, 0.0f, 1.0f), 1.0f) };
    for (int frame = 0; frame < 9; ++frame) { heavy.Step(target, 1.0f / 90.0f); light.Step(target, 1.0f / 90.0f); }
    Check(QuatAngleBetween(heavy.GetState().orientation, target.orientation) >
        QuatAngleBetween(light.GetState().orientation, target.orientation));
}

static void TestSpeedClamps()
{
    const Params p = ParamsForClass(WeightClass::Heavy);
    Spring spring(p);
    spring.Reset(Pose{});
    // A 170-degree flick: the weapon may never rotate faster than the preset.
    const Pose target{ Vector(0.0f, 0.0f, 0.0f), QuatFromAxisAngle(Vector(0.0f, 1.0f, 0.0f), 170.0f * kDegToRad) };
    Quat previous = spring.GetState().orientation;
    const float dt = 1.0f / 90.0f;
    bool reachedLimit = false;
    for (int frame = 0; frame < 120; ++frame)
    {
        spring.Step(target, dt);
        Check(spring.AngularSpeedDegPerSec() <= p.maxAngularSpeedDegPerSec + 0.01f);
        const float travelled = QuatAngleBetween(previous, spring.GetState().orientation) * kRadToDeg;
        Check(travelled <= p.maxAngularSpeedDegPerSec * dt + 0.05f);
        reachedLimit = reachedLimit || spring.AngularSpeedDegPerSec() > p.maxAngularSpeedDegPerSec - 1.0f;
        previous = spring.GetState().orientation;
    }
    Check(reachedLimit);
    Check(QuatAngleBetween(spring.GetState().orientation, target.orientation) < 0.01f);

    // Linear speed clamp.
    Spring linear(p);
    linear.Reset(Pose{});
    const Pose far{ Vector(0.9f, 0.0f, 0.0f), Quat{} };
    for (int frame = 0; frame < 30; ++frame)
    {
        linear.Step(far, dt);
        Check(linear.GetState().velocity.Length() <= p.maxLinearSpeed + 1e-4f);
    }
}

static void TestResetAndValidation()
{
    Spring spring(ParamsForClass(WeightClass::Medium));
    const Pose a{ Vector(1.0f, 2.0f, 3.0f), QuatFromAxisAngle(Vector(0.0f, 0.0f, 1.0f), 0.5f) };
    Check(!spring.Initialized());
    // First step snaps.
    Check(spring.Step(a, 1.0f / 90.0f) && spring.Initialized());
    Check(Near(spring.GetState().position, a.position) && NearQuat(spring.GetState().orientation, a.orientation));
    const Pose b{ Vector(1.3f, 2.0f, 3.0f), QuatFromAxisAngle(Vector(1.0f, 0.0f, 0.0f), 1.0f) };
    for (int i = 0; i < 3; ++i) spring.Step(b, 1.0f / 90.0f);
    Check(spring.GetState().velocity.Length() > 0.1f && spring.AngularSpeedDegPerSec() > 1.0f);
    Check(spring.Reset(b));
    Check(Near(spring.GetState().position, b.position) && NearQuat(spring.GetState().orientation, b.orientation));
    Check(spring.GetState().velocity.Length() == 0.0f && spring.AngularSpeedDegPerSec() == 0.0f);
    // Teleport-sized jumps snap rather than dragging the weapon through the world.
    const Pose teleported{ Vector(50.0f, 2.0f, 3.0f), b.orientation };
    Check(spring.Step(teleported, 1.0f / 90.0f) && Near(spring.GetState().position, teleported.position));
    // Non-finite input is rejected and leaves the state untouched.
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const State before = spring.GetState();
    Check(!spring.Step(Pose{ Vector(nan, 0.0f, 0.0f), Quat{} }, 1.0f / 90.0f));
    Check(!spring.Step(teleported, nan));
    Check(!spring.Reset(Pose{ Vector(0.0f, 0.0f, 0.0f), Quat{ nan, 0.0f, 0.0f, 0.0f } }));
    Check(Near(spring.GetState().position, before.position));
    // Zero / negative dt is a no-op.
    Check(spring.Step(Pose{ Vector(50.5f, 2.0f, 3.0f), Quat{} }, 0.0f) && Near(spring.GetState().position, before.position));
    Check(spring.Step(Pose{ Vector(50.5f, 2.0f, 3.0f), Quat{} }, -1.0f) && Near(spring.GetState().position, before.position));
}

static void TestLargeDtStable()
{
    for (WeightClass weight : { WeightClass::Heavy, WeightClass::Medium, WeightClass::Light, WeightClass::Chainsaw })
    {
        Spring spring(ParamsForClass(weight));
        spring.Reset(Pose{});
        const Pose target{ Vector(0.5f, -0.3f, 0.2f), QuatFromAxisAngle(Vector(1.0f, 1.0f, 0.0f), 2.5f) };
        float previous = 1e9f;
        for (int frame = 0; frame < 200; ++frame)
        {
            // Hitches (0.5 s, 10 s) are clamped and substepped.
            Check(spring.Step(target, frame % 7 == 0 ? 10.0f : 0.5f));
            const State& s = spring.GetState();
            Check(Finite(s.position) && QuatFinite(s.orientation) && Finite(s.velocity) && Finite(s.angularVelocity));
            const float error = (s.position - target.position).Length();
            Check(error <= previous + 1e-6f);
            previous = error;
        }
        Check(previous < 1e-3f && QuatAngleBetween(spring.GetState().orientation, target.orientation) < 2e-3f);
    }
    // One clamped 10 s step is identical to one 0.05 s step.
    Spring a(ParamsForClass(WeightClass::Medium)), b(ParamsForClass(WeightClass::Medium));
    a.Reset(Pose{});
    b.Reset(Pose{});
    const Pose target{ Vector(0.2f, 0.0f, 0.0f), Quat{} };
    a.Step(target, 10.0f);
    b.Step(target, kMaxStepDt);
    Check(Near(a.GetState().position, b.GetState().position, 1e-6f));
}

static void TestDeltaReapplication()
{
    Spring spring(ParamsForClass(WeightClass::Heavy));
    Pose controller{ Vector(0.1f, 0.2f, 1.2f), QuatFromAxisAngle(Vector(0.0f, 0.0f, 1.0f), 0.3f) };
    spring.Reset(controller);
    for (int frame = 0; frame < 20; ++frame)
    {
        // Controller sweeps quickly so the lag is substantial.
        controller.position = controller.position + Vector(0.02f, -0.01f, 0.005f);
        controller.orientation = QuatMultiply(QuatFromAxisAngle(Vector(0.2f, 1.0f, 0.1f), 0.08f), controller.orientation);
        spring.Step(controller, 1.0f / 90.0f);
    }
    const Pose lagged = spring.Lagged();
    Check((lagged.position - controller.position).Length() > 0.01f);
    Check(QuatAngleBetween(lagged.orientation, controller.orientation) > 0.05f);
    const Pose delta = spring.Delta(controller);
    // Re-applying the delta to the same controller reproduces the lagged pose exactly.
    const Pose rebuilt = ApplyLagDelta(controller, delta);
    Check(Near(rebuilt.position, lagged.position, 1e-5f) && NearQuat(rebuilt.orientation, lagged.orientation, 1e-4f));
    // On a fresher controller pose the lag is carried rigidly with the controller.
    const Pose motion{ Vector(0.05f, 0.0f, -0.02f), QuatFromAxisAngle(Vector(0.0f, 1.0f, 0.0f), 0.2f) };
    const Pose fresher = PoseCompose(controller, motion);
    const Pose reapplied = ApplyLagDelta(fresher, delta);
    const Pose relativeBefore = LagDelta(controller, lagged), relativeAfter = LagDelta(fresher, reapplied);
    Check(Near(relativeBefore.position, relativeAfter.position, 1e-5f) &&
        NearQuat(relativeBefore.orientation, relativeAfter.orientation, 1e-4f));
    // Delta is in the controller's local frame: identity controller -> delta equals lagged.
    const Pose identityDelta = LagDelta(Pose{}, lagged);
    Check(Near(identityDelta.position, lagged.position, 1e-6f) && NearQuat(identityDelta.orientation, lagged.orientation));
    // Converged spring -> identity delta.
    for (int frame = 0; frame < 400; ++frame) spring.Step(controller, 1.0f / 90.0f);
    const Pose settled = spring.Delta(controller);
    Check(settled.position.Length() < 1e-3f && NearQuat(settled.orientation, Quat{}, 2e-3f));
    // Axes helpers agree with the pose orientation.
    Vector f{}, r{}, u{};
    QuatToAxes(controller.orientation, f, r, u);
    Pose fromAxes{};
    Check(PoseFromAxes(controller.position, f, r, u, fromAxes) && NearQuat(fromAxes.orientation, controller.orientation));
}

int main()
{
    TestQuaternionUtilities();
    TestPresets();
    TestCriticallyDampedStepResponse();
    TestSpeedClamps();
    TestResetAndValidation();
    TestLargeDtStable();
    TestDeltaReapplication();
    std::printf("vr_melee_inertia tests passed\n");
    return 0;
}

#include "../L4D2VR/vr_mag_slap.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>

using namespace l4d2vr_mag_slap;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

static bool Near(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) < eps; }

// The AK-47 clip box in clip-bone space (official profile, source units), with
// the clip bone's -Y pointing at world down (-Z) and +Z at world forward (+X):
// how the magazine hangs below a level gun.
static Box AkSeatedBox()
{
    Box box{};
    box.origin = { 10.0f, 2.0f, 50.0f };
    box.axisX = { 0.0f, 1.0f, 0.0f };
    box.axisY = { 0.0f, 0.0f, 1.0f };
    box.axisZ = { 1.0f, 0.0f, 0.0f };
    box.mins = { -0.65f, -9.53f, -0.69f };
    box.maxs = { 0.63f, 1.34f, 7.11f };
    return box;
}

static void TestGap()
{
    const Box seated = AkSeatedBox();
    CHECK(Gap(seated, seated) == 0.0f);
    // Moved 1.5 units past the thin side: about 1.5 apart, either order.
    const float width = seated.maxs.x - seated.mins.x;
    const Box beside = Translated(seated, Scale(seated.axisX, width + 1.5f));
    CHECK(Near(Gap(seated, beside), 1.5f, 0.01f));
    CHECK(Near(Gap(beside, seated), 1.5f, 0.01f));
    // Overlapping partially along the length still touches.
    CHECK(Gap(seated, Translated(seated, { 0.0f, 0.0f, -5.0f })) == 0.0f);
    // A thin box crossing the seated one at right angles through its middle.
    Box crossing = seated;
    crossing.axisX = { 1.0f, 0.0f, 0.0f };
    crossing.axisZ = { 0.0f, -1.0f, 0.0f };
    crossing.origin = Add(Center(seated), { -3.0f, 0.0f, 4.0f });
    CHECK(Gap(seated, crossing) < 0.5f);       // no lattice point inside: a fraction of a unit
    Box broken = seated;
    broken.mins.y = 5.0f;                      // mins above maxs
    CHECK(!std::isfinite(Gap(seated, broken)));
}

static void TestOutwardAxis()
{
    const Box seated = AkSeatedBox();
    Vec3 out{};
    CHECK(OutwardAxis(seated, { 0.0f, -1.0f, 0.0f }, out));   // ManualReloadMagazineInsertionAxisLocal default
    CHECK(Near(out.x, 0.0f) && Near(out.y, 0.0f) && Near(out.z, -1.0f)); // straight down out of the well
    // The sign follows the box: a flipped configured axis still points to the base.
    CHECK(OutwardAxis(seated, { 0.0f, 2.0f, 0.0f }, out));
    CHECK(Near(out.z, -1.0f));
    // The gun rolled 90 degrees right: the magazine leaves sideways.
    Box rolled = seated;
    rolled.axisY = { 0.0f, 1.0f, 0.0f };
    rolled.axisX = { 0.0f, 0.0f, -1.0f };
    CHECK(OutwardAxis(rolled, { 0.0f, -1.0f, 0.0f }, out));
    CHECK(Near(out.y, -1.0f));
    CHECK(!OutwardAxis(seated, { 0.0f, 0.0f, 0.0f }, out));
}

static void TestRelativeVelocity()
{
    // Walking or swinging both hands together is no strike.
    const Vec3 walk = RelativeVelocity({ 1.2f, 0.3f, -0.1f }, { 1.2f, 0.3f, -0.1f }, 37.0f);
    CHECK(Near(Length(walk), 0.0f));
    // The stick-turn offset rotates tracking space into the world.
    const Vec3 turned = RelativeVelocity({ 1.0f, 0.0f, -0.5f }, { 0.0f, 0.0f, 0.0f }, 90.0f);
    CHECK(Near(turned.x, 0.0f) && Near(turned.y, 1.0f) && Near(turned.z, -0.5f));
    // A gun hand moving up into a still magazine is the same strike as the
    // magazine moving down onto the gun.
    const Vec3 a = RelativeVelocity({ 0.0f, 0.0f, -1.0f }, { 0.0f, 0.0f, 0.0f }, 0.0f);
    const Vec3 b = RelativeVelocity({ 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 1.0f }, 0.0f);
    CHECK(Near(a.z, b.z));
}

static void TestEvaluateThresholds()
{
    Params ak{};
    ak.speedMetersPerSecond = 0.8f;   // Magazine_AKBase
    ak.forwardDot = 0.6f;
    const Vec3 down{ 0.0f, 0.0f, -1.0f };
    CHECK(Evaluate(ak, Scale(down, 0.8f), down, 0.0f).hit);     // Pavlov rejects only below the threshold
    CHECK(!Evaluate(ak, Scale(down, 0.79f), down, 0.0f).hit);
    CHECK(Evaluate(ak, Scale(down, 2.0f), down, 0.029f).hit);   // within 3 cm counts as contact
    CHECK(!Evaluate(ak, Scale(down, 2.0f), down, 0.031f).hit);
    CHECK(!Evaluate(ak, Scale(down, 2.0f), down, INFINITY).hit);
    // Forward-and-down at 45 degrees (the real AK slap) passes 0.6; level does not.
    const Vec3 forwardDown{ 0.70710678f, 0.0f, -0.70710678f };
    const Result angled = Evaluate(ak, Scale(forwardDown, 1.2f), down, 0.0f);
    CHECK(angled.hit && Near(angled.dot, 0.7071f));
    CHECK(!Evaluate(ak, { 1.2f, 0.0f, 0.0f }, down, 0.0f).hit);
    CHECK(!Evaluate(ak, Scale(down, -1.5f), down, 0.0f).hit);   // pushing it back up into the well
    // Dot just either side of 0.6.
    const float s59 = std::sqrt(1.0f - 0.59f * 0.59f), s61 = std::sqrt(1.0f - 0.61f * 0.61f);
    CHECK(!Evaluate(ak, Scale({ s59, 0.0f, -0.59f }, 1.5f), down, 0.0f).hit);
    CHECK(Evaluate(ak, Scale({ s61, 0.0f, -0.61f }, 1.5f), down, 0.0f).hit);
    // Defaults (Galil, AutoSniper): 90 cm/s and 0.68.
    const Params defaults{};
    CHECK(!Evaluate(defaults, Scale(down, 0.85f), down, 0.0f).hit);
    CHECK(Evaluate(defaults, Scale(forwardDown, 1.5f), down, 0.0f).hit);    // 0.707 >= 0.68
    const float s66 = std::sqrt(1.0f - 0.66f * 0.66f);
    CHECK(!Evaluate(defaults, Scale({ s66, 0.0f, -0.66f }, 1.5f), down, 0.0f).hit);
    const Result still = Evaluate(defaults, { 0.0f, 0.0f, 0.0f }, down, 0.0f);
    CHECK(!still.hit && still.contact && still.speed == 0.0f);
}

static void TestReinsertDelay()
{
    CHECK(MayReinsert(-1.0f, 0.15f));       // no slap this session
    CHECK(!MayReinsert(0.0f, 0.15f));
    CHECK(!MayReinsert(0.149f, 0.15f));
    CHECK(MayReinsert(0.15f, 0.15f));
}

static void TestStrikeOnLiveGeometry()
{
    // The held magazine (same box) a centimetre below the seated one, moving down
    // at 1 m/s relative to the gun while the player walks: a slap.
    const Box seated = AkSeatedBox();
    Vec3 out{};
    CHECK(OutwardAxis(seated, { 0.0f, -1.0f, 0.0f }, out));
    const float unitsPerMeter = 40.0f;
    const Box held = Translated(seated, Scale(out, 0.01f * unitsPerMeter));
    const float gapMeters = Gap(seated, held) / unitsPerMeter;
    const Vec3 rel = RelativeVelocity({ 1.0f, 0.0f, -1.0f }, { 1.0f, 0.0f, 0.0f }, 0.0f);
    Params ak{};
    ak.speedMetersPerSecond = 0.8f;
    ak.forwardDot = 0.6f;
    CHECK(Evaluate(ak, rel, out, gapMeters).hit);
    // The same motion 20 cm to the side of the gun misses.
    const Box aside = Translated(held, Scale(seated.axisX, 0.20f * unitsPerMeter));
    CHECK(!Evaluate(ak, rel, out, Gap(seated, aside) / unitsPerMeter).hit);
}

// ---- Physical slap ----------------------------------------------------------------
static const float kUnitsPerMeter = 40.0f;

static Params AkPavlov()
{
    Params ak{};
    ak.speedMetersPerSecond = 0.8f;
    ak.forwardDot = 0.6f;
    return ak;
}

struct StrikeRun
{
    bool hit = false;
    bool contact = false;
    float release = 0.0f;
    float approach = 0.0f;
    float maxClamp = 0.0f;
};

// The held magazine (same box, offset `startOffsetM` metres from the seated
// one in world axes) moves at `velocity` m/s for `seconds`, sampled at `hz`.
static StrikeRun RunStrike(const Box& seated, const Vec3& outward, const Vec3& gunForward, const Vec3& startOffsetM,
    const Vec3& velocity, float seconds, float hz, const PhysicsParams& p)
{
    Collider c;
    c.Reset();
    StrikeRun run{};
    const int frames = static_cast<int>(seconds * hz);
    for (int f = 0; f <= frames; ++f)
    {
        const float t = f / hz;
        const Vec3 offset = Scale(Add(startOffsetM, Scale(velocity, t)), kUnitsPerMeter);
        const Box held = Translated(seated, offset);
        const Collider::Outcome o = c.Update(p, t, seated, held, &velocity, 1, outward, gunForward, kUnitsPerMeter);
        if (o.contactBegan && !run.contact)
        {
            run.contact = true;
            run.release = o.impulse.release;
            run.approach = o.impulse.approach;
        }
        run.maxClamp = std::max(run.maxClamp, Length(o.clampOffset) / kUnitsPerMeter);
        if (o.hit)
        {
            run.hit = true;
            break;
        }
    }
    return run;
}

static void TestPhysicalParams()
{
    const PhysicsParams ak = Physical(AkPavlov());
    CHECK(Near(ak.releaseSpeedMetersPerSecond, 0.432f));
    CHECK(Near(Physical(Params{}).releaseSpeedMetersPerSecond, 0.9f * 0.68f * 0.9f));
}

static void TestSatAndSweep()
{
    const Box seated = AkSeatedBox();
    // 2 units apart beside the thin side: separation 2 along world Y (the box's x).
    const float width = seated.maxs.x - seated.mins.x;
    const SatResult apart = SatQuery(seated, Translated(seated, { 0.0f, width + 2.0f, 0.0f }));
    CHECK(apart.valid && Near(apart.separation, 2.0f, 0.01f) && Near(apart.axis.y, 1.0f));
    const SatResult inside = SatQuery(seated, Translated(seated, { 0.0f, 0.5f, 0.0f }));
    CHECK(inside.valid && inside.separation < 0.0f);
    // A pass that jumps over the magazine in one frame still hits, on the face it came from.
    const Box after = Translated(seated, { 0.0f, -(width + 10.0f), 0.0f });
    const Vec3 before = RelativeOrigin(seated, Translated(seated, { 0.0f, width + 10.0f, 0.0f }));
    const SweepResult sweep = Sweep(seated, after, &before, 0.1f, 24);
    CHECK(sweep.hit && !sweep.started && sweep.normal.y > 0.9f);
    CHECK(!Sweep(seated, after, nullptr, 0.1f, 24).hit);
}

static void TestImpulse()
{
    const PhysicsParams p = Physical(AkPavlov());
    const Vec3 down{ 0.0f, 0.0f, -1.0f }, forward{ 1.0f, 0.0f, 0.0f };
    // Hitting the back face (normal toward the stock) moving toward the muzzle.
    const Impulse push = StrikeImpulse(p, { 1.0f, 0.0f, 0.0f }, { -1.0f, 0.0f, 0.0f }, down, forward);
    CHECK(Near(push.approach, 1.0f) && Near(push.release, 1.0f));
    // The front face moving toward the stock: nothing along a release direction.
    CHECK(Near(StrikeImpulse(p, { -1.0f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f }, down, forward).release, 0.0f));
    // The bottom moving up into the well: nothing, and flagged as an insert.
    const Impulse insert = StrikeImpulse(p, { 0.0f, 0.0f, 1.0f }, { 0.0f, 0.0f, -1.0f }, down, forward);
    CHECK(Near(insert.release, 0.0f) && insert.intoWell > 0.9f);
    // Sliding down a side face: friction only, limited by the small approach.
    const Impulse glance = StrikeImpulse(p, { 0.0f, -0.1f, -1.0f }, { 0.0f, 1.0f, 0.0f }, down, forward);
    CHECK(Near(glance.approach, 0.1f) && Near(glance.release, 0.05f));
    // Moving away from the face: no impulse at all.
    CHECK(StrikeImpulse(p, { 0.0f, 1.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, down, forward).approach == 0.0f);
}

static void TestStrikes()
{
    const Box seated = AkSeatedBox();
    Vec3 out{};
    CHECK(OutwardAxis(seated, { 0.0f, -1.0f, 0.0f }, out));
    const Vec3 forward{ 1.0f, 0.0f, 0.0f };
    const PhysicsParams ak = Physical(AkPavlov());
    struct Case { const char* name; Vec3 start; Vec3 velocity; float hz; bool hit; };
    const Case cases[] = {
        // From behind toward the muzzle (the AK rock-out), several speeds.
        { "toward the muzzle 1.0 m/s", { -0.30f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f }, 90.0f, true },
        { "toward the muzzle 0.5 m/s", { -0.30f, 0.0f, 0.0f }, { 0.5f, 0.0f, 0.0f }, 90.0f, true },
        { "forward and down 0.9 m/s", { -0.25f, 0.0f, 0.15f }, { 0.64f, 0.0f, -0.64f }, 90.0f, true },
        { "down onto the top 1.0 m/s", { 0.0f, 0.0f, 0.35f }, { 0.0f, 0.0f, -1.0f }, 90.0f, true },
        // Tunnelling: 5.5 m/s at 30 Hz is 18 cm a frame.
        { "toward the muzzle 5.5 m/s at 30 Hz", { -0.40f, 0.0f, 0.0f }, { 5.5f, 0.0f, 0.0f }, 30.0f, true },
        // Never: slow pushes, the wrong way, into the well, glancing, too fast to be a hand.
        { "slow push toward the muzzle 0.3 m/s", { -0.30f, 0.0f, 0.0f }, { 0.3f, 0.0f, 0.0f }, 90.0f, false },
        { "toward the stock 1.5 m/s", { 0.30f, 0.0f, 0.0f }, { -1.5f, 0.0f, 0.0f }, 90.0f, false },
        { "up into the well 1.5 m/s", { 0.0f, 0.0f, -0.40f }, { 0.0f, 0.0f, 1.5f }, 90.0f, false },
        { "slow insert 0.3 m/s", { 0.0f, 0.0f, -0.40f }, { 0.0f, 0.0f, 0.3f }, 90.0f, false },
        { "glancing down the side 1.0 m/s", { 0.0f, 0.052f, 0.30f }, { 0.0f, -0.05f, -1.0f }, 90.0f, false },
        { "sideways 1.5 m/s", { 0.0f, 0.30f, 0.0f }, { 0.0f, -1.5f, 0.0f }, 90.0f, false },
        { "tracking jump 9 m/s", { -0.40f, 0.0f, 0.0f }, { 9.0f, 0.0f, 0.0f }, 90.0f, false },
    };
    for (const Case& c : cases)
    {
        const StrikeRun r = RunStrike(seated, out, forward, c.start, c.velocity, 0.8f, c.hz, ak);
        if (r.hit != c.hit)
            std::printf("  case '%s': hit=%d (want %d) contact=%d approach=%.2f release=%.2f\n", c.name,
                r.hit ? 1 : 0, c.hit ? 1 : 0, r.contact ? 1 : 0, r.approach, r.release);
        CHECK(r.hit == c.hit);
    }
    // The glancing pass does touch the side face; the slow push touches too.
    CHECK(RunStrike(seated, out, forward, { 0.0f, 0.052f, 0.30f }, { 0.0f, -0.05f, -1.0f }, 0.8f, 90.0f, ak).contact);
    CHECK(RunStrike(seated, out, forward, { -0.30f, 0.0f, 0.0f }, { 0.3f, 0.0f, 0.0f }, 0.8f, 90.0f, ak).contact);
    // 12 cm beside, moving along it: never touches.
    CHECK(!RunStrike(seated, out, forward, { 0.0f, 0.12f, 0.35f }, { 0.0f, 0.0f, -1.5f }, 0.8f, 90.0f, ak).contact);
}

static void TestRestingContactClampsAndNeverSlaps()
{
    // A slow push against the back face: contact, no slap, and the held box is
    // held on the surface (the clamp grows with the push).
    const Box seated = AkSeatedBox();
    Vec3 out{};
    OutwardAxis(seated, { 0.0f, -1.0f, 0.0f }, out);
    const PhysicsParams ak = Physical(AkPavlov());
    const StrikeRun slow = RunStrike(seated, out, { 1.0f, 0.0f, 0.0f }, { -0.25f, 0.0f, 0.0f }, { 0.2f, 0.0f, 0.0f },
        2.0f, 90.0f, ak);
    CHECK(slow.contact && !slow.hit && slow.maxClamp > 0.1f);
    // Taken already overlapping it, then struck from there: nothing until apart.
    Collider c;
    c.Reset();
    const Vec3 fast{ 2.0f, 0.0f, 0.0f };
    for (int f = 0; f < 20; ++f)
    {
        const Box held = Translated(seated, { f * 2.0f * kUnitsPerMeter / 90.0f, 0.0f, 0.0f });
        CHECK(!c.Update(ak, f / 90.0f, seated, held, &fast, 1, out, { 1.0f, 0.0f, 0.0f }, kUnitsPerMeter).hit);
    }
}

static Vec3 Cross(const Vec3& a, const Vec3& b)
{
    return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
}

static void TestCantedGunFromHeadsetLog()
{
    // The user's logged outward axes (gun canted 25-75 degrees). Seated box
    // with that outward axis and a level barrel; strikes from clear of it.
    const Vec3 logged[] = { { 0.22f, -0.84f, -0.49f }, { 0.02f, -0.80f, -0.60f }, { 0.13f, -0.36f, -0.92f },
        { 0.39f, -0.84f, -0.37f }, { 0.44f, -0.79f, -0.43f }, { 0.42f, -0.86f, -0.29f } };
    const PhysicsParams ak = Physical(AkPavlov());
    for (const Vec3& o : logged)
    {
        Vec3 out{}, forward{};
        Normalize(o, out);
        Normalize(Cross(out, { 0.0f, 0.0f, 1.0f }), forward);
        Box seated = AkSeatedBox();
        seated.axisY = Scale(out, -1.0f);
        seated.axisZ = forward;
        seated.axisX = Cross(seated.axisY, seated.axisZ);
        Vec3 check{};
        CHECK(OutwardAxis(seated, { 0.0f, -1.0f, 0.0f }, check) && Near(Dot(check, out), 1.0f, 0.01f));
        // Along the magazine out of the well, from inside the gun: slaps at 0.8 m/s.
        CHECK(RunStrike(seated, out, forward, Scale(out, -0.35f), Scale(out, 0.8f), 0.8f, 90.0f, ak).hit);
        // From behind toward the muzzle: slaps at 0.9 m/s.
        CHECK(RunStrike(seated, out, forward, Scale(forward, -0.30f), Scale(forward, 0.9f), 0.8f, 90.0f, ak).hit);
        // A slow insert from below never does.
        CHECK(!RunStrike(seated, out, forward, Scale(out, 0.40f), Scale(out, -0.3f), 1.5f, 90.0f, ak).hit);
        // Pushed in hard from below: never.
        CHECK(!RunStrike(seated, out, forward, Scale(out, 0.40f), Scale(out, -2.0f), 0.8f, 90.0f, ak).hit);
    }
}

int main()
{
    TestPhysicalParams();
    TestSatAndSweep();
    TestImpulse();
    TestStrikes();
    TestRestingContactClampsAndNeverSlaps();
    TestCantedGunFromHeadsetLog();
    TestGap();
    TestOutwardAxis();
    TestRelativeVelocity();
    TestEvaluateThresholds();
    TestReinsertDelay();
    TestStrikeOnLiveGeometry();
    if (g_failures)
    {
        std::printf("%d failure(s)\n", g_failures);
        return EXIT_FAILURE;
    }
    std::printf("vr_mag_slap tests passed\n");
    return EXIT_SUCCESS;
}

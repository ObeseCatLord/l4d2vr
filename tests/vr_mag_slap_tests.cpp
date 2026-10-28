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

// ---- Forgiving detector --------------------------------------------------------
// A scripted strike: the held magazine (same box) starts `startCm` before the
// seated one along `dir` and moves at `speed` m/s for `seconds`, sampled at
// `hz`, with the tracked velocity optionally lagging. Returns whether a slap
// was detected.
struct StrikeRun
{
    bool hit = false;
    bool nearMiss = false;
    float speed = 0.0f;
    float dot = -1.0f;
};

static StrikeRun RunStrike(const Box& seated, const Vec3& outward, const Vec3& gunForward, const Vec3& dirIn,
    float speed, float startCm, float travelCm, float hz, const ForgivingParams& p,
    const Vec3& sideOffsetCm = { 0.0f, 0.0f, 0.0f }, bool decelerate = false)
{
    const float unitsPerMeter = 40.0f;
    Vec3 dir{};
    Normalize(dirIn, dir);
    Detector detector;
    detector.Reset();
    StrikeRun run{};
    const float dt = 1.0f / hz;
    const float total = travelCm * 0.01f;
    float travelled = 0.0f;
    float t = 0.0f;
    Vec3 previous{};
    bool havePrevious = false;
    // Start position: centred on the seated magazine, moved back along the strike.
    const Vec3 start = Add(Scale(dir, -startCm * 0.01f * unitsPerMeter), Scale(sideOffsetCm, 0.01f * unitsPerMeter));
    for (int frame = 0; frame < 400 && travelled <= total + 1e-4f; ++frame)
    {
        // Decelerating: full speed until the touch region, then half.
        const float v = decelerate && travelled > startCm * 0.01f * 0.8f ? speed * 0.4f : speed;
        const Box held = Translated(seated, Add(start, Scale(dir, travelled * unitsPerMeter)));
        const bool touching = TouchingSwept(seated, held, havePrevious ? &previous : nullptr, p.contactMeters * unitsPerMeter);
        previous = RelativeOrigin(seated, held);
        havePrevious = true;
        const Vec3 strike = Scale(dir, v);
        const Detector::Outcome o = detector.Update(p, t, &strike, 1, outward, gunForward, touching);
        if (o.hit && !run.hit)
        {
            run.hit = true;
            run.speed = o.strike.speed;
            run.dot = o.strike.direction.dot;
        }
        if (o.nearMiss)
            run.nearMiss = true;
        travelled += v * dt;
        t += dt;
    }
    return run;
}

static Vec3 Rotate(const Vec3& v, const Vec3& axis, float deg)
{
    Vec3 k{};
    Normalize(axis, k);
    const float r = deg * 3.14159265f / 180.0f, c = std::cos(r), s = std::sin(r);
    const Vec3 kxv{ k.y * v.z - k.z * v.y, k.z * v.x - k.x * v.z, k.x * v.y - k.y * v.x };
    return Add(Add(Scale(v, c), Scale(kxv, s)), Scale(k, Dot(k, v) * (1.0f - c)));
}

static void TestForgivingParams()
{
    Params ak{};
    ak.speedMetersPerSecond = 0.8f;
    ak.forwardDot = 0.6f;
    const ForgivingParams f = Forgiving(ak);
    CHECK(Near(f.speedMetersPerSecond, 0.64f));
    CHECK(Near(f.dot, 0.5f));
}

static void TestDirectionMatch()
{
    const Vec3 down{ 0.0f, 0.0f, -1.0f }, forward{ 1.0f, 0.0f, 0.0f };
    CHECK(Near(MatchDirection(down, down, forward).dot, 1.0f));
    CHECK(Near(MatchDirection(forward, down, forward).dot, 1.0f));                 // knocked toward the muzzle
    CHECK(Near(MatchDirection({ 1.0f, 0.0f, -1.0f }, down, forward).dot, 1.0f));   // forward and down
    CHECK(MatchDirection({ -1.0f, 0.0f, 0.0f }, down, forward).dot < 0.75f);       // toward the stock
    CHECK(Near(MatchDirection({ 0.0f, 0.0f, 1.0f }, down, forward).intoWell, 1.0f));
    CHECK(MatchDirection({ 0.0f, 1.0f, 0.0f }, down, forward).dot < 0.01f);        // sideways
    // A gun forward along the magazine (degenerate) falls back to the outward axis.
    CHECK(Near(MatchDirection(down, down, down).dot, 1.0f));
}

static void TestOverlapWithMargin()
{
    const Box seated = AkSeatedBox();
    CHECK(OverlapWithMargin(seated, seated, 0.0f));
    const float width = seated.maxs.x - seated.mins.x;
    const Box beside = Translated(seated, Scale(seated.axisX, width + 1.5f));
    CHECK(!OverlapWithMargin(seated, beside, 1.4f));
    CHECK(OverlapWithMargin(seated, beside, 1.6f));
    // Crossing at right angles with no lattice point inside: it overlaps.
    Box crossing = seated;
    crossing.axisX = { 1.0f, 0.0f, 0.0f };
    crossing.axisZ = { 0.0f, -1.0f, 0.0f };
    crossing.origin = Add(Center(seated), { -3.0f, 0.0f, 4.0f });
    CHECK(OverlapWithMargin(seated, crossing, 0.0f));
    // A pass that skips over the seated magazine in one step still touches when swept.
    const Box before = Translated(seated, { 0.0f, 30.0f, 0.0f });
    const Box after = Translated(seated, { 0.0f, -30.0f, 0.0f });
    const Vec3 prev = RelativeOrigin(seated, before);
    CHECK(!OverlapWithMargin(seated, after, 1.0f));
    CHECK(TouchingSwept(seated, after, &prev, 1.0f));
    CHECK(!TouchingSwept(seated, after, nullptr, 1.0f));
}

static void TestDetectorRealisticSlaps()
{
    const Box seated = AkSeatedBox();   // level gun: magazine straight down, forward +X
    Vec3 out{};
    CHECK(OutwardAxis(seated, { 0.0f, -1.0f, 0.0f }, out));
    const Vec3 forward{ 1.0f, 0.0f, 0.0f };
    Params akPavlov{};
    akPavlov.speedMetersPerSecond = 0.8f;
    akPavlov.forwardDot = 0.6f;
    const ForgivingParams ak = Forgiving(akPavlov);
    const ForgivingParams galil = Forgiving(Params{});
    // Each strike starts 12 cm before the seated magazine and goes 20 cm.
    struct Case { const char* name; Vec3 dir; float speed; float hz; bool decel; bool hit; };
    const Vec3 down = out;
    const Vec3 fwdDown = Add(forward, out);
    const Case cases[] = {
        { "straight down 1.2 m/s 90 Hz", down, 1.2f, 90.0f, false, true },
        { "straight down 0.75 m/s (under Pavlov's 0.8)", down, 0.75f, 90.0f, false, true },
        { "forward and down 1.0 m/s", fwdDown, 1.0f, 90.0f, false, true },
        { "toward the muzzle 1.0 m/s", forward, 1.0f, 90.0f, false, true },
        { "60 deg off down toward the muzzle", Rotate(down, { 0.0f, 1.0f, 0.0f }, -60.0f), 1.0f, 90.0f, false, true },
        { "70 deg off down toward the stock", Rotate(down, { 0.0f, 1.0f, 0.0f }, 70.0f), 1.0f, 90.0f, false, false },
        { "fast 2.5 m/s at 45 Hz (skips the contact)", fwdDown, 2.5f, 45.0f, false, true },
        { "lands and slows (1.5 -> 0.6 m/s)", down, 1.5f, 90.0f, true, true },
        // Negative: slow strikes, inserts and wrong directions.
        { "slow down 0.4 m/s", down, 0.4f, 90.0f, false, false },
        { "slow insert up 0.3 m/s", Scale(out, -1.0f), 0.3f, 90.0f, false, false },
        { "fast insert up 1.5 m/s", Scale(out, -1.0f), 1.5f, 90.0f, false, false },
        { "fast forward and up 1.5 m/s", Add(forward, Scale(out, -1.0f)), 1.5f, 90.0f, false, false },
        { "sideways 1.5 m/s", { 0.0f, 1.0f, 0.0f }, 1.5f, 90.0f, false, false },
        { "toward the stock 1.5 m/s", Scale(forward, -1.0f), 1.5f, 90.0f, false, false },
    };
    for (const Case& c : cases)
    {
        const StrikeRun r = RunStrike(seated, out, forward, c.dir, c.speed, 12.0f, 20.0f, c.hz, ak, { 0.0f, 0.0f, 0.0f }, c.decel);
        if (r.hit != c.hit)
            std::printf("  case '%s': hit=%d (want %d) speed=%.2f dot=%.2f\n", c.name, r.hit ? 1 : 0, c.hit ? 1 : 0, r.speed, r.dot);
        CHECK(r.hit == c.hit);
    }
    // A fast strike that passes 12 cm beside the magazine never touches it.
    const StrikeRun wide = RunStrike(seated, out, forward, down, 1.5f, 12.0f, 20.0f, 90.0f, ak, { 0.0f, 12.0f, 0.0f });
    CHECK(!wide.hit && !wide.nearMiss);
    // Galil/AutoSniper: 0.72 m/s and 0.58.
    CHECK(RunStrike(seated, out, forward, down, 0.8f, 12.0f, 20.0f, 90.0f, galil).hit);
    CHECK(!RunStrike(seated, out, forward, down, 0.6f, 12.0f, 20.0f, 90.0f, galil).hit);
    // A touch with a slow hand is reported as a near miss (for the log).
    CHECK(RunStrike(seated, out, forward, down, 0.4f, 12.0f, 20.0f, 90.0f, ak).nearMiss);
}

static void TestDetectorCantedGunFromHeadsetLog()
{
    // The user's logged outward axes (the gun canted 30-60 degrees): the
    // magazine leaves the well partly sideways. Build the seated box with that
    // outward axis and the barrel level.
    const Vec3 logged[] = { { 0.22f, -0.84f, -0.49f }, { 0.02f, -0.80f, -0.60f }, { 0.13f, -0.36f, -0.92f },
        { 0.39f, -0.84f, -0.37f }, { 0.44f, -0.79f, -0.43f }, { 0.42f, -0.86f, -0.29f } };
    Params akPavlov{};
    akPavlov.speedMetersPerSecond = 0.8f;
    akPavlov.forwardDot = 0.6f;
    const ForgivingParams ak = Forgiving(akPavlov);
    for (const Vec3& o : logged)
    {
        Vec3 out{};
        Normalize(o, out);
        // Barrel: horizontal and square to the magazine.
        Vec3 forward{};
        const Vec3 up{ 0.0f, 0.0f, 1.0f };
        const Vec3 side{ out.y * up.z - out.z * up.y, out.z * up.x - out.x * up.z, out.x * up.y - out.y * up.x };
        Normalize(side, forward);
        Box seated = AkSeatedBox();
        seated.axisY = Scale(out, -1.0f);   // the box's +Y points into the well
        seated.axisZ = forward;
        const Vec3 x{ seated.axisY.y * seated.axisZ.z - seated.axisY.z * seated.axisZ.y,
            seated.axisY.z * seated.axisZ.x - seated.axisY.x * seated.axisZ.z,
            seated.axisY.x * seated.axisZ.y - seated.axisY.y * seated.axisZ.x };
        seated.axisX = x;
        Vec3 check{};
        CHECK(OutwardAxis(seated, { 0.0f, -1.0f, 0.0f }, check));
        CHECK(Near(Dot(check, out), 1.0f, 0.01f));
        // A world-down slap (what a player does with a canted gun) and one along the magazine.
        CHECK(RunStrike(seated, out, forward, { 0.0f, 0.0f, -1.0f }, 1.0f, 12.0f, 20.0f, 90.0f, ak).hit ==
            (MatchDirection({ 0.0f, 0.0f, -1.0f }, out, forward).dot >= ak.dot));
        CHECK(RunStrike(seated, out, forward, out, 0.8f, 12.0f, 20.0f, 90.0f, ak).hit);
        CHECK(RunStrike(seated, out, forward, Add(out, forward), 0.9f, 12.0f, 20.0f, 90.0f, ak).hit);
        CHECK(!RunStrike(seated, out, forward, Scale(out, -1.0f), 0.3f, 12.0f, 20.0f, 90.0f, ak).hit);
    }
}

static void TestDetectorRestingContact()
{
    // The spare resting against the old magazine, then jerked: no new touch, no slap.
    const Box seated = AkSeatedBox();
    Vec3 out{};
    OutwardAxis(seated, { 0.0f, -1.0f, 0.0f }, out);
    Params akPavlov{};
    akPavlov.speedMetersPerSecond = 0.8f;
    akPavlov.forwardDot = 0.6f;
    const ForgivingParams ak = Forgiving(akPavlov);
    Detector d;
    d.Reset();
    const Vec3 still{ 0.0f, 0.0f, 0.0f };
    for (int i = 0; i < 60; ++i)
        CHECK(!d.Update(ak, i / 90.0f, &still, 1, out, { 1.0f, 0.0f, 0.0f }, true).hit);
    const Vec3 jerk = Scale(out, 2.0f);
    for (int i = 60; i < 70; ++i)
        CHECK(!d.Update(ak, i / 90.0f, &jerk, 1, out, { 1.0f, 0.0f, 0.0f }, true).hit);
    // Either of two velocity estimates may carry the strike.
    d.Reset();
    const Vec3 two[2] = { { 0.0f, 0.0f, 0.0f }, Scale(out, 1.0f) };
    d.Update(ak, 0.0f, two, 2, out, { 1.0f, 0.0f, 0.0f }, false);
    CHECK(d.Update(ak, 0.011f, two, 2, out, { 1.0f, 0.0f, 0.0f }, true).hit);
}

int main()
{
    TestForgivingParams();
    TestDirectionMatch();
    TestOverlapWithMargin();
    TestDetectorRealisticSlaps();
    TestDetectorCantedGunFromHeadsetLog();
    TestDetectorRestingContact();
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

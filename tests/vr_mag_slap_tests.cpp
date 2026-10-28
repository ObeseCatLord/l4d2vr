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

int main()
{
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

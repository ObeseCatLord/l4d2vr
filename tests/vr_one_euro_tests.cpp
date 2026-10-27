#include "../L4D2VR/vr_one_euro.h"
#include <cstdio>
#include <cstdlib>

using namespace l4d2vr_filter;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

static Dir3 D(float x, float y, float z) { Dir3 d; d.x = x; d.y = y; d.z = z; NormalizeDir(d); return d; }
static float AngleDeg(const Dir3& a, const Dir3& b)
{
    const float dot = (std::max)(-1.0f, (std::min)(1.0f, a.x * b.x + a.y * b.y + a.z * b.z));
    return std::acos(dot) * 57.2957795f;
}

static void TestStillJitterIsDamped()
{
    OneEuroDirection filter;
    const OneEuroParams params{};
    const float dt = 1.0f / 90.0f;
    float worst = 0.0f;
    for (int i = 0; i < 900; ++i)
    {
        // +-0.5 degree alternating jitter around +x.
        const float j = (i % 2 ? 1.0f : -1.0f) * 0.0087f;
        const Dir3 out = filter.Update(D(1.0f, j, 0.0f), dt, params);
        if (i > 90)
            worst = (std::max)(worst, AngleDeg(out, D(1.0f, 0.0f, 0.0f)));
    }
    CHECK(worst < 0.15f); // jitter cut to well under a third
}

static void TestFastMotionHasLittleLag()
{
    OneEuroDirection filter;
    const OneEuroParams params{};
    const float dt = 1.0f / 90.0f;
    const float rate = 3.0f; // rad/s sweep
    Dir3 out{};
    float lag = 0.0f;
    for (int i = 0; i < 90; ++i)
    {
        const float a = rate * dt * i;
        const Dir3 in = D(std::cos(a), std::sin(a), 0.0f);
        out = filter.Update(in, dt, params);
        lag = AngleDeg(out, in);
    }
    // A fixed 60 ms smoother would trail by about 10 degrees at 3 rad/s.
    CHECK(lag < 8.0f);
}

static void TestResetsOnGap()
{
    OneEuroDirection filter;
    const OneEuroParams params{};
    filter.Update(D(1, 0, 0), 0.011f, params);
    const Dir3 out = filter.Update(D(0, 1, 0), 0.5f, params); // long gap: snap
    CHECK(AngleDeg(out, D(0, 1, 0)) < 0.01f);
    Dir3 bad{ 0.0f, 0.0f, 0.0f };
    const Dir3 kept = filter.Update(bad, 0.011f, params);
    CHECK(AngleDeg(kept, D(0, 1, 0)) < 0.01f);
}

int main()
{
    TestStillJitterIsDamped();
    TestFastMotionHasLittleLag();
    TestResetsOnGap();
    if (g_failures)
    {
        std::printf("%d failure(s)\n", g_failures);
        return EXIT_FAILURE;
    }
    std::printf("vr_one_euro tests passed\n");
    return EXIT_SUCCESS;
}

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
#include "../L4D2VR/vr_dropped_mags.h"
#include <cstdio>
#include <cstdlib>

using namespace l4d2vr_dropped_mags;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

// Flat floor at z = 0.
static bool Floor(void*, const Vector& from, const Vector& to, Vector& hit, Vector& normal)
{
    if (from.z >= 0.0f && to.z < 0.0f)
    {
        const float t = from.z / (from.z - to.z);
        hit = from + (to - from) * t;
        normal = Vector(0.0f, 0.0f, 1.0f);
        return true;
    }
    return false;
}

static void TestFallsAndSettles()
{
    const Params params{};
    Mag mag = Spawn(Vector(0.0f, 0.0f, 40.0f), Vector(30.0f, 0.0f, 0.0f), params);
    CHECK(mag.active);
    for (int i = 0; i < 400 && !mag.settled; ++i)
        Step(mag, 1.0f / 90.0f, params, &Floor, nullptr);
    CHECK(mag.settled);
    CHECK(std::fabs(mag.position.z - 0.5f) < 0.01f);
    CHECK(mag.position.x > 0.0f);
}

static void TestExpires()
{
    Params params{};
    params.lifetimeSeconds = 1.0f;
    Mag mag = Spawn(Vector(0.0f, 0.0f, 10.0f), Vector(0.0f, 0.0f, 0.0f), params);
    bool alive = true;
    for (int i = 0; i < 200 && alive; ++i)
        alive = Step(mag, 0.016f, params, &Floor, nullptr);
    CHECK(!alive && !mag.active);
}

static void TestNoGroundFallsAway()
{
    Params params{};
    params.maxDropDistance = 200.0f;
    Mag mag = Spawn(Vector(0.0f, 0.0f, 10.0f), Vector(0.0f, 0.0f, 0.0f), params);
    bool alive = true;
    for (int i = 0; i < 600 && alive; ++i)
        alive = Step(mag, 0.016f, params, nullptr, nullptr);
    CHECK(!alive);
}

static void TestPoolReplacesOldest()
{
    Pool<2> pool;
    const Params params{};
    Mag a = Spawn(Vector(0, 0, 1), Vector(0, 0, 0), params);
    a.age = 3.0f;
    Mag b = Spawn(Vector(0, 0, 2), Vector(0, 0, 0), params);
    b.age = 1.0f;
    CHECK(pool.Add(a) == 0);
    CHECK(pool.Add(b) == 1);
    Mag c = Spawn(Vector(0, 0, 3), Vector(0, 0, 0), params);
    CHECK(pool.Add(c) == 0);
    CHECK(pool.mags[0].position.z == 3.0f);
}

static void TestRejectsNonFinite()
{
    const Params params{};
    Mag mag = Spawn(Vector(NAN, 0.0f, 0.0f), Vector(0.0f, 0.0f, 0.0f), params);
    CHECK(!mag.active);
}

int main()
{
    TestFallsAndSettles();
    TestExpires();
    TestNoGroundFallsAway();
    TestPoolReplacesOldest();
    TestRejectsNonFinite();
    if (g_failures)
    {
        std::printf("%d failure(s)\n", g_failures);
        return EXIT_FAILURE;
    }
    std::printf("vr_dropped_mags tests passed\n");
    return EXIT_SUCCESS;
}

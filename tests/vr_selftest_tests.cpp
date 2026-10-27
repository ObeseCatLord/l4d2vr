#include "../L4D2VR/vr_selftest.h"
#include <cstdio>
#include <cstdlib>

using namespace l4d2vr_selftest;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

static bool Near(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) <= eps; }

static Vec3 V(float x, float y, float z) { Vec3 v; v.x = x; v.y = y; v.z = z; return v; }

static void TestPath()
{
    const std::vector<Key> keys = { Key{ 0.0f, V(0, 0, 0) }, Key{ 1.0f, V(1, 0, 0), false }, Key{ 2.0f, V(1, 2, 0) } };
    CHECK(Near(Evaluate(keys, -1.0f).x, 0.0f));
    CHECK(Near(Evaluate(keys, 0.5f).x, 0.5f));          // linear segment
    CHECK(Near(Evaluate(keys, 1.5f).y, 1.0f));          // smooth midpoint
    CHECK(Near(Evaluate(keys, 5.0f).y, 2.0f));
    CHECK(Near(SegmentSpeed(keys, 1), 1.0f));
}

static void TestTransforms()
{
    // Yaw 90: forward is +y (Source left), the right hand side is +x.
    const Vec3 p = HeadLocalToTracking(V(1, 1, 1), 90.0f, V(1, 1, 0));
    CHECK(Near(p.x, 2.0f) && Near(p.y, 2.0f) && Near(p.z, 1.0f));
    // World offset round trip through the stick-turn pivot.
    const float rot = 30.0f, scale = 40.0f;
    const Vec3 head = V(0.5f, -0.2f, 1.6f);
    const Vec3 tracking = WorldOffsetToTracking(head, V(20.0f, 0.0f, -8.0f), rot, scale);
    const float dx = tracking.x - head.x, dy = tracking.y - head.y;
    const float a = rot * 3.14159265f / 180.0f;
    CHECK(Near((dx * std::cos(a) - dy * std::sin(a)) * scale, 20.0f, 1e-2f));
    CHECK(Near((dx * std::sin(a) + dy * std::cos(a)) * scale, 0.0f, 1e-2f));
    CHECK(Near((tracking.z - head.z) * scale, -8.0f, 1e-2f));
}

static void TestButtons()
{
    bool down = false, changed = false;
    CHECK(!Lookup(7u, down, changed));                   // inactive registry
    Get().active.store(true);
    BeginFrame();
    SetButton(7u, true);
    CHECK(Lookup(7u, down, changed) && down && changed);
    BeginFrame();
    CHECK(Lookup(7u, down, changed) && down && !changed);
    SetButton(7u, false);
    CHECK(Lookup(7u, down, changed) && !down && changed);
    CHECK(!Lookup(8u, down, changed));
    ReleaseAll();
}

static void TestParse()
{
    const auto steps = ParseSteps(" Features, holster ,,throwPipe ");
    CHECK(steps.size() == 3u && steps[0] == "features" && steps[1] == "holster" && steps[2] == "throwpipe");
}

int main()
{
    TestPath();
    TestTransforms();
    TestButtons();
    TestParse();
    if (g_failures)
    {
        std::printf("%d failure(s)\n", g_failures);
        return EXIT_FAILURE;
    }
    std::printf("vr_selftest tests passed\n");
    return EXIT_SUCCESS;
}

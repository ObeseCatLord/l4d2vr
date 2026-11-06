#include "../L4D2VR/vr_grab_hover.h"
#include <cstdio>
#include <cstdlib>
#include <limits>

using namespace l4d2vr_grab_hover;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

static const float kInf = std::numeric_limits<float>::infinity();

static void TestEntryPulsesOnce()
{
    Settings s{};
    Latch latch;
    std::uint64_t t = 1000u;
    CHECK(!latch.Update(0, kInf, true, t, s));
    CHECK(latch.Update(2, -0.01f, true, t += 11u, s));  // enters slot2: one tick
    for (int i = 0; i < 50; ++i)
        CHECK(!latch.Update(2, -0.02f, true, t += 11u, s)); // staying: no buzz
    CHECK(latch.Key() == 2);
}

static void TestHysteresisAtTheEdge()
{
    Settings s{};
    Latch latch;
    std::uint64_t t = 0u;
    CHECK(latch.Update(1, 0.0f, true, t, s));
    // Jitter across the boundary: outside by 1 cm, inside, outside... never re-ticks.
    for (int i = 0; i < 40; ++i)
    {
        const bool inside = (i % 2) == 0;
        CHECK(!latch.Update(inside ? 1 : 0, inside ? -0.002f : 0.01f, true, t += 11u, s));
    }
    // Leaves beyond the margin, but only for a moment: still latched.
    CHECK(!latch.Update(0, 0.05f, true, t += 11u, s));
    CHECK(!latch.Update(1, -0.001f, true, t += 11u, s));
    // Leaves for good (past the exit delay), comes back: a new tick.
    CHECK(!latch.Update(0, 0.05f, true, t += 11u, s));
    CHECK(!latch.Update(0, 0.05f, true, t += 100u, s));
    CHECK(latch.Key() == 0);
    CHECK(latch.Update(1, -0.01f, true, t += 11u, s));
}

static void TestContactOnlySourceDebounces()
{
    Settings s{};
    Latch latch;
    std::uint64_t t = 0u;
    CHECK(latch.Update(1, -0.001f, true, t, s));
    // A one-frame dropout (touching flickers): no second tick.
    CHECK(!latch.Update(0, kInf, true, t += 11u, s));
    CHECK(!latch.Update(1, -0.001f, true, t += 11u, s));
    CHECK(!latch.Update(0, kInf, true, t += 11u, s));
    CHECK(!latch.Update(0, kInf, true, t += 80u, s));
    CHECK(latch.Key() == 0);
}

static void TestBusyHandIsSilent()
{
    Settings s{};
    Latch latch;
    std::uint64_t t = 0u;
    // Holding an item into a volume: tracked, no tick...
    CHECK(!latch.Update(3, -0.02f, false, t, s));
    // ... and letting go of it there does not tick either.
    CHECK(!latch.Update(3, -0.02f, true, t += 11u, s));
    // Moving to another item (after leaving this one) ticks.
    CHECK(!latch.Update(4, 0.10f, true, t += 11u, s));
    CHECK(latch.Update(4, 0.10f, true, t += 100u, s));
}

static void TestNeighbourWaitsForExit()
{
    Settings s{};
    Latch latch;
    std::uint64_t t = 0u;
    CHECK(latch.Update(1, -0.01f, true, t, s));
    // A neighbour wins while the hand is still within the first one's margin: no chatter.
    CHECK(!latch.Update(2, 0.01f, true, t += 11u, s));
    CHECK(!latch.Update(1, -0.001f, true, t += 11u, s));
    CHECK(!latch.Update(2, 0.01f, true, t += 11u, s));
    CHECK(latch.Key() == 1);
}

static void TestGateLimitsRate()
{
    Settings s{};
    Gate gate;
    CHECK(gate.Allow(1000u, s));
    CHECK(!gate.Allow(1050u, s));
    CHECK(gate.Allow(1000u + s.minIntervalMs, s));
    CHECK(Settings{}.amplitude <= 0.3f && Settings{}.durationSeconds >= 0.015f && Settings{}.durationSeconds <= 0.025f);
}

int main()
{
    TestEntryPulsesOnce();
    TestHysteresisAtTheEdge();
    TestContactOnlySourceDebounces();
    TestBusyHandIsSilent();
    TestNeighbourWaitsForExit();
    TestGateLimitsRate();
    if (g_failures)
    {
        std::printf("%d failure(s)\n", g_failures);
        return EXIT_FAILURE;
    }
    std::printf("vr_grab_hover tests passed\n");
    return EXIT_SUCCESS;
}

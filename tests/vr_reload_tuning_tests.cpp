#include "../L4D2VR/vr_reload_tuning.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>

using namespace l4d2vr_reload_tuning;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

static bool Near(float a, float b) { return std::fabs(a - b) < 1e-4f; }

static void TestPavlovNumbersCarryOver()
{
    Tuning t{};
    CHECK(ForWeapon(5, t));                 // M16: 7 cm x 1.0 cocks, back to 0.7 chambers
    CHECK(Near(t.pullMeters, 0.07f));
    CHECK(Near(t.returnMeters, 0.049f));
    CHECK(Near(t.insertDepthMeters, 0.02f));
    CHECK(ForWeapon(1, t));                 // pistol <- M9: 20 cm x 0.38
    CHECK(Near(t.pullMeters, 0.076f));
    CHECK(Near(t.returnMeters, 0.04f));
    CHECK(t.visualTravelMeters <= 0.03f + 1e-5f); // the slide itself only moves 3 cm
}

static void TestClampsKeepRatios()
{
    Tuning t{};
    CHECK(ForWeapon(10, t));                // AutoSniper 30 cm x 0.38 = 11.4 -> clamped to 10
    CHECK(Near(t.pullMeters, kMaxPullMeters));
    CHECK(Near(t.returnMeters, kMaxPullMeters * (0.11f / 0.38f)));
    CHECK(ForWeapon(7, t));                 // Vector 5 cm x 1.0 -> 5 cm
    CHECK(Near(t.pullMeters, 0.05f));
    CHECK(ForWeapon(26, t));                // AK: 1 cm goal stays above the minimum
    CHECK(t.insertDepthMeters >= kMinInsertMeters);
}

static void TestEveryEntryIsUsable()
{
    for (int id = 0; id < 64; ++id)
    {
        Tuning t{};
        if (!ForWeapon(id, t))
            continue;
        CHECK(t.pullMeters >= kMinPullMeters && t.pullMeters <= kMaxPullMeters);
        CHECK(t.returnMeters >= 0.0f && t.returnMeters < t.pullMeters);
        CHECK(t.insertDepthMeters >= kMinInsertMeters && t.insertDepthMeters <= kMaxInsertMeters);
        CHECK(t.visualTravelMeters > 0.0f && t.visualTravelMeters <= t.pullMeters);
    }
    Tuning t{};
    CHECK(!ForWeapon(21, t));               // grenade launcher keeps its stock reload
    CHECK(!ForWeapon(37, t));               // M60 too
}

int main()
{
    TestPavlovNumbersCarryOver();
    TestClampsKeepRatios();
    TestEveryEntryIsUsable();
    if (g_failures)
    {
        std::printf("%d failure(s)\n", g_failures);
        return EXIT_FAILURE;
    }
    std::printf("vr_reload_tuning tests passed\n");
    return EXIT_SUCCESS;
}

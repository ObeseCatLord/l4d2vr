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

static void TestMagazineSlapGuns()
{
    Slap s{};
    CHECK(SlapForWeapon(26, s));            // AK-47 <- Magazine_AKBase: 80 cm/s, 0.6
    CHECK(Near(s.speedMetersPerSecond, 0.80f));
    CHECK(Near(s.forwardDot, 0.60f));
    CHECK(Near(s.reinsertSeconds, 0.15f));
    CHECK(SlapForWeapon(34, s));            // SG552 <- Galil: VRMagazine defaults
    CHECK(Near(s.speedMetersPerSecond, 0.90f));
    CHECK(Near(s.forwardDot, 0.68f));
    CHECK(SlapForWeapon(10, s));            // military sniper <- AutoSniper: defaults
    CHECK(Near(s.speedMetersPerSecond, 0.90f));
    CHECK(Near(s.forwardDot, 0.68f));
    CHECK(Near(s.reinsertSeconds, kSlapReinsertSeconds));
    const int noSlap[] = { 1, 2, 5, 6, 7, 9, 32, 33, 35, 36, 3, 4, 8, 11, 21, 37 };
    for (int id : noSlap)
        CHECK(!SlapForWeapon(id, s));       // only the Pavlov bAllowMagSlap counterparts
    for (int id = 0; id < 64; ++id)
    {
        Tuning t{};
        if (!SlapForWeapon(id, s))
            continue;
        CHECK(ForWeapon(id, t));            // every slap gun also has a physical reload
        CHECK(s.speedMetersPerSecond > 0.0f && s.speedMetersPerSecond < 2.0f);
        CHECK(s.forwardDot > 0.0f && s.forwardDot < 1.0f);
    }
}

static void TestMagazineHoldTilt()
{
    // Pavlov PickRotationOffset pitch: magazine -75 against its gun.
    CHECK(Near(MagazineHoldTiltDeg(5), 15.0f));   // M16: -75 vs Gun_Base -60
    CHECK(Near(MagazineHoldTiltDeg(26), 15.0f));  // AK-47
    CHECK(Near(MagazineHoldTiltDeg(33), 15.0f));  // MP5
    CHECK(Near(MagazineHoldTiltDeg(3), 15.0f));   // pump shotgun shell
    CHECK(Near(MagazineHoldTiltDeg(1), 5.0f));    // pistol: -75 vs M9 -70
    CHECK(Near(MagazineHoldTiltDeg(32), 5.0f));   // magnum
    CHECK(Near(MagazineHoldTiltDeg(2), 5.0f));    // Uzi
    CHECK(Near(MagazineHoldTiltDeg(7), 5.0f));    // MAC-10
    for (int id = 0; id < 64; ++id)
        CHECK(MagazineHoldTiltDeg(id) >= 0.0f && MagazineHoldTiltDeg(id) <= 15.0f);
}

int main()
{
    TestMagazineHoldTilt();
    TestPavlovNumbersCarryOver();
    TestClampsKeepRatios();
    TestEveryEntryIsUsable();
    TestMagazineSlapGuns();
    if (g_failures)
    {
        std::printf("%d failure(s)\n", g_failures);
        return EXIT_FAILURE;
    }
    std::printf("vr_reload_tuning tests passed\n");
    return EXIT_SUCCESS;
}

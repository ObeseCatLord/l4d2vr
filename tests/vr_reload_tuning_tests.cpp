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
    CHECK(Near(t.insertDepthMeters, 0.019f)); // seats at 95 % of the 2 cm goal (Pavlov)
    CHECK(ForWeapon(1, t));                 // pistol <- M9: 20 cm x 0.38
    CHECK(Near(t.pullMeters, 0.076f));
    CHECK(Near(t.returnMeters, 0.04f));
    CHECK(t.visualTravelMeters <= 0.03f + 1e-5f); // the slide itself only moves 3 cm
}

static void TestClampsKeepRatios()
{
    Tuning t{};
    CHECK(ForWeapon(10, t));                // AutoSniper 30 cm x 0.38 = 11.4 cm, Pavlov's longest pull
    CHECK(Near(t.pullMeters, kMaxPullMeters));
    CHECK(Near(t.pullMeters, 0.114f));
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

static void TestLockOpenAndEasySlide()
{
    // bLockBolt: M9, Desert Eagle, M16, SCAR (<- M16 / SCAR20), SKS.
    const int locks[] = { 1, 32, 5, 9, 6 };
    for (int id : locks)
        CHECK(LocksOpenWhenEmpty(id));
    const int noLock[] = { 2, 7, 33, 26, 34, 10, 35, 36, 3, 8, 4, 11, 21, 37, 0 };
    for (int id : noLock)
        CHECK(!LocksOpenWhenEmpty(id));
    // bEasyMagSlide: once in the well it goes home, so the shallowest seat.
    const int easy[] = { 1, 32, 6, 36, 3, 8, 4, 11 };
    for (int id : easy)
    {
        Tuning t{};
        CHECK(ForWeapon(id, t));
        CHECK(Near(t.insertDepthMeters, kMinInsertMeters));
    }
}

// 2.0.12: Pavlov VRMagazineSlot numbers per L4D2 gun (MagSlideBounds, SlideGoal, speed).
static void TestPavlovMagazineSlots()
{
    using namespace l4d2vr_reload_tuning;
    for (int id : {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 26, 32, 33, 34, 35, 36})
    {
        const MagSlot slot = SlotForWeapon(id);
        const float bound = SlotLateralBoundMeters(id);
        CHECK(bound >= 0.0999f && bound <= 0.2001f);
        CHECK(slot.slideGoalCm >= 1.0f && slot.slideGoalCm <= 13.0f);
        CHECK(ClampReloadSpeed(slot.reloadSpeed) == slot.reloadSpeed);
        float box[3]{};
        HandleBoxCm(id, box);
        CHECK(box[0] > 0.0f && box[1] > 0.0f && box[2] > 0.0f);
    }
    CHECK(std::fabs(SlotLateralBoundMeters(6) - 0.15f) < 1e-6f && std::fabs(SlotLateralBoundMeters(1) - 0.10f) < 1e-6f); // SKS 15 cm, M9 10 cm
    // 2.0.14: the AK-47 seats at Gun_AK47's 1 cm (the AK base's 10 cm was used).
    CHECK(SlotForWeapon(1).slideGoalCm == 3.0f && SlotForWeapon(26).slideGoalCm == 1.0f);
    CHECK(ClampReloadSpeed(20.0f) == 10.0f && ClampReloadSpeed(1.0f) == 2.5f);
    CHECK(std::fabs(kSeatFraction - 0.95f) < 1e-6f);
    CHECK(std::fabs(kReleaseCompleteFraction - 0.35f) < 1e-6f);
}

// 2.0.14: the SCAR has the M16's handle (the gun its recoil uses), HK guns
// lock open on an empty rack, and every handle has Pavlov's haptic ramp.
static void TestPavlovPolish2014()
{
    Tuning scar{}, m16{};
    CHECK(ForWeapon(9, scar) && ForWeapon(5, m16));
    CHECK(Near(scar.pullMeters, m16.pullMeters) && Near(scar.returnMeters, m16.returnMeters));
    CHECK(Near(SlotForWeapon(9).slideGoalCm, SlotForWeapon(5).slideGoalCm));
    CHECK(LocksOpenWhenEmpty(9) && LocksOpenWhenEmpty(5));

    CHECK(LocksOpenOnManualRack(33) && LocksOpenOnManualRack(10));
    for (int id : {1, 2, 3, 4, 5, 6, 7, 8, 9, 11, 26, 32, 34, 35, 36, 37, 0})
        CHECK(!LocksOpenOnManualRack(id));
    for (int id = 0; id < 64; ++id)
        CHECK(!(LocksOpenOnManualRack(id) && LocksOpenWhenEmpty(id)));

    // Pistols ramp 0.5 -> 1.0, rifles 0.05 -> 0.7, the M60 0.1 -> 1.0.
    CHECK(Near(HandleHapticForWeapon(1).minAmp, 0.5f) && Near(HandleHapticForWeapon(1).maxAmp, 1.0f));
    CHECK(Near(HandleHapticForWeapon(26).minAmp, 0.05f) && Near(HandleHapticForWeapon(26).maxAmp, 0.7f));
    CHECK(Near(HandleHapticForWeapon(37).minAmp, 0.1f) && Near(HandleHapticForWeapon(37).rangeCm, 50.0f));
    // Bolt actions and pumps use their own event buffers.
    for (int id : {35, 36, 3, 8})
        CHECK(!HandleHapticForWeapon(id).enabled);
    CHECK(IsBoltAction(35) && IsBoltAction(36) && !IsBoltAction(6));
    // Every gun with a racked handle: the range is the one ForWeapon cocks from.
    for (int id : {1, 2, 4, 5, 6, 7, 9, 10, 11, 26, 32, 33, 34})
    {
        Tuning t{};
        CHECK(ForWeapon(id, t));
        CHECK(HandleHapticForWeapon(id).enabled);
        const float cock = CockInput(id);
        CHECK(cock > 0.05f && cock <= 1.0001f);
    }
    // M9: 2 cm of hand travel is input 0.1 (the grab sound), the cock at 0.38.
    CHECK(Near(HandleInput(1, 0.02f), 0.1f));
    CHECK(Near(CockInput(1), 0.38f));
    CHECK(Near(CockInput(5), 1.0f));       // M16 cocks at full travel
    CHECK(HandleInput(1, -0.01f) == 0.0f);
}

int main()
{
    TestPavlovMagazineSlots();
    TestLockOpenAndEasySlide();
    TestMagazineHoldTilt();
    TestPavlovNumbersCarryOver();
    TestClampsKeepRatios();
    TestEveryEntryIsUsable();
    TestMagazineSlapGuns();
    TestPavlovPolish2014();
    if (g_failures)
    {
        std::printf("%d failure(s)\n", g_failures);
        return EXIT_FAILURE;
    }
    std::printf("vr_reload_tuning tests passed\n");
    return EXIT_SUCCESS;
}

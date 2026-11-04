#include "../L4D2VR/vr_chest_sling.h"
#include <cmath>
#include <cstdio>

using namespace l4d2vr_chest_sling;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

static void TestLongGuns()
{
    const int longGuns[] = { 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 21, 26, 33, 34, 35, 36, 37 };
    for (int id : longGuns)
        CHECK(IsLongGun(id));
    // pistol, magnum, kits, grenades, pills, melee, chainsaw, adrenaline, defib, upgrade packs
    const int others[] = { 0, 1, 32, 12, 13, 14, 15, 19, 20, 23, 24, 25, 30, 31 };
    for (int id : others)
        CHECK(!IsLongGun(id));
}

static void TestRelease()
{
    ReleaseInput in{};
    in.weaponId = 5;  // M16
    in.handToSlingMeters = 0.35f;
    CHECK(OnRelease(in) == ReleaseAction::None);           // nothing let go
    in.dropRequested = true;
    CHECK(OnRelease(in) == ReleaseAction::Sling);          // near the body: slung
    in.handToSlingMeters = 0.75f;
    CHECK(OnRelease(in) == ReleaseAction::Drop);           // arm stretched away: dropped on purpose
    in.handToSlingMeters = 0.6f;
    CHECK(OnRelease(in) == ReleaseAction::Sling);          // the threshold itself still slings
    in.handToSlingMeters = NAN;
    CHECK(OnRelease(in) == ReleaseAction::Sling);          // no measurement: never drop by accident
    in.handToSlingMeters = 0.3f;
    in.weaponId = 1;                                       // pistol: its holster rules, dropped in the open
    CHECK(OnRelease(in) == ReleaseAction::Drop);
    in.weaponId = 37;
    in.enabled = false;                                    // ChestSlingEnabled=false: the old drop
    CHECK(OnRelease(in) == ReleaseAction::Drop);
}

static void TestSidearmRelease()
{
    using namespace l4d2vr_chest_sling;
    // Forefront: a sidearm let go near the body returns to the hip holster.
    CHECK(IsSidearm(1) && IsSidearm(32) && IsSidearm(19) && IsSidearm(20));
    CHECK(!IsSidearm(5) && !IsSidearm(12) && !IsSidearm(14) && !IsSidearm(15));
    ReleaseInput in{};
    in.dropRequested = true;
    in.weaponId = 1;
    in.handToSlingMeters = 0.30f;
    CHECK(OnSidearmRelease(in) == ReleaseAction::Sling);       // near the hip: holstered
    in.handToSlingMeters = 0.75f;
    CHECK(OnSidearmRelease(in) == ReleaseAction::Drop);        // arm stretched out: dropped
    in.handToSlingMeters = -1.0f;
    CHECK(OnSidearmRelease(in) == ReleaseAction::Sling);       // no body frame: keep it
    in.handToSlingMeters = 0.20f;
    in.weaponId = 19;
    in.handSpeedMetersPerSecond = 4.0f;
    CHECK(OnSidearmRelease(in) == ReleaseAction::Drop);        // a melee throw stays a throw
    in.handSpeedMetersPerSecond = 1.0f;
    CHECK(OnSidearmRelease(in) == ReleaseAction::Sling);
    in.weaponId = 14;                                          // a pipe bomb: not a sidearm
    CHECK(OnSidearmRelease(in) == ReleaseAction::None);
    in.weaponId = 1;
    in.enabled = false;                                        // HolsterEmptyHands=false: the old rules
    CHECK(OnSidearmRelease(in) == ReleaseAction::None);
    in.enabled = true;
    in.dropRequested = false;
    CHECK(OnSidearmRelease(in) == ReleaseAction::None);
}

static void TestToggleGripPutAway()
{
    // 2.0.6 toggle grip: the owner's squeeze puts it away only at its holster.
    ReleaseInput in{};
    in.dropRequested = true;
    in.toggleGrip = true;
    in.putAwayReachMeters = 0.15f;
    in.weaponId = 5;                                       // M16 at the sling
    in.handToSlingMeters = 0.10f;
    CHECK(OnRelease(in) == ReleaseAction::Sling);
    in.handToSlingMeters = 0.15f;
    CHECK(OnRelease(in) == ReleaseAction::Sling);          // the reach itself still puts it away
    in.handToSlingMeters = 0.28f;                          // in front of the body: dropped
    CHECK(OnRelease(in) == ReleaseAction::Drop);
    in.handToSlingMeters = -1.0f;
    CHECK(OnRelease(in) == ReleaseAction::Sling);          // no body frame: never drop by accident
    in.weaponId = 19;                                      // melee at the hip
    in.putAwayReachMeters = 0.17f;
    in.handToSlingMeters = 0.12f;
    in.handSpeedMetersPerSecond = 4.0f;
    CHECK(OnSidearmRelease(in) == ReleaseAction::Sling);   // a squeeze in the holster always holsters
    in.handToSlingMeters = 0.40f;
    CHECK(OnSidearmRelease(in) == ReleaseAction::Drop);    // mid-swing elsewhere: dropped with the hand's motion (thrown)
    in.handSpeedMetersPerSecond = 0.2f;
    CHECK(OnSidearmRelease(in) == ReleaseAction::Drop);    // and slowly too: away from the holster drops
    in.weaponId = 14;
    CHECK(OnSidearmRelease(in) == ReleaseAction::None);    // grenades keep their own rules
}

static void TestGrab()
{
    GrabInput in{};
    CHECK(GrabHand(in) == -1);
    in.pressEdge[0] = true;
    in.toHandleMeters[0] = 0.10f;
    CHECK(GrabHand(in) == 0);                              // left hand on the handle
    in.toHandleMeters[0] = 0.30f;
    CHECK(GrabHand(in) == -1);                             // squeezed in the air
    in.toForegripMeters[0] = 0.05f;
    CHECK(GrabHand(in) == 0);                              // the foregrip takes it too
    in.claimed[0] = true;
    CHECK(GrabHand(in) == -1);                             // the squeeze went to a holster
    in.claimed[0] = false;
    in.pressEdge[1] = true;
    in.toHandleMeters[1] = 0.14f;
    CHECK(GrabHand(in) == 1);                              // a handle grab beats a foregrip grab
    in.toHandleMeters[0] = 0.05f;
    CHECK(GrabHand(in) == 0);                              // both on the handle: the closer hand
    in.pressEdge[0] = false;
    CHECK(GrabHand(in) == 1);
    in.toHandleMeters[1] = -1.0f;
    in.toForegripMeters[1] = -1.0f;
    CHECK(GrabHand(in) == -1);                             // no geometry: no grab
}

static void TestSlingState()
{
    Sling sling;
    const uintptr_t rifle = 0x1000, pistol = 0x2000;
    CHECK(!sling.Update(true, true, rifle));
    sling.Hang(rifle);
    const uint32_t s0 = sling.Serial();
    CHECK(sling.Update(true, true, rifle) && sling.Slung() && sling.Weapon() == rifle);
    CHECK(sling.Serial() == s0);
    sling.Take();
    CHECK(!sling.Slung() && sling.Serial() == s0 + 1);
    sling.Hang(rifle);
    CHECK(!sling.Update(true, true, pistol));              // drew the pistol from the hip: the sling forgets
    sling.Hang(rifle);
    CHECK(!sling.Update(true, false, rifle));              // died
    sling.Hang(rifle);
    CHECK(!sling.Update(false, true, rifle));              // option switched off
    sling.Take();                                          // nothing slung: no serial change
    const uint32_t s1 = sling.Serial();
    sling.Take();
    CHECK(sling.Serial() == s1);
}

static void TestButtons()
{
    const int attack = 1 << 0, jump = 1 << 1, attack2 = 1 << 11, use = 1 << 5, reload = 1 << 13;
    CHECK(FilterButtons(attack | jump | attack2 | use | reload, true) == (jump | use));
    CHECK(FilterButtons(attack | reload, false) == (attack | reload));
}

int main()
{
    TestLongGuns();
    TestRelease();
    TestSidearmRelease();
    TestToggleGripPutAway();
    TestGrab();
    TestSlingState();
    TestButtons();
    if (g_failures == 0)
        std::printf("vr_chest_sling_tests: all passed\n");
    return g_failures == 0 ? 0 : 1;
}

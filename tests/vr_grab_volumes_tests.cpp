// 2.0.12: Pavlov's hand and foregrip colliders (vr_grab_volumes.h).
#include "../L4D2VR/vr_grab_volumes.h"
#include <cstdio>
#include <cstdlib>
#include <initializer_list>

static int g_checks = 0;
static void Check(bool ok, const char* what)
{
    ++g_checks;
    if (!ok) { std::printf("FAILED: %s\n", what); std::exit(1); }
}

using namespace l4d2vr_grab;

int main()
{
    Check(kHandSphereRadiusMeters == 0.115f && kAutoUngripMeters == 0.30f && kSupportSlideSpeed == 5.0f, "Pavlov numbers");
    // Every firearm has a box; nothing else does.
    for (int id : { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 21, 26, 32, 33, 34, 35, 36, 37 })
    {
        Box b{};
        Check(ForegripBox(id, b), "firearm has a foregrip box");
        Check(b.half.x > 0.0f && b.half.y > 0.0f && b.half.z > 0.0f, "box has size");
    }
    for (int id : { 0, 12, 13, 15, 19, 20, 24 })
    {
        Box b{};
        Check(!ForegripBox(id, b), "items have no foregrip");
    }
    Box pistol{}, rifle{}, pump{}, autoShotgun{};
    ForegripBox(1, pistol); ForegripBox(5, rifle); ForegripBox(3, pump); ForegripBox(11, autoShotgun);
    Check(pistol.half.x == 0.108f && pistol.half.y == 0.032f && pistol.half.z == 0.054f, "pistol box half sizes");
    Check(rifle.half.x == 0.0627f && rifle.center.x > 0.28f && rifle.center.x < 0.33f, "rifle box ~30 cm ahead");
    Check(pump.half.x == 0.0992f && pump.center.x > 0.40f && autoShotgun.center.x > 0.33f, "shotgun boxes 34-42 cm ahead");
    // The support hand cupped under a pistol grip engages; at the rifle's
    // foregrip it engages, 20 cm off to the side it does not.
    Check(SphereGap(Vec3{ 0.05f, 0.0f, -0.08f }, pistol) <= 0.0f, "cupping a pistol");
    Check(SphereGap(Vec3{ 0.30f, 0.0f, 0.02f }, rifle) <= 0.0f, "rifle foregrip under the handguard");
    Check(SphereGap(Vec3{ 0.30f, 0.20f, 0.07f }, rifle) > 0.0f, "beside the rifle: no grip");
    Check(SphereGap(Vec3{ 0.0f, 0.0f, -0.25f }, rifle) > 0.0f, "at the belly: no grip");
    // Auto-ungrip past 30 cm from the foregrip.
    Check(!AutoUngrip(Vec3{ 0.30f, 0.0f, -0.10f }, rifle), "near the foregrip keeps it");
    Check(AutoUngrip(Vec3{ 0.30f, -0.35f, 0.0f }, rifle), "35 cm away lets go");
    // The bore offset puts the support point on the bore line; it slides to the
    // hand with speed 5 (about 63 % gone after 0.2 s, 92 % after 0.5 s).
    const Vec3 off = BoreOffset(Vec3{ 0.3f, 0.04f, -0.05f });
    Check(off.x == 0.0f && off.y == -0.04f && off.z == 0.05f, "offset to the bore line");
    Check(SlideRemaining(0.0f) == 1.0f && SlideRemaining(0.2f) < 0.38f && SlideRemaining(0.2f) > 0.36f &&
        SlideRemaining(0.5f) < 0.09f, "slide at speed 5");
    // Pistols and machine pistols: the second hand only steadies.
    for (int id : { 1, 32, 2, 7 })
        Check(SupportOnly(id), "support only");
    for (int id : { 3, 5, 33, 35, 37 })
        Check(!SupportOnly(id), "long guns steer");
    std::printf("vr_grab_volumes_tests: %d checks passed\n", g_checks);
    return 0;
}

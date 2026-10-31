#include "../L4D2VR/vr_two_hand_aim.h"
#include <cstdio>

using namespace l4d2vr_two_hand;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

int main()
{
    // Short guns: pistol, magnum, Uzi, silenced SMG (MAC-10), MP5.
    const int shortGuns[] = { 1, 2, 7, 32, 33 };
    for (int id : shortGuns)
    {
        CHECK(IsShortGun(id));
        CHECK(!SupportHandSteersAim(id, true));   // aim from the trigger hand only
        CHECK(SupportHandSteersAim(id, false));   // option off: old two-handed aim
    }
    // Long guns keep two-handed aim: pump/auto/chrome/SPAS shotguns, M16, hunting
    // rifle, SCAR, military sniper, grenade launcher, AK, SG552, AWP, Scout, M60.
    const int longGuns[] = { 3, 4, 5, 6, 8, 9, 10, 11, 21, 26, 34, 35, 36, 37 };
    for (int id : longGuns)
    {
        CHECK(!IsShortGun(id));
        CHECK(SupportHandSteersAim(id, true));
    }
    CHECK(!IsShortGun(0));
    CHECK(!IsShortGun(19));   // melee
    // Recoil: the foregrip steadies every gun, short ones too.
    CHECK(SupportHandSteadiesRecoil(true) && !SupportHandSteadiesRecoil(false));
    if (g_failures)
    {
        std::printf("vr_two_hand_aim: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("vr_two_hand_aim tests passed\n");
    return 0;
}

// 2.0.12 (review s12 issue 15, E13, G5): after the game switches the active
// item by itself, the item returns to the hand that last held it.
#include "../L4D2VR/vr_item_hand.h"
#include <cstdio>
#include <cstdlib>

static int g_checks = 0;
static void Check(bool ok, const char* what)
{
    ++g_checks;
    if (!ok) { std::printf("FAILED: %s\n", what); std::exit(1); }
}

using l4d2vr_item_hand::OwnerHands;

int main()
{
    constexpr bool kLeft = true, kRight = false;
    constexpr std::uintptr_t kGun = 0x100, kKit = 0x200, kPills = 0x300, kGrenade = 0x400, kPistol = 0x500,
        kNewGun = 0x600;
    std::uint64_t t = 10000;
    auto step = [&](OwnerHands& o, std::uintptr_t item, int slot, bool current, bool pending = false,
        bool incap = false, bool akimbo = false) { t += 16; return o.Update(item, slot, current, kRight, pending, incap, akimbo, t); };

    // Spawn: the gun in the right hand.
    OwnerHands o;
    Check(!step(o, kGun, 0, kRight).autoSwitch, "spawn: nothing moves");
    Check(o.Owner(0) == 0, "the right hand owns the gun");
    // The free (left) hand draws the medkit: a mod switch.
    o.ModSelected(t);
    Check(!step(o, kKit, 3, kLeft).autoSwitch, "a holster draw is the mod's switch");
    Check(o.Owner(3) == 1, "the left hand owns the kit");
    // Heal done: the game switches back to the gun, still in the left hand.
    t += 5000;
    OwnerHands::Decision d = step(o, kGun, 0, kLeft);
    Check(d.autoSwitch && d.change && !d.physicalLeft, "medkit used: the gun goes back to the right hand");
    // A null frame between a throw and the switch back is not a new life.
    o.ModSelected(t);
    Check(!step(o, kGrenade, 2, kRight).autoSwitch, "right hand draws a grenade");
    t += 3000;
    Check(!step(o, 0u, -1, kRight).autoSwitch, "thrown: no item for a moment");
    d = step(o, kGun, 0, kRight);
    Check(d.autoSwitch && !d.change && !d.physicalLeft, "after the throw the gun stays right");
    // Left hand draws pills; pills taken; the game returns the gun to the right.
    o.ModSelected(t);
    step(o, kPills, 4, kLeft);
    t += 4000;
    d = step(o, kGun, 0, kLeft);
    Check(d.change && !d.physicalLeft, "pills used: the gun returns right");
    // The gun drawn by the LEFT hand returns left after a consumable used by the right.
    o.Note(0, kLeft);                  // the left hand took the stowed gun from the sling (UpdateChestSling)
    o.ModSelected(t);
    step(o, kKit, 3, kRight);          // the right hand draws the kit
    t += 5000;
    d = step(o, kGun, 0, kRight);
    Check(d.change && d.physicalLeft, "the gun returns to the left hand that last held it");
    // Primary dry: the game raises the pistol; never held before -> dominant hand.
    t += 5000;
    d = step(o, kPistol, 1, kLeft);
    Check(d.autoSwitch && !d.physicalLeft, "an item never held goes to the dominant hand");
    // A picked-up replacement in slot1 belongs to the slot's last hand.
    t += 5000;
    d = step(o, kNewGun, 0, kRight);
    Check(d.autoSwitch && d.physicalLeft, "a new slot1 gun returns to the slot's last hand");
    // Incapacitated: the pistol in the dominant hand.
    o.Note(1, kLeft);
    t += 5000;
    d = step(o, kPistol, 1, kLeft, false, true);
    Check(d.change && !d.physicalLeft, "incapacitated: pistol to the dominant hand");
    // Pending hand change or dual pistols: the current hand becomes the owner.
    t += 5000;
    Check(!step(o, kKit, 3, kLeft, true).autoSwitch && o.Owner(3) == 1, "pending switch: no auto return");
    t += 5000;
    Check(!step(o, kPistol, 1, kRight, false, false, true).autoSwitch && o.Owner(1) == 0, "akimbo: no auto return");
    // An old mod selection does not cover a later game switch.
    o.ModSelected(t);
    t += 2000;
    d = step(o, kGun, 0, kRight);
    Check(d.autoSwitch, "the mod window is 1.5 s");
    // Death: everything forgotten, the next life starts where it is.
    o.Reset();
    Check(o.Owner(0) == -1 && !step(o, kGun, 0, kLeft).autoSwitch, "reset");
    // AutoReturn changes hands and adopts the grip state.
    Check(l4d2vr_item_hand::ChangesHands(l4d2vr_item_hand::Reason::AutoReturn) &&
        l4d2vr_item_hand::AdoptsGrip(l4d2vr_item_hand::Reason::AutoReturn), "AutoReturn reason");
    std::printf("vr_item_owner_tests: %d checks passed\n", g_checks);
    return 0;
}

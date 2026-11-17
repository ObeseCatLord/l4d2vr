// 2.0.12 (review s12 12/13, E5, G2): no grip squeeze ever drops an item.
#include "../L4D2VR/vr_drop_policy.h"
#include "../L4D2VR/vr_grip_release.h"
#include "../L4D2VR/vr_grip_toggle.h"
#include "../L4D2VR/vr_pistol_detachment.h"
#include "../L4D2VR/vr_chest_sling.h"
#include <cstdlib>
#include <iostream>

static int g_Checks = 0;
static void Check(bool passed, const char* contract)
{
    ++g_Checks;
    if (!passed)
    {
        std::cerr << "FAILED: " << contract << '\n';
        std::exit(1);
    }
}

int main()
{
    using namespace l4d2vr_drop;
    Check(kMode == Mode::Off, "the shipped mode never drops");

    // Every item, every place: a release away from a holster keeps the item.
    const int items[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 19, 20, 21, 23, 24, 25, 26,
        30, 31, 32, 33, 34, 35, 36, 37 };
    for (int id : items)
    {
        ReleaseInput in{};
        in.released = true;
        in.weaponId = id;
        Check(Resolve(in) == Outcome::Keep, "uncaught release keeps the item");
        in.deliberateDrop = true;   // a future chord has no effect while the mode is Off
        Check(Resolve(in) == Outcome::Keep, "Off mode ignores the deliberate drop");
        in.caught = true;
        Check(Resolve(in) == Outcome::PutAway, "a holster catch puts it away");
        in.caught = false;
        in.thrown = true;
        Check(Resolve(in) == Outcome::Throw, "a hand throw is a throw");
    }
    {
        ReleaseInput none{};
        Check(Resolve(none) == Outcome::None, "no release, nothing");
        ReleaseInput prop{};
        prop.released = true;
        prop.carriedProp = true;
        Check(Resolve(prop) == Outcome::None, "carried props are released by the trigger, not here");
    }
    // The later chord: drops anything but slot 2.
    for (int id : items)
    {
        ReleaseInput in{};
        in.mode = Mode::Chord;
        in.released = true;
        in.deliberateDrop = true;
        in.weaponId = id;
        const bool sidearm = id == 1 || id == 19 || id == 20 || id == 32;
        Check(IsNeverDroppable(id) == sidearm, "sidearms are slot 2");
        Check(Resolve(in) == (sidearm ? Outcome::Keep : Outcome::Drop), "the chord never drops a sidearm");
        in.deliberateDrop = false;
        Check(Resolve(in) == Outcome::Keep, "the chord needs the deliberate input");
    }

    // The release latch after a kept release: the grip owns the item again
    // and the next squeeze/release cycle is again a release (kept again).
    {
        l4d2vr_grip::ReleaseLatch latch;
        latch.AdoptWeapon(7u);
        Check(!latch.Update(true, true, true, 7u), "held");
        Check(latch.Update(true, true, false, 7u), "second squeeze lets go");
        latch.ReclaimWeapon(7u);   // CreateMove: kept in the hand
        Check(latch.Owns(), "the grip owns it again");
        Check(!latch.Update(true, true, true, 7u), "held again (toggle re-held)");
        Check(latch.Update(true, true, false, 7u), "the next squeeze is a release again");
    }
    // The toggle grip is held again after a kept release.
    {
        l4d2vr_grip_toggle::Toggle toggle;
        l4d2vr_grip_toggle::Input in{};
        in.active = in.ready = true;
        in.nowMs = 1000u;
        in.rawDown = true;
        in.takeSerial = 1u;
        Check(toggle.Update(in), "take");
        in.takeSerial = 2u;
        in.nowMs = 1010u;
        Check(toggle.Update(in), "taken");
        in.rawDown = false;
        in.nowMs = 1200u;
        Check(toggle.Update(in), "kept after letting go");
        in.rawDown = true;
        in.nowMs = 3000u;
        Check(!toggle.Update(in), "second squeeze releases");
        toggle.Adopt(true, true);   // ProcessInput: m_GripToggleRehold
        Check(toggle.Held(), "held again");
        in.nowMs = 3010u;
        Check(toggle.Update(in), "still held while the squeeze is down");
        in.rawDown = false;
        in.nowMs = 3100u;
        Check(toggle.Update(in), "still held after letting go");
    }
    // A pistol hand keeps its pistol: AdoptPickup gives the hand's latch back.
    {
        l4d2vr_pistol::Ownership own;
        own.Observe(true, 9u, false, 0u);
        own.AdoptPickup(l4d2vr_dual::Hand::Right);
        own.Release(true, true, false, false, 10u);
        const l4d2vr_dual::Hand released = own.Release(true, false, false, false, 20u);
        Check(released == l4d2vr_dual::Hand::Right, "the right pistol hand lets go");
        own.AdoptPickup(released);
        Check(own.Owns(l4d2vr_dual::Hand::Right), "the pistol stays owned by its hand");
    }
    // The sidearm put-away rule is unchanged inside the hip reach; beyond it the
    // client now keeps the item instead of dropping it (no Sling there).
    {
        l4d2vr_chest_sling::ReleaseInput in{};
        in.dropRequested = true;
        in.weaponId = 1;
        in.toggleGrip = true;
        in.handToSlingMeters = 0.10f;
        Check(l4d2vr_chest_sling::OnSidearmRelease(in) == l4d2vr_chest_sling::ReleaseAction::Sling, "hip catch");
        in.handToSlingMeters = 0.50f;
        ReleaseInput r{};
        r.released = true;
        r.weaponId = 1;
        r.caught = l4d2vr_chest_sling::OnSidearmRelease(in) == l4d2vr_chest_sling::ReleaseAction::Sling;
        Check(Resolve(r) == Outcome::Keep, "a pistol squeezed at arm's length stays in the hand");
    }
    std::cout << "vr_drop_policy_tests: " << g_Checks << " checks passed\n";
    return 0;
}

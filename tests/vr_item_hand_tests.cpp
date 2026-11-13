#include "../L4D2VR/vr_item_hand.h"
#include "../L4D2VR/vr_grip_toggle.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>

using namespace l4d2vr_item_hand;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

static void TestMapping()
{
    // Right-handed: the item hand is the right controller until the left takes an item.
    CHECK(!ItemHandIsPhysicalLeft(false, false));
    CHECK(ItemHandIsPhysicalLeft(false, true));
    // LeftHanded: mirrored.
    CHECK(ItemHandIsPhysicalLeft(true, false));
    CHECK(!ItemHandIsPhysicalLeft(true, true));
    for (int lh = 0; lh < 2; ++lh)
        for (int left = 0; left < 2; ++left)
            CHECK(ItemHandIsPhysicalLeft(lh != 0, SwappedForPhysicalHand(lh != 0, left != 0)) == (left != 0));
}

static void TestSwitchCommitsOnce()
{
    Switch sw;
    bool swapped = false;
    Reason reason = Reason::None;
    CHECK(!sw.Commit(swapped, reason) && reason == Reason::None);
    sw.Request(true, Reason::HolsterDraw);
    CHECK(sw.Pending() && sw.PendingTarget());
    CHECK(!swapped);                                  // nothing changes before the commit
    CHECK(sw.Commit(swapped, reason) && swapped && reason == Reason::HolsterDraw);
    CHECK(!sw.Pending());
    CHECK(!sw.Commit(swapped, reason) && swapped);    // once
    sw.Request(true, Reason::SlingTake);              // already there: no change, reason reported
    CHECK(!sw.Commit(swapped, reason) && swapped && reason == Reason::SlingTake);
    sw.Request(false, Reason::SlingTake);
    sw.Request(true, Reason::HolsterDraw);            // the last request wins
    CHECK(!sw.Commit(swapped, reason) && swapped);
    sw.Request(false, Reason::Reset);
    sw.Cancel();
    CHECK(!sw.Commit(swapped, reason) && swapped);
}

static void TestOnlyTakesChangeHands()
{
    // 2.0.6: only taking an item from the body moves it to a hand.
    CHECK(ChangesHands(Reason::HolsterDraw));
    CHECK(ChangesHands(Reason::SlingTake));
    CHECK(ChangesHands(Reason::Reset));
    CHECK(ChangesHands(Reason::SelfTest));
    CHECK(!ChangesHands(Reason::None));
    CHECK(AdoptsGrip(Reason::SlingTake));             // the squeezing hand keeps the slung gun
    CHECK(!AdoptsGrip(Reason::HolsterDraw));          // a draw presses afresh next frame
    CHECK(!AdoptsGrip(Reason::Reset));
    CHECK(ChangesHands(Reason::DrawCancel));          // 2.0.11: undo of a draw that never drew
    CHECK(AdoptsGrip(Reason::DrawCancel));            // ... the old hand keeps its grip state
}

static Under At(bool takeable, bool ownHolster, bool foregrip, bool heldItem)
{
    Under u{};
    u.takeable = takeable; u.ownHolster = ownHolster; u.foregrip = foregrip; u.heldItem = heldItem;
    return u;
}

static void TestOwnershipToggle()
{
    const int L = 0, R = 1;
    const Under air = At(false, false, false, false);
    const Under holster = At(true, false, false, false);
    const Under own = At(false, true, false, false);
    const Under handle = At(false, false, false, true);
    const Under fore = At(false, false, true, false);
    Ownership o(true, true);
    CHECK(o.Owner() == -1);
    CHECK(o.Press(R, air) == Action::None);           // an empty squeeze takes nothing
    CHECK(o.Press(R, holster) == Action::Take && o.Owner() == R);
    CHECK(o.Release(R, air) == Action::None && o.Owner() == R);   // letting go never drops
    // The free hand grips the held gun's handle: never ownership.
    CHECK(o.Press(L, handle) == Action::None && o.Owner() == R);
    CHECK(o.Release(L, handle) == Action::None && o.Owner() == R);
    // ... and its foregrip: the support grip, a toggle.
    CHECK(o.Press(L, fore) == Action::SupportGrip && o.Support() && o.Owner() == R);
    CHECK(o.Release(L, fore) == Action::None && o.Support());
    CHECK(o.Press(L, fore) == Action::SupportRelease && !o.Support() && o.Owner() == R);
    // The support hand's squeeze never drops or holsters the gun, wherever it is.
    CHECK(o.Press(L, own) == Action::None && o.Owner() == R);
    // The owner's squeeze in the open drops it (and the support hand lets go).
    CHECK(o.Press(L, fore) == Action::SupportGrip);
    CHECK(o.Press(R, air) == Action::Drop && o.Owner() == -1 && !o.Support());
    // Taken again, the owner's squeeze at its own holster puts it away.
    CHECK(o.Press(R, holster) == Action::Take);
    CHECK(o.Press(R, own) == Action::PutAway && o.Owner() == -1);
    // The only way into the other hand: from the body.
    CHECK(o.Press(L, holster) == Action::Take && o.Owner() == L);
    CHECK(o.Press(R, handle) == Action::None && o.Owner() == L);
    CHECK(o.Press(L, own) == Action::PutAway);
    CHECK(o.Press(R, holster) == Action::Take && o.Owner() == R);
    // A different item from a holster with the free hand: that hand owns it.
    CHECK(o.Press(L, holster) == Action::Take && o.Owner() == L && !o.Support());
}

static void TestOwnershipHold()
{
    const int L = 0, R = 1;
    const Under air = At(false, false, false, false);
    const Under holster = At(true, false, false, false);
    const Under own = At(false, true, false, false);
    const Under fore = At(false, false, true, false);
    Ownership o(false, false);                        // GripToggle=false, held foregrip
    CHECK(o.Press(R, holster) == Action::Take);
    CHECK(o.Press(L, fore) == Action::SupportGrip);
    CHECK(o.Release(L, fore) == Action::SupportRelease && o.Owner() == R);
    CHECK(o.Release(R, air) == Action::Drop && o.Owner() == -1);
    CHECK(o.Press(R, holster) == Action::Take);
    CHECK(o.Release(R, own) == Action::PutAway);
    // Letting go while the other hand holds the foregrip still lets it go
    // (no transfer to the foregrip hand).
    CHECK(o.Press(R, holster) == Action::Take);
    CHECK(o.Press(L, fore) == Action::SupportGrip);
    CHECK(o.Release(R, air) == Action::Drop && o.Owner() == -1 && !o.Support());
}

static void TestRoleRemap()
{
    const int invalid = 0, primary = 1, secondary = 2, weaponGrip = 3, offGrip = 4, jump = 5;
    auto r = [&](int a, bool swapped) { return RemapRole(a, swapped, primary, secondary, weaponGrip, offGrip, invalid); };
    for (int a = 0; a <= 5; ++a)
        CHECK(r(a, false) == a);                // right-handed play is untouched
    CHECK(r(primary, true) == secondary);       // the left trigger fires the left-held gun
    CHECK(r(secondary, true) == primary);       // the right trigger is the free hand's
    CHECK(r(weaponGrip, true) == offGrip);
    CHECK(r(offGrip, true) == weaponGrip);
    CHECK(r(jump, true) == jump);               // sticks and buttons stay
    CHECK(r(invalid, true) == invalid);
    // An unbound partner never replaces a bound action.
    CHECK(RemapRole(primary, true, primary, invalid, weaponGrip, offGrip, invalid) == primary);
}

static void TestToggleAdopt()
{
    using namespace l4d2vr_grip_toggle;
    Toggle toggle;
    Input in{};
    in.active = in.ready = true;
    in.heldItemTag = 9u;
    auto step = [&](bool raw) { in.nowMs += 11u; in.rawDown = raw; return toggle.Update(in); };
    // The old hand holds the item (drawn and taken).
    CHECK(step(true));
    in.takeSerial++;
    CHECK(step(true));
    CHECK(step(false));
    CHECK(toggle.Held());
    // A sling take: the taking hand adopts the item with its grip down, which
    // is not a fresh press (no release, no drop).
    toggle.Adopt(true, true);
    CHECK(step(true));
    CHECK(step(false));                         // letting go keeps it (toggle)
    for (int i = 0; i < 200; ++i) CHECK(step(false));
    CHECK(!step(true));                         // the next squeeze lets go
    // Adopting an item the grip never took keeps it not held.
    Toggle loose;
    Input in2{};
    in2.active = in2.ready = true;
    loose.Adopt(true, false);
    in2.rawDown = true; in2.nowMs = 100u;
    CHECK(!loose.Update(in2));                  // no press edge from the taking squeeze
}

// 2.0.11 (review F1): a free-hand draw that never draws is undone.
static void TestDrawWatch()
{
    DrawWatch watch;
    CHECK(!watch.Update(1u, 10u, false, 0u));       // idle
    watch.Start(5u, 10u, false, true, 1000u);
    CHECK(watch.Active() && !watch.PreviousPhysicalLeft() && watch.PreviousHeld());
    CHECK(!watch.Update(5u, 10u, true, 2000u));     // the hand change is still pending
    CHECK(!watch.Update(5u, 10u, false, 1100u));    // within the window
    CHECK(watch.Update(5u, 10u, false, 1300u));     // no draw: undo once
    CHECK(!watch.Active() && !watch.Update(5u, 10u, false, 1400u));
    watch.Start(5u, 10u, true, false, 1000u);
    CHECK(!watch.Update(6u, 10u, false, 1050u));    // the draw happened
    CHECK(!watch.Active() && !watch.Update(6u, 10u, false, 2000u));
    watch.Start(5u, 10u, true, false, 1000u);
    CHECK(!watch.Update(5u, 11u, false, 1050u));    // another item became active (not this draw's business)
    CHECK(!watch.Active());
    watch.Start(5u, 10u, true, false, 1000u);
    watch.Cancel();                                 // a reset / sling take
    CHECK(!watch.Update(5u, 10u, false, 5000u));
}

int main()
{
    TestDrawWatch();
    TestMapping();
    TestSwitchCommitsOnce();
    TestOnlyTakesChangeHands();
    TestOwnershipToggle();
    TestOwnershipHold();
    TestRoleRemap();
    TestToggleAdopt();
    if (g_failures)
    {
        std::printf("vr_item_hand: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("vr_item_hand tests passed\n");
    return 0;
}

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
    sw.Request(true, Reason::HandOff);                // already there: no change, reason reported
    CHECK(!sw.Commit(swapped, reason) && swapped && reason == Reason::HandOff);
    sw.Request(false, Reason::HandOff);
    sw.Request(true, Reason::HolsterDraw);            // the last request wins
    CHECK(!sw.Commit(swapped, reason) && swapped);
    sw.Request(false, Reason::Reset);
    sw.Cancel();
    CHECK(!sw.Commit(swapped, reason) && swapped);
}

static void TestHandOff()
{
    HandOffInput in{};
    in.enabled = in.ready = in.pressEdge = in.itemInHand = true;
    in.handDistanceMeters = 0.08f;
    CHECK(HandOff(in));
    HandOffInput far = in; far.handDistanceMeters = 0.30f;
    CHECK(!HandOff(far));                       // a squeeze in the air is not a hand-off
    HandOffInput edge = in; edge.handDistanceMeters = 0.14f;
    CHECK(HandOff(edge));
    HandOffInput claimed = in; claimed.claimed = true;
    CHECK(!HandOff(claimed));                   // foregrip, magazine, bolt, holster win
    HandOffInput held = in; held.pressEdge = false;
    CHECK(!HandOff(held));                      // only a fresh squeeze
    HandOffInput empty = in; empty.itemInHand = false;
    CHECK(!HandOff(empty));
    HandOffInput off = in; off.enabled = false;
    CHECK(!HandOff(off));
    HandOffInput dead = in; dead.ready = false;
    CHECK(!HandOff(dead));
    HandOffInput bad = in; bad.handDistanceMeters = std::nanf("");
    CHECK(!HandOff(bad));
    bad.handDistanceMeters = -1.0f;
    CHECK(!HandOff(bad));
}

static void TestReleaseTransfer()
{
    ReleaseInput in{};
    in.enabled = in.ready = in.released = in.twoHandGrip = in.supportHolds = true;
    CHECK(ReleaseTransfer(in));
    ReleaseInput holster = in; holster.overHolster = true;
    CHECK(!ReleaseTransfer(holster));           // letting go in a holster still puts it away
    ReleaseInput noFore = in; noFore.twoHandGrip = false;
    CHECK(!ReleaseTransfer(noFore));            // letting go in the open still drops
    ReleaseInput still = in; still.released = false;
    CHECK(!ReleaseTransfer(still));
    ReleaseInput loose = in; loose.supportHolds = false;
    CHECK(!ReleaseTransfer(loose));
    ReleaseInput off = in; off.enabled = false;
    CHECK(!ReleaseTransfer(off));
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
    // The other hand squeezes it: it adopts the held item with its grip down,
    // which is not a fresh press (no release, no drop).
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
    CHECK(!loose.Update(in2));                  // no press edge from the hand-off squeeze
}

int main()
{
    TestMapping();
    TestSwitchCommitsOnce();
    TestHandOff();
    TestReleaseTransfer();
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

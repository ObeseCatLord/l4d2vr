#include "../L4D2VR/vr_grip_toggle.h"
#include <cstdio>
#include <cstdlib>

using namespace l4d2vr_grip_toggle;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

struct Rig
{
    Toggle toggle;
    Input in;
    Rig()
    {
        in.active = true;
        in.ready = true;
        in.heldItemTag = 7u;
    }
    bool Step(bool raw, std::uint64_t dtMs = 11u)
    {
        in.nowMs += dtMs;
        in.rawDown = raw;
        return toggle.Update(in);
    }
    // Squeeze and let go over a few frames; returns the virtual state after.
    bool Squeeze()
    {
        Step(true);
        Step(true);
        return Step(false);
    }
};

static void TestDrawKeepsItemAfterLettingGo()
{
    Rig r;
    CHECK(r.Step(true));          // squeeze at a holster
    r.in.takeSerial++;            // the holster drew the item
    CHECK(r.Step(true));
    CHECK(r.Step(false));         // let go: still held
    r.in.heldItemTag = 9u;        // the drawn item becomes active
    for (int i = 0; i < 200; ++i)
        CHECK(r.Step(false));
    CHECK(!r.Squeeze());          // next squeeze lets it go
    CHECK(!r.Step(false, 1000u));
}

static void TestEmptyGrabBecomesTap()
{
    Rig r;
    CHECK(r.Step(true));
    CHECK(r.Step(false));
    bool released = false;
    for (int i = 0; i < 40 && !released; ++i)
        released = !r.Step(false);
    CHECK(released);
    CHECK(r.in.nowMs < 400u);
    CHECK(r.Step(true));          // the next squeeze grabs again, not releases
}

static void TestOwnHolsterSqueezeIsImmediateTap()
{
    Rig r;
    r.in.inOwnHolster = true;
    CHECK(r.Step(true));          // one frame down...
    CHECK(!r.Step(true));         // ...then up, so the holster sees a release
}

static void TestPickupExtendsWait()
{
    Rig r;
    CHECK(r.Step(true));
    r.in.pickupSerial++;          // contact pickup requested
    for (int i = 0; i < 60; ++i)  // ~660 ms waiting for the server
        CHECK(r.Step(false));
    r.in.takeSerial++;            // adopted
    CHECK(r.Step(false));
    CHECK(r.Step(false, 5000u));
}

static void TestItemReplacedEndsHold()
{
    Rig r;
    r.Step(true);
    r.in.takeSerial++;
    r.Step(false);
    r.in.heldItemTag = 9u;
    CHECK(r.Step(false, 2000u));  // pinned to 9
    r.in.heldItemTag = 3u;        // thrown, died, scripted switch
    CHECK(!r.Step(false));
    CHECK(r.Step(true));          // a fresh squeeze grabs
}

static void TestNotReadyClearsAndDisabledPassesThrough()
{
    Rig r;
    r.Step(true);
    r.in.takeSerial++;
    r.Step(false);
    r.in.ready = false;
    CHECK(!r.Step(false));
    r.in.ready = true;
    CHECK(!r.Step(false));
    r.in.enabled = false;
    CHECK(r.Step(true));
    CHECK(!r.Step(false));
}

static void TestSqueezeOverOtherHolsterSwaps()
{
    Rig r;
    r.Step(true);
    r.in.takeSerial++;
    r.Step(false);
    r.in.heldItemTag = 9u;
    CHECK(r.Step(false, 2000u));
    r.in.inOtherHolster = true;
    CHECK(!r.Step(true));         // one frame let go...
    CHECK(r.Step(true));          // ...then grab from the other holster
    r.in.takeSerial++;
    CHECK(r.Step(false));
    r.in.inOtherHolster = false;
    r.in.heldItemTag = 4u;
    CHECK(r.Step(false, 2000u));  // the new item is held
}

// 2.0.11 (review F4): a free-hand holster draw hands the grip to the other
// controller. The old hand's grip was physically down (not held by the
// toggle: the spawn pistol); the new hand already squeezes at the holster.
// Without ExpectFreshPress the new hand's squeeze is not an edge and nothing
// is drawn.
static void TestFreshPressAfterHandChange()
{
    Rig r;
    for (int i = 0; i < 60; ++i)
        r.Step(true);                 // the old item hand keeps squeezing: the empty grab timed out
    CHECK(!r.toggle.Held());
    r.toggle.ExpectFreshPress();      // the hands changed: the grip is now the new hand's
    CHECK(r.Step(true));              // the new hand's squeeze is a press
    // Without it, the same raw state would not be a press.
    Rig old;
    for (int i = 0; i < 60; ++i)
        old.Step(true);
    CHECK(!old.Step(true));
    // A held item stays held; the fresh press then lets it go / swaps.
    Rig held;
    CHECK(held.Step(true));
    held.in.takeSerial++;
    held.Step(true);
    held.Step(false);
    held.toggle.ExpectFreshPress();
    CHECK(held.toggle.Held());
    held.in.inOtherHolster = true;
    CHECK(!held.Step(true));          // one frame up (swap) ...
    CHECK(held.Step(true));           // ... then the grab from the other holster
}

// 2.0.11 (review F3): the pinned tag is published once the taken item settled.
static void TestPinnedTag()
{
    Rig r;
    CHECK(r.toggle.PinnedTag() == 0u);
    CHECK(r.Step(true));
    CHECK(r.toggle.PinnedTag() == 0u);        // pending: nothing taken yet
    r.in.takeSerial++;
    r.Step(true);
    r.Step(false);
    r.in.heldItemTag = 9u;
    CHECK(r.toggle.PinnedTag() == 0u);        // not settled yet
    r.Step(false, 2000u);
    r.Step(false);
    CHECK(r.toggle.PinnedTag() == 9u);
    CHECK(!r.Squeeze());                      // let go
    CHECK(r.toggle.PinnedTag() == 0u);
}

int main()
{
    TestFreshPressAfterHandChange();
    TestPinnedTag();
    TestSqueezeOverOtherHolsterSwaps();
    TestDrawKeepsItemAfterLettingGo();
    TestEmptyGrabBecomesTap();
    TestOwnHolsterSqueezeIsImmediateTap();
    TestPickupExtendsWait();
    TestItemReplacedEndsHold();
    TestNotReadyClearsAndDisabledPassesThrough();
    if (g_failures)
    {
        std::printf("%d failure(s)\n", g_failures);
        return EXIT_FAILURE;
    }
    std::printf("vr_grip_toggle tests passed\n");
    return EXIT_SUCCESS;
}

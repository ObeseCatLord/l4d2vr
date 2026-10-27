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

int main()
{
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

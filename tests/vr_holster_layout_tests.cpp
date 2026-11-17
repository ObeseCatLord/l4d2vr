// 2.0.12 (review s12 issue 7, G1): selectable holster layouts (forefront,
// pavlov), the chest pockets at Pavlov's vest heights, the empty-hand rule and
// nearest-wins with hysteresis; replay of the 415 grip presses of the s12 log.
#include <limits>
#if defined(VECTOR_PARANOIA) && !defined(VEC_T_NAN)
#define VEC_T_NAN std::numeric_limits<float>::quiet_NaN()
#endif
#ifdef _MSC_VER
#pragma warning(push, 0)
#endif
#include "../L4D2VR/sdk/vector.h"
#ifdef _MSC_VER
#pragma warning(pop)
#endif
#include "../L4D2VR/vr_holster.h"
#include <cmath>
#include <cstdio>
#include <string>

using namespace l4d2vr_holster;

static int g_failures = 0;
static int g_checks = 0;
#define CHECK(cond) do { ++g_checks; if (!(cond)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

namespace
{
    constexpr float kS = 40.0f; // units per metre
    struct Press { int left; int ready; int zone; float fwd, dom, up; int active; };
    const Press kPresses[] = {
#include "vr_holster_s12_presses.inc"
    };

    BodyFrame Frame(const Layout& layout, bool leftHanded = false)
    {
        BodyFrame f = MakeFrame(Vector(100.0f, 50.0f, 1.7f * kS), 0.0f, leftHanded, kS, 1.0f);
        const Vector fwd(1.0f, 0.0f, 0.0f), right(0.0f, -1.0f, 0.0f), up(0.0f, 0.0f, 1.0f);
        ApplyPavlovRoot(f, layout, fwd, right, up, leftHanded, -1.0f);
        return f;
    }
    // HMD-relative (forward, dominant, up) metres of a root offset.
    float HmdZ(const Layout& l, const Vector& socket) { return l.rootDropMeters + socket.z; }
}

static void TestLayoutTables()
{
    bool known = false;
    CHECK(LayoutKindFromName("forefront", &known) == LayoutKind::Forefront && known);
    CHECK(LayoutKindFromName("Pavlov", &known) == LayoutKind::Pavlov && known);
    CHECK(LayoutKindFromName("bonelab", &known) == LayoutKind::Forefront && !known); // not shipped yet
    CHECK(LayoutKindFromName(nullptr, &known) == LayoutKind::Forefront && !known);
    CHECK(std::string(LayoutKindName(LayoutKind::Pavlov)) == "pavlov");

    const Layout ff = MakeLayout(LayoutKind::Forefront);
    const Layout pv = MakeLayout(LayoutKind::Pavlov);
    CHECK(ff.layoutKind == LayoutKind::Forefront && pv.layoutKind == LayoutKind::Pavlov);
    // The default Layout{} is Forefront.
    CHECK(Layout{}.socket[2].z == ff.socket[2].z && Layout{}.kind[0] == ZoneKind::Sling);
    // Forefront: grenade and magazines on Pavlov's top pocket row, 30 cm apart.
    CHECK(std::fabs(HmdZ(ff, ff.socket[2]) - -0.31f) < 0.005f);
    CHECK(std::fabs(HmdZ(ff, ff.magPouch) - -0.33f) < 0.005f);
    const Vector gm = ff.socket[2] - ff.magPouch;
    CHECK(std::sqrt(DotProduct(gm, gm)) > 0.29f);
    CHECK(ff.pocketReachMeters <= 0.12f && ff.pocketItemGrabMarginMeters <= 0.03f);
    // Pavlov: root 0.75 m below the HMD; rifle on the back; pockets 0.31-0.54 m down.
    CHECK(pv.rootDropMeters == -0.75f && pv.lookDownBackMeters > 0.0f && pv.reachFloorBackMeters > 0.0f);
    CHECK(pv.kind[0] == ZoneKind::Back && pv.kind[1] == ZoneKind::Hip && pv.kind[2] == ZoneKind::Pocket &&
        pv.kind[3] == ZoneKind::Pocket && pv.kind[4] == ZoneKind::Pocket);
    for (int slot : { 2, 3, 4 })
    {
        const float z = HmdZ(pv, pv.socket[slot]);
        CHECK(z <= -0.30f && z >= -0.55f);
    }
    CHECK(std::fabs(HmdZ(pv, pv.socket[2]) - -0.309f) < 0.005f);       // top pocket, dominant
    CHECK(pv.socket[2].y > 0.1f && pv.socket[4].y < -0.1f);
    CHECK(std::fabs(HmdZ(pv, pv.socket[1]) - -0.523f) < 0.005f);       // hip item rest
    CHECK(pv.magPouch.y < -0.15f && HmdZ(pv, pv.magPouch) < -0.5f);     // off-hand hip pouch
    // Every pair of pocket/pouch volumes is separated.
    const Vector pockets[] = { pv.socket[2], pv.socket[3], pv.socket[4], pv.magPouch };
    for (int i = 0; i < 4; ++i)
        for (int j = i + 1; j < 4; ++j)
        {
            const Vector d = pockets[i] - pockets[j];
            CHECK(std::sqrt(DotProduct(d, d)) > 0.15f);
        }
}

static void TestEveryZoneReachable()
{
    for (LayoutKind kind : { LayoutKind::Forefront, LayoutKind::Pavlov })
        for (int handed = 0; handed < 2; ++handed)
        {
            const Layout layout = MakeLayout(kind);
            const BodyFrame f = Frame(layout, handed == 1);
            CHECK(f.valid);
            for (int z = 0; z < kZoneCount; ++z)
            {
                const Zone zone = static_cast<Zone>(z);
                CHECK(FindZone(f, layout, ZoneCenterWorld(f, layout, zone), 0x1Fu) == zone);
                // The drawn item's pose is a proper rotation.
                const ItemPose pose = BuildItemPose(f, layout, zone, zone == Zone::Primary ? ItemShape::LongGun : ItemShape::Equipment);
                CHECK(pose.valid);
                Vector c{};
                CrossProduct(pose.axisX, pose.axisY, c);
                CHECK(DotProduct(c, pose.axisZ) > 0.99f);
            }
            // The magazine pouch is no holster.
            CHECK(FindZone(f, layout, RootToWorld(f, layout.magPouch), 0x1Fu) == Zone::None);
            // A gun at the aiming position takes nothing.
            CHECK(FindZone(f, layout, ToWorld(f, Vector(0.35f, 0.12f, -0.20f)), 0x1Fu) == Zone::None);
        }
    // Pavlov: the dominant hip mirrors for the left-handed.
    const Layout pv = MakeLayout(LayoutKind::Pavlov);
    const BodyFrame r = Frame(pv, false), l = Frame(pv, true);
    const Vector hr = ZoneCenterWorld(r, pv, Zone::Secondary), hl = ZoneCenterWorld(l, pv, Zone::Secondary);
    CHECK(std::fabs((hr.y - r.head.y) + (hl.y - l.head.y)) < 0.01f && std::fabs(hr.y - r.head.y) > 0.2f * kS);
}

static void TestGrenadeHeightAndEmptyHand()
{
    const Layout ff = MakeLayout(LayoutKind::Forefront);
    const BodyFrame f = Frame(ff);
    // (b) an empty hand at Pavlov's top pocket row takes the grenade; at the old
    // collar-bone place (HMD - 0.20, where the gun hand aims) nothing.
    CHECK(FindZone(f, ff, ToWorld(f, Vector(0.05f, 0.15f, -0.31f)), 0x1Fu) == Zone::Throwable);
    CHECK(FindZone(f, ff, ToWorld(f, Vector(0.07f, 0.10f, -0.20f)), 0x1Fu) == Zone::None);
    // (a) the item hand holding slot1 squeezes at the new and the old grenade place: no draw.
    for (const Vector& local : { Vector(0.05f, 0.15f, -0.31f), Vector(0.07f, 0.10f, -0.20f) })
    {
        GrabFsm fsm;
        GrabInput in{};
        in.ready = true;
        in.activeSlot = 0;
        in.occupiedMask = 0x1Fu;
        in.handZone = FindZone(f, ff, ToWorld(f, local), 0x1Fu & ~1u);
        in.handEmpty = false;
        fsm.Update(in);
        in.gripDown = true;
        const GrabOutput out = fsm.Update(in);
        CHECK(out.selectSlot == -1);
    }
    // (c) an empty hand (the free hand, or the item hand after putting away) draws.
    {
        GrabFsm fsm;
        GrabInput in{};
        in.ready = true;
        in.activeSlot = -1;
        in.occupiedMask = 0x1Fu;
        in.handZone = Zone::Throwable;
        in.handEmpty = true;
        fsm.Update(in);
        in.gripDown = true;
        CHECK(fsm.Update(in).selectSlot == 2);
    }
    // A full hand still puts its own item away at its own holster.
    {
        GrabFsm fsm;
        GrabInput in{};
        in.ready = true;
        in.activeSlot = 1;
        in.occupiedMask = 0x1Fu;
        in.handEmpty = false;
        in.ownZoneUnderHand = Zone::Secondary;
        fsm.Update(in);
        in.gripDown = true;
        fsm.Update(in);
        in.gripDown = false;
        const GrabOutput out = fsm.Update(in);
        CHECK(out.putAwaySlot == 1 && out.selectSlot == -1);
    }
}

static void TestNearestWithHysteresis()
{
    // Two pockets 10 cm apart (a synthetic layout): nearest wins, the hovered
    // one keeps winning within 2 cm.
    Layout l = MakeLayout(LayoutKind::Forefront);
    l.kind[4] = ZoneKind::Pocket;
    l.socket[2] = Vector(0.10f, 0.05f, 0.30f);
    l.socket[4] = Vector(0.10f, -0.05f, 0.30f);
    l.magPouch = Vector(0.5f, 0.0f, 1.0f);   // out of the way
    const BodyFrame f = Frame(l);
    auto at = [&](float dom) { return RootToWorld(f, Vector(0.10f, dom, 0.30f)); };
    CHECK(FindZone(f, l, at(-0.005f), 0x1Fu) == Zone::Consumable);          // nearer to slot5
    CHECK(FindZone(f, l, at(-0.005f), 0x1Fu, nullptr, nullptr, nullptr, Zone::Throwable) == Zone::Throwable);
    CHECK(FindZone(f, l, at(-0.03f), 0x1Fu, nullptr, nullptr, nullptr, Zone::Throwable) == Zone::Consumable);
    CHECK(FindZone(f, l, at(0.005f), 0x1Fu, nullptr, nullptr, nullptr, Zone::Consumable) == Zone::Consumable);
    // A hovered zone the hand left entirely is not kept.
    CHECK(FindZone(f, l, ToWorld(f, Vector(0.6f, 0.0f, 0.0f)), 0x1Fu, nullptr, nullptr, nullptr, Zone::Throwable) == Zone::None);
}

static void TestReplayS12Presses()
{
    // The 2.0.11 layout (grenade at HMD - 0.20, reach 0.15) against 2.0.12.
    Layout old = MakeLayout(LayoutKind::Forefront);
    old.socket[2] = Vector(0.07f, 0.10f, 0.40f);
    old.pocketReachMeters = 0.15f;
    old.magPouch = Vector(0.07f, -0.10f, 0.32f);
    const Layout now = MakeLayout(LayoutKind::Forefront);
    int heldPresses = 0, oldSlot3 = 0, newSlot3 = 0, draws = 0;
    for (const Press& p : kPresses)
    {
        if (p.left || p.active < 0)
            continue;   // the item hand holding an item
        ++heldPresses;
        const BodyFrame fo = Frame(old), fn = Frame(now);
        const Vector handO = ToWorld(fo, Vector(p.fwd, p.dom, p.up));
        const Vector handN = ToWorld(fn, Vector(p.fwd, p.dom, p.up));
        const std::uint32_t mask = 0x1Fu & ~(1u << p.active);
        if (FindZone(fo, old, handO, mask) == Zone::Throwable) ++oldSlot3;
        const Zone zn = FindZone(fn, now, handN, mask);
        if (zn == Zone::Throwable) ++newSlot3;
        // The rule: a hand that holds an item never draws.
        GrabFsm fsm;
        GrabInput in{};
        in.ready = true;
        in.activeSlot = p.active;
        in.occupiedMask = 0x1Fu;
        in.handZone = zn;
        in.handEmpty = false;
        fsm.Update(in);
        in.gripDown = true;
        if (fsm.Update(in).selectSlot >= 0)
            ++draws;
    }
    std::printf("s12 replay: %d item-hand presses with an item held; slot3 under the hand: 2.0.11 %d, 2.0.12 %d; draws %d\n",
        heldPresses, oldSlot3, newSlot3, draws);
    CHECK(heldPresses > 150);
    CHECK(draws == 0);
    CHECK(oldSlot3 >= 10);              // the log's ten grenade draws (and more presses at the face)
    CHECK(newSlot3 * 2 <= oldSlot3);    // the comfort move alone halves them; the rule removes the rest
}

int main()
{
    TestLayoutTables();
    TestEveryZoneReachable();
    TestGrenadeHeightAndEmptyHand();
    TestNearestWithHysteresis();
    TestReplayS12Presses();
    if (g_failures)
    {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("vr_holster_layout tests passed (%d checks)\n", g_checks);
    return 0;
}

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
#include <cstdio>
#include <string>
#include <cstdlib>

using namespace l4d2vr_holster;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

static bool Near(float a, float b, float eps) { return std::fabs(a - b) <= eps; }

static void TestYawDeadband()
{
    float yaw = SmoothBodyYaw(0.0f, false, 30.0f, 0.0f, 0.016f, 20.0f, 0.25f);
    CHECK(Near(yaw, 30.0f, 0.001f)); // first sample snaps
    yaw = SmoothBodyYaw(30.0f, true, 45.0f, 0.0f, 0.016f, 20.0f, 0.25f);
    CHECK(Near(yaw, 30.0f, 0.001f)); // inside the dead band
    float y = 0.0f;
    for (int i = 0; i < 200; ++i)
        y = SmoothBodyYaw(y, true, 90.0f, 0.0f, 0.016f, 20.0f, 0.25f);
    CHECK(Near(y, 70.0f, 0.5f)); // settles at the dead band edge
    // Turning applies instantly.
    y = SmoothBodyYaw(10.0f, true, 100.0f, 90.0f, 0.016f, 20.0f, 0.25f);
    CHECK(Near(y, 100.0f, 0.001f));
    // Wraparound.
    y = SmoothBodyYaw(170.0f, true, -175.0f, 0.0f, 0.016f, 20.0f, 0.25f);
    CHECK(Near(y, 170.0f, 0.001f));
}

static BodyFrame Frame(bool leftHanded = false)
{
    return MakeFrame(Vector(0.0f, 0.0f, 64.0f), 0.0f, leftHanded, 40.0f, 1.0f);
}

static void TestZones()
{
    const BodyFrame frame = Frame();
    const Layout layout{};
    // Right hand (dominant = -y in Source for yaw 0) at the hip.
    const Vector hip = ToWorld(frame, layout.center[1]);
    CHECK(hip.y < 0.0f);
    CHECK(FindZone(frame, layout, hip, 0x1Fu) == Zone::Secondary);
    CHECK(FindZone(frame, layout, hip, 0x1Du) == Zone::None); // secondary empty
    // Shoulder zone needs the hand behind the head plane.
    const Vector shoulder = ToWorld(frame, layout.center[0]);
    CHECK(FindZone(frame, layout, shoulder, 0x1Fu) == Zone::Primary);
    const Vector inFront = ToWorld(frame, Vector(0.12f, 0.16f, -0.12f));
    float distance = 0.0f;
    CHECK(!HandFitsZone(frame, layout, Zone::Primary, inFront, distance));
    // Chest items are separate.
    CHECK(FindZone(frame, layout, ToWorld(frame, layout.center[2]), 0x1Fu) == Zone::Throwable);
    CHECK(FindZone(frame, layout, ToWorld(frame, layout.center[4]), 0x1Fu) == Zone::Consumable);
    // Left-handed mirrors the hip to +y.
    const BodyFrame left = Frame(true);
    CHECK(ToWorld(left, layout.center[1]).y > 0.0f);
}

static void TestItemPoseOrthonormal()
{
    const BodyFrame frame = Frame();
    const Layout layout{};
    for (int shape = 0; shape < 4; ++shape)
        for (int zone = 0; zone < kZoneCount; ++zone)
        {
            const ItemPose pose = BuildItemPose(frame, layout, static_cast<Zone>(zone), static_cast<ItemShape>(shape));
            CHECK(pose.valid);
            CHECK(Near(DotProduct(pose.axisX, pose.axisX), 1.0f, 1e-3f));
            CHECK(Near(DotProduct(pose.axisY, pose.axisY), 1.0f, 1e-3f));
            CHECK(Near(DotProduct(pose.axisZ, pose.axisZ), 1.0f, 1e-3f));
            CHECK(Near(DotProduct(pose.axisX, pose.axisY), 0.0f, 1e-3f));
            CHECK(Near(DotProduct(pose.axisY, pose.axisZ), 0.0f, 1e-3f));
            Vector cross{};
            CrossProduct(pose.axisX, pose.axisY, cross);
            CHECK(DotProduct(cross, pose.axisZ) > 0.99f); // right-handed
        }
    const ItemPose pistol = BuildItemPose(frame, layout, Zone::Secondary, ItemShape::Pistol);
    CHECK(pistol.axisZ.z < -0.99f); // muzzle down
}

static void TestGrabFsm()
{
    GrabFsm fsm;
    GrabInput in{};
    in.ready = true;
    in.activeSlot = 0;
    in.occupiedMask = 0x07u;
    in.handZone = Zone::Secondary;
    GrabOutput out = fsm.Update(in);
    CHECK(out.hoverEntered == Zone::Secondary);
    in.gripDown = true;
    out = fsm.Update(in);
    CHECK(out.selectSlot == 1);
    CHECK(out.drawing);
    // Moving away and releasing keeps the item.
    in.activeSlot = 1;
    in.handZone = Zone::None;
    in.ownZoneUnderHand = Zone::None;
    out = fsm.Update(in);
    in.gripDown = false;
    out = fsm.Update(in);
    CHECK(out.holsterTo == -1);
    // Grip again away from the body, release inside the hip: holster -> primary.
    in.gripDown = true;
    out = fsm.Update(in);
    CHECK(out.selectSlot == -1);
    in.gripDown = false;
    in.ownZoneUnderHand = Zone::Secondary;
    out = fsm.Update(in);
    CHECK(out.holsterTo == 0);
    // Not ready resets.
    in.ready = false;
    in.gripDown = true;
    out = fsm.Update(in);
    CHECK(out.selectSlot == -1 && !fsm.Holding());
}

static void TestNextSlot()
{
    CHECK(GrabFsm::NextSlotAfterHolster(0, 0x03u) == 1);
    CHECK(GrabFsm::NextSlotAfterHolster(1, 0x03u) == 0);
    CHECK(GrabFsm::NextSlotAfterHolster(1, 0x02u) == -1);
    CHECK(GrabFsm::NextSlotAfterHolster(2, 0x03u) == 1);
}

static void TestWeaponClasses()
{
    CHECK(std::string(l4d2vr_holster::WeaponClassForId(1)) == "weapon_pistol");
    CHECK(std::string(l4d2vr_holster::WeaponClassForId(14)) == "weapon_pipe_bomb");
    CHECK(std::string(l4d2vr_holster::WeaponClassForId(19)) == "weapon_melee");
    CHECK(std::string(l4d2vr_holster::WeaponClassForId(26)) == "weapon_rifle_ak47");
    CHECK(l4d2vr_holster::WeaponClassForId(16) == nullptr);   // gas can is carried, not slotted
    CHECK(l4d2vr_holster::WeaponClassForId(0) == nullptr);
}

int main()
{
    TestWeaponClasses();
    TestYawDeadband();
    TestZones();
    TestItemPoseOrthonormal();
    TestGrabFsm();
    TestNextSlot();
    if (g_failures)
    {
        std::printf("%d failure(s)\n", g_failures);
        return EXIT_FAILURE;
    }
    std::printf("vr_holster tests passed\n");
    return EXIT_SUCCESS;
}

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
#include "vr_holster_control_ready_source.inl"
#include <cstdio>
#include <string>
#include <cstdlib>

using namespace l4d2vr_holster;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

static bool Near(float a, float b, float eps) { return std::fabs(a - b) <= eps; }

static void TestYawFollow()
{
    // Pavlov: the body yaw interpolates toward the head yaw (speed 20), no dead band.
    float yaw = FollowBodyYaw(0.0f, false, 30.0f, 0.0f, 0.016f, 20.0f);
    CHECK(Near(yaw, 30.0f, 0.001f)); // first sample snaps
    yaw = FollowBodyYaw(30.0f, true, 45.0f, 0.0f, 0.016f, 20.0f);
    CHECK(yaw > 30.0f && yaw < 45.0f); // a small glance already moves it
    float y = 0.0f;
    for (int i = 0; i < 60; ++i)
        y = FollowBodyYaw(y, true, 90.0f, 0.0f, 0.016f, 20.0f);
    CHECK(Near(y, 90.0f, 0.5f)); // settles on the head yaw within a second
    // Turning applies instantly.
    y = FollowBodyYaw(10.0f, true, 100.0f, 90.0f, 0.016f, 20.0f);
    CHECK(Near(y, 100.0f, 0.001f));
    // Wraparound takes the short way.
    y = FollowBodyYaw(170.0f, true, -170.0f, 0.0f, 0.016f, 20.0f);
    CHECK(y > 170.0f || y < -170.0f);
}

static BodyFrame Frame(bool leftHanded = false)
{
    BodyFrame frame = MakeFrame(Vector(0.0f, 0.0f, 64.0f), 0.0f, leftHanded, 40.0f, 1.0f);
    // Level head looking along +x; Source right is -y.
    ApplyPavlovRoot(frame, Layout{}, Vector(1.0f, 0.0f, 0.0f), Vector(0.0f, -1.0f, 0.0f),
        Vector(0.0f, 0.0f, 1.0f), leftHanded, -1.0f);
    return frame;
}

static void TestZones()
{
    const BodyFrame frame = Frame();
    const Layout layout{};
    // Hands at real body positions (metres from the head: forward, toward the
    // dominant hand, up), not at our own zone centres.
    const Vector atHip = ToWorld(frame, Vector(-0.05f, 0.22f, -0.80f));
    const Vector relaxedAtSide = ToWorld(frame, Vector(0.0f, 0.25f, -0.75f));
    const Vector overShoulder = ToWorld(frame, Vector(-0.10f, 0.15f, -0.05f));
    const Vector gunReady = ToWorld(frame, Vector(0.35f, 0.20f, -0.30f));
    CHECK(atHip.y < 0.0f); // dominant (right) side is -y
    CHECK(FindZone(frame, layout, atHip, 0x1Fu) == Zone::Secondary);
    CHECK(FindZone(frame, layout, relaxedAtSide, 0x1Fu) == Zone::Secondary);
    CHECK(FindZone(frame, layout, atHip, 0x1Du) == Zone::None); // secondary empty
    CHECK(FindZone(frame, layout, overShoulder, 0x1Fu) == Zone::Primary);
    CHECK(FindZone(frame, layout, gunReady, 0x1Fu) == Zone::None);
    // Chest pockets: reach for the pocketed item.
    CHECK(FindZone(frame, layout, ZoneCenterWorld(frame, layout, Zone::Throwable), 0x1Fu) == Zone::Throwable);
    CHECK(FindZone(frame, layout, ZoneCenterWorld(frame, layout, Zone::Consumable), 0x1Fu) == Zone::Consumable);
    CHECK(FindZone(frame, layout, ZoneCenterWorld(frame, layout, Zone::Medical), 0x1Fu) == Zone::Medical);
    // Putting a chest item back: anywhere in the vest.
    CHECK(HandInHolsterVolume(frame, layout, Zone::Consumable, ToWorld(frame, Vector(0.10f, 0.0f, -0.45f))));
    CHECK(!HandInHolsterVolume(frame, layout, Zone::Consumable, gunReady + Vector(20.0f, 0.0f, 0.0f)));
    // Left-handed mirrors the hip to +y.
    const BodyFrame left = Frame(true);
    CHECK(ZoneCenterWorld(left, layout, Zone::Secondary).y > 0.0f);
}

static void TestPavlovRoot()
{
    const Layout layout{};
    BodyFrame frame = MakeFrame(Vector(0.0f, 0.0f, 100.0f), 0.0f, false, 40.0f, 1.0f);
    ApplyPavlovRoot(frame, layout, Vector(1.0f, 0.0f, 0.0f), Vector(0.0f, -1.0f, 0.0f), Vector(0.0f, 0.0f, 1.0f), false, -1.0f);
    CHECK(Near(frame.root.z, 100.0f - 0.75f * 40.0f, 0.01f)); // 75 cm below the head
    CHECK(Near(frame.root.x, 0.0f, 0.01f));
    // Looking straight down pulls it back 8 cm.
    BodyFrame down = MakeFrame(Vector(0.0f, 0.0f, 100.0f), 0.0f, false, 40.0f, 1.0f);
    ApplyPavlovRoot(down, layout, Vector(0.001f, 0.0f, -1.0f), Vector(0.0f, -1.0f, 0.0f), Vector(1.0f, 0.0f, 0.0f), false, -1.0f);
    CHECK(Near(down.root.x, -0.08f * 40.0f, 0.2f));
    // The off hand at the floor pushes it 60 cm back; above 25 cm, not at all.
    BodyFrame reach = MakeFrame(Vector(0.0f, 0.0f, 100.0f), 0.0f, false, 40.0f, 1.0f);
    ApplyPavlovRoot(reach, layout, Vector(1.0f, 0.0f, 0.0f), Vector(0.0f, -1.0f, 0.0f), Vector(0.0f, 0.0f, 1.0f), false, 0.0f);
    CHECK(Near(reach.root.x, -0.60f * 40.0f, 0.01f));
    CHECK(Near(ReachFloorBackFraction(0.171f), 26.5f / 60.0f, 0.01f));
    CHECK(ReachFloorBackFraction(0.30f) == 0.0f);
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

static void TestDiagPacer()
{
    DiagPacer pacer;
    CHECK(pacer.Due(10000u, 1u, 2000u, 5000u));   // first call reports
    CHECK(!pacer.Due(10500u, 1u, 2000u, 5000u));  // same state, too soon
    CHECK(!pacer.Due(11000u, 2u, 2000u, 5000u));  // changed, but under the minimum interval
    CHECK(pacer.Due(12000u, 2u, 2000u, 5000u));   // changed after the minimum interval
    CHECK(!pacer.Due(16000u, 2u, 2000u, 5000u));  // unchanged, period not reached
    CHECK(pacer.Due(17000u, 2u, 2000u, 5000u));   // heartbeat
    // A disabled heartbeat still reports state changes.
    DiagPacer changesOnly;
    CHECK(changesOnly.Due(0u, 7u, 2000u, ~0ull));
    CHECK(!changesOnly.Due(1000000u, 7u, 2000u, ~0ull));
    CHECK(changesOnly.Due(1000001u, 8u, 2000u, ~0ull));
}

static void TestControlReadyUsesCameraReport()
{
    // Regression: with FirstPersonBodyEnabled=false the body lifecycle clears
    // g_FirstPersonBodyActualFirstPerson each CreateMove just before the
    // readiness check, so readiness (and with it the holster snapshot, the
    // weapon SetupBones hook and holster grabs) never became true.
    const std::string source(kControlReadySource);
    CHECK(source.find("g_FirstPersonCameraActualFirstPerson") != std::string::npos);
    CHECK(source.find("g_FirstPersonBodyActualFirstPerson") == std::string::npos);
}

static void TestHolsterPlacementNearHead()
{
    // Default layout at VRScale 43.2: every item rests on its vest socket, on the
    // body below the head.
    BodyFrame frame = MakeFrame(Vector(100.0f, 200.0f, 64.0f), 90.0f, false, 43.2f, 1.0f);
    ApplyPavlovRoot(frame, Layout{}, Vector(0.0f, 1.0f, 0.0f), Vector(1.0f, 0.0f, 0.0f), Vector(0.0f, 0.0f, 1.0f), false, -1.0f);
    const Layout layout{};
    for (int zone = 0; zone < kZoneCount; ++zone)
    {
        const ItemShape shape = zone == 0 ? ItemShape::LongGun : (zone == 1 ? ItemShape::Pistol : ItemShape::Equipment);
        const ItemPose pose = BuildItemPose(frame, layout, static_cast<Zone>(zone), shape);
        CHECK(pose.valid);
        const Vector centerUnits = ReferenceToCenterUnits(shape);
        const Vector center = pose.origin + pose.axisX * centerUnits.x + pose.axisY * centerUnits.y +
            pose.axisZ * centerUnits.z;
        const Vector socket = RootToWorld(frame, layout.socket[zone]);
        CHECK(Near(center.x, socket.x, 0.01f) && Near(center.y, socket.y, 0.01f) && Near(center.z, socket.z, 0.01f));
        const Vector d = center - frame.head;
        const float meters = std::sqrt(DotProduct(d, d)) / frame.unitsPerMeter;
        CHECK(meters > 0.25f && meters < 0.7f);
        CHECK(center.z < frame.head.z);
    }
    // Hip (slot 2) sits on the dominant (right) side: yaw 90 faces +y, right is +x.
    const ItemPose hip = BuildItemPose(frame, layout, Zone::Secondary, ItemShape::Pistol);
    CHECK(hip.origin.x > frame.head.x);
    // Chest items stand upright and in front of the back plane.
    const ItemPose pills = BuildItemPose(frame, layout, Zone::Consumable, ItemShape::Equipment);
    CHECK(pills.axisY.z > 0.99f);
    CHECK(ToLocal(frame, RootToWorld(frame, layout.socket[4])).x > 0.0f);
    // Only the long gun's slot is on the back.
    CHECK(IsBackZone(Zone::Primary) && !IsBackZone(Zone::Medical));
}

int main()
{
    TestWeaponClasses();
    TestDiagPacer();
    TestControlReadyUsesCameraReport();
    TestHolsterPlacementNearHead();
    TestYawFollow();
    TestPavlovRoot();
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

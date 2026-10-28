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
    // Default layout at VRScale 43.2: every holster is within reach of the head
    // and on the expected side, i.e. drawn on the body rather than far away.
    const BodyFrame frame = MakeFrame(Vector(100.0f, 200.0f, 64.0f), 90.0f, false, 43.2f, 1.0f);
    const Layout layout{};
    for (int zone = 0; zone < kZoneCount; ++zone)
    {
        const ItemPose pose = BuildItemPose(frame, layout, static_cast<Zone>(zone), ItemShape::LongGun);
        CHECK(pose.valid);
        const Vector d = pose.origin - frame.head;
        const float meters = std::sqrt(DotProduct(d, d)) / frame.unitsPerMeter;
        CHECK(meters > 0.1f && meters < 0.9f);
        CHECK(pose.origin.z < frame.head.z); // below the head
        // The item's visible centre (reference bone + centre offset) is on the draw centre.
        const Vector centerUnits = ReferenceToCenterUnits(ItemShape::LongGun);
        const Vector center = pose.origin + pose.axisX * centerUnits.x + pose.axisY * centerUnits.y +
            pose.axisZ * centerUnits.z;
        const Vector local = ToLocal(frame, center);
        CHECK(Near(local.x, layout.drawCenter[zone].x, 0.001f));
        CHECK(Near(local.y, layout.drawCenter[zone].y, 0.001f));
        CHECK(Near(local.z, layout.drawCenter[zone].z, 0.001f));
    }
    // Hip (slot 2) sits on the dominant (right) side: yaw 90 faces +y, right is +x.
    const ItemPose hip = BuildItemPose(frame, layout, Zone::Secondary, ItemShape::Pistol);
    CHECK(hip.origin.x > frame.head.x);
}

// Where real hands go, from a standing-adult body model (eyes ~1.65 m),
// independent of the layout under test: metres from the eyes, x forward,
// y toward the right (dominant) hand, z up.
struct Reach { const char* name; Vector point; Zone expected; };
static const Reach kReaches[] = {
    { "right hip", Vector(-0.05f, 0.22f, -0.80f), Zone::Secondary },
    { "right hip, high belt", Vector(-0.03f, 0.20f, -0.72f), Zone::Secondary },
    { "right hip, low belt", Vector(-0.06f, 0.24f, -0.88f), Zone::Secondary },
    { "right hip, slightly forward", Vector(0.04f, 0.21f, -0.78f), Zone::Secondary },
    { "over the right shoulder", Vector(-0.10f, 0.15f, -0.05f), Zone::Primary },
    { "over the right shoulder, low", Vector(-0.12f, 0.17f, -0.22f), Zone::Primary },
    { "over the right shoulder, high", Vector(-0.06f, 0.12f, 0.05f), Zone::Primary },
    { "over the left shoulder", Vector(-0.10f, -0.14f, -0.06f), Zone::Medical },
    { "right chest pouch", Vector(0.12f, 0.10f, -0.35f), Zone::Consumable },
    { "right chest pouch, low", Vector(0.14f, 0.11f, -0.40f), Zone::Consumable },
    { "left chest pouch", Vector(0.12f, -0.10f, -0.35f), Zone::Throwable },
    { "left chest pouch, high", Vector(0.10f, -0.09f, -0.30f), Zone::Throwable },
};
// Hands that are NOT reaching for a holster.
static const Vector kNotReaches[] = {
    Vector(0.40f, 0.15f, -0.35f),  // gun ready
    Vector(0.45f, 0.20f, -0.30f),  // aiming at arm's length
    Vector(0.25f, 0.06f, -0.12f),  // aiming down the sights
    Vector(0.30f, 0.15f, -0.25f),  // close ready
    Vector(0.35f, 0.25f, -0.60f),  // low ready
    Vector(0.15f, 0.30f, -0.10f),  // gun raised beside the head (in front of the shoulder plane)
};

static void TestAnthropometricReaches()
{
    const Layout layout{};
    const float scale = 43.2f;
    const Vector eyes(100.0f, 200.0f, 1.65f * scale);
    const BodyFrame truth = MakeFrame(eyes, 0.0f, false, scale, 1.0f);
    // The tracked body yaw lags or leads the torso by up to the dead band
    // (glancing at the hip or the shoulder): the reaches must still land.
    for (const float yawError : { -30.0f, -20.0f, 0.0f, 20.0f, 30.0f })
    {
        const BodyFrame frame = MakeFrame(eyes, yawError, false, scale, 1.0f);
        for (const Reach& reach : kReaches)
        {
            const Vector hand = ToWorld(truth, reach.point);
            const Zone zone = FindZone(frame, layout, hand, 0x1Fu);
            if (zone != reach.expected)
                std::printf("  reach '%s' with yaw error %.0f -> zone %d (expected %d)\n", reach.name, yawError,
                    static_cast<int>(zone), static_cast<int>(reach.expected));
            CHECK(zone == reach.expected);
        }
        for (const Vector& point : kNotReaches)
        {
            const Zone zone = FindZone(frame, layout, ToWorld(truth, point), 0x1Fu);
            if (zone != Zone::None)
                std::printf("  hand at (%.2f %.2f %.2f) with yaw error %.0f -> zone %d\n", point.x, point.y, point.z,
                    yawError, static_cast<int>(zone));
            CHECK(zone == Zone::None);
        }
    }
}

static void TestNeutralHeadOffset()
{
    const Vector bodyForward(1.0f, 0.0f, 0.0f);
    // Level head: no correction.
    Vector offset = NeutralHeadOffset(Vector(1.0f, 0.0f, 0.0f), Vector(0.0f, 0.0f, 1.0f), bodyForward, 1.0f);
    CHECK(Near(offset.x, 0.0f, 1e-4f) && Near(offset.y, 0.0f, 1e-4f) && Near(offset.z, 0.0f, 1e-4f));
    // Looking 60 degrees down at the hip: the eyes dropped ~0.13 m and moved
    // forward ~0.04 m; the correction restores the level-head position.
    const float p = 60.0f * kPi / 180.0f;
    offset = NeutralHeadOffset(Vector(std::cos(p), 0.0f, -std::sin(p)), Vector(std::sin(p), 0.0f, std::cos(p)),
        bodyForward, 1.0f);
    CHECK(offset.z > 0.10f && offset.z < 0.16f);
    CHECK(offset.x < -0.02f && offset.x > -0.08f);
    // A right hip reach while looking down still lands in the hip zone.
    const Layout layout{};
    const Vector eyesLevel(0.0f, 0.0f, 1.65f);
    const Vector eyesDown = eyesLevel - offset; // tracked eyes after the head pitched down
    const BodyFrame looking = MakeFrame(eyesDown + offset, 0.0f, false, 1.0f, 1.0f);
    const BodyFrame truth = MakeFrame(eyesLevel, 0.0f, false, 1.0f, 1.0f);
    CHECK(FindZone(looking, layout, ToWorld(truth, Vector(-0.05f, 0.22f, -0.80f)), 0x1Fu) == Zone::Secondary);
    // Garbage in, no correction.
    offset = NeutralHeadOffset(Vector(0.0f, 0.0f, 0.0f), Vector(0.0f, 0.0f, 1.0f), bodyForward, 1.0f);
    CHECK(Near(offset.z, 0.0f, 1e-6f));
}

static void TestHeadYawLookingDown()
{
    // Looking 85 degrees down with a little roll: the yaw still faces forward.
    const float p = 85.0f * kPi / 180.0f;
    const Vector forward(std::cos(p), 0.02f, -std::sin(p));
    const Vector up(std::sin(p), 0.0f, std::cos(p));
    CHECK(std::fabs(HeadYawDegrees(forward, up, 0.0f)) < 3.0f);
    // Looking up does not flip the yaw.
    CHECK(std::fabs(HeadYawDegrees(Vector(0.5f, 0.0f, 0.866f), Vector(-0.866f, 0.0f, 0.5f), 0.0f)) < 0.01f);
    CHECK(Near(HeadYawDegrees(Vector(0.0f, 1.0f, 0.0f), Vector(0.0f, 0.0f, 1.0f), 0.0f), 90.0f, 0.01f));
}

static void TestLegacyMigration()
{
    const Layout defaults{};
    for (int zone = 0; zone < kZoneCount; ++zone)
    {
        Vector center = LegacyCenter(zone);
        float radius = LegacyRadius(zone);
        MigrateLegacyZone(zone, defaults, center, radius);
        CHECK(NearlyEqual(center, defaults.center[zone]));
        CHECK(Near(radius, defaults.radius[zone], 1e-6f));
    }
    // A player's own tuning is kept.
    Vector custom(0.05f, 0.25f, -0.75f);
    float customRadius = 0.22f;
    MigrateLegacyZone(1, defaults, custom, customRadius);
    CHECK(NearlyEqual(custom, Vector(0.05f, 0.25f, -0.75f)) && Near(customRadius, 0.22f, 1e-6f));
}

static void TestHeadsetSessionPresses()
{
    // headset-session3: every right-grip press logged the chest pouch (slot 5)
    // as the nearest zone centre at 0.25-0.37 m. With the old layout that is a
    // hand held out in front (gun ready), whatever the body yaw; a reach to
    // the hip, shoulder or chest would have been within ~0.2 m of its zone.
    const Vector oldChest = LegacyCenter(4);
    const Vector ready(0.40f, 0.15f, -0.35f);
    const Vector d = ready - oldChest;
    const float distance = std::sqrt(DotProduct(d, d));
    CHECK(distance > 0.24f && distance < 0.30f);
    const Layout layout{};
    const BodyFrame frame = MakeFrame(Vector(0.0f, 0.0f, 0.0f), 0.0f, false, 1.0f, 1.0f);
    CHECK(FindZone(frame, layout, ToWorld(frame, ready), 0x1Fu) == Zone::None);
}

static void TestBackItemsOutOfForwardView()
{
    // Back items hang behind and below the head; hip and chest items are drawn
    // in their grab zones.
    const BodyFrame frame = MakeFrame(Vector(0.0f, 0.0f, 64.0f), 0.0f, false, 43.2f, 1.0f);
    const Layout layout{};
    for (int zone = 0; zone < kZoneCount; ++zone)
    {
        const bool back = IsBackZone(static_cast<Zone>(zone));
        const Vector& draw = layout.drawCenter[zone];
        if (back)
            CHECK(draw.x <= -0.20f && draw.z <= -0.25f && draw.z >= -0.45f);
        else
            CHECK(Near(draw.x, layout.center[zone].x, 1e-4f) && Near(draw.y, layout.center[zone].y, 1e-4f) &&
                Near(draw.z, layout.center[zone].z, 1e-4f));
    }
    // A slung rifle (about 50 units, 15 behind the bone and 35 ahead of it)
    // stays behind the eyes along its whole length.
    const ItemPose rifle = BuildItemPose(frame, layout, Zone::Primary, ItemShape::LongGun);
    for (float along = -15.0f; along <= 35.0f; along += 5.0f)
        CHECK(ToLocal(frame, rifle.origin + rifle.axisZ * along).x < -0.15f);
    CHECK(rifle.axisY.x < -0.99f); // sights face away from the back
    // A kit on the back lies flat against it: its thin local X runs front to back.
    const ItemPose kit = BuildItemPose(frame, layout, Zone::Medical, ItemShape::Equipment);
    CHECK(std::fabs(kit.axisX.x) > 0.99f && kit.axisY.z > 0.99f);
    CHECK(ToLocal(frame, kit.origin).x < -0.2f);
    // Chest items stand upright.
    const ItemPose pills = BuildItemPose(frame, layout, Zone::Consumable, ItemShape::Equipment);
    CHECK(pills.axisY.z > 0.99f);
}

int main()
{
    TestAnthropometricReaches();
    TestNeutralHeadOffset();
    TestHeadYawLookingDown();
    TestLegacyMigration();
    TestHeadsetSessionPresses();
    TestWeaponClasses();
    TestDiagPacer();
    TestControlReadyUsesCameraReport();
    TestHolsterPlacementNearHead();
    TestBackItemsOutOfForwardView();
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

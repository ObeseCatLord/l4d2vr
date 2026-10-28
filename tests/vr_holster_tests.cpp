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
    // The kit is on the back now, over the off shoulder: never at the belly.
    CHECK(FindZone(frame, layout, ZoneCenterWorld(frame, layout, Zone::Medical), 0x1Fu) == Zone::Medical);
    CHECK(FindZone(frame, layout, ToWorld(frame, Vector(0.08f, 0.0f, -0.51f)), 0x1Fu) == Zone::None);
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
    for (int shape = 0; shape < kItemShapeCount; ++shape)
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
    CHECK(pistol.axisZ.z < -0.9f); // muzzle down (canted 23 degrees forward, as in Pavlov)
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
        const ItemShape shape = zone == 0 ? ItemShape::LongGun : (zone == 1 ? ItemShape::Pistol :
            (zone == 3 ? ItemShape::Kit : ItemShape::Equipment));
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
    // The long gun and the kit are on the back; the hip and pockets are not.
    CHECK(IsBackZone(Zone::Primary) && IsBackZone(Zone::Medical));
    CHECK(!IsBackZone(Zone::Secondary) && !IsBackZone(Zone::Throwable) && !IsBackZone(Zone::Consumable));
}

static void TestNearAnyZone()
{
    // Gun bashes are ignored at the holsters: in or near any zone; a hand held
    // out in front (gun ready, end of a push) is not near one.
    const Layout layout{};
    BodyFrame frame = MakeFrame(Vector(0.0f, 0.0f, 64.0f), 0.0f, false, 43.2f, 1.0f);
    ApplyPavlovRoot(frame, layout, Vector(1.0f, 0.0f, 0.0f), Vector(0.0f, -1.0f, 0.0f), Vector(0.0f, 0.0f, 1.0f), false, -1.0f);
    for (int zone = 0; zone < kZoneCount; ++zone)
        CHECK(NearAnyZone(frame, layout, ZoneCenterWorld(frame, layout, static_cast<Zone>(zone)), 0.05f));
    CHECK(!NearAnyZone(frame, layout, ToWorld(frame, Vector(0.45f, 0.15f, -0.30f)), 0.05f)); // gun ready, arm out
    CHECK(!NearAnyZone(frame, layout, ToWorld(frame, Vector(0.65f, 0.15f, -0.30f)), 0.05f)); // end of a push
}

// ---- A realistic standing body -------------------------------------------
// Eyes 1.65 m above the floor at VRScale 43.2, facing an arbitrary yaw; the
// head can pitch (positive = down) and roll. Hand positions are where a real
// adult's hands go, in metres in the head-yaw frame toward the PHYSICAL right
// hand (the same points as the in-game self-test's body model), so they never
// borrow the layout's own numbers.
struct Body
{
    Vector head{};
    Vector fwd{};   // horizontal, head yaw
    Vector right{}; // horizontal, physical right
    BodyFrame frame{};
    float s = 43.2f;

    Vector At(float x, float y, float z) const
    {
        return head + fwd * (x * s) + right * (y * s) + Vector(0.0f, 0.0f, z * s);
    }
};

static Vector Rotate(const Vector& v, const Vector& axis, float degrees)
{
    // Rodrigues around a unit axis.
    const float r = degrees * kPi / 180.0f;
    const float c = std::cos(r), sn = std::sin(r);
    Vector k = axis;
    Vector kxv{};
    CrossProduct(k, v, kxv);
    return v * c + kxv * sn + k * (DotProduct(k, v) * (1.0f - c));
}

static Body MakeBody(float yawDeg, float pitchDownDeg, float rollDeg, bool leftHanded, float bodyYawOffsetDeg = 0.0f)
{
    Body body{};
    body.head = Vector(310.0f, -120.0f, 1.65f * body.s);
    const float y = yawDeg * kPi / 180.0f;
    body.fwd = Vector(std::cos(y), std::sin(y), 0.0f);
    body.right = Vector(body.fwd.y, -body.fwd.x, 0.0f);
    // The headset's own axes: pitch about the right axis, then roll about forward.
    // (Rotating about +right by a positive angle tips forward up, so negate.)
    Vector headForward = Rotate(body.fwd, body.right, -pitchDownDeg);
    Vector headUp = Rotate(Vector(0.0f, 0.0f, 1.0f), body.right, -pitchDownDeg);
    Vector headRight = body.right;
    headUp = Rotate(headUp, headForward, rollDeg);
    headRight = Rotate(headRight, headForward, rollDeg);
    body.frame = MakeFrame(body.head, yawDeg + bodyYawOffsetDeg, leftHanded, body.s, 1.0f);
    ApplyPavlovRoot(body.frame, Layout{}, headForward, headRight, headUp, leftHanded, 1.0f);
    return body;
}

// Physical hand targets (head-yaw frame, y toward the physical right hand).
static const Vector kOverRightShoulder(-0.10f, 0.15f, -0.05f);
static const Vector kOverLeftShoulder(-0.10f, -0.15f, -0.05f);
static const Vector kRightHip(-0.05f, 0.22f, -0.80f);
static const Vector kLeftHip(-0.05f, -0.22f, -0.80f);
static const Vector kRightChest(0.10f, 0.12f, -0.35f);
static const Vector kLeftChest(0.10f, -0.12f, -0.35f);
static const Vector kRightReady(0.40f, 0.15f, -0.35f);
static const Vector kLeftReady(0.32f, -0.18f, -0.42f);
static const Vector kMagPouchReach(0.02f, -0.20f, -0.55f); // off hand at the pouch (headset log)

static Vector Mirror(const Vector& v) { return Vector(v.x, -v.y, v.z); }

static void TestBackHalvesRealisticBody()
{
    const Layout layout{};
    for (int handed = 0; handed < 2; ++handed)
    {
        const bool leftHanded = handed == 1;
        // Physical side of the dominant shoulder / hip.
        const Vector domShoulder = leftHanded ? kOverLeftShoulder : kOverRightShoulder;
        const Vector offShoulder = leftHanded ? kOverRightShoulder : kOverLeftShoulder;
        const Vector domHip = leftHanded ? kLeftHip : kRightHip;
        const Vector offHip = leftHanded ? kRightHip : kLeftHip;
        const Vector domChest = leftHanded ? kLeftChest : kRightChest;
        const Vector offChest = leftHanded ? kRightChest : kLeftChest;
        const Vector pouch = leftHanded ? Mirror(kMagPouchReach) : kMagPouchReach;
        for (float yaw : { 0.0f, 37.0f, -150.0f })
        for (float pitch : { 0.0f, 35.0f, 60.0f, -25.0f, -40.0f })
        for (float roll : { 0.0f, 15.0f, -15.0f })
        {
            const Body body = MakeBody(yaw, pitch, roll, leftHanded);
            const BodyFrame& f = body.frame;
            CHECK(f.valid);
            CHECK((pitch > 0.0f) == (f.headForward.z < -0.1f)); // positive pitch looks down
            // Over the dominant shoulder: the long gun; over the off shoulder:
            // the kit, whichever hand reaches (the zones only see positions).
            CHECK(FindZone(f, layout, body.At(domShoulder.x, domShoulder.y, domShoulder.z), 0x1Fu) == Zone::Primary);
            CHECK(FindZone(f, layout, body.At(offShoulder.x, offShoulder.y, offShoulder.z), 0x1Fu) == Zone::Medical);
            // A hand held further back over each shoulder (fully reached behind).
            const Vector deepDom = domShoulder + Vector(-0.12f, 0.0f, -0.05f);
            const Vector deepOff = offShoulder + Vector(-0.12f, 0.0f, -0.05f);
            CHECK(FindZone(f, layout, body.At(deepDom.x, deepDom.y, deepDom.z), 0x1Fu) == Zone::Primary);
            CHECK(FindZone(f, layout, body.At(deepOff.x, deepOff.y, deepOff.z), 0x1Fu) == Zone::Medical);
            // An empty half takes nothing: reaching for the missing rifle never
            // draws the kit, and the other way round.
            CHECK(FindZone(f, layout, body.At(domShoulder.x, domShoulder.y, domShoulder.z), 0x1Fu & ~0x01u) == Zone::None);
            CHECK(FindZone(f, layout, body.At(offShoulder.x, offShoulder.y, offShoulder.z), 0x1Fu & ~0x08u) == Zone::None);
            // Hips and chest pockets.
            CHECK(FindZone(f, layout, body.At(domHip.x, domHip.y, domHip.z), 0x1Fu) == Zone::Secondary);
            CHECK(FindZone(f, layout, body.At(offChest.x, offChest.y, offChest.z), 0x1Fu) == Zone::Throwable);
            CHECK(FindZone(f, layout, body.At(domChest.x, domChest.y, domChest.z), 0x1Fu) == Zone::Consumable);
            // The off-hand hip is the magazine pouch: never a holster draw,
            // even looking up, where the back sphere swings down to it.
            CHECK(FindZone(f, layout, body.At(offHip.x, offHip.y, offHip.z), 0x1Fu) == Zone::None);
            CHECK(FindZone(f, layout, body.At(pouch.x, pouch.y, pouch.z), 0x1Fu) == Zone::None);
            CHECK(HandAtMagazinePouch(f, layout, body.At(pouch.x, pouch.y, pouch.z)));
            CHECK(!HandAtMagazinePouch(f, layout, body.At(domHip.x, domHip.y, domHip.z)));
            // Hands held out in front take nothing.
            CHECK(FindZone(f, layout, body.At(kRightReady.x, kRightReady.y, kRightReady.z), 0x1Fu) == Zone::None);
            CHECK(FindZone(f, layout, body.At(kLeftReady.x, kLeftReady.y, kLeftReady.z), 0x1Fu) == Zone::None);
            // Holding the rifle (not drawable): its own half takes nothing
            // (squeezing there puts it away), the kit half swaps to the kit.
            CHECK(FindZone(f, layout, body.At(domShoulder.x, domShoulder.y, domShoulder.z), 0x1Fu & ~0x01u) == Zone::None);
            CHECK(HandInHolsterVolume(f, layout, Zone::Primary, body.At(domShoulder.x, domShoulder.y, domShoulder.z), 0x1Fu));
            CHECK(!HandInHolsterVolume(f, layout, Zone::Primary, body.At(offShoulder.x, offShoulder.y, offShoulder.z), 0x1Fu));
            // ... and with no kit, the whole back takes the rifle back (no drop).
            CHECK(HandInHolsterVolume(f, layout, Zone::Primary, body.At(offShoulder.x, offShoulder.y, offShoulder.z), 0x1Fu & ~0x08u));
            CHECK(HandInHolsterVolume(f, layout, Zone::Medical, body.At(offShoulder.x, offShoulder.y, offShoulder.z), 0x1Fu));
            CHECK(!HandInHolsterVolume(f, layout, Zone::Medical, body.At(domShoulder.x, domShoulder.y, domShoulder.z), 0x1Fu));
            CHECK(HandInHolsterVolume(f, layout, Zone::Medical, body.At(domShoulder.x, domShoulder.y, domShoulder.z), 0x1Fu & ~0x01u));
            // Every reach counts as "near a holster" for the bash filter.
            for (const Vector& p : { domShoulder, offShoulder, domHip, domChest, offChest })
                CHECK(NearAnyZone(f, layout, body.At(p.x, p.y, p.z), 0.05f));
            CHECK(!NearAnyZone(f, layout, body.At(0.65f, 0.15f, -0.30f), 0.05f));
        }
        // A body yaw still catching up with a fast head turn (Pavlov's
        // interpolation): the back split follows the head, so a 30 degree
        // lag changes nothing.
        for (float lag : { 30.0f, -30.0f })
        {
            const Body body = MakeBody(20.0f, 20.0f, 0.0f, leftHanded, lag);
            CHECK(FindZone(body.frame, layout, body.At(domShoulder.x, domShoulder.y, domShoulder.z), 0x1Fu) == Zone::Primary);
            CHECK(FindZone(body.frame, layout, body.At(offShoulder.x, offShoulder.y, offShoulder.z), 0x1Fu) == Zone::Medical);
        }
    }
}

static void TestItemOrientationRealisticBody()
{
    const Layout layout{};
    for (int handed = 0; handed < 2; ++handed)
    {
        const bool leftHanded = handed == 1;
        const Body body = MakeBody(-63.0f, 0.0f, 0.0f, leftHanded);
        const BodyFrame& f = body.frame;
        const Vector up(0.0f, 0.0f, 1.0f);
        const Vector domSide = leftHanded ? body.right * -1.0f : body.right; // physical dominant side
        auto local = [&](const ItemPose& pose, ItemShape shape) {
            const Vector c = ReferenceToCenterUnits(shape);
            const Vector centre = pose.origin + pose.axisX * c.x + pose.axisY * c.y + pose.axisZ * c.z;
            const Vector d = centre - body.head;
            return Vector(DotProduct(d, body.fwd) / body.s, DotProduct(d, domSide) / body.s, d.z / body.s);
        };
        // First aid kit: strapped upright on the off shoulder blade, its face
        // (the cross, +X) out of the back. Never in front, never sideways.
        for (ItemShape shape : { ItemShape::Kit, ItemShape::Defib })
        {
            const ItemPose kit = BuildItemPose(f, layout, Zone::Medical, shape);
            CHECK(kit.valid);
            CHECK(kit.axisY.z > 0.99f);                          // upright
            CHECK(DotProduct(kit.axisX, body.fwd) < -0.99f);     // face out of the back
            const Vector p = local(kit, shape);
            CHECK(p.x < -0.15f && p.x > -0.40f);                 // behind the head, on the back
            CHECK(p.y < -0.05f);                                 // off-shoulder side
            CHECK(p.z < -0.25f && p.z > -0.55f);                 // shoulder blade height
        }
        // Upgrade packs: flat side (+Z) out of the back, upright.
        const ItemPose pack = BuildItemPose(f, layout, Zone::Medical, ItemShape::Pack);
        CHECK(pack.axisY.z > 0.99f && DotProduct(pack.axisZ, body.fwd) < -0.99f);
        // Long gun on the back (Pavlov's back socket): muzzle up, leaning toward
        // the dominant shoulder, sights toward the dominant side, its flat side
        // on the back.
        const ItemPose rifle = BuildItemPose(f, layout, Zone::Primary, ItemShape::LongGun);
        CHECK(rifle.axisZ.z > 0.95f);
        CHECK(DotProduct(rifle.axisZ, domSide) > 0.1f);
        CHECK(DotProduct(rifle.axisY, domSide) > 0.95f);
        CHECK(std::fabs(DotProduct(rifle.axisX, body.fwd)) > 0.95f);
        const Vector rp = local(rifle, ItemShape::LongGun);
        CHECK(rp.x < -0.2f && rp.y > 0.0f && rp.z < -0.3f);
        // Pistol in the dominant hip holster: muzzle down and a little forward,
        // slide toward the front, so the grip sticks up and back.
        const ItemPose pistol = BuildItemPose(f, layout, Zone::Secondary, ItemShape::Pistol);
        CHECK(pistol.axisZ.z < -0.9f && DotProduct(pistol.axisZ, body.fwd) > 0.3f);
        CHECK(DotProduct(pistol.axisY, body.fwd) > 0.9f);
        CHECK(std::fabs(DotProduct(pistol.axisX, domSide)) > 0.99f);
        const Vector hp = local(pistol, ItemShape::Pistol);
        CHECK(hp.y > 0.15f && hp.z < -0.4f);
        // Pills and grenades: upright in the upper chest pockets, face forward.
        const ItemPose pills = BuildItemPose(f, layout, Zone::Consumable, ItemShape::Equipment);
        const ItemPose grenade = BuildItemPose(f, layout, Zone::Throwable, ItemShape::Equipment);
        for (const ItemPose* pose : { &pills, &grenade })
        {
            CHECK(pose->axisY.z > 0.99f);
            CHECK(DotProduct(pose->axisX, body.fwd) > 0.99f);
        }
        const Vector pp = local(pills, ItemShape::Equipment);
        const Vector gp = local(grenade, ItemShape::Equipment);
        CHECK(pp.y > 0.1f && gp.y < -0.1f);                  // pills dominant side, grenade off side
        CHECK(pp.x > -0.05f && gp.x > -0.05f);               // on the chest, not behind it
        CHECK(pp.z < -0.25f && pp.z > -0.40f);
        // Left-handed mirrors the world side; the poses stay proper rotations.
        for (const ItemPose* pose : { &pills, &grenade, &pistol, &rifle, &pack })
        {
            Vector cross{};
            CrossProduct(pose->axisX, pose->axisY, cross);
            CHECK(DotProduct(cross, pose->axisZ) > 0.99f);
        }
    }
    // Shapes per weapon id.
    CHECK(ItemShapeForWeaponId(12) == ItemShape::Kit);
    CHECK(ItemShapeForWeaponId(24) == ItemShape::Defib);
    CHECK(ItemShapeForWeaponId(30) == ItemShape::Pack && ItemShapeForWeaponId(31) == ItemShape::Pack);
    CHECK(ItemShapeForWeaponId(15) == ItemShape::Equipment && ItemShapeForWeaponId(13) == ItemShape::Equipment);
    CHECK(ItemShapeForWeaponId(1) == ItemShape::Pistol && ItemShapeForWeaponId(32) == ItemShape::Pistol);
    CHECK(ItemShapeForWeaponId(19) == ItemShape::Melee);
    CHECK(ItemShapeForWeaponId(26) == ItemShape::LongGun);
}

int main()
{
    TestBackHalvesRealisticBody();
    TestItemOrientationRealisticBody();
    TestNearAnyZone();
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

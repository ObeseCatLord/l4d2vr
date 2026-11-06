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
    // Forefront (speed 0, the default): the head yaw every frame, no lag.
    CHECK(Near(FollowBodyYaw(0.0f, true, 75.0f, 0.0f, 0.011f, 0.0f), 75.0f, 0.001f));
    CHECK(Near(FollowBodyYaw(10.0f, true, -120.0f, 5.0f, 0.011f, 0.0f), -120.0f, 0.001f));
    CHECK(Near(Layout{}.rootDropMeters, -0.60f, 1e-6f));
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
    const Vector slungGrip = ToWorld(frame, Vector(0.14f, 0.10f, -0.50f));
    const Vector gunReady = ToWorld(frame, Vector(0.35f, 0.20f, -0.30f));
    CHECK(atHip.y < 0.0f); // dominant (right) side is -y
    CHECK(FindZone(frame, layout, atHip, 0x1Fu) == Zone::Secondary);
    CHECK(FindZone(frame, layout, relaxedAtSide, 0x1Fu) == Zone::Secondary);
    CHECK(FindZone(frame, layout, atHip, 0x1Du) == Zone::None); // secondary empty
    CHECK(FindZone(frame, layout, slungGrip, 0x1Fu) == Zone::Primary);
    CHECK(FindZone(frame, layout, slungGrip, 0x1Eu) == Zone::None); // no long gun
    CHECK(FindZone(frame, layout, gunReady, 0x1Fu) == Zone::None);
    // Every zone takes a hand at its own handle.
    for (int zone = 0; zone < kZoneCount; ++zone)
        CHECK(FindZone(frame, layout, ZoneCenterWorld(frame, layout, static_cast<Zone>(zone)), 0x1Fu) == static_cast<Zone>(zone));
    // The middle of the belly, between the sling's grip and the hips, is not a holster.
    CHECK(FindZone(frame, layout, ToWorld(frame, Vector(0.30f, 0.0f, -0.45f)), 0x1Fu) == Zone::None);
    // Putting an item back: in its own grab volume.
    CHECK(HandInHolsterVolume(frame, layout, Zone::Consumable, ZoneCenterWorld(frame, layout, Zone::Consumable)));
    CHECK(!HandInHolsterVolume(frame, layout, Zone::Consumable, gunReady + Vector(20.0f, 0.0f, 0.0f)));
    CHECK(HandInHolsterVolume(frame, layout, Zone::Primary, slungGrip));
    // Left-handed mirrors the hip to +y.
    const BodyFrame left = Frame(true);
    CHECK(ZoneCenterWorld(left, layout, Zone::Secondary).y > 0.0f);
    CHECK(ZoneCenterWorld(left, layout, Zone::Consumable).y < 0.0f);
}

static void TestForefrontRoot()
{
    const Layout layout{};
    BodyFrame frame = MakeFrame(Vector(0.0f, 0.0f, 100.0f), 0.0f, false, 40.0f, 1.0f);
    ApplyPavlovRoot(frame, layout, Vector(1.0f, 0.0f, 0.0f), Vector(0.0f, -1.0f, 0.0f), Vector(0.0f, 0.0f, 1.0f), false, -1.0f);
    CHECK(Near(frame.root.z, 100.0f - 0.60f * 40.0f, 0.01f)); // Forefront: 60 cm straight below the head
    CHECK(Near(frame.root.x, 0.0f, 0.01f));
    // No Pavlov curves: looking straight down and reaching to the floor move nothing.
    BodyFrame down = MakeFrame(Vector(0.0f, 0.0f, 100.0f), 0.0f, false, 40.0f, 1.0f);
    ApplyPavlovRoot(down, layout, Vector(0.001f, 0.0f, -1.0f), Vector(0.0f, -1.0f, 0.0f), Vector(1.0f, 0.0f, 0.0f), false, 0.0f);
    CHECK(Near(down.root.x, 0.0f, 0.01f) && Near(down.root.y, 0.0f, 0.01f));
    // The Pavlov curves still work when a layout sets them.
    Layout pavlov{};
    pavlov.rootDropMeters = -0.75f;
    pavlov.lookDownBackMeters = 0.08f;
    pavlov.reachFloorBackMeters = 0.60f;
    BodyFrame reach = MakeFrame(Vector(0.0f, 0.0f, 100.0f), 0.0f, false, 40.0f, 1.0f);
    ApplyPavlovRoot(reach, pavlov, Vector(1.0f, 0.0f, 0.0f), Vector(0.0f, -1.0f, 0.0f), Vector(0.0f, 0.0f, 1.0f), false, 0.0f);
    CHECK(Near(reach.root.x, -0.60f * 40.0f, 0.01f));
    CHECK(Near(reach.root.z, 100.0f - 0.75f * 40.0f, 0.01f));
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
    CHECK(pistol.axisZ.z < -0.99f); // muzzle straight down (Forefront's secondary)
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
    CHECK(out.putAwaySlot == 1); // empty hands: the pistol goes away, nothing is drawn
    // The only item: put away with nothing to switch to (empty hands still stow it).
    {
        GrabFsm only;
        GrabInput one{};
        one.ready = true;
        one.activeSlot = 1;
        one.occupiedMask = 0x02u;
        one.gripDown = true;
        only.Update(one);
        one.gripDown = false;
        one.ownZoneUnderHand = Zone::Secondary;
        const GrabOutput put = only.Update(one);
        CHECK(put.putAwaySlot == 1 && put.holsterTo == -1);
        // Stowed (no item in hand, activeSlot -1): a squeeze in the air holds
        // nothing, so letting go puts nothing away.
        one.activeSlot = -1;
        one.ownZoneUnderHand = Zone::None;
        one.gripDown = true;
        only.Update(one);
        one.gripDown = false;
        one.ownZoneUnderHand = Zone::Secondary;
        const GrabOutput none = only.Update(one);
        CHECK(none.putAwaySlot == -1 && none.holsterTo == -1);
    }
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
    // Default layout at VRScale 43.2: every item rests at its socket on the
    // body below the head (hip items hang from their grip at the socket, the
    // slung gun's grip is at the slot1 socket, the rest are centred on it).
    BodyFrame frame = MakeFrame(Vector(100.0f, 200.0f, 64.0f), 90.0f, false, 43.2f, 1.0f);
    ApplyPavlovRoot(frame, Layout{}, Vector(0.0f, 1.0f, 0.0f), Vector(1.0f, 0.0f, 0.0f), Vector(0.0f, 0.0f, 1.0f), false, -1.0f);
    const Layout layout{};
    for (int zone = 0; zone < kZoneCount; ++zone)
    {
        const ItemShape shape = zone == 0 ? ItemShape::LongGun : (zone == 1 ? ItemShape::Pistol :
            (zone == 3 ? ItemShape::Kit : ItemShape::Equipment));
        const ItemPose pose = BuildItemPose(frame, layout, static_cast<Zone>(zone), shape);
        CHECK(pose.valid);
        const Vector socket = RootToWorld(frame, layout.socket[zone]);
        Vector anchor{};
        if (zone == 0)
        {
            // The gun's centre is slingHandleToCenterMeters down the barrel from the grip.
            const Vector c = ReferenceToCenterUnits(shape);
            anchor = pose.origin + pose.axisX * c.x + pose.axisY * c.y + pose.axisZ * c.z -
                pose.axisZ * (layout.slingHandleToCenterMeters * frame.unitsPerMeter);
        }
        else if (zone == 1)
        {
            anchor = pose.origin; // the pistol's bone (grip end) at the hip socket
        }
        else
        {
            const Vector c = ReferenceToCenterUnits(shape);
            anchor = pose.origin + pose.axisX * c.x + pose.axisY * c.y + pose.axisZ * c.z;
        }
        CHECK(Near(anchor.x, socket.x, 0.01f) && Near(anchor.y, socket.y, 0.01f) && Near(anchor.z, socket.z, 0.01f));
        const Vector d = socket - frame.head;
        const float meters = std::sqrt(DotProduct(d, d)) / frame.unitsPerMeter;
        CHECK(meters > 0.2f && meters < 0.7f);
        CHECK(socket.z < frame.head.z);
    }
    // Hip (slot 2) sits on the dominant (right) side: yaw 90 faces +y, right is +x.
    const ItemPose hip = BuildItemPose(frame, layout, Zone::Secondary, ItemShape::Pistol);
    CHECK(hip.origin.x > frame.head.x);
    // The pills hang on the other hip, the grenade on the dominant upper chest.
    CHECK(RootToWorld(frame, layout.socket[4]).x < frame.head.x);
    CHECK(RootToWorld(frame, layout.socket[2]).x > frame.head.x);
    CHECK(ToLocal(frame, RootToWorld(frame, layout.socket[2])).x > 0.0f);
    // Only the kit is on the back now; the long gun is on the chest.
    CHECK(IsBackZone(Zone::Medical) && !IsBackZone(Zone::Primary));
    CHECK(!IsBackZone(Zone::Secondary) && !IsBackZone(Zone::Throwable) && !IsBackZone(Zone::Consumable));
    CHECK(IsHipZone(Zone::Secondary) && IsHipZone(Zone::Consumable) && !IsHipZone(Zone::Throwable));
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
// Where a real adult's hand goes for each body place (independent of the
// layout's numbers; the same body model as the in-game self-test).
static const Vector kOverRightShoulder(-0.10f, 0.15f, -0.05f);
static const Vector kOverLeftShoulder(-0.10f, -0.15f, -0.05f);
static const Vector kRightHip(-0.05f, 0.22f, -0.80f);
static const Vector kLeftHip(-0.05f, -0.22f, -0.80f);
static const Vector kRightBelt(-0.02f, 0.24f, -0.62f);   // hand on a belt holster's grip
static const Vector kLeftBelt(-0.02f, -0.24f, -0.62f);
static const Vector kRightUpperChest(0.10f, 0.10f, -0.25f); // by the collar bone
static const Vector kLeftUpperChest(0.10f, -0.10f, -0.30f);
static const Vector kSlungGripRight(0.14f, 0.10f, -0.50f);  // pistol grip of a rifle slung across the chest
static const Vector kSlungForegripRight(0.14f, -0.08f, -0.66f); // its handguard, toward the other hip
static const Vector kRightReady(0.40f, 0.15f, -0.35f);
static const Vector kLeftReady(0.32f, -0.18f, -0.42f);

static Vector Mirror(const Vector& v) { return Vector(v.x, -v.y, v.z); }

static void TestForefrontLayoutRealisticBody()
{
    const Layout layout{};
    for (int handed = 0; handed < 2; ++handed)
    {
        const bool leftHanded = handed == 1;
        auto dom = [&](const Vector& v) { return leftHanded ? Mirror(v) : v; }; // physical point on the dominant side
        const Vector domShoulder = dom(kOverRightShoulder), offShoulder = dom(kOverLeftShoulder);
        const Vector domHip = dom(kRightHip), offHip = dom(kLeftHip);
        const Vector domBelt = dom(kRightBelt), offBelt = dom(kLeftBelt);
        const Vector domUpperChest = dom(kRightUpperChest), offUpperChest = dom(kLeftUpperChest);
        const Vector slungGrip = dom(kSlungGripRight), slungForegrip = dom(kSlungForegripRight);
        for (float yaw : { 0.0f, 37.0f, -150.0f })
        for (float pitch : { 0.0f, 35.0f, 60.0f, -25.0f, -40.0f })
        for (float roll : { 0.0f, 15.0f, -15.0f })
        {
            const Body body = MakeBody(yaw, pitch, roll, leftHanded);
            const BodyFrame& f = body.frame;
            auto at = [&](const Vector& v) { return body.At(v.x, v.y, v.z); };
            CHECK(f.valid);
            CHECK((pitch > 0.0f) == (f.headForward.z < -0.1f)); // positive pitch looks down
            // slot1, the chest sling: by its grip or along the gun.
            CHECK(FindZone(f, layout, at(slungGrip), 0x1Fu) == Zone::Primary);
            CHECK(FindZone(f, layout, at(slungForegrip), 0x1Fu) == Zone::Primary);
            // slot2 dominant hip, slot5 off-hand hip: the belt grip or the hand
            // hanging lower at the side.
            CHECK(FindZone(f, layout, at(domHip), 0x1Fu) == Zone::Secondary);
            CHECK(FindZone(f, layout, at(domBelt), 0x1Fu) == Zone::Secondary);
            CHECK(FindZone(f, layout, at(offHip), 0x1Fu) == Zone::Consumable);
            CHECK(FindZone(f, layout, at(offBelt), 0x1Fu) == Zone::Consumable);
            // slot3: the dominant upper chest.
            CHECK(FindZone(f, layout, at(domUpperChest), 0x1Fu) == Zone::Throwable);
            // slot4: the back, over either shoulder (the whole sphere is the kit's).
            CHECK(FindZone(f, layout, at(offShoulder), 0x1Fu) == Zone::Medical);
            CHECK(FindZone(f, layout, at(domShoulder), 0x1Fu) == Zone::Medical);
            CHECK(FindZone(f, layout, at(offShoulder + Vector(-0.12f, 0.0f, -0.05f)), 0x1Fu) == Zone::Medical);
            // The magazine pouch (off-hand upper chest) never draws a holster
            // item, even looking up, where the back sphere swings down to it.
            CHECK(HandAtMagazinePouch(f, layout, at(offUpperChest)));
            CHECK(FindZone(f, layout, at(offUpperChest), 0x1Fu) == Zone::None);
            CHECK(!HandAtMagazinePouch(f, layout, at(domUpperChest)));
            CHECK(!HandAtMagazinePouch(f, layout, at(slungGrip)));
            // Empty slots take nothing: no rifle, no pistol, no grenade.
            CHECK(FindZone(f, layout, at(slungGrip), 0x1Eu) == Zone::None);
            CHECK(FindZone(f, layout, at(domBelt), 0x1Du) == Zone::None);
            CHECK(FindZone(f, layout, at(domUpperChest), 0x1Bu) != Zone::Throwable);
            // Hands held out in front take nothing.
            CHECK(FindZone(f, layout, at(kRightReady), 0x1Fu) == Zone::None);
            CHECK(FindZone(f, layout, at(kLeftReady), 0x1Fu) == Zone::None);
            // Putting items back: in their own volume, not in another one.
            CHECK(HandInHolsterVolume(f, layout, Zone::Primary, at(slungGrip)));
            CHECK(HandInHolsterVolume(f, layout, Zone::Secondary, at(domBelt)));
            CHECK(!HandInHolsterVolume(f, layout, Zone::Secondary, at(offBelt)));
            CHECK(HandInHolsterVolume(f, layout, Zone::Consumable, at(offBelt)));
            CHECK(HandInHolsterVolume(f, layout, Zone::Throwable, at(domUpperChest)));
            CHECK(HandInHolsterVolume(f, layout, Zone::Medical, at(offShoulder)));
            CHECK(!HandInHolsterVolume(f, layout, Zone::Medical, at(kRightReady)));
            // Every reach counts as "near a holster" for the bash filter.
            for (const Vector& p : { domShoulder, offShoulder, domBelt, offBelt, domUpperChest, slungGrip })
                CHECK(NearAnyZone(f, layout, at(p), 0.05f));
            CHECK(!NearAnyZone(f, layout, body.At(0.65f, 0.15f, -0.30f), 0.05f));
        }
        // The equipment follows the head yaw exactly (Forefront): a body frame
        // that lags is not a case; the zones turn with the head.
        const Body turned = MakeBody(90.0f, 0.0f, 0.0f, leftHanded);
        CHECK(FindZone(turned.frame, layout, turned.At(slungGrip.x, slungGrip.y, slungGrip.z), 0x1Fu) == Zone::Primary);
    }
}

// 2.0.9: an item hanging on the body is grabbed anywhere on its drawn body;
// overlapping items go to the one the hand is nearest (on).
static void TestBodyGrabAnywhere()
{
    const Layout layout{};
    BodyItems items{};
    items.mask = 0x1Fu;
    items.shape[0] = ItemShape::LongGun;
    items.shape[1] = ItemShape::Pistol;
    items.shape[2] = ItemShape::Equipment;
    items.shape[3] = ItemShape::Kit;
    items.shape[4] = ItemShape::Equipment;
    auto along = [](const ItemPose& p, float x, float y, float z) { return p.origin + p.axisX * x + p.axisY * y + p.axisZ * z; };
    for (int handed = 0; handed < 2; ++handed)
    {
        const bool leftHanded = handed == 1;
        auto dom = [&](const Vector& v) { return leftHanded ? Mirror(v) : v; };
        for (float yaw : { 0.0f, 37.0f, -150.0f })
        for (float pitch : { 0.0f, 35.0f, -25.0f })
        {
            const Body body = MakeBody(yaw, pitch, 0.0f, leftHanded);
            const BodyFrame& f = body.frame;
            auto at = [&](const Vector& v) { return body.At(v.x, v.y, v.z); };
            // The 2.0.8 reaches give the same zones with the item bodies.
            CHECK(FindZone(f, layout, at(dom(kSlungGripRight)), 0x1Fu, nullptr, &items) == Zone::Primary);
            CHECK(FindZone(f, layout, at(dom(kSlungForegripRight)), 0x1Fu, nullptr, &items) == Zone::Primary);
            CHECK(FindZone(f, layout, at(dom(kRightHip)), 0x1Fu, nullptr, &items) == Zone::Secondary);
            CHECK(FindZone(f, layout, at(dom(kRightBelt)), 0x1Fu, nullptr, &items) == Zone::Secondary);
            CHECK(FindZone(f, layout, at(dom(kLeftHip)), 0x1Fu, nullptr, &items) == Zone::Consumable);
            CHECK(FindZone(f, layout, at(dom(kLeftBelt)), 0x1Fu, nullptr, &items) == Zone::Consumable);
            CHECK(FindZone(f, layout, at(dom(kRightUpperChest)), 0x1Fu, nullptr, &items) == Zone::Throwable);
            CHECK(FindZone(f, layout, at(dom(kOverLeftShoulder)), 0x1Fu, nullptr, &items) == Zone::Medical);
            CHECK(FindZone(f, layout, at(dom(kOverRightShoulder)), 0x1Fu, nullptr, &items) == Zone::Medical);
            CHECK(FindZone(f, layout, at(dom(kLeftUpperChest)), 0x1Fu, nullptr, &items) == Zone::None); // the pouch
            CHECK(FindZone(f, layout, at(kRightReady), 0x1Fu, nullptr, &items) == Zone::None);
            CHECK(FindZone(f, layout, at(kLeftReady), 0x1Fu, nullptr, &items) == Zone::None);

            // The slung gun: by the muzzle end and by the stock, far from the
            // grip-to-foregrip handle volume.
            const ItemPose sling = SlingPose(f, layout);
            const Vector muzzle = along(sling, 0.0f, 0.0f, 30.0f);
            const Vector stock = along(sling, 0.0f, -1.0f, -13.0f);
            CHECK(FindZone(f, layout, muzzle, 0x1Fu) == Zone::None);
            CHECK(FindZone(f, layout, muzzle, 0x1Fu, nullptr, &items) == Zone::Primary);
            CHECK(FindZone(f, layout, stock, 0x1Fu) == Zone::None);
            CHECK(FindZone(f, layout, stock, 0x1Fu, nullptr, &items) == Zone::Primary);
            // ... a little off the gun's side still counts, well off it does not.
            CHECK(FindZone(f, layout, muzzle + sling.axisX * (0.08f * body.s), 0x1Fu, nullptr, &items) == Zone::Primary);
            CHECK(FindZone(f, layout, muzzle + sling.axisX * (0.25f * body.s), 0x1Fu, nullptr, &items) == Zone::None);
            // Not slung (held, or no long gun): the body does not count.
            BodyItems noGun = items;
            noGun.mask &= ~1u;
            CHECK(FindZone(f, layout, muzzle, 0x1Fu, nullptr, &noGun) == Zone::None);
            CHECK(FindZone(f, layout, muzzle, 0x1Eu, nullptr, &items) == Zone::None);
            float score = 0.0f;
            CHECK(FindZone(f, layout, muzzle, 0x1Fu, nullptr, &items, &score) == Zone::Primary && score <= 0.0f);

            // The hip pistol by the end of its barrel (it hangs muzzle down).
            const ItemPose hip = BuildItemPose(f, layout, Zone::Secondary, ItemShape::Pistol);
            const Vector pistolMuzzle = along(hip, 0.0f, 0.0f, 12.0f);
            CHECK(FindZone(f, layout, pistolMuzzle, 0x1Fu, nullptr, &items) == Zone::Secondary);
            // A melee weapon on the hip by the end of its blade.
            BodyItems melee = items;
            melee.shape[1] = ItemShape::Melee;
            const ItemPose blade = BuildItemPose(f, layout, Zone::Secondary, ItemShape::Melee);
            const Vector bladeTip = along(blade, 0.0f, 24.0f, 0.0f);
            CHECK(FindZone(f, layout, bladeTip, 0x1Fu) == Zone::None);
            CHECK(FindZone(f, layout, bladeTip, 0x1Fu, nullptr, &melee) == Zone::Secondary);

            // Where the slung gun's barrel passes in front of the off-hand hip
            // item, the hand on the barrel takes the gun and the hand on the
            // item takes the item (nearest surface).
            const ItemPose offHip = BuildItemPose(f, layout, Zone::Consumable, ItemShape::Equipment);
            CHECK(FindZone(f, layout, offHip.origin, 0x1Fu, nullptr, &items) == Zone::Consumable);
            // The grenade pocket next to the stock: on the grenade, the grenade.
            const ItemPose pocket = BuildItemPose(f, layout, Zone::Throwable, ItemShape::Equipment);
            CHECK(FindZone(f, layout, pocket.origin, 0x1Fu, nullptr, &items) == Zone::Throwable);
            CHECK(FindZone(f, layout, pocket.origin, 0x1Bu, nullptr, &items) != Zone::Throwable);
            // Nearest surface: a point on the gun's axis beside the hip item.
            const Vector onBarrel = along(sling, 0.0f, 0.0f, 22.0f);
            CHECK(FindZone(f, layout, onBarrel, 0x1Fu, nullptr, &items) == Zone::Primary);
        }
    }
    // Signed box distance: inside negative, outside the Euclidean gap.
    ItemPose unit{};
    unit.valid = true;
    CHECK(Near(SignedBoxDistanceMeters(unit, Vector(0, 0, 0), Vector(1, 2, 3), Vector(0, 0, 0), 1.0f), -1.0f, 1e-5f));
    CHECK(Near(SignedBoxDistanceMeters(unit, Vector(0, 0, 0), Vector(1, 2, 3), Vector(4, 0, 0), 1.0f), 3.0f, 1e-5f));
    CHECK(Near(SignedBoxDistanceMeters(unit, Vector(0, 0, 0), Vector(1, 2, 3), Vector(4, 6, 0), 2.0f), 2.5f, 1e-5f));
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
        auto local = [&](const Vector& p) {
            const Vector d = p - body.head;
            return Vector(DotProduct(d, body.fwd) / body.s, DotProduct(d, domSide) / body.s, d.z / body.s);
        };
        auto centre = [&](const ItemPose& pose, ItemShape shape) {
            const Vector c = ReferenceToCenterUnits(shape);
            return local(pose.origin + pose.axisX * c.x + pose.axisY * c.y + pose.axisZ * c.z);
        };
        // First aid kit: strapped upright on the off shoulder blade (unchanged
        // from 2.0.4), its face (the cross, +X) out of the back.
        for (ItemShape shape : { ItemShape::Kit, ItemShape::Defib })
        {
            const ItemPose kit = BuildItemPose(f, layout, Zone::Medical, shape);
            CHECK(kit.valid);
            CHECK(kit.axisY.z > 0.99f);                          // upright
            CHECK(DotProduct(kit.axisX, body.fwd) < -0.99f);     // face out of the back
            const Vector p = centre(kit, shape);
            CHECK(p.x < -0.15f && p.x > -0.40f);                 // behind the head, on the back
            CHECK(p.y < -0.05f);                                 // off-shoulder side
            CHECK(p.z < -0.25f && p.z > -0.55f);                 // shoulder blade height
        }
        {
            // Same world place as 2.0.4 (head offset (-0.25, -0.13, -0.39)).
            const Vector socket = local(RootToWorld(f, layout.socket[3]));
            CHECK(Near(socket.x, -0.25f, 0.005f) && Near(socket.y, -0.13f, 0.005f) && Near(socket.z, -0.39f, 0.005f));
        }
        // Upgrade packs: flat side (+Z) out of the back, upright.
        const ItemPose pack = BuildItemPose(f, layout, Zone::Medical, ItemShape::Pack);
        CHECK(pack.axisY.z > 0.99f && DotProduct(pack.axisZ, body.fwd) < -0.99f);
        // Long gun on the chest sling (Forefront's primary): barrel down toward
        // the off-hand hip at 45 degrees, sights up toward the off shoulder, in
        // the frontal plane, its left side against the body (right-handed).
        const ItemPose rifle = BuildItemPose(f, layout, Zone::Primary, ItemShape::LongGun);
        CHECK(Near(rifle.axisZ.z, -0.7071f, 0.01f) && Near(DotProduct(rifle.axisZ, domSide), -0.7071f, 0.01f));
        CHECK(Near(rifle.axisY.z, 0.7071f, 0.01f) && Near(DotProduct(rifle.axisY, domSide), -0.7071f, 0.01f));
        CHECK(std::fabs(DotProduct(rifle.axisZ, body.fwd)) < 0.01f);
        CHECK(DotProduct(rifle.axisX, body.fwd) * (leftHanded ? -1.0f : 1.0f) < -0.99f);
        const Vector grip = local(RootToWorld(f, layout.socket[0]));
        CHECK(grip.x > 0.0f && grip.y > 0.0f && grip.z < -0.4f && grip.z > -0.6f); // dominant side of the belly
        const Vector rc = centre(rifle, ItemShape::LongGun);
        CHECK(rc.y < grip.y && rc.z < grip.z);                // the gun runs down toward the other hip
        // Pistol in the dominant hip holster: barrel straight down, sights
        // forward; it hangs from its grip at the socket.
        const ItemPose pistol = BuildItemPose(f, layout, Zone::Secondary, ItemShape::Pistol);
        CHECK(pistol.axisZ.z < -0.99f);
        CHECK(DotProduct(pistol.axisY, body.fwd) > 0.99f);
        const Vector hp = local(pistol.origin);
        CHECK(hp.y > 0.15f && hp.z < -0.5f && hp.z > -0.7f);
        CHECK(centre(pistol, ItemShape::Pistol).z < hp.z);  // the gun below its grip
        // Melee on the same hip: handle at the socket, blade down.
        const ItemPose melee = BuildItemPose(f, layout, Zone::Secondary, ItemShape::Melee);
        CHECK(melee.axisY.z < -0.8f);
        // Pills on the off-hand hip, the grenade on the dominant upper chest: upright, face forward.
        const ItemPose pills = BuildItemPose(f, layout, Zone::Consumable, ItemShape::Equipment);
        const ItemPose grenade = BuildItemPose(f, layout, Zone::Throwable, ItemShape::Equipment);
        for (const ItemPose* pose : { &pills, &grenade })
        {
            CHECK(pose->axisY.z > 0.99f);
            CHECK(DotProduct(pose->axisX, body.fwd) > 0.99f);
        }
        const Vector pp = centre(pills, ItemShape::Equipment);
        const Vector gp = centre(grenade, ItemShape::Equipment);
        CHECK(pp.y < -0.15f && gp.y > 0.05f);                // pills off-hand hip, grenade dominant side
        CHECK(gp.x > 0.0f);                                  // on the chest, in front of the eyes' plane
        CHECK(gp.z < -0.15f && gp.z > -0.30f);               // upper chest
        CHECK(pp.z < -0.5f && pp.z > -0.7f);                 // hip
        // The magazine pouch: off-hand upper chest, 20 cm from the grenade.
        const Vector mp = local(RootToWorld(f, layout.magPouch));
        CHECK(mp.y < -0.05f && mp.z < -0.2f && mp.z > -0.35f && mp.x > 0.0f);
        const Vector gm = mp - gp;
        CHECK(std::sqrt(DotProduct(gm, gm)) > 0.2f);
        // Left-handed mirrors the world side; the poses stay proper rotations.
        for (const ItemPose* pose : { &pills, &grenade, &pistol, &rifle, &pack, &melee })
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
    TestForefrontLayoutRealisticBody();
    TestBodyGrabAnywhere();
    TestItemOrientationRealisticBody();
    TestNearAnyZone();
    TestWeaponClasses();
    TestDiagPacer();
    TestControlReadyUsesCameraReport();
    TestHolsterPlacementNearHead();
    TestYawFollow();
    TestForefrontRoot();
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

#pragma once
#include "vr_interaction_geometry.h"
#include <cmath>
#include <cstdint>

// Body holsters placed the way Pavlov VR places its inventory (decoded from
// the game; research/pavlov/inventory/SPEC.md). An inventory root hangs 75 cm
// below the headset, its yaw follows the head yaw continuously (interpolation
// speed 20), looking down pulls it back a little and reaching to the floor with
// the off hand pushes it back. Hip holsters are upright capsules beside the
// root, the back slot is a sphere behind the head in the head's own frame, and
// chest items sit in vest pockets. Stick/snap turns move everything instantly.
//
// Slot assignment (the player's choice, mirrored for the left-handed): Pavlov's
// one back sphere is split down the middle on the head's lateral axis, the
// dominant shoulder holds the long gun (slot1) and the off shoulder the first
// aid kit, defibrillator or upgrade pack (slot4). The dominant hip holds the
// pistol or melee weapon (slot2), the upper chest pockets the grenade (slot3,
// off side) and pills or adrenaline (slot5, dominant side), and the off-hand
// hip is the magazine pouch (Pavlov's empty-hand-at-hip action).
namespace l4d2vr_holster
{
    constexpr float kPi = 3.14159265358979323846f;

    enum class Zone : int
    {
        Primary = 0,    // slot1, back, dominant-shoulder half (the long gun)
        Secondary = 1,  // slot2, dominant hip (pistols and melee)
        Throwable = 2,  // slot3, chest pocket, off-hand side (upper)
        Medical = 3,    // slot4, back, off-shoulder half (kit, defibrillator, upgrade pack)
        Consumable = 4, // slot5, chest pocket, dominant side (upper)
        Count = 5,
        None = -1
    };

    constexpr int kZoneCount = static_cast<int>(Zone::Count);

    inline int SlotIndex(Zone zone) { return static_cast<int>(zone); }

    // Entity class for "use <class>", which switches weapons directly instead
    // of going through the HUD's slot selection. Null for unknown ids.
    inline const char* WeaponClassForId(int id)
    {
        switch (id)
        {
        case 1: return "weapon_pistol";
        case 2: return "weapon_smg";
        case 3: return "weapon_pumpshotgun";
        case 4: return "weapon_autoshotgun";
        case 5: return "weapon_rifle";
        case 6: return "weapon_hunting_rifle";
        case 7: return "weapon_smg_silenced";
        case 8: return "weapon_shotgun_chrome";
        case 9: return "weapon_rifle_desert";
        case 10: return "weapon_sniper_military";
        case 11: return "weapon_shotgun_spas";
        case 12: return "weapon_first_aid_kit";
        case 13: return "weapon_molotov";
        case 14: return "weapon_pipe_bomb";
        case 15: return "weapon_pain_pills";
        case 19: return "weapon_melee";
        case 20: return "weapon_chainsaw";
        case 21: return "weapon_grenade_launcher";
        case 23: return "weapon_adrenaline";
        case 24: return "weapon_defibrillator";
        case 25: return "weapon_vomitjar";
        case 26: return "weapon_rifle_ak47";
        case 30: return "weapon_upgradepack_incendiary";
        case 31: return "weapon_upgradepack_explosive";
        case 32: return "weapon_pistol_magnum";
        case 33: return "weapon_smg_mp5";
        case 34: return "weapon_rifle_sg552";
        case 35: return "weapon_sniper_awp";
        case 36: return "weapon_sniper_scout";
        case 37: return "weapon_rifle_m60";
        default: return nullptr;
        }
    }

    // Metres, x forward, y toward the DOMINANT side, z up (mirrors for the
    // left-handed). "Root" offsets are from the inventory root; "head" offsets
    // are in the headset's own frame. Values are Pavlov's (UE cm / 100).
    struct Layout
    {
        float rootDropMeters = -0.75f;          // root = head + (0,0,-0.75)
        float lookDownBackMeters = 0.08f;       // looking straight down pulls the root back
        float reachFloorBackMeters = 0.60f;     // off hand at the floor pushes it back
        float handRadiusMeters = 0.115f;        // the hand's grab sphere
        // Hip holster: upright capsule beside the root.
        Vector hipCenter{ -0.2247f, 0.352f, 0.0557f };
        float hipRadiusMeters = 0.20f;
        float hipHalfHeightMeters = 0.44f;
        // Vest: tilted capsule over the chest (putting a chest item away).
        Vector vestCenter{ -0.0581f, 0.0f, 0.2749f };
        Vector vestAxis{ -0.2153f, 0.0f, 0.9766f }; // top tilted back 12.4 deg
        float vestRadiusMeters = 0.22f;
        float vestHalfHeightMeters = 0.3122f;
        // Back: sphere behind the head, in the head's frame (follows pitch too).
        // Split on the head's lateral axis: the dominant half (lateral >= 0)
        // is slot1, the off half slot4.
        Vector backCenterHead{ -0.4414f, 0.0f, -0.0948f };
        float backRadiusMeters = 0.40f;
        // A hand reaching over the dominant shoulder (head frame; the off
        // shoulder mirrors y). Only a reach target for self-tests and the
        // per-zone diagnostics: the sphere halves decide.
        Vector backReachHead{ -0.12f, 0.15f, -0.08f };
        // Where items rest (root offsets). Hip and chest items sit on Pavlov's
        // vest sockets (hip slot1, pockets slot4 and slot7); the back items
        // share Pavlov's back socket (slot2), the long gun moved toward the
        // dominant shoulder and the kit onto the off shoulder blade. A chest
        // item is taken by reaching for it in its pocket.
        Vector socket[kZoneCount] = {
            Vector(-0.30f, 0.06f, 0.3043f),     // back, dominant shoulder
            Vector(-0.0615f, 0.25f, 0.2271f),   // dominant hip
            Vector(0.0225f, -0.1646f, 0.4396f), // chest, off-hand side (upper)
            Vector(-0.25f, -0.13f, 0.36f),      // back, off shoulder blade
            Vector(0.0242f, 0.1461f, 0.4413f),  // chest, dominant side (upper)
        };
        float pocketReachMeters = 0.16f;        // hand to a pocketed item
        // The magazine pouch on the off-hand hip (Pavlov's ammo-pouch mesh,
        // root offset toward the off side) and the magazine logic's default
        // reach to it (MagazineInteractionFreshMagazineGrabRangeMeters 0.20 +
        // the hand). The holsters leave a hand there to the magazines.
        Vector magPouch{ -0.0233f, -0.187f, 0.19f };
        float magPouchReachMeters = 0.315f;
    };

    struct BodyFrame
    {
        Vector head{ 0.0f, 0.0f, 0.0f };
        Vector root{ 0.0f, 0.0f, 0.0f };        // inventory root (set by ApplyPavlovRoot)
        Vector headForward{ 1.0f, 0.0f, 0.0f }; // the headset's own axes (world)
        Vector headDominant{ 0.0f, -1.0f, 0.0f };
        Vector headUp{ 0.0f, 0.0f, 1.0f };
        Vector forward{ 1.0f, 0.0f, 0.0f };
        Vector dominant{ 0.0f, -1.0f, 0.0f }; // unit vector toward the dominant hand side
        float unitsPerMeter = 1.0f;
        float heightScale = 1.0f;
        bool valid = false;
    };

    inline BodyFrame MakeFrame(const Vector& head, float yawDeg, bool leftHanded,
        float unitsPerMeter, float heightScale)
    {
        BodyFrame frame{};
        if (!l4d2vr_interaction::Finite(head) || !std::isfinite(yawDeg) ||
            !std::isfinite(unitsPerMeter) || unitsPerMeter <= 0.001f ||
            !std::isfinite(heightScale) || heightScale <= 0.1f)
            return frame;
        const float radians = yawDeg * kPi / 180.0f;
        frame.head = head;
        frame.forward = Vector(std::cos(radians), std::sin(radians), 0.0f);
        // Source is right-handed with +y to the left; right = forward x up.
        const Vector right(frame.forward.y, -frame.forward.x, 0.0f);
        frame.dominant = leftHanded ? right * -1.0f : right;
        frame.unitsPerMeter = unitsPerMeter;
        frame.heightScale = heightScale;
        frame.root = head + Vector(0.0f, 0.0f, -0.75f * unitsPerMeter * heightScale);
        frame.headForward = frame.forward;
        frame.headDominant = frame.dominant;
        frame.valid = true;
        return frame;
    }

    // Pavlov's look-down curve: 0 above 20 deg down, easing to the full pull
    // back at 90 deg down.
    inline float LookDownBackFraction(float pitchDownDeg)
    {
        float t = (pitchDownDeg - 20.0f) / 70.0f;
        t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
        return t * t * (3.0f - 2.0f * t);
    }

    // Pavlov's "crouch" curve, keyed by the off hand's height above the floor:
    // 0 cm -> 60 cm back, 17.1 -> 26.5, 25 and above -> 0 (fraction of 60 cm).
    inline float ReachFloorBackFraction(float handHeightMeters)
    {
        const float h = handHeightMeters * 100.0f;
        if (!std::isfinite(h) || h >= 25.0f)
            return 0.0f;
        if (h <= 0.0f)
            return 1.0f;
        if (h <= 17.1f)
            return (60.0f + (26.5f - 60.0f) * (h / 17.1f)) / 60.0f;
        return (26.5f * (1.0f - (h - 17.1f) / (25.0f - 17.1f))) / 60.0f;
    }

    // Places the inventory root and records the headset's own axes.
    // offHandHeightMeters < 0 means unknown (no floor push).
    inline void ApplyPavlovRoot(BodyFrame& frame, const Layout& layout, const Vector& headForward,
        const Vector& headRight, const Vector& headUp, bool leftHanded, float offHandHeightMeters)
    {
        if (!frame.valid)
            return;
        const float s = frame.unitsPerMeter;
        if (l4d2vr_interaction::Finite(headForward) && l4d2vr_interaction::Finite(headUp) &&
            l4d2vr_interaction::Finite(headRight))
        {
            frame.headForward = headForward;
            frame.headDominant = leftHanded ? headRight * -1.0f : headRight;
            frame.headUp = headUp;
        }
        Vector root = frame.head + Vector(0.0f, 0.0f, layout.rootDropMeters * s * frame.heightScale);
        // Looking down pulls the root back along the head's horizontal forward.
        Vector flat(frame.headForward.x, frame.headForward.y, 0.0f);
        const float flatLength = std::sqrt(flat.x * flat.x + flat.y * flat.y);
        const float pitchDown = std::atan2(-frame.headForward.z, flatLength > 1e-4f ? flatLength : 1e-4f) * 180.0f / kPi;
        flat = flatLength > 1e-3f ? flat * (1.0f / flatLength) : frame.forward;
        root = root - flat * (LookDownBackFraction(pitchDown) * layout.lookDownBackMeters * s);
        // Reaching to the floor with the off hand pushes it back (out of the way).
        if (offHandHeightMeters >= 0.0f)
            root = root - frame.forward * (ReachFloorBackFraction(offHandHeightMeters) * layout.reachFloorBackMeters * s);
        frame.root = root;
    }

    inline Vector RootToWorld(const BodyFrame& frame, const Vector& localMeters)
    {
        const float s = frame.unitsPerMeter;
        return frame.root + frame.forward * (localMeters.x * s) + frame.dominant * (localMeters.y * s) +
            Vector(0.0f, 0.0f, localMeters.z * s * frame.heightScale);
    }

    inline Vector HeadToWorld(const BodyFrame& frame, const Vector& localMeters)
    {
        const float s = frame.unitsPerMeter;
        return frame.head + frame.headForward * (localMeters.x * s) + frame.headDominant * (localMeters.y * s) +
            frame.headUp * (localMeters.z * s);
    }

    inline Vector ToWorld(const BodyFrame& frame, const Vector& localMeters)
    {
        const float s = frame.unitsPerMeter;
        return frame.head + frame.forward * (localMeters.x * s) + frame.dominant * (localMeters.y * s) +
            Vector(0.0f, 0.0f, localMeters.z * s * frame.heightScale);
    }

    inline Vector ToLocal(const BodyFrame& frame, const Vector& world)
    {
        const Vector d = world - frame.head;
        const float s = frame.unitsPerMeter;
        return Vector(DotProduct(d, frame.forward) / s, DotProduct(d, frame.dominant) / s,
            d.z / (s * frame.heightScale));
    }

    inline bool IsBackZone(Zone zone)
    {
        return zone == Zone::Primary || zone == Zone::Medical;
    }

    // The other half of the back sphere.
    inline Zone OtherBackZone(Zone zone)
    {
        return zone == Zone::Primary ? Zone::Medical : (zone == Zone::Medical ? Zone::Primary : Zone::None);
    }

    // The off-hand hip volume (Pavlov's other hip quick slot): the magazine pouch.
    inline Vector OffHipCenter(const Layout& layout)
    {
        return Vector(layout.hipCenter.x, -layout.hipCenter.y, layout.hipCenter.z);
    }

    // Where a hand reaches for a zone (over the zone's shoulder, the hip
    // capsule centre, or the pocketed item).
    inline Vector ZoneCenterWorld(const BodyFrame& frame, const Layout& layout, Zone zone)
    {
        switch (zone)
        {
        case Zone::Primary: return HeadToWorld(frame, layout.backReachHead);
        case Zone::Medical:
            return HeadToWorld(frame, Vector(layout.backReachHead.x, -layout.backReachHead.y, layout.backReachHead.z));
        case Zone::Secondary: return RootToWorld(frame, layout.hipCenter);
        default: return RootToWorld(frame, layout.socket[SlotIndex(zone) < 0 ? 0 : SlotIndex(zone)]);
        }
    }

    // How close (metres) the hand's centre must be to the zone's geometry
    // (back: the sphere centre; hip: the capsule axis; chest: the item).
    inline float ZoneReachMeters(const Layout& layout, Zone zone)
    {
        switch (zone)
        {
        case Zone::Primary:
        case Zone::Medical: return layout.backRadiusMeters + layout.handRadiusMeters;
        case Zone::Secondary: return layout.hipRadiusMeters + layout.handRadiusMeters;
        default: return layout.pocketReachMeters;
        }
    }

    // Pavlov's body yaw: interpolates toward the head yaw every frame (UE
    // RInterpTo, speed 20 per second), no dead band; turns apply instantly.
    inline float FollowBodyYaw(float previousYaw, bool previousValid, float headYaw, float turnDelta,
        float dt, float speed)
    {
        if (!std::isfinite(headYaw))
            return previousValid ? l4d2vr_interaction::WrapYaw(previousYaw + turnDelta) : 0.0f;
        if (!previousValid || !std::isfinite(previousYaw))
            return l4d2vr_interaction::WrapYaw(headYaw);
        float yaw = l4d2vr_interaction::WrapYaw(previousYaw + (std::isfinite(turnDelta) ? turnDelta : 0.0f));
        const float error = l4d2vr_interaction::WrapYaw(headYaw - yaw);
        const float alpha = (!std::isfinite(dt) || dt <= 0.0f) ? 0.0f : std::fmin(1.0f, dt * std::fmax(0.0f, speed));
        return l4d2vr_interaction::WrapYaw(yaw + error * alpha);
    }

    // Smoothed body yaw. Inside the dead band the body keeps its yaw; beyond it
    // the body approaches the head yaw exponentially. Turn offsets (stick/snap
    // turn) are applied immediately so the holsters never lag a turn.
    inline float SmoothBodyYaw(float previousYaw, bool previousValid, float headYaw, float turnDelta,
        float dt, float deadbandDeg, float smoothingSeconds)
    {
        if (!std::isfinite(headYaw))
            return previousValid ? l4d2vr_interaction::WrapYaw(previousYaw + turnDelta) : 0.0f;
        if (!previousValid || !std::isfinite(previousYaw))
            return l4d2vr_interaction::WrapYaw(headYaw);
        float yaw = l4d2vr_interaction::WrapYaw(previousYaw + (std::isfinite(turnDelta) ? turnDelta : 0.0f));
        const float error = l4d2vr_interaction::WrapYaw(headYaw - yaw);
        const float band = std::fmax(0.0f, deadbandDeg);
        const float excess = std::fabs(error) - band;
        if (excess <= 0.0f)
            return yaw;
        const float step = (!std::isfinite(dt) || dt <= 0.0f) ? 0.0f :
            (smoothingSeconds <= 0.0001f ? 1.0f : 1.0f - std::exp(-std::fmin(dt, 0.25f) / smoothingSeconds));
        // Pull the yaw toward the edge of the dead band; a very large error
        // (e.g. turning around in room scale) snaps the remaining excess.
        const float pull = excess > 120.0f ? excess : excess * step;
        yaw += error > 0.0f ? pull : -pull;
        return l4d2vr_interaction::WrapYaw(yaw);
    }

    inline float HeadYawDegrees(const Vector& worldHeadForward, float fallback)
    {
        if (!l4d2vr_interaction::Finite(worldHeadForward) ||
            worldHeadForward.x * worldHeadForward.x + worldHeadForward.y * worldHeadForward.y < 0.0004f)
            return fallback;
        return std::atan2(worldHeadForward.y, worldHeadForward.x) * 180.0f / kPi;
    }

    inline float DistanceToSegment(const Vector& p, const Vector& a, const Vector& b)
    {
        const Vector ab = b - a;
        const float len2 = DotProduct(ab, ab);
        float t = len2 > 1e-9f ? DotProduct(p - a, ab) / len2 : 0.0f;
        t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
        const Vector d = p - (a + ab * t);
        return std::sqrt(DotProduct(d, d));
    }

    // Distance (metres) from the hand centre to an upright hip capsule's axis.
    inline float HipAxisDistance(const BodyFrame& frame, const Layout& layout, const Vector& rootCenter, const Vector& hand)
    {
        const float s = frame.unitsPerMeter;
        const float halfSegment = (layout.hipHalfHeightMeters - layout.hipRadiusMeters) * s * frame.heightScale;
        const Vector c = RootToWorld(frame, rootCenter);
        return DistanceToSegment(hand, c - Vector(0.0f, 0.0f, halfSegment), c + Vector(0.0f, 0.0f, halfSegment)) / s;
    }

    // The hand is at the off-hand hip (Pavlov's other hip quick slot, whose
    // empty-hand action spawns a magazine) or at the magazine pouch. Pavlov
    // tests the hips before the back (first overlap wins), so a hand there
    // never draws from the back.
    inline bool HandAtMagazinePouch(const BodyFrame& frame, const Layout& layout, const Vector& hand)
    {
        if (!frame.valid || !l4d2vr_interaction::Finite(hand))
            return false;
        if (HipAxisDistance(frame, layout, OffHipCenter(layout), hand) <= layout.hipRadiusMeters + layout.handRadiusMeters)
            return true;
        const Vector d = hand - RootToWorld(frame, layout.magPouch);
        return std::sqrt(DotProduct(d, d)) / frame.unitsPerMeter <= layout.magPouchReachMeters;
    }

    // Signed distance (metres) of the hand from the back split plane, positive
    // toward the dominant shoulder (head frame, so it follows head roll).
    inline float BackLateralMeters(const BodyFrame& frame, const Layout& layout, const Vector& hand)
    {
        const Vector c = HeadToWorld(frame, layout.backCenterHead);
        return DotProduct(hand - c, frame.headDominant) / frame.unitsPerMeter;
    }

    // Taking from a zone: the hand's grab sphere overlaps the back sphere's
    // half for that zone or the hip capsule, or reaches the item in its chest
    // pocket. wholeBack: the back zone owns the whole sphere (the other half is
    // empty; used for putting an item back). outDistanceMeters is the hand
    // centre's distance to the zone's geometry; in the other back half it is
    // pushed past the reach.
    inline bool HandFitsZone(const BodyFrame& frame, const Layout& layout, Zone zone, const Vector& hand,
        float& outDistanceMeters, bool wholeBack = false)
    {
        outDistanceMeters = 1.0e9f;
        if (!frame.valid || zone == Zone::None || !l4d2vr_interaction::Finite(hand))
            return false;
        const float s = frame.unitsPerMeter;
        float distance = 1.0e9f;
        float reach = 0.0f;
        if (zone == Zone::Secondary)
        {
            distance = HipAxisDistance(frame, layout, layout.hipCenter, hand);
            reach = layout.hipRadiusMeters + layout.handRadiusMeters;
        }
        else if (IsBackZone(zone))
        {
            const Vector d = hand - HeadToWorld(frame, layout.backCenterHead);
            distance = std::sqrt(DotProduct(d, d)) / s;
            reach = ZoneReachMeters(layout, zone);
            const float lateral = BackLateralMeters(frame, layout, hand);
            const bool ownHalf = zone == Zone::Primary ? lateral >= 0.0f : lateral < 0.0f;
            if (!ownHalf && !wholeBack)
            {
                outDistanceMeters = std::fmax(distance, reach + std::fabs(lateral) + 1e-4f);
                return false;
            }
        }
        else
        {
            const Vector c = ZoneCenterWorld(frame, layout, zone);
            const Vector d = hand - c;
            distance = std::sqrt(DotProduct(d, d)) / s;
            reach = ZoneReachMeters(layout, zone);
        }
        outDistanceMeters = distance;
        return std::isfinite(distance) && distance <= reach;
    }

    // The hand is in, or within marginMeters of, any holster zone (occupied or
    // not). Motion gestures (gun bash) ignore hands at the holsters.
    inline bool NearAnyZone(const BodyFrame& frame, const Layout& layout, const Vector& hand, float marginMeters)
    {
        for (int i = 0; i < kZoneCount; ++i)
        {
            const Zone zone = static_cast<Zone>(i);
            float distance = 0.0f;
            HandFitsZone(frame, layout, zone, hand, distance, true);
            if (distance <= ZoneReachMeters(layout, zone) + marginMeters)
                return true;
        }
        return false;
    }

    // Putting the held item away: chest items go back when the hand is in the
    // vest; the hip uses the same volume as taking. A back item goes back in
    // its own half of the back sphere, or anywhere in it while the other half
    // is empty (Pavlov's back is one slot). occupiedMask as in FindZone.
    inline bool HandInHolsterVolume(const BodyFrame& frame, const Layout& layout, Zone zone, const Vector& hand,
        std::uint32_t occupiedMask = 0x1Fu)
    {
        if (!frame.valid || zone == Zone::None || !l4d2vr_interaction::Finite(hand))
            return false;
        if (zone == Zone::Throwable || zone == Zone::Consumable)
        {
            const float s = frame.unitsPerMeter;
            const Vector c = RootToWorld(frame, layout.vestCenter);
            const Vector axis = frame.forward * layout.vestAxis.x + frame.dominant * layout.vestAxis.y +
                Vector(0.0f, 0.0f, layout.vestAxis.z);
            const float halfSegment = (layout.vestHalfHeightMeters - layout.vestRadiusMeters) * s;
            const float distance = DistanceToSegment(hand, c - axis * halfSegment, c + axis * halfSegment) / s;
            return distance <= layout.vestRadiusMeters + layout.handRadiusMeters;
        }
        const Zone other = OtherBackZone(zone);
        const bool wholeBack = other != Zone::None && (occupiedMask & (1u << SlotIndex(other))) == 0u;
        float distance = 0.0f;
        return HandFitsZone(frame, layout, zone, hand, distance, wholeBack);
    }

    // The zone the hand takes from (occupiedMask bit i = slot i holds an item;
    // 0x1F ignores occupancy). Order, first match wins:
    //  1. the nearest pocketed chest item within reach (the hand touches it;
    //     the small pocket reach must beat the big back sphere when looking up),
    //  2. the dominant hip, then the off-hand hip, which is the magazine pouch
    //     and takes nothing here (Pavlov: hips before the back),
    //  3. the half of the back sphere the hand is in. The halves never overlap:
    //     an empty half takes nothing, so a reach for the rifle never draws the kit.
    inline Zone FindZone(const BodyFrame& frame, const Layout& layout, const Vector& hand,
        std::uint32_t occupiedMask, float* outDistanceMeters = nullptr)
    {
        if (outDistanceMeters)
            *outDistanceMeters = 1.0e9f;
        Zone best = Zone::None;
        float bestDistance = 1.0e9f;
        static const Zone kPockets[] = { Zone::Throwable, Zone::Consumable };
        for (Zone zone : kPockets)
        {
            if ((occupiedMask & (1u << SlotIndex(zone))) == 0u)
                continue;
            float distance = 0.0f;
            if (HandFitsZone(frame, layout, zone, hand, distance) && distance < bestDistance)
            {
                bestDistance = distance;
                best = zone;
            }
        }
        if (best != Zone::None)
        {
            if (outDistanceMeters)
                *outDistanceMeters = bestDistance;
            return best;
        }
        float distance = 0.0f;
        if ((occupiedMask & (1u << SlotIndex(Zone::Secondary))) != 0u &&
            HandFitsZone(frame, layout, Zone::Secondary, hand, distance))
        {
            if (outDistanceMeters)
                *outDistanceMeters = distance;
            return Zone::Secondary;
        }
        if (HandAtMagazinePouch(frame, layout, hand))
            return Zone::None;
        for (Zone zone : { Zone::Primary, Zone::Medical })
        {
            if ((occupiedMask & (1u << SlotIndex(zone))) != 0u && HandFitsZone(frame, layout, zone, hand, distance))
            {
                if (outDistanceMeters)
                    *outDistanceMeters = distance;
                return zone;
            }
        }
        return Zone::None;
    }

    // World placement for a holstered item. The returned basis is for the item's
    // reference bone, ValveBiped.weapon_bone (on the v_ models that the local
    // player's carried weapons report, and on the w_ models): axisZ = the
    // bone's local +Z (barrel), axisY = local +Y (sights; the top of kits,
    // bottles and grenades), axisX = Y x Z (a gun's left side). Melee blades
    // run along local +Y, so they use a separate preset. Measured from the
    // stock v_ models: the first aid kit and defibrillator are thin along X
    // with the detailed face on +X, the upgrade packs thin along Z.
    struct ItemPose
    {
        Vector origin{ 0.0f, 0.0f, 0.0f };
        Vector axisX{ 1.0f, 0.0f, 0.0f };
        Vector axisY{ 0.0f, 1.0f, 0.0f };
        Vector axisZ{ 0.0f, 0.0f, 1.0f };
        bool valid = false;
    };

    enum class ItemShape : int
    {
        LongGun = 0,   // barrel on local +Z
        Pistol = 1,    // barrel on local +Z
        Melee = 2,     // blade on local +Y
        Equipment = 3, // pills, adrenaline, grenades: upright (+Y up)
        Kit = 4,       // first aid kit: upright, face (+X) out from the body
        Pack = 5,      // upgrade packs: upright, flat side (+Z) out from the body
        Defib = 6,     // defibrillator: like the kit, mesh off-centre on the bone
        Count = 7
    };

    constexpr int kItemShapeCount = static_cast<int>(ItemShape::Count);

    // The holster shape of a weapon id (C_WeaponCSBase::WeaponID values).
    inline ItemShape ItemShapeForWeaponId(int id)
    {
        switch (id)
        {
        case 1: case 32: return ItemShape::Pistol;                 // pistol, magnum
        case 19: case 20: return ItemShape::Melee;                 // melee, chainsaw
        case 12: return ItemShape::Kit;                            // first aid kit
        case 24: return ItemShape::Defib;
        case 30: case 31: return ItemShape::Pack;                  // upgrade packs
        case 13: case 14: case 15: case 23: case 25: return ItemShape::Equipment; // grenades, pills, adrenaline
        default: return ItemShape::LongGun;
        }
    }

    inline Vector Normalized(const Vector& v, const Vector& fallback)
    {
        const float length = std::sqrt(DotProduct(v, v));
        if (!std::isfinite(length) || length < 0.0001f)
            return fallback;
        return v * (1.0f / length);
    }

    // The visible centre of an item relative to its reference bone, in model
    // units along the bone's axes (stock models: rifles and shotguns reach about
    // 32 units forward of the bone and 15 behind it, pistols 8-15 forward, the
    // defibrillator sits 3 units along X and 7 along Z; kits, packs, bottles
    // and grenades are centred on it).
    inline Vector ReferenceToCenterUnits(ItemShape shape)
    {
        switch (shape)
        {
        case ItemShape::LongGun: return Vector(0.0f, 0.0f, 9.0f);
        case ItemShape::Pistol: return Vector(0.0f, 0.0f, 5.0f);
        case ItemShape::Defib: return Vector(3.0f, -0.5f, 6.6f);
        default: return Vector(0.0f, 0.0f, 0.0f);
        }
    }

    inline ItemPose BuildItemPose(const BodyFrame& frame, const Layout& layout, Zone zone, ItemShape shape)
    {
        ItemPose pose{};
        if (!frame.valid || zone == Zone::None || SlotIndex(zone) < 0 || SlotIndex(zone) >= kZoneCount)
            return pose;
        const Vector up(0.0f, 0.0f, 1.0f);
        const Vector f = frame.forward;
        const Vector d = frame.dominant;
        const bool back = IsBackZone(zone);
        // Out of the body: forward on the chest and hips, backward on the back.
        const Vector out = back ? f * -1.0f : f;
        Vector axisY = up;
        Vector axisZ = f;
        switch (shape)
        {
        case ItemShape::LongGun:
            if (back)
            {
                // Pavlov's back socket (slot2): upright, muzzle up and leaning
                // toward the dominant shoulder, sights toward the dominant side,
                // the gun's flat side against the back.
                axisZ = Normalized(f * -0.09f + d * 0.14f + up * 0.99f, up);
                axisY = Normalized(d * 0.99f + up * -0.14f, d);
                break;
            }
            // A long gun anywhere else hangs like a pistol.
            [[fallthrough]];
        case ItemShape::Pistol:
            // Pavlov's hip sockets (slot0/1): muzzle down, canted 23 degrees
            // forward, slide toward the front; the grip sticks up and back.
            axisZ = Normalized(f * 0.40f + up * -0.92f, up * -1.0f);
            axisY = Normalized(f * 0.92f + up * 0.40f, f);
            break;
        case ItemShape::Melee:
            // Blade (local +Y) down and back, local +Z toward the dominant side.
            axisY = Normalized(up * -0.85f + f * -0.5f, up * -1.0f);
            axisZ = Normalized(d, Vector(0.0f, -1.0f, 0.0f));
            break;
        case ItemShape::Pack:
            // Upright, flat side against the body.
            axisY = up;
            axisZ = out;
            break;
        case ItemShape::Equipment:
        case ItemShape::Kit:
        case ItemShape::Defib:
        default:
        {
            // Upright (Pavlov's pocket sockets are unrotated), face out of the
            // body: X = out, Y = up, so Z = X x Y.
            axisY = up;
            CrossProduct(out, up, axisZ);
            axisZ = Normalized(axisZ, d);
            break;
        }
        }

        // Re-orthonormalize (Y against Z), then X = Y x Z for a right-handed basis.
        axisZ = Normalized(axisZ, up);
        axisY = Normalized(axisY - axisZ * DotProduct(axisY, axisZ), f);
        Vector axisX{};
        CrossProduct(axisY, axisZ, axisX);
        axisX = Normalized(axisX, d);

        // The item's visible centre goes to the rest socket.
        const Vector centerUnits = ReferenceToCenterUnits(shape);
        pose.origin = RootToWorld(frame, layout.socket[SlotIndex(zone)]) -
            (axisX * centerUnits.x + axisY * centerUnits.y + axisZ * centerUnits.z);
        pose.axisX = axisX;
        pose.axisY = axisY;
        pose.axisZ = axisZ;
        pose.valid = l4d2vr_interaction::Finite(pose.origin) && l4d2vr_interaction::Finite(pose.axisX);
        return pose;
    }

    // Paces a diagnostic log stream: a line is due on the first call, when the
    // state key changed (at most once per minIntervalMs), or after periodMs
    // without a line.
    struct DiagPacer
    {
        std::uint64_t lastMs = 0;
        std::uint64_t lastKey = 0;
        bool started = false;

        bool Due(std::uint64_t nowMs, std::uint64_t key, std::uint64_t minIntervalMs, std::uint64_t periodMs)
        {
            const std::uint64_t elapsed = nowMs - lastMs;
            const bool due = !started || (key != lastKey && elapsed >= minIntervalMs) || elapsed >= periodMs;
            if (!due)
                return false;
            started = true;
            lastMs = nowMs;
            lastKey = key;
            return true;
        }
    };

    // Grab / holster state machine for the dominant hand.
    struct GrabInput
    {
        bool gripDown = false;
        bool ready = false;            // local player can act (not incapped, not pinned, in game)
        Zone handZone = Zone::None;    // zone under the hand, restricted to occupied slots
        Zone ownZoneUnderHand = Zone::None; // zone under the hand ignoring occupancy (for holstering)
        int activeSlot = -1;           // 0-based slot of the active weapon, -1 unknown
        std::uint32_t occupiedMask = 0u;
    };

    struct GrabOutput
    {
        int selectSlot = -1;   // issue slotN (0-based) to draw this item
        int holsterTo = -1;    // switch to this slot because the held item was holstered
        Zone hoverEntered = Zone::None; // haptic tick
        bool drawing = false;  // an item was drawn and the grip is still held
    };

    class GrabFsm
    {
    public:
        GrabOutput Update(const GrabInput& in)
        {
            GrabOutput out{};
            if (!in.ready)
            {
                m_Holding = false;
                m_WasDown = in.gripDown;
                m_Hover = Zone::None;
                return out;
            }
            if (in.handZone != m_Hover)
            {
                m_Hover = in.handZone;
                if (m_Hover != Zone::None && !in.gripDown)
                    out.hoverEntered = m_Hover;
            }
            const bool pressed = in.gripDown && !m_WasDown;
            const bool released = !in.gripDown && m_WasDown;
            m_WasDown = in.gripDown;

            if (pressed && in.handZone != Zone::None)
            {
                const int slot = SlotIndex(in.handZone);
                m_Holding = true;
                m_HeldSlot = slot;
                if (slot != in.activeSlot)
                    out.selectSlot = slot;
            }
            else if (pressed && in.activeSlot >= 0)
            {
                // Grip pressed away from the body while already holding an item:
                // treat it as holding the current item so release-into-holster works.
                m_Holding = true;
                m_HeldSlot = in.activeSlot;
            }
            if (released && m_Holding)
            {
                m_Holding = false;
                // Releasing the held item inside its own holster puts it away.
                if (in.ownZoneUnderHand != Zone::None && SlotIndex(in.ownZoneUnderHand) == m_HeldSlot &&
                    m_HeldSlot == in.activeSlot)
                {
                    out.holsterTo = NextSlotAfterHolster(m_HeldSlot, in.occupiedMask);
                }
                m_HeldSlot = -1;
            }
            out.drawing = m_Holding;
            return out;
        }

        static int NextSlotAfterHolster(int holsteredSlot, std::uint32_t occupiedMask)
        {
            // Swap primary and secondary first, then fall back to any other weapon slot.
            const int preferred = holsteredSlot == 0 ? 1 : (holsteredSlot == 1 ? 0 : 1);
            if ((occupiedMask & (1u << preferred)) != 0u)
                return preferred;
            for (int slot = 0; slot < 2; ++slot)
                if (slot != holsteredSlot && (occupiedMask & (1u << slot)) != 0u)
                    return slot;
            return -1;
        }

        bool Holding() const { return m_Holding; }
        int HeldSlot() const { return m_HeldSlot; }
        void Reset() { m_Holding = false; m_WasDown = false; m_HeldSlot = -1; m_Hover = Zone::None; }

    private:
        bool m_Holding = false;
        bool m_WasDown = false;
        int m_HeldSlot = -1;
        Zone m_Hover = Zone::None;
    };
}

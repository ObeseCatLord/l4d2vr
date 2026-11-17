#pragma once
#include "vr_interaction_geometry.h"
#include <cmath>
#include <cstdint>

// Body holsters placed the way Forefront (Triangle Factory, Unity) places its
// equipment (decoded from the game files; research/forefront/HOLSTERS.md). An
// equipment root hangs 0.60 m straight below the headset and takes the head's
// yaw every frame with no smoothing (Forefront's CharacterUpdateEquipmentRoot
// and CharacterRootRotation systems): there are no look-down or crouch curves.
// Every item hangs from its handle at its slot point (Forefront's
// VestGrabTarget), and a hand takes the nearest handle whose grab volume it is
// in (Forefront's GrabSystem). Stick/snap turns move everything instantly.
//
// Slot assignment, mirrored for the left-handed (Forefront's left-handed
// loadout mirrors the same way):
//   slot1 long gun     chest sling across the front (PrimaryWeaponTransform):
//                      the pistol grip on the dominant side of the belly, the
//                      barrel down toward the off-hand hip at 45 degrees.
//   slot2 pistol/melee dominant hip (SecondaryWeaponTransform), hanging
//                      barrel/blade down from the grip.
//   slot3 grenade      dominant upper chest (GrenadePouchTransform).
//   slot4 kit, defib,  on the back over the off shoulder blade (unchanged from
//         upgrade pack 2.0.4 at the user's request; the whole back is its
//                      grab volume now that the rifle is on the chest).
//   slot5 pills, adren off-hand hip (Forefront's second hip, its knife slot;
//                      L4D2 keeps melee weapons in slot2).
//   magazine pouch     off-hand upper chest (AmmoPouchTransform).
// The Pavlov root curves (lookDownBackMeters, reachFloorBackMeters) are kept
// in the code and set to zero here.
//
// 2.0.12 (review s12 issue 7): the slots have a grab-volume shape each
// (ZoneKind) and the whole placement is a data table per layout
// (MakeLayout: forefront, the default, and pavlov; HolsterLayout). The
// grenade and magazine pouch sit on Pavlov's top vest-pocket row (HMD - 0.31
// and - 0.33 m), only an EMPTY hand takes from the body (GrabInput::handEmpty)
// and the nearest grab wins with 2 cm hysteresis (FindZone's hovered zone).
namespace l4d2vr_holster
{
    constexpr float kPi = 3.14159265358979323846f;

    enum class Zone : int
    {
        Primary = 0,    // slot1, chest sling (the long gun)
        Secondary = 1,  // slot2, dominant hip (pistols and melee)
        Throwable = 2,  // slot3, dominant upper chest (grenades)
        Medical = 3,    // slot4, back, over the off shoulder (kit, defibrillator, upgrade pack)
        Consumable = 4, // slot5, off-hand hip (pills, adrenaline)
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

    // 2.0.12: the grab-volume shape of a slot, so a layout can put any slot
    // anywhere (the Pavlov layout carries the long gun on the back).
    enum class ZoneKind : int
    {
        Sling = 0,  // a long gun across the chest: capsule from its grip to its foregrip
        Hip = 1,    // an item hanging from its grip: capsule down from the socket
        Pocket = 2, // a chest/vest pocket: sphere at the socket
        Back = 3,   // the sphere behind the head (head frame), reached over a shoulder
    };

    // 2.0.12: selectable layouts (HolsterLayout). Adding one = one more table
    // in MakeLayout plus its name in LayoutKindFromName / LayoutKindName.
    enum class LayoutKind : int
    {
        Forefront = 0, // the default (research/forefront/HOLSTERS.md)
        Pavlov = 1,    // research/pavlov/inventory/SPEC.md
        Count = 2
    };

    // Metres, x forward, y toward the DOMINANT side, z up (mirrors for the
    // left-handed). "Root" offsets are from the equipment root (head + (0, 0,
    // rootDropMeters)); "head" offsets are in the headset's own frame. The
    // Forefront values are its Equipment-local transforms (Unity x right, y up,
    // z forward), with the L4D2 changes noted.
    struct Layout
    {
        float rootDropMeters = -0.60f;          // Forefront: Equipment = HMD - 0.6 m
        float lookDownBackMeters = 0.0f;        // Pavlov's look-down pull back (Forefront: none)
        float reachFloorBackMeters = 0.0f;      // Pavlov's floor-reach push back (Forefront: none)
        float handRadiusMeters = 0.115f;        // the hand's grab sphere (back sphere only)
        // Rest points (root offsets), one per slot: where the item's handle
        // sits. slot1 and slot3 are 2 and 4.5 cm further forward than
        // Forefront's (0.059 and 0.025): L4D2's rifles and grenades are
        // thicker than Forefront's flat pouches and sit in front of the
        // survivor's rendered chest. slot3 is 2.5 cm further toward the
        // dominant side (0.075) to keep 20 cm between the grenade and the
        // magazine pouch. slot4 keeps its 2.0.4 place on the back (head offset
        // (-0.25, -0.13, -0.39)).
        // 2.0.12 (review s12 issue 7): slot3 moved down from Forefront's
        // collar-bone pouch (0.07, 0.10, 0.40) = HMD - 0.20 m, where the gun
        // hand aims, to Pavlov's top vest row (HMD - 0.31 m).
        Vector socket[kZoneCount] = {
            Vector(0.08f, 0.05f, 0.1075f),      // slot1 sling: the pistol grip (Forefront (0.059, 0.05, 0.1075))
            Vector(-0.025f, 0.225f, 0.0f),      // slot2 dominant hip: the grip (Forefront secondary)
            Vector(0.05f, 0.15f, 0.29f),        // slot3 dominant chest, Pavlov's top pocket row (HMD - 0.31)
            Vector(-0.25f, -0.13f, 0.21f),      // slot4 back, off shoulder blade (2.0.4)
            Vector(-0.025f, -0.225f, 0.0f),     // slot5 off-hand hip (Forefront's second hip)
        };
        // The grab-volume shape of each slot.
        ZoneKind kind[kZoneCount] = { ZoneKind::Sling, ZoneKind::Hip, ZoneKind::Pocket, ZoneKind::Back, ZoneKind::Hip };
        LayoutKind layoutKind = LayoutKind::Forefront;
        // Chest sling (slot1): the gun runs from the pistol grip down toward the
        // off-hand hip at 45 degrees in the frontal plane, sights up toward the
        // off shoulder, its left side against the body (Forefront's primary
        // rotation). The grab volume is the gun from the grip to the foregrip.
        float slingHandleToForegripMeters = 0.28f;
        float slingHandleToCenterMeters = 0.12f; // the gun's visible centre along the barrel
        float slingReachMeters = 0.15f;          // hand centre to the gun body (ChestSlingGrabRadiusMeters)
        // Hips (slot2, slot5): the item hangs down from its grip; the grab
        // volume is a capsule along it (Forefront grabs the item's handle box,
        // about 15 cm half extents).
        float hipItemLengthMeters = 0.22f;
        float hipReachMeters = 0.17f;
        // Chest pocket (slot3): hand centre to the item (2.0.12: 0.15 -> 0.12,
        // review s12 7.2).
        float pocketReachMeters = 0.12f;
        // Back (slot4): sphere behind the head, in the head's frame (follows
        // pitch too). The whole sphere is slot4's.
        Vector backCenterHead{ -0.4414f, 0.0f, -0.0948f };
        float backRadiusMeters = 0.40f;
        // The back is reached over a shoulder: the hand is no further forward
        // than this and no lower than this (body-yaw frame, from the head).
        // Without these the head-frame sphere swings down over the belly and
        // hips when looking up.
        float backMaxForwardMeters = 0.05f;
        float backMinUpMeters = -0.40f;
        // A hand reaching over the off shoulder (head frame). Only a reach
        // target for self-tests and diagnostics: the sphere decides.
        Vector backReachHead{ -0.12f, -0.15f, -0.08f };
        // The magazine pouch on the off-hand upper chest (Forefront's ammo
        // pouch (0.032, -0.075, 0.3175); forward and outward like slot3). The
        // holsters leave a hand there to the magazines.
        // 2.0.12: on Pavlov's top pocket row too (HMD - 0.33 m), 30 cm from the
        // grenade (was (0.07, -0.10, 0.32) = HMD - 0.28 m).
        Vector magPouch{ 0.06f, -0.15f, 0.27f };
        float magPouchReachMeters = 0.15f;
        // 2.0.9: an item hanging on the body is grabbed anywhere on its drawn
        // body (an oriented box around the model in its holstered pose,
        // ItemBodyBox), not only at its handle: the hand's centre within this
        // of the box surface. The handle volumes above still count too.
        float itemGrabMarginMeters = 0.07f;
        // 2.0.12: the margin around a small pocket item (grenade, pills in a
        // pocket): 0.07 -> 0.03 (review s12 7.2).
        float pocketItemGrabMarginMeters = 0.03f;
        // 2.0.12: nearest grab wins with this much hysteresis toward the item
        // the hand already hovers (review-2.0.10 B23/B58), so two neighbours
        // never flicker.
        float nearestHysteresisMeters = 0.02f;
        // Pavlov's hand grab sphere (GrabVolume r 11.5 cm).
        float handGrabSphereMeters = 0.115f;
        // 2.0.14 (pavlov layout, research/pavlov/handling SPEC 7): Pavlov's hip
        // volumes. Each hip is a capsule (radius, half-height with its caps) at
        // hipVolumeCenter (root offset, dominant side; mirrored for the off
        // hip) tested against the hand's grab sphere (~31.5 cm reach). Both
        // hips are magazine pouches for the gun in the other hand unless the
        // sidearm hangs there, and the slot2 item may sit on either hip.
        bool pavlovHips = false;
        Vector hipVolumeCenter{ -0.225f, 0.352f, 0.06f };
        float hipCapsuleRadiusMeters = 0.20f;
        float hipCapsuleHalfHeightMeters = 0.44f;
        // Runtime (pavlov hips): the slot2 item was put on the off-hand hip.
        bool secondaryOffHip = false;
        // Body yaw follow speed when HolsterYawFollowSpeed is 0 (forefront:
        // the head yaw every frame; pavlov: RInterpTo 20).
        float defaultYawFollowSpeed = 0.0f;
    };

    inline const char* LayoutKindName(LayoutKind kind)
    {
        switch (kind)
        {
        case LayoutKind::Pavlov: return "pavlov";
        default: return "forefront";
        }
    }

    // Config value -> layout ("forefront", the default, for anything unknown).
    inline LayoutKind LayoutKindFromName(const char* name, bool* known = nullptr)
    {
        auto eq = [](const char* a, const char* b)
        {
            if (!a) return false;
            for (; *a && *b; ++a, ++b)
            {
                char ca = *a, cb = *b;
                if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca - 'A' + 'a');
                if (ca != cb) return false;
            }
            return *a == '\0' && *b == '\0';
        };
        if (known) *known = true;
        if (eq(name, "forefront")) return LayoutKind::Forefront;
        if (eq(name, "pavlov")) return LayoutKind::Pavlov;
        if (known) *known = false;
        return LayoutKind::Forefront;
    }

    // The data table of each layout. Root = HMD + (0, 0, rootDropMeters);
    // sockets are root offsets (forward, dominant, up). A new layout is one
    // more case here.
    inline Layout MakeLayout(LayoutKind kind)
    {
        Layout layout{};
        layout.layoutKind = kind;
        if (kind != LayoutKind::Pavlov)
            return layout;   // Forefront (the struct defaults above)

        // Pavlov VR (research/pavlov/inventory/SPEC.md sections 2-5): the
        // inventory root hangs 0.75 m below the HMD with the look-down (8 cm)
        // and floor-reach (60 cm) push-backs; the rifle goes on the back
        // (QuickSlot_Back, the sphere behind the head), pistols on the hips,
        // grenades and kits in vest pockets at HMD - 0.31 .. - 0.54 m, the
        // magazine pouch on the off-hand hip. HMD-relative numbers below are
        // Pavlov's socket positions; z = HMD-relative + 0.75.
        layout.rootDropMeters = -0.75f;
        layout.lookDownBackMeters = 0.08f;
        layout.reachFloorBackMeters = 0.60f;
        layout.kind[0] = ZoneKind::Back;     // slot1 long gun: the back
        layout.kind[1] = ZoneKind::Hip;      // slot2 dominant hip
        layout.kind[2] = ZoneKind::Pocket;   // slot3 grenade: dominant top pocket
        layout.kind[3] = ZoneKind::Pocket;   // slot4 kit: chest centre, low
        layout.kind[4] = ZoneKind::Pocket;   // slot5 pills: off-hand middle pocket
        layout.socket[0] = Vector(-0.298f, 0.0f, 0.304f);   // back item rest (-0.298, 0, -0.446)
        layout.socket[1] = Vector(-0.061f, 0.25f, 0.227f);  // hip item rest (-0.061, +-0.25, -0.523)
        layout.socket[2] = Vector(0.024f, 0.146f, 0.441f);  // top pocket (0.024, 0.146, -0.309)
        layout.socket[3] = Vector(0.081f, 0.0f, 0.238f);    // chest centre-low (0.081, 0, -0.512)
        layout.socket[4] = Vector(0.054f, -0.144f, 0.325f); // middle pocket (0.054, -0.144, -0.425)
        layout.magPouch = Vector(-0.023f, -0.187f, 0.19f);  // off-hand ammo pouch (-0.023, -0.187, -0.560)
        layout.hipReachMeters = 0.17f;
        // 2.0.14: Pavlov's hip capsules (r 20 cm, half-height 44 cm at HMD
        // (-0.225, +-0.352, -0.69)) against the 11.5 cm hand sphere; both hips
        // are pouches; the body follows the head yaw at RInterpTo 20.
        // (The game turns pavlovHips off while Classic mode is active, so
        // Classic keeps the 2.0.13 pavlov layout.)
        layout.pavlovHips = true;
        layout.hipVolumeCenter = Vector(-0.225f, 0.352f, 0.06f);
        layout.defaultYawFollowSpeed = 20.0f;
        layout.pocketReachMeters = 0.12f;
        layout.backRadiusMeters = 0.40f;                    // QuickSlot_Back r 40 at head-local (-0.441, 0, -0.095)
        return layout;
    }

    struct BodyFrame
    {
        Vector head{ 0.0f, 0.0f, 0.0f };
        Vector root{ 0.0f, 0.0f, 0.0f };        // equipment root (set by ApplyPavlovRoot)
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
        frame.root = head + Vector(0.0f, 0.0f, -0.60f * unitsPerMeter * heightScale);
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

    // Places the equipment root and records the headset's own axes. The Pavlov
    // curves only act when the layout sets them (the Forefront layout does
    // not). offHandHeightMeters < 0 means unknown (no floor push).
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
        if (layout.lookDownBackMeters != 0.0f)
            root = root - flat * (LookDownBackFraction(pitchDown) * layout.lookDownBackMeters * s);
        // Reaching to the floor with the off hand pushes it back (out of the way).
        if (offHandHeightMeters >= 0.0f && layout.reachFloorBackMeters != 0.0f)
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

    inline ZoneKind KindOf(const Layout& layout, Zone zone)
    {
        const int slot = SlotIndex(zone);
        return slot >= 0 && slot < kZoneCount ? layout.kind[slot] : ZoneKind::Pocket;
    }

    inline bool IsBackZone(const Layout& layout, Zone zone) { return KindOf(layout, zone) == ZoneKind::Back; }
    inline bool IsHipZone(const Layout& layout, Zone zone) { return KindOf(layout, zone) == ZoneKind::Hip; }
    inline bool IsSlingZone(const Layout& layout, Zone zone) { return KindOf(layout, zone) == ZoneKind::Sling; }

    inline Vector Normalized(const Vector& v, const Vector& fallback)
    {
        const float length = std::sqrt(DotProduct(v, v));
        if (!std::isfinite(length) || length < 0.0001f)
            return fallback;
        return v * (1.0f / length);
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

    // 2.0.14: a slot's rest socket; the slot2 item on the off-hand hip (pavlov
    // hips, secondaryOffHip) mirrors the dominant hip's.
    inline Vector SocketOf(const Layout& layout, Zone zone)
    {
        const int slot = SlotIndex(zone);
        Vector socket = layout.socket[slot < 0 || slot >= kZoneCount ? 0 : slot];
        if (zone == Zone::Secondary && layout.pavlovHips && layout.secondaryOffHip)
            socket.y = -socket.y;
        return socket;
    }

    // 2.0.14 (pavlov hips): the capsule's segment of a hip (offSide: the off
    // hand's hip), world units.
    inline void HipCapsuleSegment(const BodyFrame& frame, const Layout& layout, bool offSide, Vector& top, Vector& bottom)
    {
        const Vector c(layout.hipVolumeCenter.x, offSide ? -layout.hipVolumeCenter.y : layout.hipVolumeCenter.y,
            layout.hipVolumeCenter.z);
        const Vector center = RootToWorld(frame, c);
        const float half = (std::fmax)(0.0f, layout.hipCapsuleHalfHeightMeters - layout.hipCapsuleRadiusMeters) *
            frame.unitsPerMeter * frame.heightScale;
        top = center + Vector(0.0f, 0.0f, half);
        bottom = center - Vector(0.0f, 0.0f, half);
    }

    // Hand centre to a hip capsule's axis (metres); within reach when <=
    // radius + the hand's grab sphere.
    inline float HipCapsuleDistanceMeters(const BodyFrame& frame, const Layout& layout, bool offSide, const Vector& hand)
    {
        if (!frame.valid || !l4d2vr_interaction::Finite(hand))
            return 1.0e9f;
        Vector top{}, bottom{};
        HipCapsuleSegment(frame, layout, offSide, top, bottom);
        return DistanceToSegment(hand, top, bottom) / frame.unitsPerMeter;
    }

    inline float HipCapsuleReachMeters(const Layout& layout)
    {
        return layout.hipCapsuleRadiusMeters + layout.handGrabSphereMeters;
    }

    // The slung long gun's axes (world): barrel (Z) down toward the off-hand
    // hip, sights (Y) up toward the off shoulder, X = Y x Z = the gun's left
    // side, against the body.
    inline void SlingAxes(const BodyFrame& frame, Vector& axisX, Vector& axisY, Vector& axisZ)
    {
        const Vector up(0.0f, 0.0f, 1.0f);
        const Vector d = frame.dominant;
        axisZ = Normalized(d * -0.7071f + up * -0.7071f, up * -1.0f);
        axisY = Normalized(d * -0.7071f + up * 0.7071f, up);
        CrossProduct(axisY, axisZ, axisX);
        axisX = Normalized(axisX, frame.forward * -1.0f);
    }

    // The slung gun's handle (pistol grip) and foregrip (world).
    inline void SlingHandle(const BodyFrame& frame, const Layout& layout, Vector& handle, Vector& foregrip)
    {
        Vector x{}, y{}, z{};
        SlingAxes(frame, x, y, z);
        handle = RootToWorld(frame, layout.socket[0]);
        foregrip = handle + z * (layout.slingHandleToForegripMeters * frame.unitsPerMeter);
    }

    // The hip item's grip (top) and lower end (world).
    inline void HipSegment(const BodyFrame& frame, const Layout& layout, Zone zone, Vector& top, Vector& bottom)
    {
        top = RootToWorld(frame, SocketOf(layout, zone));
        bottom = top - Vector(0.0f, 0.0f, layout.hipItemLengthMeters * frame.unitsPerMeter * frame.heightScale);
    }

    // The off-hand hip used to be Pavlov's magazine pouch; the pouch is on the
    // off-hand upper chest now. Kept for callers that ask for the off hip.
    inline Vector OffHipCenter(const Layout& layout)
    {
        return layout.socket[SlotIndex(Zone::Consumable)];
    }

    // Where a hand reaches for a zone: the item's handle (the back: over the
    // off shoulder).
    inline Vector ZoneCenterWorld(const BodyFrame& frame, const Layout& layout, Zone zone)
    {
        if (IsBackZone(layout, zone))
            return HeadToWorld(frame, layout.backReachHead);
        return RootToWorld(frame, SocketOf(layout, zone));
    }

    // How close (metres) the hand's centre must be to the zone's geometry
    // (sling: the gun from grip to foregrip; hips: the hanging item; chest: the
    // item; back: the sphere centre).
    inline float ZoneReachMeters(const Layout& layout, Zone zone)
    {
        switch (KindOf(layout, zone))
        {
        case ZoneKind::Sling: return layout.slingReachMeters;
        case ZoneKind::Hip: return layout.pavlovHips ? HipCapsuleReachMeters(layout) : layout.hipReachMeters;
        case ZoneKind::Back: return layout.backRadiusMeters + layout.handRadiusMeters;
        default: return layout.pocketReachMeters;
        }
    }

    // Body yaw. speed <= 0 (Forefront, the default): the head yaw, every frame.
    // speed > 0 (Pavlov: 20): interpolates toward the head yaw (UE RInterpTo),
    // no dead band. Turns apply instantly either way.
    inline float FollowBodyYaw(float previousYaw, bool previousValid, float headYaw, float turnDelta,
        float dt, float speed)
    {
        if (!std::isfinite(headYaw))
            return previousValid ? l4d2vr_interaction::WrapYaw(previousYaw + turnDelta) : 0.0f;
        if (!previousValid || !std::isfinite(previousYaw) || !(speed > 0.0f))
            return l4d2vr_interaction::WrapYaw(headYaw);
        float yaw = l4d2vr_interaction::WrapYaw(previousYaw + (std::isfinite(turnDelta) ? turnDelta : 0.0f));
        const float error = l4d2vr_interaction::WrapYaw(headYaw - yaw);
        const float alpha = (!std::isfinite(dt) || dt <= 0.0f) ? 0.0f : std::fmin(1.0f, dt * speed);
        return l4d2vr_interaction::WrapYaw(yaw + error * alpha);
    }

    // 2.0.14: HolsterYawFollowSpeed > 0 is used as is; 0 is the layout's
    // default (forefront: the head yaw every frame; pavlov: 20); below 0 the
    // head yaw every frame for any layout.
    inline float EffectiveYawFollowSpeed(const Layout& layout, float configured)
    {
        if (!std::isfinite(configured))
            return layout.defaultYawFollowSpeed;
        if (configured > 0.0f)
            return configured;
        return configured < 0.0f ? 0.0f : layout.defaultYawFollowSpeed;
    }

    // 2.0.14 (pavlov hips): putting the slot2 item away: the hip it goes to
    // (offSide) and the hand's distance to it. Either hip takes it (Pavlov's
    // two Holsters slots), the nearer one wins.
    inline float SidearmHipDistanceMeters(const BodyFrame& frame, const Layout& layout, const Vector& hand,
        bool& offSide)
    {
        offSide = layout.secondaryOffHip;
        if (!layout.pavlovHips)
            return 1.0e9f;
        const float dominant = HipCapsuleDistanceMeters(frame, layout, false, hand);
        const float off = HipCapsuleDistanceMeters(frame, layout, true, hand);
        offSide = off < dominant;
        return offSide ? off : dominant;
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

    // The hand is at the magazine pouch (off-hand upper chest). The holsters
    // leave such a hand to the magazines when the pouch is the nearest grab.
    // 2.0.14 (pavlov hips): the hip pouch a hand reaches, for the gun in the
    // other hand: the nearer hip capsule, except the hip where the slot2 item
    // hangs (that is a draw). point: the nearest point of that hip's axis.
    inline bool PavlovHipPouch(const BodyFrame& frame, const Layout& layout, const Vector& hand,
        bool secondaryOnBody, Vector& point, float& distanceMeters, bool* offSideOut = nullptr)
    {
        distanceMeters = 1.0e9f;
        if (!layout.pavlovHips || !frame.valid || !l4d2vr_interaction::Finite(hand))
            return false;
        bool found = false;
        for (int side = 0; side < 2; ++side)
        {
            const bool offSide = side == 1;
            if (secondaryOnBody && offSide == layout.secondaryOffHip)
                continue;
            Vector top{}, bottom{};
            HipCapsuleSegment(frame, layout, offSide, top, bottom);
            const float d = DistanceToSegment(hand, top, bottom) / frame.unitsPerMeter;
            if (!(d < distanceMeters))
                continue;
            distanceMeters = d;
            const Vector ab = bottom - top;
            const float len2 = DotProduct(ab, ab);
            float t = len2 > 1e-9f ? DotProduct(hand - top, ab) / len2 : 0.0f;
            t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
            point = top + ab * t;
            if (offSideOut)
                *offSideOut = offSide;
            found = true;
        }
        return found;
    }

    inline float MagazinePouchDistanceMeters(const BodyFrame& frame, const Layout& layout, const Vector& hand,
        bool secondaryOnBody = false)
    {
        if (!frame.valid || !l4d2vr_interaction::Finite(hand))
            return 1.0e9f;
        if (layout.pavlovHips)
        {
            Vector point{};
            float distance = 1.0e9f;
            return PavlovHipPouch(frame, layout, hand, secondaryOnBody, point, distance) ? distance : 1.0e9f;
        }
        const Vector d = hand - RootToWorld(frame, layout.magPouch);
        return std::sqrt(DotProduct(d, d)) / frame.unitsPerMeter;
    }

    // The pouch's reach: the hip capsule's against the hand sphere with
    // Pavlov's hips, else the pouch sphere's.
    inline float PouchReachMeters(const Layout& layout)
    {
        return layout.pavlovHips ? HipCapsuleReachMeters(layout) : layout.magPouchReachMeters;
    }

    inline bool HandAtMagazinePouch(const BodyFrame& frame, const Layout& layout, const Vector& hand)
    {
        return MagazinePouchDistanceMeters(frame, layout, hand) <= PouchReachMeters(layout);
    }

    // Distance (metres) from the hand centre to a zone's geometry; true when it
    // is within the zone's reach. The back belongs to slot4 alone.
    // (wholeBack is kept for callers; the back is not split any more.)
    inline bool HandFitsZone(const BodyFrame& frame, const Layout& layout, Zone zone, const Vector& hand,
        float& outDistanceMeters, bool wholeBack = false)
    {
        (void)wholeBack;
        outDistanceMeters = 1.0e9f;
        if (!frame.valid || zone == Zone::None || SlotIndex(zone) < 0 || SlotIndex(zone) >= kZoneCount ||
            !l4d2vr_interaction::Finite(hand))
            return false;
        const float s = frame.unitsPerMeter;
        float distance = 1.0e9f;
        if (IsSlingZone(layout, zone))
        {
            Vector handle{}, foregrip{};
            SlingHandle(frame, layout, handle, foregrip);
            distance = DistanceToSegment(hand, handle, foregrip) / s;
        }
        else if (IsHipZone(layout, zone))
        {
            if (layout.pavlovHips)
            {
                // 2.0.14: Pavlov's hip capsule (the slot2 item's hip).
                distance = HipCapsuleDistanceMeters(frame, layout,
                    zone == Zone::Secondary ? layout.secondaryOffHip : SocketOf(layout, zone).y < 0.0f, hand);
            }
            else
            {
                Vector top{}, bottom{};
                HipSegment(frame, layout, zone, top, bottom);
                distance = DistanceToSegment(hand, top, bottom) / s;
            }
        }
        else if (IsBackZone(layout, zone))
        {
            const Vector d = hand - HeadToWorld(frame, layout.backCenterHead);
            distance = std::sqrt(DotProduct(d, d)) / s;
            const Vector local = ToLocal(frame, hand);
            if (local.x > layout.backMaxForwardMeters || local.z < layout.backMinUpMeters)
                distance = std::fmax(distance, ZoneReachMeters(layout, zone) + 0.01f +
                    std::fmax(local.x - layout.backMaxForwardMeters, layout.backMinUpMeters - local.z));
        }
        else
        {
            const Vector d = hand - RootToWorld(frame, SocketOf(layout, zone));
            distance = std::sqrt(DotProduct(d, d)) / s;
        }
        outDistanceMeters = distance;
        return std::isfinite(distance) && distance <= ZoneReachMeters(layout, zone);
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

    // Putting the held item away: the hand is in the item's own grab volume
    // (Forefront: the item returns to its slot). occupiedMask is unused (the
    // back is no longer shared) and kept for callers.
    inline bool HandInHolsterVolume(const BodyFrame& frame, const Layout& layout, Zone zone, const Vector& hand,
        std::uint32_t occupiedMask = 0x1Fu)
    {
        (void)occupiedMask;
        float distance = 0.0f;
        return HandFitsZone(frame, layout, zone, hand, distance, true);
    }

    // 2.0.11: the held item's own holster beats any other item under the hand
    // while the hand is inside that holster's grab volume (putting the item
    // away never turns into a swap because a neighbour's body overlaps it).
    inline bool OwnHolsterWins(bool handInOwnVolume, Zone otherZoneUnderHand)
    {
        return handInOwnVolume && otherZoneUnderHand != Zone::None;
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

    // The slung gun (slot1, or the active long gun on the sling): the bone
    // pose that puts the gun's visible centre slingHandleToCenterMeters down
    // the barrel from the grip point.
    inline ItemPose SlingPose(const BodyFrame& frame, const Layout& layout)
    {
        ItemPose pose{};
        if (!frame.valid)
            return pose;
        SlingAxes(frame, pose.axisX, pose.axisY, pose.axisZ);
        const Vector handle = RootToWorld(frame, layout.socket[0]);
        const Vector center = handle + pose.axisZ * (layout.slingHandleToCenterMeters * frame.unitsPerMeter);
        const Vector centerUnits = ReferenceToCenterUnits(ItemShape::LongGun);
        pose.origin = center - (pose.axisX * centerUnits.x + pose.axisY * centerUnits.y + pose.axisZ * centerUnits.z);
        pose.valid = l4d2vr_interaction::Finite(pose.origin) && l4d2vr_interaction::Finite(pose.axisX);
        return pose;
    }

    inline ItemPose BuildItemPose(const BodyFrame& frame, const Layout& layout, Zone zone, ItemShape shape)
    {
        ItemPose pose{};
        if (!frame.valid || zone == Zone::None || SlotIndex(zone) < 0 || SlotIndex(zone) >= kZoneCount)
            return pose;
        // A sling slot is the chest sling whatever the item.
        if (IsSlingZone(layout, zone))
            return SlingPose(frame, layout);
        const Vector up(0.0f, 0.0f, 1.0f);
        const Vector f = frame.forward;
        const Vector d = frame.dominant;
        const bool back = IsBackZone(layout, zone);
        // Out of the body: forward on the chest and hips, backward on the back.
        const Vector out = back ? f * -1.0f : f;
        Vector axisY = up;
        Vector axisZ = f;
        // Hip items hang from their grip at the socket (Forefront's
        // VestGrabTarget); pocket and back items are centred on it.
        bool gripAtSocket = false;
        switch (shape)
        {
        case ItemShape::LongGun:
            if (back)
            {
                // 2.0.12 (Pavlov layout): slung on the back, muzzle up over the
                // dominant shoulder, sights out of the back. 2.0.14: Pavlov's
                // back socket lies 33 deg off vertical (was ~30).
                axisZ = layout.pavlovHips ? Normalized(up * 0.8387f + d * 0.5446f, up)
                                          : Normalized(up * 0.85f + d * 0.5f, up);
                axisY = f * -1.0f;
                break;
            }
            axisZ = up * -1.0f;
            axisY = f;
            gripAtSocket = IsHipZone(layout, zone);
            break;
        case ItemShape::Pistol:
            // Forefront's secondary: barrel straight down, sights forward,
            // the grip on top (behind the slide).
            axisZ = up * -1.0f;
            axisY = f;
            // 2.0.14 (pavlov hips): Pavlov's hip socket pitch -66.7 deg: grip
            // up, the muzzle forward and down like a drop-leg holster.
            if (layout.pavlovHips && IsHipZone(layout, zone))
            {
                axisZ = Normalized(f * 0.3960f + up * -0.9182f, up * -1.0f);
                axisY = Normalized(f * 0.9182f + up * 0.3960f, f);
            }
            gripAtSocket = IsHipZone(layout, zone);
            break;
        case ItemShape::Melee:
            // Blade (local +Y) down and back, local +Z toward the dominant
            // side; the handle on top at the socket.
            axisY = Normalized(up * -0.85f + f * -0.5f, up * -1.0f);
            axisZ = Normalized(d, Vector(0.0f, -1.0f, 0.0f));
            gripAtSocket = IsHipZone(layout, zone);
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
            // Upright, face out of the body: X = out, Y = up, so Z = X x Y.
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

        const Vector socket = RootToWorld(frame, SocketOf(layout, zone));
        if (gripAtSocket)
        {
            // The reference bone sits at the grip end of pistols and melee
            // handles: the item hangs below the socket.
            pose.origin = socket;
        }
        else
        {
            // The item's visible centre goes to the rest socket.
            const Vector centerUnits = ReferenceToCenterUnits(shape);
            pose.origin = socket - (axisX * centerUnits.x + axisY * centerUnits.y + axisZ * centerUnits.z);
        }
        pose.axisX = axisX;
        pose.axisY = axisY;
        pose.axisZ = axisZ;
        pose.valid = l4d2vr_interaction::Finite(pose.origin) && l4d2vr_interaction::Finite(pose.axisX);
        return pose;
    }

    // 2.0.9: the drawn body of each item hanging on the body (grab anywhere
    // on it). An oriented box in the item's reference-bone frame (model units,
    // the ItemPose axes: Z barrel, Y sights/top, X = the gun's left side),
    // measured roughly from the stock v_ models: long guns run 15 units
    // behind the bone to 32 in front with the grip and magazine below,
    // pistols 2 behind to 12 in front, melee blades along +Y from the handle
    // at the bone, kits and the defibrillator thin along X, packs along Z.
    inline void ItemBodyBox(ItemShape shape, Vector& centerUnits, Vector& halfUnits)
    {
        switch (shape)
        {
        case ItemShape::LongGun: centerUnits = Vector(0.0f, -2.0f, 8.5f); halfUnits = Vector(2.0f, 5.0f, 23.5f); break;
        case ItemShape::Pistol: centerUnits = Vector(0.0f, -1.5f, 5.0f); halfUnits = Vector(1.5f, 3.5f, 7.0f); break;
        case ItemShape::Melee: centerUnits = Vector(0.0f, 11.0f, 0.0f); halfUnits = Vector(2.5f, 15.0f, 2.5f); break;
        case ItemShape::Kit: centerUnits = Vector(0.0f, 0.0f, 0.0f); halfUnits = Vector(2.0f, 6.0f, 7.0f); break;
        case ItemShape::Defib: centerUnits = ReferenceToCenterUnits(ItemShape::Defib); halfUnits = Vector(2.5f, 6.0f, 7.0f); break;
        case ItemShape::Pack: centerUnits = Vector(0.0f, 0.0f, 0.0f); halfUnits = Vector(6.5f, 6.5f, 2.0f); break;
        case ItemShape::Equipment:
        default: centerUnits = Vector(0.0f, 0.0f, 0.0f); halfUnits = Vector(2.5f, 4.5f, 2.5f); break;
        }
    }

    // Signed distance (metres) from a point to an oriented box: negative inside
    // (minus the depth to the nearest face).
    inline float SignedBoxDistanceMeters(const ItemPose& pose, const Vector& centerUnits, const Vector& halfUnits,
        const Vector& point, float unitsPerMeter)
    {
        const Vector center = pose.origin + pose.axisX * centerUnits.x + pose.axisY * centerUnits.y +
            pose.axisZ * centerUnits.z;
        const Vector d = point - center;
        const float local[3] = { DotProduct(d, pose.axisX), DotProduct(d, pose.axisY), DotProduct(d, pose.axisZ) };
        const float half[3] = { halfUnits.x, halfUnits.y, halfUnits.z };
        float outside2 = 0.0f, insideMax = -1.0e9f;
        for (int i = 0; i < 3; ++i)
        {
            const float q = std::fabs(local[i]) - half[i];
            if (q > 0.0f)
                outside2 += q * q;
            insideMax = std::fmax(insideMax, q);
        }
        const float units = outside2 > 0.0f ? std::sqrt(outside2) : insideMax;
        return units / (unitsPerMeter > 0.001f ? unitsPerMeter : 1.0f);
    }

    // The items hanging on the body this frame (bit i: slot i's item is drawn
    // at its holster or on the sling, shape[i] its holster shape). Held items
    // are not on the body and have only their holster's handle volume.
    struct BodyItems
    {
        std::uint32_t mask = 0u;
        ItemShape shape[kZoneCount] = { ItemShape::LongGun, ItemShape::Pistol, ItemShape::Equipment,
            ItemShape::Kit, ItemShape::Equipment };
    };

    // Signed distance (metres) from the hand centre to the drawn body of the
    // item in a zone, in its holstered pose. 1e9 when there is none.
    inline float ItemBodyDistanceMeters(const BodyFrame& frame, const Layout& layout, Zone zone, ItemShape shape,
        const Vector& hand)
    {
        if (!frame.valid || zone == Zone::None || SlotIndex(zone) < 0 || SlotIndex(zone) >= kZoneCount ||
            !l4d2vr_interaction::Finite(hand))
            return 1.0e9f;
        // slot1 holds a long gun whatever the item (on the sling: SlingPose).
        if (zone == Zone::Primary)
            shape = ItemShape::LongGun;
        const ItemPose pose = BuildItemPose(frame, layout, zone, shape);
        if (!pose.valid)
            return 1.0e9f;
        Vector center{}, half{};
        ItemBodyBox(shape, center, half);
        const float distance = SignedBoxDistanceMeters(pose, center, half, hand, frame.unitsPerMeter);
        return std::isfinite(distance) ? distance : 1.0e9f;
    }

    // The handle volumes' depth is scaled to a common 15 cm reach so the
    // scores rank exactly like the 2.0.8 distance/reach rule (a small pocket
    // still beats the big back sphere) while staying in metres, comparable
    // with the item bodies' surface distances.
    constexpr float kNominalReachMeters = 0.15f;

    // How far (metres) the hand is from being able to grab in a zone: <= 0
    // inside. The nearer of the handle volume (HandFitsZone against its reach)
    // and, when the zone's item hangs on the body (items), the item's drawn
    // body (its surface distance against itemGrabMarginMeters).
    inline float GrabScoreMeters(const BodyFrame& frame, const Layout& layout, Zone zone, const Vector& hand,
        const BodyItems* items, float* outHandleDistanceMeters = nullptr)
    {
        float distance = 1.0e9f;
        HandFitsZone(frame, layout, zone, hand, distance);
        if (outHandleDistanceMeters)
            *outHandleDistanceMeters = distance;
        const float reach = ZoneReachMeters(layout, zone);
        float score = (std::isfinite(distance) && reach > 0.0f && distance < 1.0e8f)
            ? (distance / reach - 1.0f) * kNominalReachMeters : 1.0e9f;
        const int slot = SlotIndex(zone);
        if (items && slot >= 0 && slot < kZoneCount && (items->mask & (1u << slot)) != 0u)
        {
            const float body = ItemBodyDistanceMeters(frame, layout, zone, items->shape[slot], hand);
            const float margin = KindOf(layout, zone) == ZoneKind::Pocket
                ? layout.pocketItemGrabMarginMeters : layout.itemGrabMarginMeters;
            if (body < 1.0e8f)
                score = std::fmin(score, body - margin);
        }
        return score;
    }

    // The magazine pouch on the same scale (a sphere of magPouchReachMeters).
    inline float MagazinePouchScoreMeters(const BodyFrame& frame, const Layout& layout, const Vector& hand,
        bool secondaryOnBody = false)
    {
        const float reach = PouchReachMeters(layout);
        if (!(reach > 0.0f))
            return 1.0e9f;
        const float pouch = MagazinePouchDistanceMeters(frame, layout, hand, secondaryOnBody);
        return pouch < 1.0e8f ? (pouch / reach - 1.0f) * kNominalReachMeters : 1.0e9f;
    }

    // The zone the hand takes from (occupiedMask bit i = slot i holds an item
    // the hand may take; 0x1F ignores occupancy). Forefront's rule: of the
    // grab volumes the hand is in, the nearest wins (GrabScoreMeters: the
    // handle volumes by distance relative to their reach, so a small pocket
    // beats the big back sphere; 2.0.9: the bodies of the items hanging on the
    // body (items) by their surface distance, so the hand takes the item it
    // is actually on and a long slung gun does not steal a hip or chest grab).
    // The magazine pouch takes part too: when it wins, no holster takes the
    // hand. outDistanceMeters: the winner's handle distance; outScoreMeters:
    // its score (<= 0).
    // 2.0.12: hovered = the zone this hand hovered last frame; it keeps
    // winning until another zone is nearer by more than
    // layout.nearestHysteresisMeters (no flicker between neighbours).
    inline Zone FindZone(const BodyFrame& frame, const Layout& layout, const Vector& hand,
        std::uint32_t occupiedMask, float* outDistanceMeters = nullptr, const BodyItems* items = nullptr,
        float* outScoreMeters = nullptr, Zone hovered = Zone::None)
    {
        if (outDistanceMeters)
            *outDistanceMeters = 1.0e9f;
        if (outScoreMeters)
            *outScoreMeters = 1.0e9f;
        if (!frame.valid || !l4d2vr_interaction::Finite(hand))
            return Zone::None;
        Zone best = Zone::None;
        float bestScore = 1.0e9f, bestDistance = 1.0e9f;
        float hoveredScore = 1.0e9f, hoveredDistance = 1.0e9f;
        for (int i = 0; i < kZoneCount; ++i)
        {
            if ((occupiedMask & (1u << i)) == 0u)
                continue;
            const Zone zone = static_cast<Zone>(i);
            float distance = 0.0f;
            const float score = GrabScoreMeters(frame, layout, zone, hand, items, &distance);
            if (!(score <= 0.0f))
                continue;
            if (zone == hovered)
            {
                hoveredScore = score;
                hoveredDistance = distance;
            }
            if (score < bestScore)
            {
                bestScore = score;
                bestDistance = distance;
                best = zone;
            }
        }
        if (hovered != Zone::None && best != hovered && hoveredScore <= 0.0f &&
            hoveredScore <= bestScore + layout.nearestHysteresisMeters)
        {
            best = hovered;
            bestScore = hoveredScore;
            bestDistance = hoveredDistance;
        }
        if (best != Zone::None)
        {
            // 2.0.14 (pavlov hips): the hip where the slot2 item hangs is no
            // pouch: a holstered pistol wins there (Pavlov: Get first).
            const float pouch = MagazinePouchScoreMeters(frame, layout, hand, (occupiedMask & 2u) != 0u);
            if (pouch <= 0.0f && pouch < bestScore)
                return Zone::None;
        }
        if (outDistanceMeters)
            *outDistanceMeters = bestDistance;
        if (outScoreMeters)
            *outScoreMeters = bestScore;
        return best;
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
        // 2.0.12 (review s12 7a, G1; Pavlov and Forefront): only an EMPTY hand
        // takes an item from the body. A hand that holds something only puts
        // its own item away at its own holster.
        bool handEmpty = true;
    };

    struct GrabOutput
    {
        int selectSlot = -1;   // issue slotN (0-based) to draw this item
        int holsterTo = -1;    // switch to this slot because the held item was holstered
        int putAwaySlot = -1;  // the held item (this slot) was put away in its own holster
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

            if (pressed && in.handZone != Zone::None && in.handEmpty)
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
                    out.putAwaySlot = m_HeldSlot;
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

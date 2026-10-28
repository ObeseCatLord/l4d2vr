#pragma once
#include "vr_interaction_geometry.h"
#include <cmath>
#include <cstdint>

// Pavlov Shack style body holsters. The body frame follows the headset yaw
// with a dead band and smoothing, so glancing around does not swing the
// holsters, while stick/snap turns move them instantly.
namespace l4d2vr_holster
{
    constexpr float kPi = 3.14159265358979323846f;

    enum class Zone : int
    {
        Primary = 0,    // slot1, behind the dominant shoulder
        Secondary = 1,  // slot2, dominant hip (pistols and melee)
        Throwable = 2,  // slot3, chest (off-hand side)
        Medical = 3,    // slot4, behind the off-hand shoulder
        Consumable = 4, // slot5, chest (dominant side)
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

    // Offsets are metres in the body frame relative to the head (the eyes of a
    // level-looking head, see NeutralHeadOffset): x forward, y toward the
    // DOMINANT side, z up. Mirroring is implicit in the dominant-side convention.
    // Sized for a standing adult (eyes ~1.65 m): belt 0.75-0.85 m below the eyes
    // with the hip holster 0.22 m out and slightly behind, chest pouches 0.36 m
    // below and 0.12 m forward, and over-the-shoulder reaches just behind the head.
    struct Layout
    {
        Vector center[kZoneCount] = {
            Vector(-0.10f, 0.15f, -0.08f),
            Vector(-0.04f, 0.22f, -0.78f),
            Vector(0.12f, -0.10f, -0.36f),
            Vector(-0.10f, -0.15f, -0.08f),
            Vector(0.12f, 0.10f, -0.36f),
        };
        // Generous, Pavlov-sized zones (its hips are 0.20 m capsules and its back a
        // 0.40 m sphere): a holster you have to hunt for is a holster nobody uses.
        float radius[kZoneCount] = { 0.28f, 0.20f, 0.15f, 0.28f, 0.15f };
        // Vertical capsule half-length: the hip accepts any belt height from
        // 0.66 m to 0.90 m below the eyes.
        float halfHeight[kZoneCount] = { 0.0f, 0.12f, 0.0f, 0.0f, 0.0f };
        // Shoulder zones only accept a hand that reached over the shoulder:
        // behind the head's frontal plane and above this height below the head.
        bool overShoulder[kZoneCount] = { true, false, false, true, false };
        float behindPlaneToleranceMeters = 0.06f;
        float overShoulderMaxDropMeters = 0.40f;
        // ... and within the shoulders (a gun held up beside the head is not a reach).
        float overShoulderMaxLateralMeters = 0.27f;
    };

    // The first layout's defaults. Installed config files repeat them verbatim,
    // so a configured value equal to one of these means "default", not a choice.
    inline const Vector& LegacyCenter(int zone)
    {
        static const Vector kCenters[kZoneCount] = {
            Vector(-0.10f, 0.16f, -0.12f), Vector(0.02f, 0.22f, -0.70f), Vector(0.16f, -0.09f, -0.34f),
            Vector(-0.10f, -0.16f, -0.12f), Vector(0.16f, 0.09f, -0.34f) };
        return kCenters[zone < 0 || zone >= kZoneCount ? 0 : zone];
    }
    inline float LegacyRadius(int zone)
    {
        static const float kRadii[kZoneCount] = { 0.25f, 0.20f, 0.13f, 0.25f, 0.13f };
        return kRadii[zone < 0 || zone >= kZoneCount ? 0 : zone];
    }
    inline bool NearlyEqual(const Vector& a, const Vector& b)
    {
        return std::fabs(a.x - b.x) < 0.0005f && std::fabs(a.y - b.y) < 0.0005f && std::fabs(a.z - b.z) < 0.0005f;
    }
    // Configured zone -> effective zone: legacy defaults move to the new layout,
    // anything the player tuned is kept.
    inline void MigrateLegacyZone(int zone, const Layout& defaults, Vector& center, float& radius)
    {
        if (zone < 0 || zone >= kZoneCount)
            return;
        if (NearlyEqual(center, LegacyCenter(zone)))
            center = defaults.center[zone];
        if (std::fabs(radius - LegacyRadius(zone)) < 0.0005f)
            radius = defaults.radius[zone];
    }

    // Eye position relative to the head's pivot (top of the neck): when the
    // player looks down at a holster the eyes swing forward and drop ~0.1 m,
    // which must not move the holsters. Returns world offset (units) from the
    // tracked eyes to where they would be with the head level and facing the body.
    constexpr float kEyeForwardOfNeckMeters = 0.09f;
    constexpr float kEyeAboveNeckMeters = 0.11f;
    inline Vector NeutralHeadOffset(const Vector& hmdForward, const Vector& hmdUp, const Vector& bodyForward,
        float unitsPerMeter)
    {
        if (!l4d2vr_interaction::Finite(hmdForward) || !l4d2vr_interaction::Finite(hmdUp) ||
            !l4d2vr_interaction::Finite(bodyForward) || !std::isfinite(unitsPerMeter) || unitsPerMeter <= 0.001f)
            return Vector(0.0f, 0.0f, 0.0f);
        const float fLen = std::sqrt(DotProduct(hmdForward, hmdForward));
        const float uLen = std::sqrt(DotProduct(hmdUp, hmdUp));
        const float bLen = std::sqrt(bodyForward.x * bodyForward.x + bodyForward.y * bodyForward.y);
        if (fLen < 0.5f || uLen < 0.5f || bLen < 0.5f)
            return Vector(0.0f, 0.0f, 0.0f);
        const Vector f = hmdForward * (1.0f / fLen);
        const Vector u = hmdUp * (1.0f / uLen);
        const Vector b(bodyForward.x / bLen, bodyForward.y / bLen, 0.0f);
        const Vector tracked = f * kEyeForwardOfNeckMeters + u * kEyeAboveNeckMeters;
        const Vector level = b * kEyeForwardOfNeckMeters + Vector(0.0f, 0.0f, kEyeAboveNeckMeters);
        return (level - tracked) * unitsPerMeter;
    }

    struct BodyFrame
    {
        Vector head{ 0.0f, 0.0f, 0.0f };
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
        frame.valid = true;
        return frame;
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

    inline Vector ZoneCenterWorld(const BodyFrame& frame, const Layout& layout, Zone zone)
    {
        return ToWorld(frame, layout.center[SlotIndex(zone)]);
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

    // Facing direction of the head. Looking steeply down at a holster leaves
    // little horizontal forward, so the head's up vector (which then tips
    // forward) carries the yaw instead.
    inline float HeadYawDegrees(const Vector& worldHeadForward, const Vector& worldHeadUp, float fallback)
    {
        if (!l4d2vr_interaction::Finite(worldHeadForward) || !l4d2vr_interaction::Finite(worldHeadUp))
            return fallback;
        const float down = std::fmax(0.0f, -worldHeadForward.z);
        const Vector facing = worldHeadForward + worldHeadUp * down;
        if (facing.x * facing.x + facing.y * facing.y < 0.0004f)
            return fallback;
        return std::atan2(facing.y, facing.x) * 180.0f / kPi;
    }

    inline float HeadYawDegrees(const Vector& worldHeadForward, float fallback)
    {
        if (!l4d2vr_interaction::Finite(worldHeadForward) ||
            worldHeadForward.x * worldHeadForward.x + worldHeadForward.y * worldHeadForward.y < 0.0004f)
            return fallback;
        return std::atan2(worldHeadForward.y, worldHeadForward.x) * 180.0f / kPi;
    }

    // Distance (m) from a body-frame point to a zone's centre (or its vertical
    // capsule segment), ignoring the over-the-shoulder gate.
    inline float ZoneDistance(const Layout& layout, int index, const Vector& local)
    {
        if (index < 0 || index >= kZoneCount)
            return 1.0e9f;
        Vector delta = local - layout.center[index];
        const float half = std::fmax(0.0f, layout.halfHeight[index]);
        const float dz = std::fabs(delta.z) - half;
        delta.z = dz > 0.0f ? dz : 0.0f;
        return std::sqrt(DotProduct(delta, delta));
    }

    inline bool HandFitsZone(const BodyFrame& frame, const Layout& layout, Zone zone, const Vector& hand,
        float& outDistanceMeters)
    {
        outDistanceMeters = 1.0e9f;
        if (!frame.valid || zone == Zone::None || !l4d2vr_interaction::Finite(hand))
            return false;
        const int index = SlotIndex(zone);
        const Vector local = ToLocal(frame, hand);
        if (layout.overShoulder[index])
        {
            if (local.x > layout.behindPlaneToleranceMeters)
                return false;
            if (local.z < -layout.overShoulderMaxDropMeters)
                return false;
            if (std::fabs(local.y) > layout.overShoulderMaxLateralMeters)
                return false;
        }
        const float distance = ZoneDistance(layout, index, local);
        outDistanceMeters = distance;
        return std::isfinite(distance) && distance <= layout.radius[index];
    }

    // The hand is in, or within marginMeters of, any holster zone (occupied or
    // not), honouring the over-the-shoulder gate. Motion gestures (gun bash)
    // ignore hands at the holsters.
    inline bool NearAnyZone(const BodyFrame& frame, const Layout& layout, const Vector& hand, float marginMeters)
    {
        if (!frame.valid || !l4d2vr_interaction::Finite(hand))
            return false;
        const Vector local = ToLocal(frame, hand);
        for (int i = 0; i < kZoneCount; ++i)
        {
            if (layout.overShoulder[i] && (local.x > layout.behindPlaneToleranceMeters + marginMeters ||
                    local.z < -layout.overShoulderMaxDropMeters - marginMeters ||
                    std::fabs(local.y) > layout.overShoulderMaxLateralMeters + marginMeters))
                continue;
            if (ZoneDistance(layout, i, local) <= layout.radius[i] + marginMeters)
                return true;
        }
        return false;
    }

    // Nearest zone that accepts the hand. occupiedMask bit i = slot i holds an item;
    // pass 0x1F to ignore occupancy.
    inline Zone FindZone(const BodyFrame& frame, const Layout& layout, const Vector& hand,
        std::uint32_t occupiedMask, float* outDistanceMeters = nullptr)
    {
        Zone best = Zone::None;
        float bestDistance = 1.0e9f;
        for (int i = 0; i < kZoneCount; ++i)
        {
            if ((occupiedMask & (1u << i)) == 0u)
                continue;
            float distance = 0.0f;
            if (HandFitsZone(frame, layout, static_cast<Zone>(i), hand, distance) && distance < bestDistance)
            {
                bestDistance = distance;
                best = static_cast<Zone>(i);
            }
        }
        if (outDistanceMeters)
            *outDistanceMeters = bestDistance;
        return best;
    }

    // World placement for a holstered item. The returned basis is for the item's
    // reference bone: longAxis = the bone's local +Z (barrel/long axis on L4D2
    // w_models), upAxis = local +Y (sight side). Melee blades run along local +Y
    // on the stock melee w_models, so they use a separate preset.
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
        LongGun = 0,  // barrel on local +Z
        Pistol = 1,   // barrel on local +Z
        Melee = 2,    // blade on local +Y
        Equipment = 3 // kits, pills, grenades: keep the model upright
    };

    inline Vector Normalized(const Vector& v, const Vector& fallback)
    {
        const float length = std::sqrt(DotProduct(v, v));
        if (!std::isfinite(length) || length < 0.0001f)
            return fallback;
        return v * (1.0f / length);
    }

    inline ItemPose BuildItemPose(const BodyFrame& frame, const Layout& layout, Zone zone, ItemShape shape)
    {
        ItemPose pose{};
        if (!frame.valid || zone == Zone::None)
            return pose;
        const Vector up(0.0f, 0.0f, 1.0f);
        const Vector f = frame.forward;
        const Vector d = frame.dominant;
        Vector longAxis = up;
        Vector topAxis = f;
        switch (shape)
        {
        case ItemShape::LongGun:
            // Slung diagonally across the back: grip behind the shoulder,
            // barrel down toward the opposite hip, sights facing away from the body.
            longAxis = Normalized(up * -0.8f + d * -0.6f, up * -1.0f);
            topAxis = f * -1.0f;
            break;
        case ItemShape::Pistol:
            // Muzzle down, slide toward the front of the body.
            longAxis = up * -1.0f;
            topAxis = f;
            break;
        case ItemShape::Melee:
            // Handled separately below: blade (local +Y) points down and back.
            break;
        case ItemShape::Equipment:
            longAxis = up;
            topAxis = f;
            break;
        }

        Vector axisY{}, axisZ{};
        if (shape == ItemShape::Melee)
        {
            axisY = Normalized(up * -0.85f + f * -0.5f, up * -1.0f); // blade
            axisZ = Normalized(d, Vector(0.0f, -1.0f, 0.0f));
        }
        else
        {
            axisZ = longAxis;
            axisY = topAxis;
        }
        // Re-orthonormalize (Y against Z), then X = Y x Z for a right-handed basis.
        axisY = Normalized(axisY - axisZ * DotProduct(axisY, axisZ), f);
        Vector axisX{};
        CrossProduct(axisY, axisZ, axisX);
        axisX = Normalized(axisX, d);

        pose.origin = ZoneCenterWorld(frame, layout, zone);
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

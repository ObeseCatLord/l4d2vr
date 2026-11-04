#pragma once
#include <cmath>

// GunHandOnController (2.0.5, fixed in 2.0.6): every gun's idle pose put the
// drawn gun hand somewhere else relative to the controller (up to 17 cm off).
// A per-gun translation, kept in the item controller's frame, moves the gun so
// its hand's wrist sits where the empty hand's wrist sits.
//
// 2.0.5 applied that translation to the drawn bones only, late in the draw
// hook. Everything computed from the gun before that point or outside the draw
// (the magazine well and bolt boxes published while the magazine is out, the
// engine's flashlight and muzzle attachments) stayed where the gun was
// before the shift, so a magazine at the drawn well did not seat and the
// shotgun's flashlight shone from the wrong place. 2.0.6 moves the viewmodel
// origin itself (GetRecommendedViewmodelAbsPos and the render snapshot), so the
// engine, every interaction box and the scope see the same gun that is drawn.
// The draw hook only measures how far the resting gun hand still is from its
// target and corrects the stored translation (a fraction per frame, so a frame
// or two of render latency can never make it oscillate).
//
// Std-only so the policy is unit tested (tests/vr_gun_hand_anchor_tests.cpp).
namespace l4d2vr_gun_hand_anchor
{
    struct Vec3
    {
        float x = 0.0f, y = 0.0f, z = 0.0f;
    };

    inline Vec3 Add(const Vec3& a, const Vec3& b) { return Vec3{ a.x + b.x, a.y + b.y, a.z + b.z }; }
    inline Vec3 Scale(const Vec3& a, float s) { return Vec3{ a.x * s, a.y * s, a.z * s }; }
    inline float Length(const Vec3& a) { return std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z); }
    inline bool Finite(const Vec3& a) { return std::isfinite(a.x) && std::isfinite(a.y) && std::isfinite(a.z); }

    // The empty hand's pose offset (NativeViewmodel{Right,Left}HandPoseOffsetMeters)
    // is in the controller's local frame as the hand code builds it
    // (HooksNativeViewmodelHandsOnlyBuildControllerWorldFromAxes): x = right,
    // y = up, z = back (OpenVR's raw controller convention). The gun hand's
    // target uses the same convention, returned here as (forward, right, up).
    // (2.0.5 read the offset as forward, right, up, so a tuned offset put the
    // gun hand somewhere the empty hand never was.)
    inline Vec3 HandOffsetToForwardRightUp(const Vec3& offsetRightUpBack)
    {
        return Vec3{ -offsetRightUpBack.z, offsetRightUpBack.x, offsetRightUpBack.y };
    }

    // World point of a (forward, right, up) local vector in a controller frame.
    inline Vec3 LocalToWorld(const Vec3& local, const Vec3& forward, const Vec3& right, const Vec3& up)
    {
        return Vec3{ forward.x * local.x + right.x * local.y + up.x * local.z,
            forward.y * local.x + right.y * local.y + up.y * local.z,
            forward.z * local.x + right.z * local.y + up.z * local.z };
    }

    inline Vec3 WorldToLocal(const Vec3& world, const Vec3& forward, const Vec3& right, const Vec3& up)
    {
        return Vec3{ forward.x * world.x + forward.y * world.y + forward.z * world.z,
            right.x * world.x + right.y * world.y + right.z * world.z,
            up.x * world.x + up.y * world.y + up.z * world.z };
    }

    struct Anchor
    {
        bool valid = false;
        Vec3 local{};   // (forward, right, up), game units, item controller frame
    };

    // Update from a resting frame: `estimate` is the translation that puts the
    // hand on its target, measured as target - (drawn hand - the translation
    // the viewmodel was actually placed with), so it does not depend on the
    // stored value (no feedback loop). The first estimate is taken whole; later
    // ones are blended in by `gain` (noise from controller motion between the
    // placement and the draw). Never more than maxLength; a bad estimate is
    // ignored.
    inline bool Blend(Anchor& anchor, const Vec3& estimate, float gain, float maxLength)
    {
        if (!Finite(estimate) || !(maxLength > 0.0f) || Length(estimate) >= maxLength)
            return false;
        if (!anchor.valid)
        {
            anchor.local = estimate;
            anchor.valid = true;
            return true;
        }
        anchor.local = Add(anchor.local, Scale(Add(estimate, Scale(anchor.local, -1.0f)), gain));
        return true;
    }

    // Correction step from a resting frame: `residual` is target - drawn hand
    // in the controller frame (the stored translation already applied). The
    // first measurement of a gun takes the whole residual (the gun has no
    // translation yet, so nothing is in flight); later ones a fraction `gain`
    // (0.5: stable for up to two frames of latency between the translation
    // used and the frame measured). The translation never exceeds maxLength
    // (half a metre: never drag a gun across the body); a residual that would
    // is ignored.
    inline bool Correct(Anchor& anchor, const Vec3& residual, float gain, float maxLength)
    {
        if (!Finite(residual) || !(maxLength > 0.0f))
            return false;
        const float step = anchor.valid ? gain : 1.0f;
        const Vec3 next = Add(anchor.valid ? anchor.local : Vec3{}, Scale(residual, step));
        if (!Finite(next) || Length(next) >= maxLength)
            return false;
        anchor.local = next;
        anchor.valid = true;
        return true;
    }

    struct Gate
    {
        bool enabled = true;        // GunHandOnController
        bool vrEnabled = true;
        bool mouseMode = false;
        bool thirdPerson = false;
        bool isGun = true;          // the active weapon is a gun (not melee, grenades, kits)
        bool chestSling = false;    // the gun hangs on the chest: the hand is free
        bool emptyHands = false;    // both hands empty
    };

    // Is the translation applied to the viewmodel this frame?
    inline bool Applies(const Gate& g)
    {
        return g.enabled && g.vrEnabled && !g.mouseMode && !g.thirdPerson && g.isGun && !g.chestSling &&
            !g.emptyHands;
    }
}

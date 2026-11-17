#pragma once
#include <cmath>

// 2.0.12: Pavlov VR's hand and weapon colliders for grabbing (numbers and
// mechanics only, research/pavlov/grip/SPEC.md sections 2, 4 and 7.4).
//
// - The hand grabs with a sphere of 11.5 cm (Pavlov's GrabVolume) and the
//   nearest candidate wins (holsters: vr_holster.h FindZone).
// - The support hand takes the held gun's foregrip when its sphere overlaps a
//   per-CLASS box in the gun frame (forward along the bore, right, up; origin
//   at the gun hand's grip): pistols a large box (10.8 x 3.2 x 5.4 cm half
//   sizes) about 4 cm ahead of the grip, rifles 6.3 x 2 x 2.4 about 30 cm
//   ahead, shotguns 9.9 x 2 x 4.1 about 34-42 cm ahead.
// - On engage the aim's support point starts on the bore line and slides to
//   the real hand at speed 5/s (no aim jump); the grip lets go by itself when
//   the support hand is more than 30 cm from the foregrip.
// - Pistols and machine pistols: the second hand only steadies (recoil), it
//   never steers the aim (vr_two_hand_aim.h).
//
// Metres. std-only (tests/vr_grab_volumes_tests.cpp).
namespace l4d2vr_grab
{
    constexpr float kHandSphereRadiusMeters = 0.115f;
    constexpr float kAutoUngripMeters = 0.30f;
    constexpr float kSupportSlideSpeed = 5.0f;

    struct Vec3
    {
        float x = 0.0f, y = 0.0f, z = 0.0f;   // forward, right, up
    };

    enum class GunClass : int { None, Pistol, MachinePistol, Smg, Rifle, Sniper, PumpShotgun, AutoShotgun, Lmg, Launcher };

    inline const char* GunClassName(GunClass c)
    {
        switch (c)
        {
        case GunClass::Pistol: return "pistol";
        case GunClass::MachinePistol: return "machine pistol";
        case GunClass::Smg: return "smg";
        case GunClass::Rifle: return "rifle";
        case GunClass::Sniper: return "sniper";
        case GunClass::PumpShotgun: return "pump shotgun";
        case GunClass::AutoShotgun: return "auto shotgun";
        case GunClass::Lmg: return "lmg";
        case GunClass::Launcher: return "launcher";
        default: return "none";
        }
    }

    // C_WeaponCSBase::WeaponID -> Pavlov's class.
    inline GunClass ClassOf(int weaponId)
    {
        switch (weaponId)
        {
        case 1: case 32: return GunClass::Pistol;
        case 2: case 7: return GunClass::MachinePistol;   // Uzi, MAC-10 (Pavlov's Uzi/Skorpion)
        case 33: return GunClass::Smg;                    // MP5
        case 5: case 9: case 26: case 34: return GunClass::Rifle;
        case 6: case 10: case 35: case 36: return GunClass::Sniper;
        case 3: case 8: return GunClass::PumpShotgun;
        case 4: case 11: return GunClass::AutoShotgun;
        case 37: return GunClass::Lmg;
        case 21: return GunClass::Launcher;
        default: return GunClass::None;
        }
    }

    struct Box
    {
        Vec3 center{};   // gun frame, metres from the grip
        Vec3 half{};
    };

    // Pavlov's TwoHandGripComponent boxes (cm -> m). Classes Pavlov does not
    // list use the nearest Pavlov class at the L4D2 model's foregrip.
    inline bool ForegripBox(int weaponId, Box& out)
    {
        switch (ClassOf(weaponId))
        {
        case GunClass::Pistol:
        case GunClass::MachinePistol: out = { { 0.044f, 0.0f, -0.007f }, { 0.108f, 0.032f, 0.054f } }; return true;
        case GunClass::Smg: out = { { 0.24f, 0.0f, 0.05f }, { 0.0627f, 0.02f, 0.0236f } }; return true;
        case GunClass::Rifle: out = { { 0.30f, 0.0f, 0.075f }, { 0.0627f, 0.02f, 0.0236f } }; return true;   // M4 28.7, AK 31.8
        case GunClass::Sniper: out = { { 0.33f, 0.0f, 0.07f }, { 0.0627f, 0.02f, 0.0236f } }; return true;
        case GunClass::PumpShotgun: out = { { 0.416f, 0.0f, 0.017f }, { 0.0992f, 0.02f, 0.0411f } }; return true;
        case GunClass::AutoShotgun: out = { { 0.344f, 0.0f, 0.024f }, { 0.0992f, 0.02f, 0.0411f } }; return true;
        case GunClass::Lmg: out = { { 0.35f, 0.0f, 0.06f }, { 0.0992f, 0.025f, 0.0411f } }; return true;
        case GunClass::Launcher: out = { { 0.25f, 0.0f, 0.04f }, { 0.0992f, 0.03f, 0.0411f } }; return true;
        default: return false;
        }
    }

    // Pavlov's bTwoHandSupportOnly: pistols and machine pistols.
    inline bool SupportOnly(int weaponId)
    {
        const GunClass c = ClassOf(weaponId);
        return c == GunClass::Pistol || c == GunClass::MachinePistol;
    }

    // Distance (metres) from a point (gun frame) to the box surface, 0 inside.
    inline float PointBoxDistance(const Vec3& p, const Box& box)
    {
        const float dx = std::fmax(0.0f, std::fabs(p.x - box.center.x) - box.half.x);
        const float dy = std::fmax(0.0f, std::fabs(p.y - box.center.y) - box.half.y);
        const float dz = std::fmax(0.0f, std::fabs(p.z - box.center.z) - box.half.z);
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }

    // The hand sphere (centre p, radius kHandSphereRadiusMeters) against the
    // box: <= 0 when they overlap (Pavlov's engage test).
    inline float SphereGap(const Vec3& handCenter, const Box& box, float radius = kHandSphereRadiusMeters)
    {
        return PointBoxDistance(handCenter, box) - radius;
    }

    inline float Distance(const Vec3& a, const Vec3& b)
    {
        const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }

    // Auto-ungrip: the support hand more than 30 cm from the foregrip.
    inline bool AutoUngrip(const Vec3& handCenter, const Box& box)
    {
        return Distance(handCenter, box.center) > kAutoUngripMeters;
    }

    // The support point's offset on engage: from the hand to the nearest point
    // of the bore line (the gun frame's forward axis through the grip).
    inline Vec3 BoreOffset(const Vec3& handCenter)
    {
        return Vec3{ 0.0f, -handCenter.y, -handCenter.z };
    }

    // How much of the engage offset is left after `seconds` (UE VInterpTo to 0
    // at speed 5 per frame, ~exp(-5 t)).
    inline float SlideRemaining(float seconds, float speed = kSupportSlideSpeed)
    {
        if (!std::isfinite(seconds) || seconds <= 0.0f)
            return 1.0f;
        return std::exp(-speed * seconds);
    }
}

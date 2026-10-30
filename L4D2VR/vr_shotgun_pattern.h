#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>

// Fixed shotgun pellets for VR players (std-only; used by the client, the
// listen host and the dedicated plugin).
//
// Like Pavlov VR, every blast fires the same evenly spread pattern around
// the barrel (no random clumps, no random offset of the whole pattern; the
// VR aim already replaces L4D2's random spread cone). Unlike Pavlov's tight
// square, the pattern keeps L4D2's own footprint, which suits the horde.
//
// L4D2's pellet loop (CTerrorPlayer::FireBullet) leaves the first pellet on
// the aim line and offsets the others with two SharedRandomFloat draws each:
//   r     = SharedRandomFloat("CTerrorPlayer::FireBullet Spread", -S, S, n)
//   theta = 2 * SharedRandomFloat("CTerrorPlayer::FireBullet SpreadDir", 0, 180, n)
//   pitch += (P / S) * r * sin(theta);  yaw += r * cos(theta)
// with S = PelletScatterYaw, P = PelletScatterPitch and n = 2, 3, ... for the
// second, third ... pellet: an ellipse of S x P degrees in which |r| is
// uniform. The fixed pattern returns, for pattern pellet k = 1 .. N-1,
//   r = S * k / (N - 1)                (evenly stepped out to the edge)
//   theta = k * golden angle            (a sunflower spiral)
// so it fills the same ellipse, with the same mean radius as vanilla over
// all N pellets (S / 2) and no two pellets clumped, the same way every shot. Client prediction and the server agree because both
// compute it from the pellet index alone.
namespace l4d2vr_shotgun_pattern
{
    // client -> server opt-in: "l4d2vr_shotgun_pattern 1" (0 opts out); only
    // sent when the server advertises the ShotgunPattern feature.
    inline constexpr char kOptInCommand[] = "l4d2vr_shotgun_pattern";
    inline constexpr char kRadiusName[] = "CTerrorPlayer::FireBullet Spread";
    inline constexpr char kDirectionName[] = "CTerrorPlayer::FireBullet SpreadDir";
    inline constexpr float kPi = 3.14159265358979323846f;
    inline constexpr float kGoldenAngleDeg = 137.50776405f;

    struct Weapon
    {
        int weaponId;
        int pellets;          // stock L4D2 Bullets
        float scatterPitch;   // stock L4D2 PelletScatterPitch
        float scatterYaw;     // stock L4D2 PelletScatterYaw
    };

    inline const Weapon* WeaponFor(int weaponId)
    {
        static constexpr Weapon kWeapons[] = {
            { 3, 10, 5.0f, 7.5f },   // pumpshotgun
            { 8, 8, 3.0f, 5.0f },    // shotgun_chrome
            { 4, 11, 4.0f, 8.5f },   // autoshotgun
            { 11, 9, 3.5f, 8.0f },   // shotgun_spas
        };
        for (const Weapon& w : kWeapons)
            if (w.weaponId == weaponId)
                return &w;
        return nullptr;
    }

    // The loop draws for pattern pellet `pellet` (1 = the second pellet):
    // radius in [0, S] and half the direction angle in [0, 180).
    inline void PelletDraws(const Weapon& w, int pellet, float& radius, float& halfAngleDeg)
    {
        const int others = w.pellets > 1 ? w.pellets - 1 : 1;
        float fraction = static_cast<float>(pellet) / static_cast<float>(others);
        if (fraction > 1.0f) fraction = 1.0f;
        radius = w.scatterYaw * fraction;
        float theta = std::fmod(static_cast<float>(pellet) * kGoldenAngleDeg, 360.0f);
        if (theta < 0.0f) theta += 360.0f;
        halfAngleDeg = theta * 0.5f;
    }

    // Offset of pattern pellet `pellet` (0 = the centre pellet) in degrees,
    // right of and above the aim line, as the loop applies it. Source angles:
    // pitch grows downward, yaw to the left.
    inline void PelletOffsetDeg(const Weapon& w, int pellet, float& rightDeg, float& upDeg)
    {
        rightDeg = upDeg = 0.0f;
        if (pellet <= 0 || !(w.scatterYaw > 0.0f))
            return;
        float r = 0.0f, half = 0.0f;
        PelletDraws(w, pellet, r, half);
        const float theta = 2.0f * half * (kPi / 180.0f);
        upDeg = -(w.scatterPitch / w.scatterYaw) * r * std::sin(theta);
        rightDeg = -r * std::cos(theta);
    }

    // Per shot context the SharedRandomFloat detour reads (thread-local in the
    // shells): which weapon.
    struct ShotContext
    {
        const Weapon* weapon = nullptr;
        int pendingPellet = -1;  // pellet whose direction draw follows its radius draw
        float pendingHalfAngle = 0.0f;
    };

    enum class Draw { None, Radius, Direction };

    inline Draw Classify(const char* name)
    {
        if (!name) return Draw::None;
        if (std::strcmp(name, kRadiusName) == 0) return Draw::Radius;
        if (std::strcmp(name, kDirectionName) == 0) return Draw::Direction;
        return Draw::None;
    }

    // Returns true and sets `result` when this draw belongs to the pattern.
    // A radius draw whose range is not the stock scatter (a modified weapon
    // script) leaves the vanilla pellets alone.
    inline bool Override(ShotContext& shot, const char* name, float minValue, float maxValue, int seedIndex,
        float& result)
    {
        if (!shot.weapon)
            return false;
        const Draw kind = Classify(name);
        if (kind == Draw::None)
            return false;
        const int pellet = seedIndex - 1; // loop index 2 is the second pellet (pattern pellet 1)
        if (pellet <= 0)
            return false;
        if (kind == Draw::Radius)
        {
            shot.pendingPellet = -1;
            if (std::fabs(maxValue - shot.weapon->scatterYaw) > 0.01f || std::fabs(minValue + shot.weapon->scatterYaw) > 0.01f)
                return false;
            float radius = 0.0f, half = 0.0f;
            PelletDraws(*shot.weapon, pellet, radius, half);
            shot.pendingPellet = pellet;
            shot.pendingHalfAngle = half;
            result = radius;
            return true;
        }
        if (shot.pendingPellet != pellet || minValue != 0.0f || std::fabs(maxValue - 180.0f) > 0.01f)
            return false;
        shot.pendingPellet = -1;
        result = shot.pendingHalfAngle;
        return true;
    }
}

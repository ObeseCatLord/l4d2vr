#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>

// Pavlov VR style shotgun pellets for VR players (std-only; used by the
// client, the listen host and the dedicated plugin).
//
// Pavlov fires pellet 0 dead centre and spreads the others evenly with a
// low-discrepancy (R2) sequence that starts at a random index each shot:
// pellet i points along normalize(E, u, v) in the muzzle frame (forward,
// right, up) with u, v in [-0.5, 0.5]. E ("EvenSpread") is 20 for Pavlov's
// pump shotgun and 15 for its auto shotgun: square patterns of +-1.43 and
// +-1.91 degrees.
//
// L4D2 keeps its own pellet count (damage per pellet is unchanged). Its
// pellet loop (CTerrorPlayer::FireBullet) leaves the first pellet on the aim
// line and offsets the others with two SharedRandomFloat draws per pellet:
//   r     = SharedRandomFloat("CTerrorPlayer::FireBullet Spread", -S, S, n)
//   theta = 2 * SharedRandomFloat("CTerrorPlayer::FireBullet SpreadDir", 0, 180, n)
//   pitch += (P / S) * r * sin(theta);  yaw += r * cos(theta)
// with S = PelletScatterYaw, P = PelletScatterPitch and n = 2, 3, ... for the
// second, third ... pellet. Returning chosen r and theta places every pellet
// exactly on the Pavlov pattern. Both draws come from the same shared seed on
// the client and the server, so prediction and hit registration agree; the
// pattern's start index is drawn from that seed as well.
namespace l4d2vr_shotgun_pattern
{
    // client -> server opt-in: "l4d2vr_shotgun_pattern 1" (0 opts out); only
    // sent when the server advertises the ShotgunPattern feature.
    inline constexpr char kOptInCommand[] = "l4d2vr_shotgun_pattern";
    inline constexpr char kRadiusName[] = "CTerrorPlayer::FireBullet Spread";
    inline constexpr char kDirectionName[] = "CTerrorPlayer::FireBullet SpreadDir";
    inline constexpr char kStartIndexName[] = "L4D2VR PavlovPellets";
    inline constexpr float kPi = 3.14159265358979323846f;

    struct Weapon
    {
        int weaponId;
        float evenSpread;     // Pavlov EvenSpread of the equivalent gun
        float scatterPitch;   // stock L4D2 PelletScatterPitch
        float scatterYaw;     // stock L4D2 PelletScatterYaw
    };

    // Pump and Chrome -> Pavlov Shotgun (20); auto and SPAS -> AutoShotgun (15).
    inline const Weapon* WeaponFor(int weaponId)
    {
        static constexpr Weapon kWeapons[] = {
            { 3, 20.0f, 5.0f, 7.5f },   // pumpshotgun
            { 8, 20.0f, 3.0f, 5.0f },   // shotgun_chrome
            { 4, 15.0f, 4.0f, 8.5f },   // autoshotgun
            { 11, 15.0f, 3.5f, 8.0f },  // shotgun_spas
        };
        for (const Weapon& w : kWeapons)
            if (w.weaponId == weaponId)
                return &w;
        return nullptr;
    }

    inline float Frac(double x) { return static_cast<float>(x - std::floor(x)); }

    // R2 point k (u, v in [-0.5, 0.5]).
    inline void R2(std::uint32_t k, float& u, float& v)
    {
        u = Frac(static_cast<double>(k) * 0.7548776662466927 + 0.5) - 0.5f;
        v = Frac(static_cast<double>(k) * 0.5698402909980532 + 0.5) - 0.5f;
    }

    // Offset of pellet `pellet` (0 = the centre pellet) in degrees: right of
    // and above the aim line.
    inline void PelletOffsetDeg(int pellet, std::uint32_t startIndex, float evenSpread, float& rightDeg, float& upDeg)
    {
        rightDeg = upDeg = 0.0f;
        if (pellet <= 0 || !(evenSpread > 0.0f))
            return;
        float u = 0.0f, v = 0.0f;
        R2(startIndex + static_cast<std::uint32_t>(pellet - 1), u, v);
        const float horizontal = std::sqrt(evenSpread * evenSpread + u * u);
        rightDeg = std::atan2(u, evenSpread) * (180.0f / kPi);
        upDeg = std::atan2(v, horizontal) * (180.0f / kPi);
    }

    // The two values L4D2's pellet loop must draw for pellet `pellet` so that
    // it lands at (rightDeg, upDeg). Source angles: pitch grows downward, yaw
    // to the left.
    inline bool LoopDraws(float rightDeg, float upDeg, float scatterPitch, float scatterYaw, float& radius, float& halfAngleDeg)
    {
        if (!(scatterYaw > 0.0f))
            return false;
        const float ratio = scatterPitch != 0.0f ? scatterPitch / scatterYaw : 1.0f;
        const float pitch = -upDeg / ratio; // r * sin(theta)
        const float yaw = -rightDeg;        // r * cos(theta)
        radius = std::sqrt(pitch * pitch + yaw * yaw);
        float theta = std::atan2(pitch, yaw) * (180.0f / kPi);
        if (theta < 0.0f) theta += 360.0f;
        if (theta >= 360.0f) theta -= 360.0f;
        halfAngleDeg = theta * 0.5f;
        return std::isfinite(radius) && std::isfinite(halfAngleDeg);
    }

    // Per shot context the SharedRandomFloat detour reads (thread-local in the
    // shells): which weapon, and the start index once drawn.
    struct ShotContext
    {
        const Weapon* weapon = nullptr;
        bool startDrawn = false;
        std::uint32_t startIndex = 0u;
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
    // `drawStart` must return an integer in [0, 32767] from the shared seed
    // (the original SharedRandomFloat with kStartIndexName). A radius draw
    // whose range is not the stock scatter (a modified weapon script) leaves
    // the vanilla pellets alone.
    template <typename DrawStart>
    inline bool Override(ShotContext& shot, const char* name, float minValue, float maxValue, int seedIndex,
        DrawStart&& drawStart, float& result)
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
            if (!shot.startDrawn)
            {
                shot.startIndex = static_cast<std::uint32_t>(drawStart()) & 0x7fffu;
                shot.startDrawn = true;
            }
            float right = 0.0f, up = 0.0f, radius = 0.0f, half = 0.0f;
            PelletOffsetDeg(pellet, shot.startIndex, shot.weapon->evenSpread, right, up);
            if (!LoopDraws(right, up, shot.weapon->scatterPitch, shot.weapon->scatterYaw, radius, half))
                return false;
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

#pragma once
#include <algorithm>
#include <cmath>

// Stance recoil (std-only, see docs/physical-overhaul.md "Recoil"). VR players
// have no bullet spread: the Pavlov recoil is the only thing that moves their
// aim. Vanilla L4D2 makes a gun less accurate while moving or in the air and
// more accurate crouched, through each weapon script's spread values:
//   MinStandingSpread  standing still on the ground
//   MinDuckingSpread   crouched on the ground
//   MinInAirSpread     jumping or falling
//   MaxMovementSpread  added at full speed (MaxPlayerSpeed), in proportion to speed
// This turns the same values into a multiplier on the recoil (kick, limits and
// heat), so each gun's stances keep vanilla's order and proportions:
//
//   crouch   = lerp(CrouchFloor, 1, MinDucking / MinStanding)
//              (a ducking spread of 0 means "as steady as it gets", not "no recoil")
//   air      = 1 + (AirMax - 1) * x / (x + 1),          x = MinInAir / MinStanding - 1
//   movement = 1 + (MoveMax - 1) * m / (m + 2) * speed / MaxPlayerSpeed,
//              m = MaxMovementSpread / MinStanding
//
// The raw ratios run from 1.4 to 15 (air) and 1.9 to 30 (movement), because a
// sniper's standing spread is tiny. The saturating curves keep them in order
// but bounded: air 1.6x (MAC-10) to 2.9x (snipers), full-speed movement 1.4x
// (pistols, shotguns) to 1.75x (snipers). On the ground the result is
// crouch * movement; in the air it is the air factor alone (vanilla replaces
// the stance spread with the in-air one). Each part has a player scale that
// stretches its distance from 1 (0 turns that part off).
namespace l4d2vr_recoil_stance
{
    struct WeaponSpread
    {
        float standing = 0.0f;  // MinStandingSpread
        float ducking = 0.0f;   // MinDuckingSpread (0 is a real value: perfectly steady)
        float inAir = 0.0f;     // MinInAirSpread
        float movement = 0.0f;  // MaxMovementSpread
        float maxSpeed = 0.0f;  // MaxPlayerSpeed (units/s)

        bool Valid() const
        {
            return std::isfinite(standing) && std::isfinite(ducking) && std::isfinite(inAir) &&
                std::isfinite(movement) && std::isfinite(maxSpeed) && standing > 0.0f &&
                ducking >= 0.0f && inAir >= 0.0f && movement >= 0.0f && maxSpeed > 0.0f;
        }
    };

    // Vanilla L4D2 weapon script values (scripts/weapon_*.txt, update/pak01),
    // used when the game's own script could not be read. weaponId: L4D2
    // C_WeaponCSBase::WeaponID.
    inline bool VanillaSpread(int weaponId, WeaponSpread& out)
    {
        switch (weaponId)
        {
        case 1:  out = { 1.5f,  0.5f,  3.0f,  3.0f, 250.0f }; return true; // pistol
        case 32: out = { 1.25f, 0.5f,  3.0f,  3.0f, 250.0f }; return true; // magnum
        case 2:  out = { 1.0f,  0.7f,  1.7f,  3.0f, 250.0f }; return true; // uzi (smg)
        case 7:  out = { 1.2f,  0.85f, 1.7f,  3.0f, 250.0f }; return true; // mac10 (smg_silenced)
        case 33: out = { 1.1f,  0.75f, 1.7f,  3.0f, 250.0f }; return true; // mp5
        case 5:  out = { 0.4f,  0.05f, 1.5f,  5.0f, 230.0f }; return true; // m16 (rifle)
        case 26: out = { 1.0f,  0.5f,  3.0f,  6.0f, 230.0f }; return true; // ak47
        case 9:  out = { 0.35f, 0.05f, 1.25f, 4.0f, 230.0f }; return true; // scar (rifle_desert)
        case 34: out = { 0.4f,  0.05f, 1.5f,  5.0f, 230.0f }; return true; // sg552
        case 37: out = { 1.0f,  0.5f,  3.0f,  6.0f, 230.0f }; return true; // m60
        case 6:  out = { 0.1f,  0.0f,  1.5f,  3.0f, 210.0f }; return true; // hunting rifle
        case 10: out = { 0.5f,  0.05f, 1.5f,  5.0f, 210.0f }; return true; // sniper_military
        case 35: out = { 0.1f,  0.0f,  1.5f,  3.0f, 210.0f }; return true; // awp
        case 36: out = { 0.1f,  0.0f,  1.5f,  3.0f, 210.0f }; return true; // scout
        case 3:  out = { 0.8f,  0.0f,  2.5f,  1.5f, 220.0f }; return true; // pumpshotgun
        case 8:  out = { 0.8f,  0.0f,  2.5f,  1.5f, 220.0f }; return true; // shotgun_chrome
        case 4:  out = { 0.8f,  0.0f,  2.5f,  1.5f, 240.0f }; return true; // autoshotgun
        case 11: out = { 0.75f, 0.0f,  2.5f,  1.5f, 240.0f }; return true; // shotgun_spas
        case 21: out = { 0.4f,  0.05f, 1.5f,  5.0f, 230.0f }; return true; // grenade launcher
        default: return false;
        }
    }

    struct Tuning
    {
        float crouchFloor = 0.6f;  // multiplier of a gun whose ducking spread is 0
        float airMax = 3.0f;       // the air factor approaches this
        float moveMax = 1.8f;      // the full-speed movement factor approaches this
        float crouchScale = 1.0f;  // player scales: stretch each factor's distance from 1
        float airScale = 1.0f;
        float moveScale = 1.0f;
    };

    // Per-gun factors derived from the script (before the player scales).
    struct Factors
    {
        float crouch = 1.0f;   // on the ground, crouched, still
        float air = 1.0f;      // in the air
        float moveFull = 1.0f; // on the ground at MaxPlayerSpeed (multiplies crouch)
    };

    inline Factors Derive(const WeaponSpread& s, const Tuning& t = {})
    {
        Factors f{};
        if (!s.Valid())
            return f;
        const float floor = std::clamp(std::isfinite(t.crouchFloor) ? t.crouchFloor : 0.6f, 0.1f, 1.0f);
        const float airMax = std::clamp(std::isfinite(t.airMax) ? t.airMax : 3.0f, 1.0f, 6.0f);
        const float moveMax = std::clamp(std::isfinite(t.moveMax) ? t.moveMax : 1.8f, 1.0f, 4.0f);
        const float duck = std::clamp(s.ducking / s.standing, 0.0f, 1.0f);
        f.crouch = floor + (1.0f - floor) * duck;
        const float x = (std::max)(0.0f, s.inAir / s.standing - 1.0f);
        f.air = 1.0f + (airMax - 1.0f) * x / (x + 1.0f);
        const float m = (std::max)(0.0f, s.movement / s.standing);
        f.moveFull = 1.0f + (moveMax - 1.0f) * m / (m + 2.0f);
        return f;
    }

    struct PlayerState
    {
        bool onGround = true;
        bool crouched = false;
        float horizontalSpeed = 0.0f; // same unit as WeaponSpread::maxSpeed
    };

    inline float ScaleAboutOne(float factor, float scale)
    {
        if (!std::isfinite(factor) || !std::isfinite(scale))
            return 1.0f;
        return 1.0f + (factor - 1.0f) * std::clamp(scale, 0.0f, 3.0f);
    }

    constexpr float kMinMultiplier = 0.1f;
    constexpr float kMaxMultiplier = 6.0f;

    // The target multiplier for this state (no smoothing).
    inline float Target(const WeaponSpread& s, const PlayerState& p, const Tuning& t = {})
    {
        if (!s.Valid())
            return 1.0f;
        const Factors f = Derive(s, t);
        float mul = 1.0f;
        if (!p.onGround)
            mul = ScaleAboutOne(f.air, t.airScale);
        else
        {
            const float speed = std::isfinite(p.horizontalSpeed) ? (std::max)(0.0f, p.horizontalSpeed) : 0.0f;
            const float fraction = std::clamp(speed / s.maxSpeed, 0.0f, 1.0f);
            const float move = ScaleAboutOne(1.0f + (f.moveFull - 1.0f) * fraction, t.moveScale);
            const float crouch = p.crouched ? ScaleAboutOne(f.crouch, t.crouchScale) : 1.0f;
            mul = crouch * move;
        }
        return std::clamp(mul, kMinMultiplier, kMaxMultiplier);
    }

    // Eases the multiplier toward its target, frame-rate independent. A rise
    // (jumping, starting to run) takes effect quickly; a fall (landing,
    // stopping, crouching) eases in more slowly, so the kick never drops or
    // jumps between two shots. Leaving the ground only counts after a short
    // grace (stairs and small bumps are not jumps).
    class Smoother
    {
    public:
        static constexpr float kRiseSeconds = 0.08f;
        static constexpr float kFallSeconds = 0.25f;
        static constexpr float kAirGraceSeconds = 0.10f;

        void Reset(float value = 1.0f)
        {
            m_Value = std::isfinite(value) ? std::clamp(value, kMinMultiplier, kMaxMultiplier) : 1.0f;
            m_Airborne = 0.0f;
        }

        // Returns the eased multiplier.
        float Update(const WeaponSpread& s, PlayerState p, const Tuning& t, float dt)
        {
            if (!std::isfinite(dt) || dt < 0.0f)
                dt = 0.0f;
            dt = (std::min)(dt, 0.25f);
            if (p.onGround)
                m_Airborne = 0.0f;
            else
            {
                m_Airborne += dt;
                if (m_Airborne < kAirGraceSeconds)
                    p.onGround = true; // still on the ground for the grace, at its speed
            }
            const float target = Target(s, p, t);
            const float tau = target > m_Value ? kRiseSeconds : kFallSeconds;
            const float a = 1.0f - std::exp(-dt / tau);
            m_Value += (target - m_Value) * a;
            if (!std::isfinite(m_Value))
                m_Value = 1.0f;
            return m_Value;
        }

        float Value() const { return m_Value; }
    private:
        float m_Value = 1.0f;
        float m_Airborne = 0.0f;
    };
}

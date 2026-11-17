#pragma once

// 2.0.14 Pavlov handling feel for the Physical gameplay mode: haptic
// envelopes, handle travel ticks and sounds, the muzzle-in-wall fire block,
// the foregrip-release aim ease and the combined magazine / bolt release
// button. Numbers and rules from research/pavlov/handling/SPEC.md (Pavlov
// VR's shipped data; mechanics only, no Pavlov code or assets). std-only,
// unit tested (tests/vr_pavlov_feel_tests.cpp).

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <initializer_list>

namespace l4d2vr_pavlov_feel
{
    // ---- Haptic envelopes ---------------------------------------------------
    // An amplitude curve over time: keys (seconds, amplitude 0..1), linear
    // between keys; two keys at the same time make a step (the later one wins
    // from that time on). Ends at the last key.
    struct Key
    {
        float t;
        float amp;
    };

    constexpr int kMaxKeys = 24;
    // Pulses shorter than this are not felt on most controllers; a tick that
    // falls between two frames is still sent this long.
    constexpr float kMinPulseSeconds = 0.010f;
    constexpr float kMaxPulseSeconds = 0.060f;

    struct Envelope
    {
        std::array<Key, kMaxKeys> keys{};
        int count = 0;

        float Duration() const { return count > 0 ? keys[static_cast<std::size_t>(count - 1)].t : 0.0f; }

        float Eval(float t) const
        {
            if (count <= 0 || !std::isfinite(t) || t < keys[0].t)
                return 0.0f;
            int i = 0;
            while (i + 1 < count && keys[static_cast<std::size_t>(i + 1)].t <= t)
                ++i;
            if (i + 1 >= count)
                return t <= keys[static_cast<std::size_t>(i)].t ? keys[static_cast<std::size_t>(i)].amp : 0.0f;
            const Key& a = keys[static_cast<std::size_t>(i)];
            const Key& b = keys[static_cast<std::size_t>(i + 1)];
            const float span = b.t - a.t;
            if (!(span > 0.0f))
                return b.amp;
            const float u = std::clamp((t - a.t) / span, 0.0f, 1.0f);
            return a.amp + (b.amp - a.amp) * u;
        }

        // The highest amplitude of any key inside (from, to]: a short tick that
        // fell between two polls.
        float PeakKeyBetween(float from, float to) const
        {
            float peak = 0.0f;
            for (int i = 0; i < count; ++i)
            {
                const Key& k = keys[static_cast<std::size_t>(i)];
                if (k.t > from && k.t <= to)
                    peak = std::max(peak, k.amp);
            }
            return peak;
        }

        // Seconds from t until the curve is next at zero (its end at the latest).
        float TimeToSilence(float t) const
        {
            for (int i = 0; i < count; ++i)
            {
                const Key& k = keys[static_cast<std::size_t>(i)];
                if (k.t > t && k.amp <= 0.0001f)
                    return k.t - t;
            }
            return std::max(0.0f, Duration() - t);
        }
    };

    inline Envelope MakeEnvelope(std::initializer_list<Key> keys)
    {
        Envelope e{};
        for (const Key& k : keys)
        {
            if (e.count >= kMaxKeys)
                break;
            e.keys[static_cast<std::size_t>(e.count++)] = k;
        }
        return e;
    }

    // Decoded Pavlov haptic assets (SPEC 3.2; data/haptic_effects.json).
    enum class Effect : int
    {
        None = 0,
        ShotAug,      // AUG_Shot_HapticCurve: SMGs, M16 family, Galil, launcher
        ShotAk,       // AK_Shot_HapticCurve: pistols, AK, SKS, AutoSniper, LMG
        ShotAwp,      // AWP_Shot_HapticCurve: bolt-action rifles
        ShotShotgun,  // Shotgun_Shot_HapticCurve
        GenericTick,  // GenericHaptic curve: the 50 ms "contact" double tick
        GenericPulse, // Generic_Haptic buffer: 0.27 for 100 ms, 0.08 for 100 ms
        AmmoGrab,     // AmmoGrab_Haptic: 0.39 / 0.20 / 0 / 0.20 at 100 ms steps
        MagSlide,     // MagSlide_HapticCurve
        PumpPulled,   // ShotgunPumpPulled_Haptic (50 Hz buffer)
        PumpReleased, // ShotgunPumpReleased_Haptic
        BoltTick,     // AWP_BoltHaptic: 0.5 for 20 ms
        Count
    };

    inline Envelope EnvelopeFor(Effect effect)
    {
        switch (effect)
        {
        case Effect::ShotAug:
            return MakeEnvelope({ {0.0f, 1.0f}, {0.026f, 1.0f}, {0.029f, 0.0f}, {0.224f, 0.0f}, {0.224f, 0.37f},
                {0.226f, 0.37f}, {0.227f, 0.0f} });
        case Effect::ShotAk:
            return MakeEnvelope({ {0.0f, 1.0f}, {0.036f, 1.0f}, {0.047f, 0.0f}, {0.235f, 0.0f}, {0.235f, 0.62f},
                {0.239f, 0.62f}, {0.240f, 0.0f} });
        case Effect::ShotAwp:
            return MakeEnvelope({ {0.0f, 1.0f}, {0.040f, 1.0f}, {0.069f, 0.5f}, {0.200f, 0.89f}, {0.205f, 0.0f} });
        case Effect::ShotShotgun:
            return MakeEnvelope({ {0.0f, 0.53f}, {0.038f, 0.53f}, {0.040f, 0.0f},
                {0.067f, 0.0f}, {0.067f, 0.51f}, {0.072f, 0.51f}, {0.073f, 0.0f},
                {0.104f, 0.0f}, {0.104f, 0.44f}, {0.111f, 0.44f}, {0.112f, 0.0f},
                {0.152f, 0.0f}, {0.152f, 0.27f}, {0.161f, 0.27f}, {0.162f, 0.0f},
                {0.185f, 0.0f}, {0.185f, 0.23f}, {0.190f, 0.23f}, {0.191f, 0.0f},
                {0.206f, 0.0f}, {0.206f, 0.17f}, {0.210f, 0.17f}, {0.211f, 0.0f} });
        case Effect::GenericTick:
            return MakeEnvelope({ {0.0f, 0.25f}, {0.022f, 0.03f}, {0.043f, 0.24f}, {0.050f, 0.0f} });
        case Effect::GenericPulse:
            return MakeEnvelope({ {0.0f, 0.27f}, {0.1f, 0.27f}, {0.1f, 0.08f}, {0.2f, 0.08f}, {0.2f, 0.0f} });
        case Effect::AmmoGrab:
            return MakeEnvelope({ {0.0f, 0.39f}, {0.1f, 0.39f}, {0.1f, 0.20f}, {0.2f, 0.20f}, {0.2f, 0.0f},
                {0.3f, 0.0f}, {0.3f, 0.20f}, {0.4f, 0.20f}, {0.4f, 0.0f} });
        case Effect::MagSlide:
            return MakeEnvelope({ {0.0f, 0.12f}, {0.016f, 0.17f}, {0.058f, 0.06f}, {0.078f, 0.0f} });
        case Effect::PumpPulled:
            return MakeEnvelope({ {0.0f, 0.59f}, {0.02f, 0.59f}, {0.02f, 0.0f}, {0.04f, 0.0f}, {0.04f, 0.25f},
                {0.06f, 0.25f}, {0.06f, 0.0f}, {0.10f, 0.0f}, {0.10f, 1.0f}, {0.12f, 1.0f}, {0.12f, 0.0f} });
        case Effect::PumpReleased:
            return MakeEnvelope({ {0.0f, 1.0f}, {0.06f, 1.0f}, {0.06f, 0.0f} });
        case Effect::BoltTick:
            return MakeEnvelope({ {0.0f, 0.5f}, {0.02f, 0.5f}, {0.02f, 0.0f} });
        default:
            return Envelope{};
        }
    }

    inline const char* EffectName(Effect effect)
    {
        switch (effect)
        {
        case Effect::ShotAug: return "shot-aug";
        case Effect::ShotAk: return "shot-ak";
        case Effect::ShotAwp: return "shot-awp";
        case Effect::ShotShotgun: return "shot-shotgun";
        case Effect::GenericTick: return "generic-tick";
        case Effect::GenericPulse: return "generic-pulse";
        case Effect::AmmoGrab: return "ammo-grab";
        case Effect::MagSlide: return "mag-slide";
        case Effect::PumpPulled: return "pump-pulled";
        case Effect::PumpReleased: return "pump-released";
        case Effect::BoltTick: return "bolt-tick";
        default: return "none";
        }
    }

    // Plays one envelope on one hand: polled once per frame, it says what to
    // send to the controller now (amplitude and how long). A new Start
    // replaces the running envelope (Pavlov plays one effect per hand).
    struct Pulse
    {
        float amplitude = 0.0f;
        float durationSeconds = 0.0f;
    };

    class EnvelopePlayer
    {
    public:
        void Start(Effect effect, float scale, double now)
        {
            m_Envelope = EnvelopeFor(effect);
            m_Effect = effect;
            m_Scale = std::isfinite(scale) ? std::clamp(scale, 0.0f, 1.0f) : 0.0f;
            m_Start = now;
            m_LastT = -1.0f;
            m_CoveredUntil = -1.0f;
            m_Active = m_Envelope.count > 0 && m_Scale > 0.0f && std::isfinite(now);
        }

        void Stop() { m_Active = false; }
        bool Active() const { return m_Active; }
        Effect Current() const { return m_Active ? m_Effect : Effect::None; }

        // True when a pulse is due now.
        bool Poll(double now, Pulse& out)
        {
            out = {};
            if (!m_Active || !std::isfinite(now))
                return false;
            const float t = static_cast<float>(now - m_Start);
            if (t < 0.0f)
                return false;
            const float end = m_Envelope.Duration();
            const float clampedT = std::min(t, end);
            const float current = t <= end ? m_Envelope.Eval(t) : 0.0f;
            // A key that started a tick between the last poll and this one,
            // and was not inside the pulse already sent.
            const float missed = m_Envelope.PeakKeyBetween(std::max(m_LastT, m_CoveredUntil), clampedT);
            m_LastT = clampedT;
            if (t > end)
                m_Active = false;
            const float peak = std::max(current, missed);
            if (peak <= 0.001f)
                return false;
            const float duration = current > 0.001f ? m_Envelope.TimeToSilence(t) : 0.0f;
            out.amplitude = std::clamp(peak * m_Scale, 0.0f, 1.0f);
            out.durationSeconds = std::clamp(duration, kMinPulseSeconds, kMaxPulseSeconds);
            m_CoveredUntil = std::min(t, end) + out.durationSeconds;
            return true;
        }

    private:
        Envelope m_Envelope{};
        Effect m_Effect = Effect::None;
        float m_Scale = 1.0f;
        double m_Start = 0.0;
        float m_LastT = -1.0f;
        float m_CoveredUntil = -1.0f;
        bool m_Active = false;
    };

    // ---- Fire ----------------------------------------------------------------
    // The Pavlov shot curve of an L4D2 weapon (C_WeaponCSBase::WeaponID values,
    // through the L4D2 -> Pavlov gun mapping of the recoil research). None for
    // melee, throwables and items: they keep the haptics_config.txt pulse.
    inline Effect ShotEffectForWeapon(int weaponId)
    {
        switch (weaponId)
        {
        case 2: case 7: case 33: case 5: case 9: case 34: case 21:
            return Effect::ShotAug;     // uzi, mac10, mp5, m16, scar, sg552, grenade launcher
        case 1: case 32: case 26: case 6: case 10: case 37:
            return Effect::ShotAk;      // pistol, magnum, ak47, hunting rifle, military sniper, m60
        case 35: case 36:
            return Effect::ShotAwp;     // awp, scout
        case 3: case 8: case 4: case 11:
            return Effect::ShotShotgun; // pump, chrome, auto shotgun, spas
        default:
            return Effect::None;
        }
    }

    // L4D2's own fire interval per weapon (weapon script CycleTime, seconds).
    inline float FireIntervalSeconds(int weaponId)
    {
        switch (weaponId)
        {
        case 1: return 0.175f;   // pistol
        case 32: return 0.3f;    // magnum
        case 2: return 0.0625f;  // uzi
        case 7: return 0.075f;   // mac10
        case 33: return 0.06f;   // mp5
        case 5: return 0.0875f;  // m16
        case 26: return 0.13f;   // ak47
        case 9: return 0.07f;    // scar (inside a burst)
        case 34: return 0.0825f; // sg552
        case 3: case 8: return 0.875f; // pump, chrome
        case 4: return 0.15f;    // auto shotgun
        case 11: return 0.25f;   // spas
        case 6: return 0.25f;    // hunting rifle
        case 10: return 0.25f;   // military sniper
        case 35: return 1.25f;   // awp
        case 36: return 0.9f;    // scout
        case 37: return 0.11f;   // m60
        case 21: return 1.0f;    // grenade launcher
        default: return 0.1f;
        }
    }

    // Pavlov plays the shot effect at most once per FireRate x 0.5.
    inline bool ShotHapticDue(double now, double lastStart, bool haveLast, int weaponId)
    {
        if (!haveLast || !std::isfinite(now) || !std::isfinite(lastStart) || now < lastStart)
            return true;
        return now - lastStart >= 0.5 * static_cast<double>(FireIntervalSeconds(weaponId));
    }

    // ---- Handle travel (SPEC 4.1) ---------------------------------------------
    // A 10 ms pulse every 0.01 of handle input, amplitude Min + (Max - Min) x
    // input. The first pulse fires on the grab.
    constexpr float kHandlePulseStep = 0.01f;
    constexpr float kHandlePulseSeconds = 0.010f;

    class HandleTravelTicker
    {
    public:
        void Grab(float input)
        {
            m_Last = std::isfinite(input) ? input : 0.0f;
            m_Accumulated = kHandlePulseStep * 1.01f;  // primed: a pulse on the first update
            m_Held = true;
        }
        void Release() { m_Held = false; }
        bool Held() const { return m_Held; }

        // input: hand travel / LinearRange (Pavlov's handle input). True when a
        // pulse is due, with its amplitude.
        bool Update(float input, float minAmp, float maxAmp, float& amplitude)
        {
            amplitude = 0.0f;
            if (!m_Held || !std::isfinite(input))
                return false;
            m_Accumulated += std::fabs(input - m_Last);
            m_Last = input;
            if (!(m_Accumulated > kHandlePulseStep) || input > 1.0f)
                return false;
            m_Accumulated = 0.0f;
            const float u = std::clamp(input, 0.0f, 1.0f);
            amplitude = std::clamp(minAmp + (maxAmp - minAmp) * u, 0.0f, 1.0f);
            return amplitude > 0.0f;
        }

    private:
        float m_Last = 0.0f;
        float m_Accumulated = 0.0f;
        bool m_Held = false;
    };

    // ---- Handle sounds (SPEC 3.3, 4.1) ----------------------------------------
    // GrabSound once the input passes 0.1; BoltLockSound when it reaches the
    // cock point; BoltReleaseSound when it falls back below half of it after
    // that; UngrabSound on release if the input was past 0.25.
    constexpr float kGrabSoundInput = 0.1f;
    constexpr float kUngrabSoundInput = 0.25f;

    struct HandleSoundEvents
    {
        bool grab = false;
        bool lock = false;
        bool release = false;
    };

    class HandleSoundTracker
    {
    public:
        // initialInput: where the handle is taken (a slide locked open is
        // taken at its cock point: no grab or lock sound then).
        void Grab(float initialInput = 0.0f, float cockInput = 0.0f)
        {
            const float input = std::isfinite(initialInput) ? initialInput : 0.0f;
            m_GrabPlayed = input > kGrabSoundInput;
            m_Locked = cockInput > 0.0f && input >= cockInput;
            m_LastInput = input;
            m_Held = true;
        }

        HandleSoundEvents Update(float input, float cockInput)
        {
            HandleSoundEvents e{};
            if (!m_Held || !std::isfinite(input))
                return e;
            m_LastInput = input;
            if (!m_GrabPlayed && input > kGrabSoundInput)
            {
                m_GrabPlayed = true;
                e.grab = true;
            }
            if (!m_Locked && cockInput > 0.0f && input >= cockInput)
            {
                m_Locked = true;
                e.lock = true;
            }
            else if (m_Locked && input < cockInput * 0.5f)
            {
                m_Locked = false;
                e.release = true;
            }
            return e;
        }

        // On release: the ungrab sound is due when the handle was let go past
        // 0.25 of its travel.
        bool Ungrab()
        {
            const bool due = m_Held && m_LastInput > kUngrabSoundInput;
            m_Held = false;
            m_Locked = false;
            return due;
        }

        bool Held() const { return m_Held; }

    private:
        float m_LastInput = 0.0f;
        bool m_GrabPlayed = false;
        bool m_Locked = false;
        bool m_Held = false;
    };

    // ---- Magazine slide cues (SPEC 3.3) -----------------------------------------
    // The MagSlide curve on the magazine hand when the held magazine enters the
    // well's capture bounds (Pavlov MagSlideBounds, the slide-in start) and
    // again half way to the well (MagHalfSlideSoundThreshold 0.5). Leaving the
    // bounds by 20 % re-arms both.
    struct MagazineSlideEvents
    {
        bool start = false;
        bool half = false;
    };

    class MagazineSlideCue
    {
    public:
        MagazineSlideEvents Update(float distanceMeters, float boundMeters)
        {
            MagazineSlideEvents e{};
            if (!std::isfinite(distanceMeters) || !(boundMeters > 0.0f))
                return e;
            if (distanceMeters > boundMeters * 1.2f)
            {
                m_Started = false;
                m_Half = false;
                return e;
            }
            if (!m_Started && distanceMeters <= boundMeters)
            {
                m_Started = true;
                e.start = true;
            }
            if (m_Started && !m_Half && distanceMeters <= boundMeters * 0.5f)
            {
                m_Half = true;
                e.half = true;
            }
            return e;
        }
        void Reset() { m_Started = false; m_Half = false; }

    private:
        bool m_Started = false;
        bool m_Half = false;
    };

    // ---- Sounds (the game's own samples only) -----------------------------------
    // The click when a racked handle reaches its cock point (Pavlov
    // BoltLockSound): the gun family's magazine-lock click. Null where the gun
    // has none (shotguns, bolt-action rifles: they have their own cues).
    inline const char* HandleLockSound(int weaponId)
    {
        switch (weaponId)
        {
        case 1: return "weapons/pistol/gunother/pistol_clip_locked_1.wav";
        case 32: return "weapons/magnum/gunother/pistol_clip_locked_1.wav";
        case 2: case 33: return "weapons/smg/gunother/smg_clip_locked_1.wav";
        case 7: return "weapons/smg_silenced/gunother/smg_clip_locked_1.wav";
        case 5: case 34: return "weapons/rifle/gunother/rifle_clip_locked_1.wav";
        case 26: return "weapons/rifle_ak47/gunother/rifle_clip_locked_1.wav";
        case 9: return "weapons/rifle_desert/gunother/rifle_clip_locked_1.wav";
        case 6: return "weapons/hunting_rifle/gunother/hunting_rifle_cliplocked.wav";
        case 10: return "weapons/sniper_military/gunother/sniper_military_clip_locked_1.wav";
        case 37: return "weapons/machinegun_m60/gunother/rifle_clip_locked_1.wav";
        default: return nullptr;
        }
    }

    // Pavlov TwoHandGripSound: a light grab foley for pistols and SMGs
    // (Grab_TwoHand_Small), a heavier one for long guns (_Medium).
    inline bool LightForegripFoley(int weaponId)
    {
        switch (weaponId)
        {
        case 1: case 32: case 2: case 7: case 33:
            return true;
        default:
            return false;
        }
    }
    inline const char* ForegripGrabSound(int weaponId)
    {
        return LightForegripFoley(weaponId) ? "physics/body/body_medium_impact_soft1.wav"
                                            : "physics/metal/weapon_impact_soft1.wav";
    }
    inline float ForegripGrabVolume(int weaponId) { return LightForegripFoley(weaponId) ? 0.35f : 0.45f; }

    // The gun's dry-fire click (Pavlov TriggerSound) when a shot is refused.
    inline const char* DryFireSound(int weaponId)
    {
        return (weaponId == 1 || weaponId == 32) ? "weapons/clipempty_pistol.wav" : "weapons/clipempty_rifle.wav";
    }

    // A put-away or draw the holster refuses (Pavlov Put/GetFailureSound).
    constexpr const char* kHolsterFailureSound = "weapons/clipempty_pistol.wav";

    // ---- Gun inside a wall (SPEC 2.4) -------------------------------------------
    // Firing is refused while the gun overlaps the world; the gun hand gets the
    // Generic pulse after 0.5 s overlapped, then every 0.75 s.
    constexpr float kWallPulseFirstSeconds = 0.5f;
    constexpr float kWallPulseRepeatSeconds = 0.75f;

    class WallPulseTimer
    {
    public:
        bool Update(bool blocked, float dt)
        {
            if (!blocked || !std::isfinite(dt))
            {
                m_Timer = 0.0f;
                return false;
            }
            m_Timer += std::max(0.0f, dt);
            if (m_Timer >= kWallPulseFirstSeconds)
            {
                m_Timer = kWallPulseFirstSeconds - kWallPulseRepeatSeconds;   // -0.25: next one 0.75 s later
                return true;
            }
            return false;
        }
        void Reset() { m_Timer = 0.0f; }

    private:
        float m_Timer = 0.0f;
    };

    // Barrel length (metres) from the gun hand to the muzzle per weapon class:
    // the stretch traced for the wall test.
    inline float MuzzleReachMeters(int weaponId)
    {
        switch (weaponId)
        {
        case 1: case 32: return 0.18f;            // pistol, magnum
        case 2: case 7: return 0.24f;             // uzi, mac10
        case 33: return 0.32f;                    // mp5
        case 5: case 9: case 26: case 34: return 0.48f; // rifles
        case 3: case 8: case 4: case 11: return 0.55f;  // shotguns
        case 6: case 10: return 0.60f;            // hunting rifle, military sniper
        case 35: case 36: return 0.68f;           // awp, scout
        case 37: return 0.62f;                    // m60
        case 21: return 0.40f;                    // grenade launcher
        default: return 0.0f;                     // not a gun: never blocked
        }
    }

    // The wall test: the eye to the gun hand and the gun hand to the muzzle
    // must be clear (fractions of the two traces, 1 = clear). A trace that
    // starts inside a solid counts as blocked.
    inline bool MuzzleInWall(float eyeToHandFraction, bool eyeToHandStartSolid,
        float handToMuzzleFraction, bool handToMuzzleStartSolid)
    {
        const bool eyeBlocked = eyeToHandStartSolid || (std::isfinite(eyeToHandFraction) && eyeToHandFraction < 0.999f);
        const bool muzzleBlocked = handToMuzzleStartSolid ||
            (std::isfinite(handToMuzzleFraction) && handToMuzzleFraction < 0.999f);
        return eyeBlocked || muzzleBlocked;
    }

    // ---- Foregrip release ease (SPEC 2.3) ---------------------------------------
    // On release Pavlov eases LastTwoHandDirection back to the gun's forward
    // (1, 0, 0) with VInterpTo at speed 5 (about 0.2 s), in the gun hand's frame.
    struct V3
    {
        float x = 0.0f, y = 0.0f, z = 0.0f;
    };

    inline float Length(const V3& v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }

    inline bool Normalize(V3& v)
    {
        const float l = Length(v);
        if (!std::isfinite(l) || l < 1.0e-6f)
            return false;
        v.x /= l; v.y /= l; v.z /= l;
        return true;
    }

    // UE FMath::VInterpTo.
    inline V3 VInterpTo(const V3& current, const V3& target, float dt, float speed)
    {
        if (!(speed > 0.0f))
            return target;
        const V3 d{ target.x - current.x, target.y - current.y, target.z - current.z };
        if (d.x * d.x + d.y * d.y + d.z * d.z < 1.0e-8f)
            return target;
        const float a = std::clamp((std::isfinite(dt) ? dt : 0.0f) * speed, 0.0f, 1.0f);
        return { current.x + d.x * a, current.y + d.y * a, current.z + d.z * a };
    }

    constexpr float kTwoHandReleaseEaseSpeed = 5.0f;
    constexpr float kEaseDoneCos = 0.99999f;   // ~0.26 degrees

    class ReleaseEase
    {
    public:
        // localDir: the last two-handed aim direction in the gun hand's frame
        // (x forward). False (and inactive) when it is already the hand's forward.
        bool Begin(V3 localDir)
        {
            m_Active = Normalize(localDir) && localDir.x < kEaseDoneCos;
            m_Dir = localDir;
            return m_Active;
        }
        void Cancel() { m_Active = false; }
        bool Active() const { return m_Active; }
        const V3& Direction() const { return m_Dir; }

        // One frame: the direction (gun-hand frame) to aim along now.
        V3 Step(float dt, float speed = kTwoHandReleaseEaseSpeed)
        {
            if (!m_Active)
                return V3{ 1.0f, 0.0f, 0.0f };
            V3 next = VInterpTo(m_Dir, V3{ 1.0f, 0.0f, 0.0f }, dt, speed);
            if (!Normalize(next) || next.x >= kEaseDoneCos)
            {
                m_Active = false;
                m_Dir = V3{ 1.0f, 0.0f, 0.0f };
                return m_Dir;
            }
            m_Dir = next;
            return m_Dir;
        }

    private:
        V3 m_Dir{ 1.0f, 0.0f, 0.0f };
        bool m_Active = false;
    };

    // ---- Magazine / bolt release button (SPEC 4.3, MagBoltRelease) --------------
    enum class MagButtonAction : int { None = 0, ReleaseBolt, EjectMagazine };

    // A slide or bolt locked open with a loaded magazine seated: the button
    // releases it (chambers). Otherwise it is the magazine release.
    inline MagButtonAction MagBoltRelease(bool pressed, bool boltLockedOpen, bool magazineSeated,
        int roundsInMagazine, bool detachableMagazine)
    {
        if (!pressed)
            return MagButtonAction::None;
        if (boltLockedOpen && magazineSeated && roundsInMagazine > 0)
            return MagButtonAction::ReleaseBolt;
        return detachableMagazine ? MagButtonAction::EjectMagazine : MagButtonAction::None;
    }

    // ---- HK lock-open (SPEC 4.3, bBoltLockManual) --------------------------------
    // Racking an HK gun with no rounds in the magazine and letting go past the
    // cock point locks the handle back; with rounds it chambers.
    enum class HkRackResult : int { Nothing = 0, LockedOpen, Chambered };

    inline HkRackResult HkRackRelease(bool reachedCockPoint, bool magazineLoaded)
    {
        if (!reachedCockPoint)
            return HkRackResult::Nothing;
        return magazineLoaded ? HkRackResult::Chambered : HkRackResult::LockedOpen;
    }
}

#pragma once

// Pavlov-style grenade throws (molotov, pipe bomb, bile jar). std-only.
//
// The native grenade only leaves the hand after its own state machine: the
// pin-pull animation holds the throw for 0.5 s after the press, and the throw
// itself waits another 0.2 s (CBaseCSGrenade::StartGrenadeThrow) before
// ThrowGrenade creates the projectile at the eye. A VR hand throw instead:
//   * measures the release velocity on the client from the tracked controller
//     (tracking space, every headset frame, the last ~80 ms before release);
//   * sends it with the release command (impulse kImpulse, see Pack* below);
//   * the server spawns the projectile inside that very command at the hand,
//     with velocity = hand velocity * scale + the player's own velocity.
//
// Wire payload of the release command (only sent when the server advertises
// the HandThrow feature; never to plain servers):
//   impulse        kImpulse (244)
//   forwardmove    ((round(move) + 512) << 12) | (vx + 2048)   exact in a float
//   sidemove       ((round(move) + 512) << 12) | (vy + 2048)
//   weaponselect   vz + 1024                                    (11 bits, never 0)
//   weaponsubtype  kSubtypeMarker | left << 2 | kind            (6 bits)
// vx/vy/vz are the hand velocity relative to the player, world axes, game
// units per second, before scaling. The decoder restores the movement
// (rounded to whole units) and clears weaponselect/weaponsubtype.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace l4d2vr_hand_throw
{
    struct V3
    {
        float x = 0.0f, y = 0.0f, z = 0.0f;
        V3() = default;
        V3(float ax, float ay, float az) : x(ax), y(ay), z(az) {}
        V3 operator+(const V3& o) const { return { x + o.x, y + o.y, z + o.z }; }
        V3 operator-(const V3& o) const { return { x - o.x, y - o.y, z - o.z }; }
        V3 operator*(float s) const { return { x * s, y * s, z * s }; }
        float Length() const { return std::sqrt(x * x + y * y + z * z); }
        bool Finite() const { return std::isfinite(x) && std::isfinite(y) && std::isfinite(z); }
    };

    // ---- Wire --------------------------------------------------------------
    constexpr std::uint8_t kImpulse = 244u;
    constexpr int kHorizontalLimit = 2047;    // |vx|, |vy| (units/s)
    constexpr int kVerticalLimit = 1023;      // |vz| (units/s)
    constexpr int kMoveLimit = 511;           // |forwardmove|, |sidemove| carried
    constexpr int kSubtypeMarker = 0x20;

    enum class Kind : int { None = 0, Molotov = 1, PipeBomb = 2, VomitJar = 3 };

    // CSWeaponID values (L4D2): molotov 13, pipe bomb 14, vomit jar 25.
    inline Kind KindFromWeaponId(int id)
    {
        switch (id)
        {
        case 13: return Kind::Molotov;
        case 14: return Kind::PipeBomb;
        case 25: return Kind::VomitJar;
        default: return Kind::None;
        }
    }
    inline int WeaponIdFromKind(Kind kind)
    {
        switch (kind)
        {
        case Kind::Molotov: return 13;
        case Kind::PipeBomb: return 14;
        case Kind::VomitJar: return 25;
        default: return 0;
        }
    }

    inline int ClampRound(float value, int limit)
    {
        if (!std::isfinite(value))
            return 0;
        const float r = std::round(value);
        return static_cast<int>(std::clamp(r, static_cast<float>(-limit), static_cast<float>(limit)));
    }

    // The movement that the decoder will hand to the game (whole units).
    inline float QuantizedMove(float move) { return static_cast<float>(ClampRound(move, kMoveLimit)); }

    inline float PackMove(float move, float velocity)
    {
        const int m = ClampRound(move, kMoveLimit) + 512;            // 1..1023
        const int v = ClampRound(velocity, kHorizontalLimit) + 2048; // 1..4095
        return static_cast<float>((m << 12) | v);                    // < 2^22: exact
    }

    inline bool UnpackMove(float packed, float& move, float& velocity)
    {
        move = 0.0f;
        velocity = 0.0f;
        if (!std::isfinite(packed) || packed < 4096.0f || packed > 4194303.0f || std::floor(packed) != packed)
            return false;
        const int n = static_cast<int>(packed);
        const int m = n >> 12, v = n & 0xFFF;
        if (m < 1 || m > 1023 || v < 1 || v > 4095)
            return false;
        move = static_cast<float>(m - 512);
        velocity = static_cast<float>(v - 2048);
        return true;
    }

    inline void PackVertical(float vz, Kind kind, bool leftHand, int& weaponselect, int& weaponsubtype)
    {
        weaponselect = ClampRound(vz, kVerticalLimit) + 1024;          // 1..2047
        weaponsubtype = kSubtypeMarker | (leftHand ? 4 : 0) | (static_cast<int>(kind) & 3);
    }

    inline bool UnpackVertical(int weaponselect, int weaponsubtype, float& vz, Kind& kind, bool& leftHand)
    {
        vz = 0.0f;
        kind = Kind::None;
        leftHand = false;
        if (weaponselect < 1 || weaponselect > 2047 || (weaponsubtype & ~0x27) != 0 ||
            (weaponsubtype & kSubtypeMarker) == 0 || (weaponsubtype & 3) == 0)
            return false;
        vz = static_cast<float>(weaponselect - 1024);
        kind = static_cast<Kind>(weaponsubtype & 3);
        leftHand = (weaponsubtype & 4) != 0;
        return true;
    }

    struct Payload
    {
        bool valid = false;
        Kind kind = Kind::None;
        bool leftHand = false;
        V3 velocity{};        // hand relative to the player, world axes, units/s (unscaled)
    };

    // Decoder half shared by the dedicated plugin and the listen host. On
    // return the four fields hold what the game must see: restored movement,
    // weaponselect/weaponsubtype 0. A malformed payload zeroes the movement.
    inline Payload DecodeFields(float& forwardmove, float& sidemove, int& weaponselect, int& weaponsubtype)
    {
        Payload p{};
        float fwd = 0.0f, side = 0.0f, vx = 0.0f, vy = 0.0f, vz = 0.0f;
        const bool moveOk = UnpackMove(forwardmove, fwd, vx) && UnpackMove(sidemove, side, vy);
        const bool vertOk = UnpackVertical(weaponselect, weaponsubtype, vz, p.kind, p.leftHand);
        forwardmove = moveOk ? fwd : 0.0f;
        sidemove = moveOk ? side : 0.0f;
        weaponselect = 0;
        weaponsubtype = 0;
        p.valid = moveOk && vertOk;
        if (p.valid)
            p.velocity = V3(vx, vy, vz);
        else
            p.kind = Kind::None;
        return p;
    }

    // ---- Server ------------------------------------------------------------
    // Final projectile velocity: the scaled hand motion plus the body's own
    // motion (a throw while running carries the run), clamped.
    inline V3 ComposeVelocity(const V3& handRelative, const V3& playerVelocity, float scale, float maxSpeed)
    {
        V3 v = handRelative * (std::isfinite(scale) ? std::max(0.0f, scale) : 0.0f);
        if (playerVelocity.Finite())
            v = v + playerVelocity;
        if (!v.Finite())
            return {};
        const float length = v.Length();
        if (!(maxSpeed > 0.0f))
            return {};
        if (length > maxSpeed && length > 0.0001f)
            v = v * (maxSpeed / length);
        return v;
    }

    // Player velocity from (time, origin) samples: least-squares slope.
    template <std::size_t N>
    inline bool OriginVelocity(const std::array<float, N>& times, const std::array<V3, N>& origins, int count, V3& out)
    {
        out = {};
        if (count < 2)
            return false;
        double st = 0.0, stt = 0.0;
        V3 sp{}, stp{};
        const double t0 = times[0];
        for (int i = 0; i < count; ++i)
        {
            const double t = times[static_cast<std::size_t>(i)] - t0;
            const V3& p = origins[static_cast<std::size_t>(i)];
            st += t;
            stt += t * t;
            sp = sp + p;
            stp = stp + p * static_cast<float>(t);
        }
        const double n = count;
        const double denom = n * stt - st * st;
        if (!(denom > 1e-9))
            return false;
        out = V3(static_cast<float>((n * stp.x - st * sp.x) / denom),
            static_cast<float>((n * stp.y - st * sp.y) / denom),
            static_cast<float>((n * stp.z - st * sp.z) / denom));
        return out.Finite();
    }

    // ---- Listen host: server.dll SendProp offsets ---------------------------
    // MSVC builds a SendProp table entry as "push <offset>; push <name>"
    // (68 imm32 68 imm32). Finds the offset of the prop named by the string at
    // `nameVa` inside `code` (mapped at `codeVa`): every such pair must agree.
    inline int FindSendPropPushOffset(const std::uint8_t* code, std::size_t size, std::uint32_t nameVa,
        int maxOffset = 0x10000)
    {
        if (!code || size < 10u || nameVa == 0u)
            return -1;
        const std::uint8_t n0 = static_cast<std::uint8_t>(nameVa), n1 = static_cast<std::uint8_t>(nameVa >> 8),
            n2 = static_cast<std::uint8_t>(nameVa >> 16), n3 = static_cast<std::uint8_t>(nameVa >> 24);
        int found = -1;
        for (std::size_t i = 5u; i + 5u <= size; ++i)
        {
            if (code[i] != 0x68u || code[i + 1] != n0 || code[i + 2] != n1 || code[i + 3] != n2 || code[i + 4] != n3 ||
                code[i - 5] != 0x68u)
                continue;
            const std::uint32_t imm = static_cast<std::uint32_t>(code[i - 4]) | (static_cast<std::uint32_t>(code[i - 3]) << 8) |
                (static_cast<std::uint32_t>(code[i - 2]) << 16) | (static_cast<std::uint32_t>(code[i - 1]) << 24);
            if (imm == 0u || imm >= static_cast<std::uint32_t>(maxOffset))
                return -1;
            if (found >= 0 && found != static_cast<int>(imm))
                return -1;
            found = static_cast<int>(imm);
        }
        return found;
    }

    // ---- Client: release velocity -----------------------------------------
    // Samples of one hand in tracking space (metres, Source axes: x forward,
    // y left, z up), one per headset frame. Excludes stick turning and the
    // body's locomotion by construction.
    class VelocityTracker
    {
    public:
        static constexpr int kCapacity = 64;

        void Reset() { m_Count = 0; m_Head = 0; }
        int Count() const { return m_Count; }

        void Add(double seconds, const V3& position)
        {
            if (!std::isfinite(seconds) || !position.Finite())
                return;
            if (m_Count > 0)
            {
                const Sample& last = At(m_Count - 1);
                if (seconds <= last.t)
                    return;                     // same or older frame
                if (seconds - last.t > 0.25)
                    Reset();                    // tracking gap: start over
            }
            m_Samples[static_cast<std::size_t>(m_Head)] = { seconds, position };
            m_Head = (m_Head + 1) % kCapacity;
            if (m_Count < kCapacity)
                ++m_Count;
        }

        // Velocity (m/s) at `now`: a least-squares fit over the last `window`
        // seconds, blended toward the fastest ~3-frame segment inside it
        // (people let go while the hand already slows down).
        bool Estimate(double now, float window, float peakBlend, V3& out) const
        {
            out = {};
            window = std::clamp(std::isfinite(window) ? window : 0.08f, 0.02f, 0.25f);
            peakBlend = std::clamp(std::isfinite(peakBlend) ? peakBlend : 0.0f, 0.0f, 1.0f);
            int first = m_Count;
            for (int i = m_Count - 1; i >= 0; --i)
            {
                const Sample& s = At(i);
                if (s.t > now + 0.001)
                    continue;
                if (now - s.t > window)
                    break;
                first = i;
            }
            int last = m_Count - 1;
            while (last >= 0 && At(last).t > now + 0.001)
                --last;
            if (first > last || last - first + 1 < 2)
            {
                // Fewer than two frames inside the window: the two newest ones.
                if (last < 1)
                    return false;
                first = last - 1;
            }
            V3 average{};
            if (!Fit(first, last, average))
                return false;
            V3 peak = average;
            float peakSpeed = average.Length();
            for (int end = first + 2; end <= last; ++end)
            {
                V3 segment{};
                if (Fit(end - 2, end, segment) && segment.Length() > peakSpeed)
                {
                    peakSpeed = segment.Length();
                    peak = segment;
                }
            }
            out = average * (1.0f - peakBlend) + peak * peakBlend;
            return out.Finite();
        }

    private:
        struct Sample { double t = 0.0; V3 p{}; };

        const Sample& At(int i) const
        {
            const int start = (m_Head - m_Count + kCapacity) % kCapacity;
            return m_Samples[static_cast<std::size_t>((start + i) % kCapacity)];
        }

        bool Fit(int first, int last, V3& out) const
        {
            const int n = last - first + 1;
            if (n < 2)
                return false;
            const double t0 = At(first).t;
            double st = 0.0, stt = 0.0;
            double sx = 0.0, sy = 0.0, sz = 0.0, sxt = 0.0, syt = 0.0, szt = 0.0;
            for (int i = first; i <= last; ++i)
            {
                const Sample& s = At(i);
                const double t = s.t - t0;
                st += t; stt += t * t;
                sx += s.p.x; sy += s.p.y; sz += s.p.z;
                sxt += s.p.x * t; syt += s.p.y * t; szt += s.p.z * t;
            }
            const double denom = n * stt - st * st;
            if (!(denom > 1e-12))
                return false;
            out = V3(static_cast<float>((n * sxt - st * sx) / denom),
                static_cast<float>((n * syt - st * sy) / denom),
                static_cast<float>((n * szt - st * sz) / denom));
            return out.Finite();
        }

        std::array<Sample, kCapacity> m_Samples{};
        int m_Count = 0;
        int m_Head = 0;
    };

    // Tracking-space velocity (m/s) to world axes (units/s): the stick-turn
    // yaw rotates the play space, the VR scale converts metres to units.
    inline V3 TrackingToWorld(const V3& v, float yawDeg, float unitsPerMetre)
    {
        const float r = yawDeg * 3.14159265358979f / 180.0f;
        const float c = std::cos(r), s = std::sin(r);
        return V3((v.x * c - v.y * s) * unitsPerMetre, (v.x * s + v.y * c) * unitsPerMetre, v.z * unitsPerMetre);
    }

    // ---- Client: hold / pin / release --------------------------------------
    // Grip holds the grenade, the trigger pulls the pin, letting go of the grip
    // throws it (Pavlov). Holding the trigger through the swing and letting go
    // of it throws too (the old habit), as does a trigger release when the grip
    // does not hold the item (it was selected another way).
    struct ReleaseInput
    {
        bool throwable = false;        // a hand-throwable grenade is the active item
        std::uintptr_t weapon = 0u;    // its identity (a change disarms)
        bool blocked = false;          // Use held, menus, pinned... (no arming, no throw)
        bool gripOwned = false;        // the weapon-hand grip holds the item
        bool gripReleased = false;     // that grip let go of it away from any holster
        bool triggerDown = false;
        bool pinRequired = true;       // a grip release without the pin drops the item
        std::uint64_t nowMs = 0u;
    };

    enum class Action { None, PinPulled, Throw };

    class ReleaseFsm
    {
    public:
        static constexpr std::uint32_t kTriggerHoldThrowMs = 300u;   // a held trigger is a "hold and swing"
        static constexpr std::uint32_t kThrownResetMs = 1500u;       // server did not take it: try again

        bool Armed() const { return m_Armed; }
        bool Thrown() const { return m_Thrown; }
        void Reset() { *this = ReleaseFsm{}; }

        // True when a grip release must become a throw rather than a drop.
        bool GripReleaseThrows(bool pinRequired) const { return !m_Thrown && (m_Armed || !pinRequired); }

        Action Update(const ReleaseInput& in)
        {
            const bool pressed = in.triggerDown && !m_PrevTrigger;
            const bool released = !in.triggerDown && m_PrevTrigger;
            m_PrevTrigger = in.triggerDown;
            if (!in.throwable || in.weapon != m_Weapon)
            {
                const bool prevTrigger = m_PrevTrigger;
                *this = ReleaseFsm{};
                m_Weapon = in.throwable ? in.weapon : 0u;
                // A trigger already held while switching to the grenade must be
                // released and pressed again before it pulls the pin.
                m_PrevTrigger = prevTrigger;
                return Action::None;
            }
            if (m_Thrown)
            {
                if (in.nowMs - m_ThrownMs > kThrownResetMs)
                {
                    m_Thrown = false;
                    m_Armed = false;
                }
                return Action::None;
            }
            if (in.blocked)
                return Action::None;
            if (in.gripReleased && in.gripOwned && GripReleaseThrows(in.pinRequired))
                return Throw(in.nowMs);
            if (pressed && !m_Armed)
            {
                m_Armed = true;
                m_PressMs = in.nowMs;
                return Action::PinPulled;
            }
            if (released && m_Armed && (!in.gripOwned || in.nowMs - m_PressMs >= kTriggerHoldThrowMs))
                return Throw(in.nowMs);
            return Action::None;
        }

    private:
        Action Throw(std::uint64_t nowMs)
        {
            m_Thrown = true;
            m_ThrownMs = nowMs;
            return Action::Throw;
        }

        std::uintptr_t m_Weapon = 0u;
        bool m_PrevTrigger = false;
        bool m_Armed = false;
        bool m_Thrown = false;
        std::uint64_t m_PressMs = 0u;
        std::uint64_t m_ThrownMs = 0u;
    };
}

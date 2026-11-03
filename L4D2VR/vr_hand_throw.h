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

        // Pavlov-style release velocity (m/s) at `now`: the hand's peak over
        // the last `window` seconds, along the direction it was moving then.
        // People let go of the trigger while the hand already slows down (the
        // release is noticed a frame or two late as well), so the speed at the
        // moment of release, or an average over the window, turns a real throw
        // into a lob at the feet. The peak is the fastest 3-frame fit; the
        // direction is a 5-frame fit centred on it (steadier than 3 frames).
        // A deliberate slow release has no fast part in the window and stays slow.
        bool EstimateRelease(double now, float window, V3& out) const
        {
            out = {};
            window = std::clamp(std::isfinite(window) ? window : 0.08f, 0.02f, 0.25f);
            int last = m_Count - 1;
            while (last >= 0 && At(last).t > now + 0.001)
                --last;
            if (last < 1)
                return false;
            int first = last;
            while (first > 0 && now - At(first - 1).t <= window)
                --first;
            if (last - first + 1 < 3)
            {
                // Fewer than three frames inside the window: the newest ones.
                first = (std::max)(0, last - 2);
                return Fit(first, last, out);
            }
            V3 peak{};
            float peakSpeed = -1.0f;
            int peakEnd = last;
            for (int end = first + 2; end <= last; ++end)
            {
                V3 segment{};
                if (Fit(end - 2, end, segment) && segment.Length() > peakSpeed)
                {
                    peakSpeed = segment.Length();
                    peak = segment;
                    peakEnd = end;
                }
            }
            if (peakSpeed < 0.0f)
                return false;
            const int a = (std::max)(first, peakEnd - 3), b = (std::min)(last, peakEnd + 1);
            V3 around{};
            const float aroundSpeed = Fit(a, b, around) ? around.Length() : 0.0f;
            out = aroundSpeed > 0.0001f ? around * (peakSpeed / aroundSpeed) : peak;
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
    // Hold the trigger of the hand holding the grenade (that pulls the pin),
    // make the throwing motion and let go of the trigger: the grenade leaves the
    // hand in that very command with the hand's release velocity. Every trigger
    // release after a press throws, however short the hold; a slow release just
    // drops or lobs it (the velocity is the hand's own). Letting go of the grip
    // with the trigger still held throws too; letting go of the grip without the
    // pin drops the grenade as an item (pinRequired).
    struct ReleaseInput
    {
        bool throwable = false;        // a hand-throwable grenade is the active item
        std::uintptr_t weapon = 0u;    // its identity (a change disarms)
        bool blocked = false;          // Use held, menus, pinned... (no arming, no throw)
        bool gripOwned = false;        // the weapon-hand grip holds the item
        bool gripReleased = false;     // that grip let go of it away from any holster
        bool triggerDown = false;      // the item hand's trigger
        bool pinRequired = true;       // a grip release without the pin drops the item
        std::uint64_t nowMs = 0u;
    };

    enum class Action { None, PinPulled, Throw };

    class ReleaseFsm
    {
    public:
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
            {
                // No throw into a menu or while pinned; a release while blocked
                // disarms (press again to pull the pin).
                if (released)
                    m_Armed = false;
                return Action::None;
            }
            if (in.gripReleased && in.gripOwned && GripReleaseThrows(in.pinRequired))
                return Throw(in.nowMs);
            if (released && m_Armed)
                return Throw(in.nowMs);
            if (pressed && !m_Armed)
            {
                m_Armed = true;
                return Action::PinPulled;
            }
            return Action::None;
        }

    private:
        Action Throw(std::uint64_t nowMs)
        {
            m_Thrown = true;
            m_Armed = false;
            m_ThrownMs = nowMs;
            return Action::Throw;
        }

        std::uintptr_t m_Weapon = 0u;
        bool m_PrevTrigger = false;
        bool m_Armed = false;
        bool m_Thrown = false;
        std::uint64_t m_ThrownMs = 0u;
    };

    // The throw boost: the projectile leaves with the hand's release velocity
    // (its peak, EstimateRelease) times this, plus the body's own motion.
    // ManualThrowHandVelocityScale / the plugin's ThrowHandVelocityScale.
    // 2.0.4: 2.5 (was 1.5 on a half-peak blend: a 3.5 m/s throw left at
    // 227 units/s, 5 m/s, and landed a couple of metres away). A natural
    // overhand (peak 5-6 m/s at the hand) now leaves at 12-15 m/s; a slow
    // deliberate release (under 1 m/s) still only drops the grenade.
    constexpr float kDefaultThrowBoost = 2.5f;

    // ---- Client: physics throw assistance (GrenadeThrowAssist, 2.0.5) -------
    // In GrenadeThrowMode=physics the grenade leaves with the hand's own
    // velocity, and release timing makes that direction noisy. The assistance
    // gently bends a throw toward the arc that lands where the hand points,
    // but only when the throw is already heading there: inside a heading cone,
    // a launch-elevation window and a limited speed change. Anything else (a
    // throw at something else, a drop, a toss at the feet) stays the hand's own.
    // Velocities are the final projectile velocity (world, units/s: the boosted
    // hand velocity plus the body's own); gravity is the projectile's (L4D2
    // throwables: 0.4 x sv_gravity = 320 units/s^2).
    struct AssistInput
    {
        V3 origin{};               // release point (the projectile spawns here)
        V3 velocity{};             // the unassisted launch velocity
        V3 target{};               // where the hand points (ThrowAssistTarget)
        bool targetValid = false;
        float gravity = 320.0f;
        float strength = 0.5f;     // GrenadeThrowAssistStrength, 0 (off) .. 1 (lands on the target)
        float coneDeg = 25.0f;     // heading difference allowed (full assist inside half of it)
        float elevationDeg = 20.0f;// launch elevation difference allowed (full inside half)
        float maxSpeedChange = 0.35f; // the solution may be at most this much faster or slower
        float minSpeed = 150.0f;   // slower releases (drops) are never assisted
        float maxSpeed = 1400.0f;  // server clamp (ManualThrowMaxVelocity / ThrowMaxSpeed)
    };

    struct AssistResult
    {
        V3 velocity{};             // the assisted launch velocity (== input when not applied)
        V3 solution{};             // the arc that lands on the target (when one was found)
        float weight = 0.0f;       // strength x gates actually applied (0..1)
        bool applied = false;
        const char* reason = "off";// why not (or "assisted")
    };

    inline float AssistFalloff(float x, float full, float zero)
    {
        if (!(x > full))
            return 1.0f;
        if (!(x < zero))
            return 0.0f;
        const float u = (x - full) / (zero - full);
        return 1.0f - u * u * (3.0f - 2.0f * u);
    }

    // The launch at `speed` that passes through `target` (a projectile under
    // `gravity`), on the branch whose elevation is nearest `preferElevationRad`.
    // False when `speed` cannot reach the target.
    inline bool BallisticVelocityTo(const V3& origin, const V3& target, float gravity, float speed,
        float preferElevationRad, V3& out)
    {
        out = {};
        const V3 d = target - origin;
        const float dh = std::sqrt(d.x * d.x + d.y * d.y);
        if (!d.Finite() || !(dh > 1.0f) || !(gravity > 0.0f) || !(speed > 0.0f) || !std::isfinite(speed))
            return false;
        const float s2 = speed * speed;
        const float disc = s2 * s2 - gravity * (gravity * dh * dh + 2.0f * d.z * s2);
        if (!(disc >= 0.0f))
            return false;
        const float root = std::sqrt(disc);
        const float low = std::atan((s2 - root) / (gravity * dh));
        const float high = std::atan((s2 + root) / (gravity * dh));
        const float elevation = std::fabs(low - preferElevationRad) <= std::fabs(high - preferElevationRad) ? low : high;
        const float hx = d.x / dh, hy = d.y / dh;
        out = V3(hx * std::cos(elevation) * speed, hy * std::cos(elevation) * speed, std::sin(elevation) * speed);
        return out.Finite();
    }

    // The slowest launch that reaches `target` (its elevation bisects the
    // vertical and the line to the target).
    inline float MinimumSpeedTo(const V3& origin, const V3& target, float gravity)
    {
        const V3 d = target - origin;
        const float dist = d.Length();
        if (!d.Finite() || !(gravity > 0.0f))
            return 0.0f;
        return std::sqrt(std::max(0.0f, gravity * (d.z + dist)));
    }

    // Where a launch comes down through z = groundZ (descending); false if never.
    inline bool BallisticLanding(const V3& origin, const V3& velocity, float gravity, float groundZ, V3& out)
    {
        out = {};
        if (!origin.Finite() || !velocity.Finite() || !(gravity > 0.0f) || !std::isfinite(groundZ))
            return false;
        const float disc = velocity.z * velocity.z + 2.0f * gravity * (origin.z - groundZ);
        if (!(disc >= 0.0f))
            return false;
        const float t = (velocity.z + std::sqrt(disc)) / gravity;
        if (!(t >= 0.0f) || !std::isfinite(t))
            return false;
        out = V3(origin.x + velocity.x * t, origin.y + velocity.y * t, groundZ);
        return out.Finite();
    }

    // "Where the throw is heading" when the hand's aim ray finds nothing: the
    // throw's own landing on groundZ, turned about the release point onto the
    // hand's aim heading (only the sideways error is corrected).
    inline bool HeadingTarget(const V3& origin, const V3& velocity, float gravity, float groundZ, const V3& aim, V3& out)
    {
        out = {};
        V3 landing{};
        const float ah = std::sqrt(aim.x * aim.x + aim.y * aim.y);
        if (!aim.Finite() || !(ah > 1e-4f) || !BallisticLanding(origin, velocity, gravity, groundZ, landing))
            return false;
        const float dx = landing.x - origin.x, dy = landing.y - origin.y;
        const float range = std::sqrt(dx * dx + dy * dy);
        if (!(range > 1.0f))
            return false;
        out = V3(origin.x + aim.x / ah * range, origin.y + aim.y / ah * range, groundZ);
        return out.Finite();
    }

    inline AssistResult AssistThrow(const AssistInput& in)
    {
        AssistResult r{};
        r.velocity = in.velocity;
        const float strength = std::isfinite(in.strength) ? std::clamp(in.strength, 0.0f, 1.0f) : 0.0f;
        if (!(strength > 0.0f))
            return r;
        if (!in.targetValid || !in.target.Finite() || !in.origin.Finite() || !in.velocity.Finite())
        {
            r.reason = "no target";
            return r;
        }
        const float speed = in.velocity.Length();
        if (!(speed >= in.minSpeed))
        {
            r.reason = "slow release";
            return r;
        }
        const V3 d = in.target - in.origin;
        const float dh = std::sqrt(d.x * d.x + d.y * d.y);
        const float vh = std::sqrt(in.velocity.x * in.velocity.x + in.velocity.y * in.velocity.y);
        if (!(dh > 16.0f) || !(vh > speed * 0.05f))
        {
            r.reason = "target under the hand or throw straight up";
            return r;
        }
        constexpr float kRad = 3.14159265358979f / 180.0f;
        const float cosHeading = std::clamp((in.velocity.x * d.x + in.velocity.y * d.y) / (vh * dh), -1.0f, 1.0f);
        const float headingDeg = std::acos(cosHeading) / kRad;
        const float cone = std::clamp(in.coneDeg, 1.0f, 90.0f);
        const float wHeading = AssistFalloff(headingDeg, cone * 0.5f, cone);
        if (!(wHeading > 0.0f))
        {
            r.reason = "thrown elsewhere";
            return r;
        }
        const float throwElevation = std::atan2(in.velocity.z, vh);
        float solveSpeed = speed;
        const float minSpeed = MinimumSpeedTo(in.origin, in.target, in.gravity);
        if (solveSpeed < minSpeed * 1.001f)
            solveSpeed = minSpeed * 1.001f;
        V3 solution{};
        if (!BallisticVelocityTo(in.origin, in.target, in.gravity, solveSpeed, throwElevation, solution))
        {
            r.reason = "unreachable";
            return r;
        }
        r.solution = solution;
        const float maxChange = std::clamp(in.maxSpeedChange, 0.0f, 4.0f);
        const float speedRatio = solveSpeed / speed;
        if (speedRatio > 1.0f + maxChange)
        {
            r.reason = "needs a much harder throw";
            return r;
        }
        const float sh = std::sqrt(solution.x * solution.x + solution.y * solution.y);
        const float elevationDiffDeg = std::fabs(std::atan2(solution.z, sh) - throwElevation) / kRad;
        const float elevationWindow = std::clamp(in.elevationDeg, 1.0f, 90.0f);
        const float wElevation = AssistFalloff(elevationDiffDeg, elevationWindow * 0.5f, elevationWindow);
        if (!(wElevation > 0.0f))
        {
            r.reason = "thrown much higher or lower";
            return r;
        }
        const float w = strength * wHeading * wElevation;
        const V3 from = in.velocity * (1.0f / speed);
        const V3 to = solution * (1.0f / solveSpeed);
        V3 dir = from * (1.0f - w) + to * w;
        const float dirLength = dir.Length();
        if (!(dirLength > 1e-4f))
        {
            r.reason = "degenerate";
            return r;
        }
        dir = dir * (1.0f / dirLength);
        float magnitude = speed * (1.0f - w) + solveSpeed * w;
        if (in.maxSpeed > 0.0f)
            magnitude = std::min(magnitude, in.maxSpeed);
        r.velocity = dir * magnitude;
        r.weight = w;
        r.applied = r.velocity.Finite();
        if (!r.applied)
            r.velocity = in.velocity;
        r.reason = r.applied ? "assisted" : "degenerate";
        return r;
    }

    // The hand velocity (relative, unscaled: what goes on the wire) that the
    // server turns into `launch` (launch = hand * scale + body).
    inline V3 HandVelocityForLaunch(const V3& launch, const V3& body, float scale)
    {
        if (!(scale > 1e-3f) || !launch.Finite())
            return {};
        const V3 b = body.Finite() ? body : V3();
        return (launch - b) * (1.0f / scale);
    }
}

#pragma once
#include "sdk/vector.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

// Pure, deterministic motion-gesture detectors. Every input is in metres,
// seconds and the world frame (Source: +z up); angular velocity is deg/s.
// Callers convert game units with m_VRScale before building a Sample.
namespace l4d2vr_gesture
{
    constexpr float kPi = 3.14159265358979323846f;
    constexpr float kDegToRad = kPi / 180.0f;
    // A larger gap between samples (hitch, pause, tracking loss) restarts all timing windows.
    constexpr float kMaxSampleGap = 0.25f;

    struct Sample
    {
        float time = 0.0f;
        Vector handPosition{ 0.0f, 0.0f, 0.0f };
        Vector handForward{ 1.0f, 0.0f, 0.0f };
        Vector handVelocity{ 0.0f, 0.0f, 0.0f };           // m/s
        Vector handAngularVelocityDeg{ 0.0f, 0.0f, 0.0f }; // deg/s, world axis
        Vector hmdPosition{ 0.0f, 0.0f, 0.0f };
        Vector hmdForward{ 1.0f, 0.0f, 0.0f };
        Vector hmdUp{ 0.0f, 0.0f, 1.0f };
        // Optional. When absent the detectors differentiate hmdPosition between samples.
        Vector hmdVelocity{ 0.0f, 0.0f, 0.0f };
        bool hasHmdVelocity = false;
    };

    inline bool Finite(const Vector& v)
    {
        return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
    }
    inline bool SampleFinite(const Sample& s)
    {
        return std::isfinite(s.time) && Finite(s.handPosition) && Finite(s.handForward) && Finite(s.handVelocity) &&
            Finite(s.handAngularVelocityDeg) && Finite(s.hmdPosition) && Finite(s.hmdForward) && Finite(s.hmdUp) &&
            (!s.hasHmdVelocity || Finite(s.hmdVelocity));
    }
    inline bool Normalize(const Vector& v, Vector& result)
    {
        const float length = v.Length();
        if (!std::isfinite(length) || length < 1e-6f) return false;
        result = v * (1.0f / length);
        return true;
    }
    // Velocity of a point rigidly attached to the hand at `lever` from the hand origin.
    inline Vector PointVelocity(const Vector& linear, const Vector& angularDeg, const Vector& lever)
    {
        return linear + CrossProduct(angularDeg * kDegToRad, lever);
    }

    // Shared sample clock: validates monotonic time and estimates HMD velocity.
    class SampleClock
    {
    public:
        enum class Status { Invalid, First, Continuous };

        void Reset() { m_Has = false; }
        // `dt` is 0 for First. A time regression or a gap above kMaxSampleGap yields First.
        Status Update(const Sample& s, float& dt, Vector& hmdVelocity)
        {
            dt = 0.0f;
            hmdVelocity = s.hasHmdVelocity ? s.hmdVelocity : Vector(0.0f, 0.0f, 0.0f);
            if (!SampleFinite(s)) return Status::Invalid;
            if (m_Has && s.time == m_Time) return Status::Invalid; // duplicate sample
            const bool continuous = m_Has && s.time > m_Time && s.time - m_Time <= kMaxSampleGap;
            if (continuous)
            {
                dt = s.time - m_Time;
                if (!s.hasHmdVelocity) hmdVelocity = (s.hmdPosition - m_Hmd) * (1.0f / dt);
            }
            m_Has = true;
            m_Time = s.time;
            m_Hmd = s.hmdPosition;
            return continuous ? Status::Continuous : Status::First;
        }

    private:
        bool m_Has = false;
        float m_Time = 0.0f;
        Vector m_Hmd{ 0.0f, 0.0f, 0.0f };
    };

    // ------------------------------------------------------------------ swing
    struct SwingParams
    {
        float bladeLength = 0.5f;       // hand origin to the striking part, along handForward
        float startSpeed = 1.6f;        // tip speed relative to the HMD that starts a swing
        float releaseSpeed = 0.6f;      // swing ends below this
        float restSpeed = 0.5f;         // must stay below this ...
        float restDuration = 0.08f;     // ... for this long before re-arming
        float cooldown = 0.3f;          // minimum time between swing starts
        float maxAxialAlignment = 0.85f; // |cos| between tip velocity and blade axis; above = thrust, not swing
    };

    struct SwingEvent
    {
        bool started = false;           // true on the single sample a swing begins
        bool swinging = false;          // true while the swing lasts
        Vector direction{ 0.0f, 0.0f, 0.0f }; // normalized relative tip velocity (zero when still)
        float tipSpeed = 0.0f;          // m/s relative to the HMD
    };

    // Starts in the rest-required state: a weapon adopted mid-motion cannot swing until it settles.
    class SwingDetector
    {
    public:
        explicit SwingDetector(const SwingParams& params = SwingParams{}) : m_Params(params) {}
        void SetParams(const SwingParams& params) { m_Params = params; }
        const SwingParams& GetParams() const { return m_Params; }
        void Reset() { *this = SwingDetector(m_Params); }
        bool Armed() const { return m_Phase == Phase::Armed; }
        bool Swinging() const { return m_Phase == Phase::Swinging; }

        SwingEvent Update(const Sample& s)
        {
            SwingEvent event{};
            Vector forward{};
            if (!SampleFinite(s) || !Normalize(s.handForward, forward)) return event;
            float dt = 0.0f;
            Vector hmdVelocity{};
            const SampleClock::Status status = m_Clock.Update(s, dt, hmdVelocity);
            if (status == SampleClock::Status::Invalid) return event;
            if (status == SampleClock::Status::First)
            {
                m_Resting = false;
                if (m_HasSwung && s.time < m_LastStart) m_HasSwung = false; // clock went backwards
            }
            const Vector tip = PointVelocity(s.handVelocity - hmdVelocity, s.handAngularVelocityDeg,
                forward * m_Params.bladeLength);
            const float speed = tip.Length();
            event.tipSpeed = speed;
            if (speed > 1e-4f) event.direction = tip * (1.0f / speed);

            switch (m_Phase)
            {
            case Phase::Swinging:
                if (speed < m_Params.releaseSpeed) { m_Phase = Phase::NeedRest; m_Resting = false; }
                break;
            case Phase::NeedRest:
                if (speed < m_Params.restSpeed)
                {
                    if (!m_Resting) { m_Resting = true; m_RestStart = s.time; }
                    const bool cooled = !m_HasSwung || s.time - m_LastStart >= m_Params.cooldown;
                    if (s.time - m_RestStart >= m_Params.restDuration && cooled) m_Phase = Phase::Armed;
                }
                else m_Resting = false;
                break;
            case Phase::Armed:
                if (speed >= m_Params.startSpeed &&
                    std::fabs(DotProduct(event.direction, forward)) <= m_Params.maxAxialAlignment)
                {
                    m_Phase = Phase::Swinging;
                    m_HasSwung = true;
                    m_LastStart = s.time;
                    event.started = true;
                }
                break;
            }
            event.swinging = m_Phase == Phase::Swinging;
            return event;
        }

    private:
        enum class Phase { NeedRest, Armed, Swinging };
        SwingParams m_Params{};
        SampleClock m_Clock{};
        Phase m_Phase = Phase::NeedRest;
        bool m_Resting = false;
        float m_RestStart = 0.0f;
        bool m_HasSwung = false;
        float m_LastStart = 0.0f;
    };

    // ----------------------------------------------------------------- thrust
    struct ThrustParams
    {
        float forwardSpeed = 1.8f;        // relative velocity along the weapon forward axis
        float maxAngularSpeedDeg = 250.0f; // peak hand angular speed allowed in the window
        float minTravel = 0.15f;          // forward travel accumulated ...
        float window = 0.25f;             // ... within this many seconds
        float releaseSpeed = 0.6f;        // forward speed must drop below this to re-arm
        float cooldown = 0.4f;

        static ThrustParams Melee() { return ThrustParams{}; }
        static ThrustParams GunBash() { ThrustParams p{}; p.forwardSpeed = 2.0f; return p; }
    };

    struct ThrustEvent
    {
        bool fired = false;
        Vector direction{ 0.0f, 0.0f, 0.0f }; // normalized relative hand velocity at the fire sample
        float forwardSpeed = 0.0f;
        float travel = 0.0f;              // forward travel within the window
    };

    // Melee shove / forward jab. Velocity is taken relative to the HMD so walking does not thrust.
    class ThrustDetector
    {
    public:
        explicit ThrustDetector(const ThrustParams& params = ThrustParams::Melee()) : m_Params(params) {}
        void SetParams(const ThrustParams& params) { m_Params = params; }
        const ThrustParams& GetParams() const { return m_Params; }
        void Reset() { *this = ThrustDetector(m_Params); }

        ThrustEvent Update(const Sample& s)
        {
            ThrustEvent event{};
            Vector forward{};
            if (!SampleFinite(s) || !Normalize(s.handForward, forward)) return event;
            float dt = 0.0f;
            Vector hmdVelocity{};
            const SampleClock::Status status = m_Clock.Update(s, dt, hmdVelocity);
            if (status == SampleClock::Status::Invalid) return event;
            if (status == SampleClock::Status::First)
            {
                m_Count = 0;
                if (m_HasFired && s.time < m_LastFire) m_HasFired = false;
            }
            const Vector relative = s.handVelocity - hmdVelocity;
            const float forwardSpeed = DotProduct(relative, forward);
            const float angular = s.handAngularVelocityDeg.Length();
            if (status == SampleClock::Status::Continuous) Push(Entry{ s.time, forwardSpeed * dt, angular });
            while (m_Count > 0 && At(0).time <= s.time - m_Params.window) Pop();
            float travel = 0.0f, peakAngular = angular;
            for (std::size_t i = 0; i < m_Count; ++i)
            {
                travel += At(i).travel;
                peakAngular = std::max(peakAngular, At(i).angular);
            }
            event.forwardSpeed = forwardSpeed;
            event.travel = travel;

            if (!m_Armed && forwardSpeed < m_Params.releaseSpeed &&
                (!m_HasFired || s.time - m_LastFire >= m_Params.cooldown))
                m_Armed = true;
            if (m_Armed && forwardSpeed >= m_Params.forwardSpeed && peakAngular < m_Params.maxAngularSpeedDeg &&
                travel >= m_Params.minTravel)
            {
                m_Armed = false;
                m_HasFired = true;
                m_LastFire = s.time;
                event.fired = true;
                if (!Normalize(relative, event.direction)) event.direction = forward;
            }
            return event;
        }

    private:
        struct Entry { float time; float travel; float angular; };
        // 128 entries cover the window at up to ~500 Hz; older entries are dropped beyond that.
        void Push(const Entry& entry)
        {
            if (m_Count == m_Entries.size()) Pop();
            m_Entries[(m_Head + m_Count) % m_Entries.size()] = entry;
            ++m_Count;
        }
        void Pop() { m_Head = (m_Head + 1) % m_Entries.size(); --m_Count; }
        const Entry& At(std::size_t i) const { return m_Entries[(m_Head + i) % m_Entries.size()]; }

        ThrustParams m_Params{};
        SampleClock m_Clock{};
        std::array<Entry, 128> m_Entries{};
        std::size_t m_Head = 0, m_Count = 0;
        bool m_Armed = true;
        bool m_HasFired = false;
        float m_LastFire = 0.0f;
    };

    // --------------------------------------------------------------- gun bash
    struct GunBashParams
    {
        float muzzleLength = 0.35f;        // hand origin to muzzle along handForward
        float minSpeed = 2.2f;             // muzzle speed relative to the HMD
        float maxBelowHorizontalDeg = 60.0f; // steeper downward motions are reload gestures
        float minForwardDot = -0.2f;       // horizontal direction vs HMD forward; below = pulling back
        float releaseSpeed = 0.8f;
        float cooldown = 0.5f;
        Vector worldUp{ 0.0f, 0.0f, 1.0f };
    };

    struct GunBashEvent
    {
        bool fired = false;
        Vector direction{ 0.0f, 0.0f, 0.0f }; // normalized relative muzzle velocity
        float speed = 0.0f;
    };

    class GunBashDetector
    {
    public:
        explicit GunBashDetector(const GunBashParams& params = GunBashParams{}) : m_Params(params) {}
        void SetParams(const GunBashParams& params) { m_Params = params; }
        const GunBashParams& GetParams() const { return m_Params; }
        void Reset() { *this = GunBashDetector(m_Params); }

        GunBashEvent Update(const Sample& s)
        {
            GunBashEvent event{};
            Vector forward{}, up{};
            if (!SampleFinite(s) || !Normalize(s.handForward, forward) || !Normalize(m_Params.worldUp, up))
                return event;
            float dt = 0.0f;
            Vector hmdVelocity{};
            const SampleClock::Status status = m_Clock.Update(s, dt, hmdVelocity);
            if (status == SampleClock::Status::Invalid) return event;
            if (status == SampleClock::Status::First && m_HasFired && s.time < m_LastFire) m_HasFired = false;
            const Vector muzzle = PointVelocity(s.handVelocity - hmdVelocity, s.handAngularVelocityDeg,
                forward * m_Params.muzzleLength);
            const float speed = muzzle.Length();
            event.speed = speed;
            if (speed > 1e-4f) event.direction = muzzle * (1.0f / speed);

            if (!m_Armed && speed < m_Params.releaseSpeed && (!m_HasFired || s.time - m_LastFire >= m_Params.cooldown))
                m_Armed = true;
            if (!m_Armed || speed < m_Params.minSpeed) return event;
            if (DotProduct(event.direction, up) < -std::sin(m_Params.maxBelowHorizontalDeg * kDegToRad))
                return event;
            Vector horizontal{}, view{};
            if (Normalize(event.direction - up * DotProduct(event.direction, up), horizontal) &&
                Normalize(s.hmdForward - up * DotProduct(s.hmdForward, up), view) &&
                DotProduct(horizontal, view) < m_Params.minForwardDot)
                return event;
            m_Armed = false;
            m_HasFired = true;
            m_LastFire = s.time;
            event.fired = true;
            return event;
        }

    private:
        GunBashParams m_Params{};
        SampleClock m_Clock{};
        bool m_Armed = true;
        bool m_HasFired = false;
        float m_LastFire = 0.0f;
    };

    // ------------------------------------------------------------ pills mouth
    struct PillsParams
    {
        float mouthDown = 0.08f;          // along -hmdUp
        float mouthForward = 0.07f;       // along hmdForward
        float radius = 0.11f;
        float dwell = 0.15f;
        float rearmScale = 1.5f;          // re-arms after leaving radius * rearmScale
        float itemForwardOffset = 0.0f;   // item point = hand + handForward * offset
    };

    struct PillsEvent
    {
        bool fired = false;
        bool inside = false;
        float distance = 0.0f;
        float dwellProgress = 0.0f;       // 0..1 while inside and armed
    };

    inline bool MouthPoint(const Vector& hmdPosition, const Vector& hmdForward, const Vector& hmdUp,
        const PillsParams& params, Vector& mouth)
    {
        Vector forward{}, up{};
        if (!Finite(hmdPosition) || !Normalize(hmdForward, forward) || !Normalize(hmdUp, up)) return false;
        mouth = hmdPosition - up * params.mouthDown + forward * params.mouthForward;
        return true;
    }

    class PillsMouthDetector
    {
    public:
        explicit PillsMouthDetector(const PillsParams& params = PillsParams{}) : m_Params(params) {}
        void SetParams(const PillsParams& params) { m_Params = params; }
        const PillsParams& GetParams() const { return m_Params; }
        void Reset() { *this = PillsMouthDetector(m_Params); }

        PillsEvent Update(const Sample& s)
        {
            PillsEvent event{};
            Vector mouth{}, forward{};
            if (!SampleFinite(s) || !Normalize(s.handForward, forward) ||
                !MouthPoint(s.hmdPosition, s.hmdForward, s.hmdUp, m_Params, mouth))
                return event;
            float dt = 0.0f;
            Vector hmdVelocity{};
            const SampleClock::Status status = m_Clock.Update(s, dt, hmdVelocity);
            if (status == SampleClock::Status::Invalid) return event;
            if (status == SampleClock::Status::First) m_Inside = false;
            const float distance = (s.handPosition + forward * m_Params.itemForwardOffset - mouth).Length();
            event.distance = distance;
            event.inside = distance <= m_Params.radius;
            if (!m_Armed)
            {
                if (distance > m_Params.radius * m_Params.rearmScale) m_Armed = true;
                return event;
            }
            if (!event.inside) { m_Inside = false; return event; }
            if (!m_Inside) { m_Inside = true; m_EnterTime = s.time; }
            const float held = s.time - m_EnterTime;
            event.dwellProgress = m_Params.dwell > 0.0f ? std::clamp(held / m_Params.dwell, 0.0f, 1.0f) : 1.0f;
            if (held >= m_Params.dwell)
            {
                m_Armed = false;
                m_Inside = false;
                event.fired = true;
            }
            return event;
        }

    private:
        PillsParams m_Params{};
        SampleClock m_Clock{};
        bool m_Armed = true;
        bool m_Inside = false;
        float m_EnterTime = 0.0f;
    };

    // ------------------------------------------------------- adrenaline stab
    struct AdrenalineParams
    {
        float tipLength = 0.10f;          // needle tip = hand + handForward * tipLength
        float minApproachSpeed = 1.2f;    // closing speed toward the target
        float hitRadius = 0.10f;          // tip must end within this distance
        float approachWindow = 0.12f;     // the fast approach may precede arrival by this long
        float rearmDistance = 0.20f;      // after firing, the tip must leave this distance of the target
        float armBackOffset = 0.15f;      // see ArmTargetPoint
    };

    enum class StabTarget { None, Chest, Arm };

    struct StabTargets
    {
        Vector chest{ 0.0f, 0.0f, 0.0f };
        bool chestValid = false;
        Vector arm{ 0.0f, 0.0f, 0.0f };
        bool armValid = false;
    };

    struct StabEvent
    {
        bool fired = false;
        StabTarget target = StabTarget::None;
        float approachSpeed = 0.0f;       // peak closing speed in the window for the hit target
    };

    // Upper-arm target from the other hand: back along its forward axis (toward the elbow).
    inline bool ArmTargetPoint(const Vector& otherHandPosition, const Vector& otherHandForward, float backOffset,
        Vector& result)
    {
        Vector forward{};
        if (!Finite(otherHandPosition) || !Normalize(otherHandForward, forward) || !std::isfinite(backOffset))
            return false;
        result = otherHandPosition - forward * backOffset;
        return true;
    }

    // Closing speed is the rate the tip-to-target distance shrinks, so walking (tip and
    // target moving together) never counts as an approach.
    class AdrenalineStabDetector
    {
    public:
        explicit AdrenalineStabDetector(const AdrenalineParams& params = AdrenalineParams{}) : m_Params(params) {}
        void SetParams(const AdrenalineParams& params) { m_Params = params; }
        const AdrenalineParams& GetParams() const { return m_Params; }
        void Reset() { *this = AdrenalineStabDetector(m_Params); }

        StabEvent Update(const Sample& s, const StabTargets& targets)
        {
            StabEvent event{};
            Vector forward{};
            if (!SampleFinite(s) || !Normalize(s.handForward, forward)) return event;
            float dt = 0.0f;
            Vector hmdVelocity{};
            const SampleClock::Status status = m_Clock.Update(s, dt, hmdVelocity);
            if (status == SampleClock::Status::Invalid) return event;
            const Vector tip = s.handPosition + forward * m_Params.tipLength;
            const bool continuous = status == SampleClock::Status::Continuous;
            float distances[2] = { INFINITY, INFINITY };
            UpdateTarget(m_Targets[0], targets.chestValid && Finite(targets.chest), targets.chest, tip, s.time, dt,
                continuous, distances[0]);
            UpdateTarget(m_Targets[1], targets.armValid && Finite(targets.arm), targets.arm, tip, s.time, dt,
                continuous, distances[1]);
            int best = -1;
            for (int i = 0; i < 2; ++i)
            {
                const TargetState& t = m_Targets[i];
                if (!t.armed || !t.hasFast || distances[i] > m_Params.hitRadius ||
                    s.time - t.fastTime > m_Params.approachWindow)
                    continue;
                if (best < 0 || distances[i] < distances[best]) best = i;
            }
            if (best >= 0)
            {
                event.fired = true;
                event.target = best == 0 ? StabTarget::Chest : StabTarget::Arm;
                event.approachSpeed = m_Targets[best].fastSpeed;
                for (TargetState& t : m_Targets) t.armed = false; // one injection per approach
            }
            return event;
        }

    private:
        struct TargetState
        {
            bool hasPrevious = false;
            float previousDistance = 0.0f;
            bool hasFast = false;
            float fastTime = 0.0f;
            float fastSpeed = 0.0f;
            bool armed = true;
        };

        void UpdateTarget(TargetState& t, bool valid, const Vector& point, const Vector& tip, float time, float dt,
            bool continuous, float& distance) const
        {
            if (!valid) { const bool armed = t.armed; t = TargetState{}; t.armed = armed; return; }
            distance = (point - tip).Length();
            if (!continuous) { t.hasPrevious = false; t.hasFast = false; }
            if (t.hasPrevious && dt > 0.0f)
            {
                const float closing = (t.previousDistance - distance) / dt;
                if (closing >= m_Params.minApproachSpeed)
                {
                    // Keep the window's peak speed while it is still fresh.
                    if (!t.hasFast || time - t.fastTime > m_Params.approachWindow || closing >= t.fastSpeed)
                        t.fastSpeed = closing;
                    t.hasFast = true;
                    t.fastTime = time;
                }
            }
            if (!t.armed && distance > m_Params.rearmDistance) { t.armed = true; t.hasFast = false; }
            t.hasPrevious = true;
            t.previousDistance = distance;
        }

        AdrenalineParams m_Params{};
        SampleClock m_Clock{};
        std::array<TargetState, 2> m_Targets{};
    };
}

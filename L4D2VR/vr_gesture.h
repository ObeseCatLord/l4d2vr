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
    // A swing must be fast and committed, like a hit in Ancient Dungeon VR:
    // the blade tip (measured on the lagged weapon) and the hand both fast,
    // and both having travelled far enough over the last 0.6 s. A wrist flick
    // (fast tip, still hand), a slow swing and a short poke never count. All
    // positions are relative to the HMD in tracking space, so turning,
    // walking and locomotion never count either. Speeds are averaged over
    // ~4 frames (45 ms).
    struct SwingParams
    {
        float bladeLength = 0.62f;        // grip to the measured point along handForward (73 % of the reach)
        float startSpeed = 4.5f;          // tip speed relative to the HMD that starts a swing ...
        float minHandleSpeed = 2.5f;      // ... with the hand at least this fast (short weapons: 3.0)
        float minTipTravel = 0.43f;       // ... the tip having moved this far ...
        float minHandleTravel = 0.13f;    // ... and the hand this far ...
        float travelWindow = 0.6f;        // ... within this window (largest distance from an earlier point)
        float speedWindow = 0.045f;       // speed averaging window
        float releaseSpeed = 2.5f;        // the swing (its hit window) ends when the tip falls below this ...
        float releaseHandleSpeed = 1.25f; // ... or the hand below this
        float restSpeed = 1.5f;           // re-arms once the tip stays below this ...
        float restDuration = 0.02f;       // ... for this long
        float cooldown = 0.35f;           // minimum time between swing starts (the server's own melee gate)
        float maxAxialAlignment = 0.85f;  // |cos| between tip velocity and blade axis; above = thrust, not swing
    };

    struct SwingEvent
    {
        bool started = false;           // true on the single sample a swing begins
        bool swinging = false;          // true while the swing (its hit window) lasts
        Vector direction{ 0.0f, 0.0f, 0.0f }; // normalized relative tip velocity (zero when still)
        float tipSpeed = 0.0f;          // m/s relative to the HMD (averaged)
        float handleSpeed = 0.0f;       // m/s relative to the HMD (averaged)
        float tipTravel = 0.0f;         // m within the travel window
        float handleTravel = 0.0f;
        float axial = 0.0f;             // |cos| between the tip velocity and the blade
        bool armed = false;
        // Every start gate but the speed itself passed or not (for near-miss logs).
        bool fastEnough = false, handleFastEnough = false, tipTravelled = false, handleTravelled = false;
    };

    // Ancient Dungeon's hit quality (0..1) for a swing: speed past 7.75 m/s and
    // committed tip and hand travel. Informational (L4D2 damage is fixed).
    inline float SwingQuality(float tipSpeed, float tipTravel, float handleTravel)
    {
        const float speedTerm = std::min((tipSpeed - 7.75f) / 10.0f, 1.0f);
        const float tipTerm = std::min(tipTravel / 2.15f, 1.0f), handleTerm = std::min(handleTravel / 0.65f, 1.0f);
        return std::clamp((1.5f * speedTerm + tipTerm + handleTerm) * 0.3f, 0.0f, 1.0f);
    }

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
                m_Count = 0;
                if (m_HasSwung && s.time < m_LastStart) m_HasSwung = false; // clock went backwards
            }
            const Entry now{ s.time, s.handPosition + forward * m_Params.bladeLength - s.hmdPosition,
                s.handPosition - s.hmdPosition };
            Push(now);
            const float window = std::max(m_Params.travelWindow, m_Params.speedWindow);
            while (m_Count > 1 && At(0).time < s.time - window - 1e-4f) Pop();

            // Averaged velocity: from the newest entry at least speedWindow old
            // (or the oldest one) to now.
            Vector tipVelocity{}, handleVelocity{};
            for (std::size_t i = m_Count; i-- > 0;)
            {
                const Entry& e = At(i);
                const float span = s.time - e.time;
                if (span >= m_Params.speedWindow - 1e-4f || i == 0)
                {
                    if (span > 1e-4f)
                    {
                        tipVelocity = (now.tip - e.tip) * (1.0f / span);
                        handleVelocity = (now.handle - e.handle) * (1.0f / span);
                    }
                    break;
                }
            }
            for (std::size_t i = 0; i < m_Count; ++i)
            {
                const Entry& e = At(i);
                if (e.time < s.time - m_Params.travelWindow - 1e-4f) continue;
                event.tipTravel = std::max(event.tipTravel, (now.tip - e.tip).Length());
                event.handleTravel = std::max(event.handleTravel, (now.handle - e.handle).Length());
            }
            const float speed = tipVelocity.Length();
            event.tipSpeed = speed;
            event.handleSpeed = handleVelocity.Length();
            if (speed > 1e-4f) event.direction = tipVelocity * (1.0f / speed);
            event.axial = std::fabs(DotProduct(event.direction, forward));
            event.fastEnough = speed >= m_Params.startSpeed;
            event.handleFastEnough = event.handleSpeed >= m_Params.minHandleSpeed;
            event.tipTravelled = event.tipTravel >= m_Params.minTipTravel;
            event.handleTravelled = event.handleTravel >= m_Params.minHandleTravel;

            switch (m_Phase)
            {
            case Phase::Swinging:
                if (speed < m_Params.releaseSpeed || event.handleSpeed < m_Params.releaseHandleSpeed)
                {
                    m_Phase = Phase::NeedRest;
                    m_Resting = false;
                }
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
                if (event.fastEnough && event.handleFastEnough && event.tipTravelled && event.handleTravelled &&
                    event.axial <= m_Params.maxAxialAlignment)
                {
                    m_Phase = Phase::Swinging;
                    m_HasSwung = true;
                    m_LastStart = s.time;
                    event.started = true;
                }
                break;
            }
            event.armed = m_Phase == Phase::Armed;
            event.swinging = m_Phase == Phase::Swinging;
            return event;
        }

    private:
        enum class Phase { NeedRest, Armed, Swinging };
        struct Entry { float time; Vector tip; Vector handle; };
        // 512 entries cover the 0.6 s window at up to ~850 Hz; older entries are dropped beyond that.
        void Push(const Entry& entry)
        {
            if (m_Count == m_Entries.size()) Pop();
            m_Entries[(m_Head + m_Count) % m_Entries.size()] = entry;
            ++m_Count;
        }
        void Pop() { m_Head = (m_Head + 1) % m_Entries.size(); --m_Count; }
        const Entry& At(std::size_t i) const { return m_Entries[(m_Head + i) % m_Entries.size()]; }

        SwingParams m_Params{};
        SampleClock m_Clock{};
        std::array<Entry, 512> m_Entries{};
        std::size_t m_Head = 0, m_Count = 0;
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

    // --------------------------------------------------------- tracking frame
    // The game places every tracked device (metres in, game units out) at
    //   world = origin + scale * (hmdCorrected + R(yaw) * (local - hmdLocal))
    // where origin is the camera anchor minus the eye height, hmdCorrected is
    // the head's accumulated (stick-rotated) room position, hmdLocal is the
    // head's raw tracking position and yaw is the stick-turn offset, pivoting
    // about the head. Smooth/snap turning, stick walking, teleports, ladders,
    // pushes and recentering only change origin, yaw and hmdCorrected, so a
    // point mapped back to tracking space moves only when the player's real
    // hand or head moves in the room. Gesture detectors work in this frame.
    inline Vector RotateYawDeg(const Vector& v, float degrees)
    {
        const float radians = degrees * kDegToRad;
        const float c = std::cos(radians), s = std::sin(radians);
        return Vector(v.x * c - v.y * s, v.x * s + v.y * c, v.z);
    }

    struct TrackingFrame
    {
        Vector origin{ 0.0f, 0.0f, 0.0f };       // world units
        float yawDeg = 0.0f;
        Vector hmdCorrected{ 0.0f, 0.0f, 0.0f }; // metres
        Vector hmdLocal{ 0.0f, 0.0f, 0.0f };     // metres, raw tracking space
        float scale = 1.0f;                      // world units per metre

        bool Valid() const
        {
            return Finite(origin) && Finite(hmdCorrected) && Finite(hmdLocal) && std::isfinite(yawDeg) &&
                std::isfinite(scale) && scale > 1e-4f;
        }
        Vector ToWorldPoint(const Vector& local) const
        {
            return origin + (hmdCorrected + RotateYawDeg(local - hmdLocal, yawDeg)) * scale;
        }
        Vector ToTrackingPoint(const Vector& world) const
        {
            return RotateYawDeg((world - origin) * (1.0f / scale) - hmdCorrected, -yawDeg) + hmdLocal;
        }
        Vector ToWorldDirection(const Vector& direction) const { return RotateYawDeg(direction, yawDeg); }
        Vector ToTrackingDirection(const Vector& direction) const { return RotateYawDeg(direction, -yawDeg); }
    };

    // Seconds since `epochSeconds`, as a float that keeps sub-millisecond
    // precision. A float of the raw clock (seconds since boot) is quantised to
    // 1/32 s after three days of uptime, which drops two of every three frames
    // as duplicates and skews every velocity estimate.
    class SessionClock
    {
    public:
        // Returns false when the epoch was moved forward (time restarts near 0);
        // callers reset their detectors then.
        bool Seconds(double nowSeconds, float& seconds)
        {
            bool continuous = true;
            if (!m_Has || nowSeconds < m_Epoch || nowSeconds - m_Epoch > kRebaseAfter)
            {
                continuous = !m_Has;
                m_Epoch = nowSeconds;
                m_Has = true;
            }
            seconds = static_cast<float>(nowSeconds - m_Epoch) + kStart;
            return continuous;
        }
        static constexpr double kRebaseAfter = 1800.0; // float ulp stays below 0.25 ms
        static constexpr float kStart = 1.0f;

    private:
        bool m_Has = false;
        double m_Epoch = 0.0;
    };

    // Builds detector samples from consecutive tracking-space poses. Hand and
    // head velocities are differentiated over the same interval, so they
    // cancel exactly when the two move together.
    class TrackingSampler
    {
    public:
        void Reset() { m_Has = false; }
        Sample Build(float time, const Vector& hand, const Vector& handForward, const Vector& handAngularDeg,
            const Vector& hmd, const Vector& hmdForward, const Vector& hmdUp)
        {
            Sample s{};
            s.time = time;
            s.handPosition = hand;
            s.handForward = handForward;
            s.handAngularVelocityDeg = handAngularDeg;
            s.hmdPosition = hmd;
            s.hmdForward = hmdForward;
            s.hmdUp = hmdUp;
            s.hasHmdVelocity = true;
            const float dt = time - m_Time;
            if (m_Has && dt > 1e-4f && dt <= kMaxSampleGap)
            {
                s.handVelocity = (hand - m_Hand) * (1.0f / dt);
                s.hmdVelocity = (hmd - m_Hmd) * (1.0f / dt);
            }
            if (!m_Has || dt > 1e-4f || dt < 0.0f)
            {
                m_Has = true;
                m_Time = time;
                m_Hand = hand;
                m_Hmd = hmd;
            }
            return s;
        }

    private:
        bool m_Has = false;
        float m_Time = 0.0f;
        Vector m_Hand{ 0.0f, 0.0f, 0.0f };
        Vector m_Hmd{ 0.0f, 0.0f, 0.0f };
    };

    // --------------------------------------------------------------- gun bash
    // A gun bash is any fast swing of the gun in front of the body: a push, a
    // sideways sweep, a downward chop or an upward butt stroke. It is measured
    // at the gun as well as the hand, relative to the head in tracking space
    // (artificial locomotion never moves the hand there):
    //  - the hand is fast (an arm swing, not a wrist flick or an aim snap) and
    //    the muzzle `tipLength` ahead of it along the barrel is fast too;
    //  - the hand has really travelled (path length, so a wind-up does not
    //    cancel the strike the way a net displacement does);
    //  - the motion is not toward the player's back (reaching for a hip,
    //    shoulder, chest or back holster, or pulling the gun to the face);
    //  - the hand is in front of the eyes and above the hip, and the head is
    //    not turning fast (turning the whole body swings the arm with it).
    // Barrel alignment, wrist rotation and swing direction are not gated: a
    // real bash rotates the gun hard and goes whichever way the player swings.
    // Speeds are averaged over ~30 ms so a single tracking spike never fires.
    struct GunBashParams
    {
        float tipLength = 0.40f;            // muzzle ahead of the hand along the barrel
        float minHandSpeed = 2.2f;          // hand speed relative to the HMD (a quick 0.3 s re-aim peaks near 1.9)
        float minTipSpeed = 2.4f;           // muzzle speed relative to the HMD
        // A straight push (mostly forward, away from the eyes, the gun not
        // turning much) is never an aim move, so it needs less hand speed.
        // Swinging the gun back out from a shoulder or hip holster turns it hard.
        float pushMinForwardShare = 0.8f;
        float minPushSpeed = 1.6f;
        float pushMaxTipRatio = 1.5f;       // muzzle speed / hand speed
        float minSwingTravel = 0.16f;       // hand path length of a swing ...
        float minPushTravel = 0.10f;        // ... or of a straight push ...
        float window = 0.20f;               // ... within this many seconds (snapping the
                                            // gun back up after a reload is shorter)
        float travelNoise = 0.0015f;        // per frame, not counted as travel
        float speedSpan = 0.03f;            // speeds are averaged over this span
        float maxBackwardFraction = 0.5f;   // backward share of the hand motion (toward the player's back)
        float minReachForward = 0.15f;      // hand ahead of the eyes when it fires
        float maxDrop = 0.50f;              // hand at most this far below the eyes (a hip reach is lower)
        float maxHeadYawRateDeg = 200.0f;   // peak head turn in the window
        // GunBashSweepEnabled=false keeps only pushes: mostly forward strikes.
        bool sweepEnabled = true;
        float pushMinForwardFraction = 0.6f;
        float releaseSpeed = 1.0f;          // hand speed that re-arms (or a reversed swing)
        float cooldown = 0.7f;              // the native shove interval (z_gun_swing_interval)
        Vector worldUp{ 0.0f, 0.0f, 1.0f };
    };

    struct GunBashEvent
    {
        bool fired = false;
        bool sweep = false;                   // fired as a swing (else a mostly forward push)
        Vector direction{ 0.0f, 0.0f, 0.0f }; // normalized shove direction (head to muzzle, pitch within 45 deg)
        // Diagnostics of the last sample.
        float speed = 0.0f;                   // hand speed relative to the HMD
        float tipSpeed = 0.0f;                // muzzle speed relative to the HMD
        float forwardFraction = 0.0f;         // forward share of the hand motion (negative: backward)
        float travel = 0.0f;                  // hand path length within the window
        float reach = 0.0f, height = 0.0f;    // hand ahead of / above the eyes
        float headYawRate = 0.0f;             // peak in the window, deg/s
        bool armed = false;
    };

    // Horizontal facing of the head. Looking down, the head's up vector tips
    // forward and carries the yaw.
    inline bool HorizontalFacing(const Vector& hmdForward, const Vector& hmdUp, const Vector& up, Vector& facing)
    {
        const float down = std::max(0.0f, -DotProduct(hmdForward, up));
        const Vector f = hmdForward + hmdUp * down;
        return Normalize(f - up * DotProduct(f, up), facing);
    }

    class GunBashDetector
    {
    public:
        explicit GunBashDetector(const GunBashParams& params = GunBashParams{}) : m_Params(params) {}
        void SetParams(const GunBashParams& params) { m_Params = params; }
        const GunBashParams& GetParams() const { return m_Params; }
        void Reset() { *this = GunBashDetector(m_Params); }

        // allowed=false (the hand is at a holster, or the grip just drew) never fires.
        GunBashEvent Update(const Sample& s, bool allowed = true)
        {
            GunBashEvent event{};
            Vector barrel{}, up{}, view{};
            if (!SampleFinite(s) || !Normalize(s.handForward, barrel) || !Normalize(m_Params.worldUp, up) ||
                !HorizontalFacing(s.hmdForward, s.hmdUp, up, view))
                return event;
            float dt = 0.0f;
            Vector hmdVelocity{};
            const SampleClock::Status status = m_Clock.Update(s, dt, hmdVelocity);
            if (status == SampleClock::Status::Invalid) return event;
            if (status == SampleClock::Status::First)
            {
                m_Count = 0;
                m_Armed = false;
                if (m_HasFired && s.time < m_LastFire) m_HasFired = false;
            }
            float headYawRate = 0.0f;
            if (status == SampleClock::Status::Continuous && dt > 0.0f)
            {
                const float sine = DotProduct(CrossProduct(m_View, view), up);
                headYawRate = std::atan2(sine, DotProduct(m_View, view)) / dt / kDegToRad;
            }
            m_View = view;
            // Hand and muzzle relative to the head. The head's own motion is
            // removed by differentiating these offsets (or, when the sample
            // reports both velocities, by the velocities' difference).
            const Vector hand = s.handPosition - s.hmdPosition;
            const Vector tip = hand + barrel * m_Params.tipLength;
            Push(Entry{ s.time, hand, tip, std::fabs(headYawRate) });
            while (m_Count > 1 && At(0).time <= s.time - m_Params.window) Pop();

            // Averaged velocities over speedSpan (at least one frame).
            Vector handVelocity(0.0f, 0.0f, 0.0f), tipVelocity(0.0f, 0.0f, 0.0f);
            bool measured = false;
            if (m_Count >= 2)
            {
                std::size_t k = m_Count - 2;
                while (k > 0 && s.time - At(k).time < m_Params.speedSpan) --k;
                const Entry& old = At(k);
                const float span = s.time - old.time;
                if (span > 1e-4f)
                {
                    handVelocity = (hand - old.hand) * (1.0f / span);
                    tipVelocity = (tip - old.tip) * (1.0f / span);
                    measured = true;
                }
            }
            float travel = 0.0f, peakYaw = 0.0f;
            for (std::size_t i = 0; i < m_Count; ++i)
            {
                // Tracking jitter never adds up to travel: each frame's first
                // `travelNoise` metres do not count.
                if (i > 0) travel += std::max(0.0f, (At(i).hand - At(i - 1).hand).Length() - m_Params.travelNoise);
                peakYaw = std::max(peakYaw, At(i).headYaw);
            }
            const float speed = handVelocity.Length();
            const float tipSpeed = tipVelocity.Length();
            const float forward = DotProduct(handVelocity, view);
            const bool cooled = !m_HasFired || s.time - m_LastFire >= m_Params.cooldown;
            const bool reversed = m_HasFired && DotProduct(handVelocity, m_FireVelocity) < 0.0f;
            if (!m_Armed && measured && cooled && (speed < m_Params.releaseSpeed || reversed)) m_Armed = true;

            event.speed = speed;
            event.tipSpeed = tipSpeed;
            event.forwardFraction = speed > 1e-4f ? forward / speed : 0.0f;
            event.travel = travel;
            event.reach = DotProduct(hand, view);
            event.height = DotProduct(hand, up);
            event.headYawRate = peakYaw;
            event.armed = m_Armed;
            if (!allowed || !m_Armed || speed < 1e-4f)
                return event;
            const bool push = event.forwardFraction >= m_Params.pushMinForwardFraction;
            const bool straightPush = event.forwardFraction >= m_Params.pushMinForwardShare &&
                speed >= m_Params.minPushSpeed && tipSpeed <= m_Params.pushMaxTipRatio * speed &&
                travel >= m_Params.minPushTravel;
            const bool swing = speed >= m_Params.minHandSpeed && tipSpeed >= m_Params.minTipSpeed &&
                travel >= m_Params.minSwingTravel;
            if ((straightPush || swing) &&
                event.forwardFraction >= -m_Params.maxBackwardFraction &&
                event.reach >= m_Params.minReachForward && event.height >= -m_Params.maxDrop &&
                peakYaw <= m_Params.maxHeadYawRateDeg &&
                (push || m_Params.sweepEnabled))
            {
                m_Armed = false;
                m_HasFired = true;
                m_LastFire = s.time;
                m_FireVelocity = handVelocity;
                event.fired = true;
                event.sweep = !push;
                // Shove toward the muzzle, from the head, no steeper than 45 deg.
                const float vertical = DotProduct(tip, up);
                const Vector level = tip - up * vertical;
                const float levelLength = level.Length();
                Vector direction = levelLength > 0.05f ? level * (1.0f / levelLength) : view;
                const float tilt = levelLength > 0.05f ? std::clamp(vertical / levelLength, -1.0f, 1.0f) : 0.0f;
                if (!Normalize(direction + up * tilt, event.direction)) event.direction = view;
            }
            return event;
        }

    private:
        struct Entry { float time; Vector hand; Vector tip; float headYaw; };
        void Push(const Entry& entry)
        {
            if (m_Count == m_Entries.size()) Pop();
            m_Entries[(m_Head + m_Count) % m_Entries.size()] = entry;
            ++m_Count;
        }
        void Pop() { m_Head = (m_Head + 1) % m_Entries.size(); --m_Count; }
        const Entry& At(std::size_t i) const { return m_Entries[(m_Head + i) % m_Entries.size()]; }

        GunBashParams m_Params{};
        SampleClock m_Clock{};
        std::array<Entry, 128> m_Entries{};
        std::size_t m_Head = 0, m_Count = 0;
        Vector m_View{ 1.0f, 0.0f, 0.0f };
        // A fresh detector waits for the hand to be still first.
        bool m_Armed = false;
        bool m_HasFired = false;
        float m_LastFire = 0.0f;
        Vector m_FireVelocity{ 0.0f, 0.0f, 0.0f };
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

    // --------------------------------------------- hand an item to a teammate
    // Hold the item against a teammate's chest for a moment to give it to them.
    struct HandoverParams
    {
        float itemForwardOffset = 0.08f;  // item point = hand + handForward * offset
        float radius = 0.30f;             // around the teammate's chest point
        float dwell = 0.25f;
        float rearmScale = 1.4f;          // re-arms once no target is within radius * rearmScale
        float minHeadDistance = 0.30f;    // never near your own head (that is the mouth gesture)
    };

    struct HandoverTarget
    {
        int id = 0;
        Vector point{ 0.0f, 0.0f, 0.0f };
    };

    struct HandoverEvent
    {
        bool fired = false;
        int target = 0;
    };

    class HandoverDetector
    {
    public:
        explicit HandoverDetector(const HandoverParams& params = HandoverParams{}) : m_Params(params) {}
        void SetParams(const HandoverParams& params) { m_Params = params; }
        void Reset() { *this = HandoverDetector(m_Params); }

        HandoverEvent Update(const Sample& s, const HandoverTarget* targets, int count)
        {
            HandoverEvent event{};
            Vector forward{};
            if (!SampleFinite(s) || !Normalize(s.handForward, forward))
                return event;
            float dt = m_HasTime ? s.time - m_LastTime : 0.0f;
            m_LastTime = s.time;
            m_HasTime = true;
            if (!std::isfinite(dt) || dt < 0.0f || dt > 0.1f)
                dt = 0.0f;

            const Vector item = s.handPosition + forward * m_Params.itemForwardOffset;
            int best = -1;
            float bestDistance = INFINITY, nearest = INFINITY;
            if ((item - s.hmdPosition).Length() >= m_Params.minHeadDistance)
            {
                for (int i = 0; targets && i < count; ++i)
                {
                    if (!Finite(targets[i].point))
                        continue;
                    const float d = (targets[i].point - item).Length();
                    nearest = (std::min)(nearest, d);
                    if (d <= m_Params.radius && d < bestDistance)
                    {
                        best = i;
                        bestDistance = d;
                    }
                }
            }
            if (!m_Armed)
            {
                if (nearest > m_Params.radius * m_Params.rearmScale)
                    m_Armed = true;
                return event;
            }
            if (best < 0)
            {
                m_Target = 0;
                m_Dwell = 0.0f;
                return event;
            }
            if (targets[best].id != m_Target)
            {
                m_Target = targets[best].id;
                m_Dwell = 0.0f;
                return event;
            }
            m_Dwell += dt;
            if (m_Dwell >= m_Params.dwell)
            {
                event.fired = true;
                event.target = m_Target;
                m_Armed = false;
                m_Target = 0;
                m_Dwell = 0.0f;
            }
            return event;
        }

    private:
        HandoverParams m_Params;
        bool m_Armed = true;
        bool m_HasTime = false;
        float m_LastTime = 0.0f;
        int m_Target = 0;
        float m_Dwell = 0.0f;
    };
}

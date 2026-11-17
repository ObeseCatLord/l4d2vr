#pragma once

// OpenXR reference-space changes (2.0.12, review s12 E16). A runtime recentre
// (XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING for the space the mod
// tracks in) moves the origin of every tracked pose at once. The mod builds
// the in-game head position from frame-to-frame HMD steps and its yaw from
// the HMD yaw plus a turn offset, so the jump would teleport and turn the
// player. The backend bumps a sequence for a change that really moves the
// space (Monado also sends bursts of identity changes); the tracking update
// then removes the one HMD step that matches the jump within a short window,
// which re-bases the room-scale offset, the turn offset and with them the
// body/holster frame that follows the head. std-only, unit tested.
#include <atomic>
#include <cmath>
#include <cstdint>

namespace l4d2vr_space_rebase
{
    // Bumped by the backend for each change of the tracking space in use
    // whose pose is not the identity (or is unknown).
    inline std::atomic<uint32_t> g_ChangeSequence{ 0u };

    // Rotation (yaw about the up axis, degrees) and translation (metres) of a
    // change pose, poseValid already checked by the caller.
    inline bool IsIdentityChange(float px, float py, float pz, float qx, float qy, float qz, float qw)
    {
        const float translation = std::sqrt(px * px + py * py + pz * pz);
        // |w| close to 1: a rotation of less than ~0.5 degrees.
        const float w = std::fabs(qw);
        (void)qx; (void)qy; (void)qz;
        return translation < 0.005f && w > 0.99999f;
    }

    // Logs a burst of change events as one line: the first event logs, later
    // ones within kBurstMs only count; the next line after the burst reports
    // how many there were.
    class BurstLog
    {
    public:
        static constexpr uint64_t kBurstMs = 10000;

        // Returns true when this event should write a line; outCount is the
        // number of events folded into the previous burst (0 for the first).
        bool OnEvent(uint64_t nowMs, uint32_t& outFolded)
        {
            outFolded = 0;
            if (m_LastLogMs != 0 && nowMs - m_LastLogMs < kBurstMs)
            {
                ++m_Folded;
                return false;
            }
            outFolded = m_Folded;
            m_Folded = 0;
            m_LastLogMs = nowMs == 0 ? 1 : nowMs;
            return true;
        }

    private:
        uint64_t m_LastLogMs = 0;
        uint32_t m_Folded = 0;
    };

    // Per tracking update: after a change was announced, the first HMD step
    // that is larger than a head can move in one frame is the runtime's jump.
    class Rebaser
    {
    public:
        static constexpr uint64_t kWindowMs = 1500;
        static constexpr float kJumpMeters = 0.12f;  // one frame of head motion is a few centimetres
        static constexpr float kJumpYawDeg = 10.0f;

        void OnChange(uint64_t nowMs)
        {
            m_PendingUntilMs = nowMs + kWindowMs;
        }

        bool Pending(uint64_t nowMs) const { return m_PendingUntilMs != 0 && nowMs <= m_PendingUntilMs; }

        // True when this frame's step is the jump: the caller drops the step
        // (position) and cancels the yaw change. A window that passes without
        // a jump (the runtime's change did not move the head) just ends.
        bool ShouldRebase(uint64_t nowMs, float stepMeters, float yawStepDeg)
        {
            if (m_PendingUntilMs == 0)
                return false;
            if (nowMs > m_PendingUntilMs)
            {
                m_PendingUntilMs = 0;
                return false;
            }
            if (!(stepMeters > kJumpMeters) && !(std::fabs(yawStepDeg) > kJumpYawDeg))
                return false;
            m_PendingUntilMs = 0;
            return true;
        }

    private:
        uint64_t m_PendingUntilMs = 0;
    };

    // Signed smallest difference b - a in degrees.
    inline float YawStep(float fromDeg, float toDeg)
    {
        float d = toDeg - fromDeg;
        d -= 360.0f * std::floor((d + 180.0f) / 360.0f);
        return d;
    }
}

#pragma once

// OpenXR frame ticket ring (OpenXR port M2). Std-only and lock-free of its
// own: the backend holds one mutex around every call and performs the XR
// calls the ring decides on.
//
// OpenXR frame rules it enforces:
//   * xrWaitFrame -> xrBeginFrame -> xrEndFrame, per frame, in order.
//   * Only one xrWaitFrame at a time, and a waited frame must be begun before
//     the next xrWaitFrame (otherwise the next wait blocks inside the runtime).
//   * xrWaitFrame for the next frame may run while the previous frame is begun
//     but not yet ended (pipelining). Beginning a new frame while an older one
//     is still begun discards the older one (XR_FRAME_DISCARDED).
//   * xrBeginFrame needs a waited frame; xrEndFrame needs a begun frame.
//
// How the mod's threads map onto it (vr_backend_openxr.cpp):
//   * The thread that calls IVrCompositor::WaitGetPoses (the pose waiter in
//     queued mode, the main thread otherwise, the loading pump while the game
//     is stalled) is the xrWaitFrame thread. Before it waits again it must
//     close a waited frame nobody began: PlanWait() says EndStaleFirst, and
//     the backend begins and ends that frame with the last submitted layers
//     (or none), so the runtime keeps receiving complete frames.
//   * The submit side (SubmitExplicitTimingData / Submit / PostPresentHandoff,
//     always under DXVK's queue lock) begins the waited frame and ends it.
//
// Every rule violation is counted (and reported through the assert hook) and
// refused, never forwarded to the runtime.

#include <cstdint>

namespace l4d2vr_openxr
{
    struct FrameTicket
    {
        uint64_t id = 0;            // 1, 2, ... in xrWaitFrame order
        int64_t displayTime = 0;    // XrFrameState::predictedDisplayTime
        int64_t displayPeriod = 0;  // XrFrameState::predictedDisplayPeriod (ns)
        bool shouldRender = false;  // XrFrameState::shouldRender
    };

    enum class RingViolation : int
    {
        None = 0,
        DoubleWait,         // a second wait while one is in flight
        WaitOverWaited,     // a wait while a waited frame was never begun
        BeginWithoutWait,   // begin with no waited frame
        EndWithoutBegin,    // end with no begun frame
        CompleteWithoutWait // a wait result without a wait in flight
    };

    inline const char* Name(RingViolation violation)
    {
        switch (violation)
        {
        case RingViolation::DoubleWait: return "double xrWaitFrame";
        case RingViolation::WaitOverWaited: return "xrWaitFrame over an unbegun frame";
        case RingViolation::BeginWithoutWait: return "xrBeginFrame without xrWaitFrame";
        case RingViolation::EndWithoutBegin: return "xrEndFrame without xrBeginFrame";
        case RingViolation::CompleteWithoutWait: return "wait result without a wait";
        default: return "none";
        }
    }

    struct FrameRingStats
    {
        uint64_t waits = 0;          // successful xrWaitFrame
        uint64_t waitFailures = 0;   // xrWaitFrame errors
        uint64_t begins = 0;
        uint64_t ends = 0;
        uint64_t discarded = 0;      // begun frames replaced by a newer begin
        uint64_t staleEnds = 0;      // frames closed by the wait thread
        uint64_t violations = 0;
        RingViolation lastViolation = RingViolation::None;
    };

    class FrameRing
    {
    public:
        using AssertHook = void (*)(RingViolation violation, void* context);

        void SetAssertHook(AssertHook hook, void* context)
        {
            m_Hook = hook;
            m_HookContext = context;
        }

        enum class WaitPlan
        {
            Wait,           // call BeginWait() and xrWaitFrame
            EndStaleFirst,  // close Waited() with BeginStale()/End() first
            Busy,           // another thread is inside xrWaitFrame
        };

        WaitPlan PlanWait() const
        {
            if (m_WaitInFlight)
                return WaitPlan::Busy;
            if (m_HasWaited)
                return WaitPlan::EndStaleFirst;
            return WaitPlan::Wait;
        }

        // Claims the right to call xrWaitFrame. False (and a violation) when a
        // wait is already in flight or a waited frame was never begun.
        bool BeginWait()
        {
            if (m_WaitInFlight)
                return Violate(RingViolation::DoubleWait);
            if (m_HasWaited)
                return Violate(RingViolation::WaitOverWaited);
            m_WaitInFlight = true;
            return true;
        }

        // Records the xrWaitFrame result. Returns the new ticket's id (0 on failure).
        uint64_t CompleteWait(bool succeeded, int64_t displayTime, int64_t displayPeriod, bool shouldRender)
        {
            if (!m_WaitInFlight)
            {
                Violate(RingViolation::CompleteWithoutWait);
                return 0;
            }
            m_WaitInFlight = false;
            if (!succeeded)
            {
                ++m_Stats.waitFailures;
                return 0;
            }
            ++m_Stats.waits;
            m_Waited.id = ++m_LastTicketId;
            m_Waited.displayTime = displayTime;
            m_Waited.displayPeriod = displayPeriod;
            m_Waited.shouldRender = shouldRender;
            m_HasWaited = true;
            return m_Waited.id;
        }

        enum class BeginResult
        {
            Begun,          // the waited frame is now begun: call xrBeginFrame
            AlreadyBegun,   // this frame was begun earlier: nothing to call
            NoFrame,        // nothing waited (a wait may be in flight): skip
        };

        // Submit side: begins the waited frame. A begun frame that was never
        // ended is discarded by the runtime (counted here). When a frame is
        // already begun and nothing newer is waited, the caller keeps using it.
        BeginResult Begin(FrameTicket* outTicket = nullptr)
        {
            if (!m_HasWaited)
            {
                if (m_HasBegun)
                {
                    if (outTicket)
                        *outTicket = m_Begun;
                    return BeginResult::AlreadyBegun;
                }
                return BeginResult::NoFrame;
            }
            if (m_HasBegun)
                ++m_Stats.discarded;
            m_Begun = m_Waited;
            m_HasBegun = true;
            m_HasWaited = false;
            ++m_Stats.begins;
            if (outTicket)
                *outTicket = m_Begun;
            return BeginResult::Begun;
        }

        // Wait side: begins the waited frame nobody took so it can be closed.
        // Identical to Begin() apart from the bookkeeping; false and a
        // violation without a waited frame.
        bool BeginStale(FrameTicket* outTicket = nullptr)
        {
            if (!m_HasWaited)
                return Violate(RingViolation::BeginWithoutWait);
            Begin(outTicket);
            ++m_Stats.staleEnds;
            return true;
        }

        // Ends the begun frame. False (and a violation) without one.
        bool End(FrameTicket* outTicket = nullptr)
        {
            if (!m_HasBegun)
                return Violate(RingViolation::EndWithoutBegin);
            if (outTicket)
                *outTicket = m_Begun;
            m_HasBegun = false;
            ++m_Stats.ends;
            return true;
        }

        // Session stopped or lost: forget every open frame (the runtime does
        // too). A wait in flight stays claimed until it completes.
        void Reset()
        {
            m_HasWaited = false;
            m_HasBegun = false;
        }

        bool HasWaited() const { return m_HasWaited; }
        bool HasBegun() const { return m_HasBegun; }
        bool WaitInFlight() const { return m_WaitInFlight; }
        const FrameTicket& Waited() const { return m_Waited; }
        const FrameTicket& Begun() const { return m_Begun; }
        uint64_t LastTicketId() const { return m_LastTicketId; }
        const FrameRingStats& Stats() const { return m_Stats; }

    private:
        bool Violate(RingViolation violation)
        {
            ++m_Stats.violations;
            m_Stats.lastViolation = violation;
            if (m_Hook)
                m_Hook(violation, m_HookContext);
            return false;
        }

        FrameTicket m_Waited{};
        FrameTicket m_Begun{};
        bool m_HasWaited = false;
        bool m_HasBegun = false;
        bool m_WaitInFlight = false;
        uint64_t m_LastTicketId = 0;
        FrameRingStats m_Stats{};
        AssertHook m_Hook = nullptr;
        void* m_HookContext = nullptr;
    };
}

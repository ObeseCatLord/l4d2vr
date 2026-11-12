#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

namespace l4d2vr_input
{
    class ActionFrame
    {
    public:
        // Readiness, update epoch and persistent failure generation share one
        // atomic snapshot, including when command generation misses a failure.
        struct Attempt { std::uint64_t token; bool suppressEdges; };
        Attempt Begin()
        {
            const auto previous = Read();
            const auto pending = (previous & kFailureMask) | ((previous + 4u) & kEpochMask);
            m_State.store(pending, std::memory_order_release);
            return { pending, previous != 0u && !Ready(previous) };
        }
        void Complete(Attempt attempt, bool ready)
        {
            if (!ready)
            {
                const auto failed = ((attempt.token + (std::uint64_t{1} << 32)) & kFailureMask) |
                    (attempt.token & kEpochMask);
                m_State.compare_exchange_strong(attempt.token, failed, std::memory_order_release);
                return;
            }
            const auto completed = attempt.token | 1u | (attempt.suppressEdges ? 2u : 0u);
            m_State.compare_exchange_strong(attempt.token, completed, std::memory_order_release);
        }
        std::uint64_t Read() const { return m_State.load(std::memory_order_acquire); }
        static std::uint64_t FailureGeneration(std::uint64_t token) { return token >> 32; }
        static bool Ready(std::uint64_t token) { return (token & 1u) != 0u; }
        static bool SuppressEdges(std::uint64_t token) { return (token & 2u) != 0u; }
        bool Matches(std::uint64_t token) const { return Ready(token) && Read() == token; }
    private:
        std::atomic<std::uint64_t> m_State{0u};
        static constexpr std::uint64_t kEpochMask = 0x00000000fffffffcULL;
        static constexpr std::uint64_t kFailureMask = 0xffffffff00000000ULL;
    };

    class GripContributors
    {
    public:
        void Reset() { m_Held = 0u; m_Context = 0u; }
        bool Update(std::uint32_t known, std::uint32_t down, std::uint32_t context)
        {
            if (context != m_Context) { m_Held = 0u; m_Context = context; }
            down &= known;
            const auto unknownHeld = m_Held & ~known;
            m_Held = unknownHeld | down;
            return known != 0u && unknownHeld == 0u;
        }
        bool Down() const { return m_Held != 0u; }
    private:
        std::uint32_t m_Held = 0u, m_Context = 0u;
    };

    struct GripSample
    {
        static std::uint32_t Generation(std::uint32_t sample) { return sample >> 2; }
        static void Publish(std::atomic<std::uint32_t>& destination, std::uint32_t flags)
        {
            const auto previous = destination.load(std::memory_order_acquire);
            auto generation = previous & ~std::uint32_t{3};
            flags &= 3u;
            if ((flags & 1u) == 0u)
                flags = 0u;
            if ((previous & 1u) != 0u && (flags & 1u) == 0u)
                generation += 4u;
            destination.store(generation | flags, std::memory_order_release);
        }
    };

    class ReleaseInputGate
    {
    public:
        void Reset() { m_NeedsNeutral = false; m_LastFailure = 0u; }
        bool Update(std::uint64_t frame, bool attackDown, bool useDown)
        {
            const auto failures = ActionFrame::FailureGeneration(frame);
            if (failures != m_LastFailure)
            {
                m_LastFailure = failures;
                m_NeedsNeutral = true;
                // The first observed sample after a missed failure may still
                // contain cleanup's neutral levels. Require a later baseline.
                return false;
            }
            if (!ActionFrame::Ready(frame) || ActionFrame::SuppressEdges(frame))
            {
                m_NeedsNeutral = true;
                return false;
            }
            if (m_NeedsNeutral)
            {
                if (!attackDown && !useDown)
                    m_NeedsNeutral = false;
                return false;
            }
            return true;
        }
    private:
        bool m_NeedsNeutral = false;
        std::uint64_t m_LastFailure = 0u;
    };

    // Both controllers fresh and stable for the full-arm IK (both arms are one
    // render product). 2.0.11: once ready, a stale or lost sample does not hide
    // the arms at once: readiness is held for kHoldMs since the last good
    // sample (the lost hand keeps its last usable pose, GetPoseData), and a
    // fresh sample within that window continues without a new 200 ms
    // stabilisation (a frame hitch, a hand briefly lost at a holster). Leaving
    // first-person control still resets at once.
    class ControllerPairReadiness
    {
    public:
        static constexpr std::uint64_t kMaxAgeMs = 150u;
        static constexpr std::uint64_t kStableMs = 200u;
        static constexpr std::uint64_t kHoldMs = 500u;

        void Reset() { *this = {}; }

        bool Update(bool enabled, bool bothValid, std::uint64_t capturedAtMs,
            std::uint32_t poseToken, std::uint64_t nowMs)
        {
            if (!enabled)
            {
                Reset();
                return false;
            }
            const bool sampleFresh = bothValid && capturedAtMs != 0u && poseToken != 0u &&
                nowMs >= capturedAtMs && nowMs - capturedAtMs <= kMaxAgeMs;
            const bool sampleOrdered = m_LastPoseToken == 0u || capturedAtMs >= m_LastCapturedAtMs;
            if (!sampleFresh || !sampleOrdered)
                return Hold(nowMs, !sampleOrdered);

            // Repeated render calls and republished snapshots do not provide a
            // new tracking sample or extend the lifetime of the previous one.
            if (poseToken == m_LastPoseToken)
            {
                if (nowMs - m_LastCapturedAtMs > kMaxAgeMs)
                    return Hold(nowMs, false);
                return m_Ready;
            }

            if (m_LastPoseToken != 0u && capturedAtMs - m_LastCapturedAtMs > kMaxAgeMs)
            {
                // A gap between two samples: continue a ready pair within the
                // hold window, otherwise start a new stability interval.
                if (!(m_Ready && nowMs - m_LastGoodMs <= kHoldMs))
                {
                    m_Ready = false;
                    m_StableSinceMs = 0u;
                    m_LastPoseToken = 0u;
                }
            }

            if (m_LastPoseToken == 0u || m_StableSinceMs == 0u)
                m_StableSinceMs = capturedAtMs;
            m_LastPoseToken = poseToken;
            m_LastCapturedAtMs = capturedAtMs;
            m_LastGoodMs = nowMs;
            m_Ready = m_Ready || capturedAtMs - m_StableSinceMs >= kStableMs;
            return m_Ready;
        }

        std::uint64_t StableDurationMs() const
        {
            return m_StableSinceMs == 0u ? 0u : m_LastCapturedAtMs - m_StableSinceMs;
        }

    private:
        bool Hold(std::uint64_t nowMs, bool hardReset)
        {
            if (!hardReset && m_Ready && nowMs >= m_LastGoodMs && nowMs - m_LastGoodMs <= kHoldMs)
                return true;
            Reset();
            return false;
        }

        std::uint32_t m_LastPoseToken = 0u;
        std::uint64_t m_LastCapturedAtMs = 0u;
        std::uint64_t m_StableSinceMs = 0u;
        std::uint64_t m_LastGoodMs = 0u;
        bool m_Ready = false;
    };

    struct HeldTarget
    {
        enum class Kind { None, VirtualKey, Command };
        Kind kind = Kind::None;
        std::uint16_t virtualKey = 0u;
        std::string pressCommand;
        std::string releaseCommand;

        bool operator==(const HeldTarget& other) const
        {
            return kind == other.kind && virtualKey == other.virtualKey &&
                pressCommand == other.pressCommand && releaseCommand == other.releaseCommand;
        }
        bool operator!=(const HeldTarget& other) const { return !(*this == other); }
    };

    class HeldCustomActions
    {
    public:
        void Reconcile(std::size_t index, const HeldTarget& target, bool held)
        {
            if (index >= m_Desired.size())
                return;
            m_Desired[index] = held ? target : HeldTarget{};
        }

        template<class Emit>
        void Flush(Emit&& emit)
        {
            // Decide from the complete frame so ownership may transfer between
            // slots without briefly releasing a key or gameplay command.
            for (std::size_t index = 0u; index < m_Owned.size(); ++index)
                if (m_Owned[index].kind != HeldTarget::Kind::None &&
                    !Contains(m_Desired, m_Owned[index], m_Desired.size()) &&
                    !Contains(m_Owned, m_Owned[index], index))
                    emit(m_Owned[index], false);
            for (std::size_t index = 0u; index < m_Desired.size(); ++index)
                if (m_Desired[index].kind != HeldTarget::Kind::None &&
                    !Contains(m_Owned, m_Desired[index], m_Owned.size()) &&
                    !Contains(m_Desired, m_Desired[index], index))
                    emit(m_Desired[index], true);
            m_Owned = m_Desired;
        }

        template<class Emit>
        void ReleaseAll(Emit&& emit)
        {
            for (std::size_t index = 0u; index < m_Owned.size(); ++index)
                Release(index, emit);
            m_Desired = {};
        }

    private:
        static bool Contains(const std::array<HeldTarget, 5u>& targets,
            const HeldTarget& target, std::size_t count)
        {
            for (std::size_t index = 0u; index < count; ++index)
                if (targets[index].kind != HeldTarget::Kind::None && targets[index] == target)
                    return true;
            return false;
        }

        template<class Emit>
        void Release(std::size_t index, Emit&& emit)
        {
            if (m_Owned[index].kind == HeldTarget::Kind::None)
                return;
            const HeldTarget previous = m_Owned[index];
            m_Owned[index] = {};
            if (!Contains(m_Owned, previous, m_Owned.size()))
                emit(previous, false);
        }

        std::array<HeldTarget, 5u> m_Owned{};
        std::array<HeldTarget, 5u> m_Desired{};
    };
}

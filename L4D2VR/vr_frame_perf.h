#pragma once

// Always-on, cheap frame-time breakdown (2.0.12, review s12 issue 11 / E3).
// The 2.0.11 logs showed 4-5 fps in third person and spectator views but not
// where the ~200 ms per frame went. The hooks add microseconds and counts to
// a few relaxed atomics; at each outer RenderView the counters of the frame
// that just ended are taken, and a frame longer than 3x the display period
// writes one "[VR][Perf] slow frame" line (at most one per second) naming the
// parts. std-only (no Windows.h) so the decision and the line are unit tested.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>

namespace l4d2vr_frame_perf
{
    enum Counter : int
    {
        kRenderViewUs,      // outer RenderView hook, inclusive
        kStereoUs,          // both eye scenes
        kPasses,            // scene passes (eyes, scope, mirror, front view)
        kExtraPassUs,       // render-to-texture passes after the eyes
        kModelDraws,        // DrawModelExecute calls (outermost)
        kModelDrawUs,       // DrawModelExecute, inclusive (outermost)
        kModelEngineUs,     // of that, inside the engine's own draw
        kWorldPoseCalls,    // player world-model VR pose requests
        kWorldPoseSolves,   // of those, full IK solves (cache misses)
        kWorldPoseUs,
        kWeaponPoseUs,      // held world weapon alignment
        kVrUpdateUs,        // VR::Update
        kCreateMoveUs,
        kHudUs,             // wrist/hand HUD overlays
        kLogLines,
        kLogUs,
        kFaults,            // memory reads caught by SEH (expensive under Wine)
        kCounterCount
    };

    inline std::atomic<uint64_t> g_Counters[kCounterCount]{};

    inline void Add(Counter counter, uint64_t value)
    {
        g_Counters[counter].fetch_add(value, std::memory_order_relaxed);
    }

    inline uint64_t ToUs(std::chrono::steady_clock::duration d)
    {
        const auto us = std::chrono::duration_cast<std::chrono::microseconds>(d).count();
        return us > 0 ? static_cast<uint64_t>(us) : 0u;
    }

    inline void AddLog(std::chrono::steady_clock::duration d)
    {
        Add(kLogLines, 1);
        Add(kLogUs, ToUs(d));
    }

    // Adds the scope's duration to one counter (and 1 to an optional count).
    class Scope
    {
    public:
        explicit Scope(Counter us, int count = -1)
            : m_Us(us), m_Count(count), m_Start(std::chrono::steady_clock::now()) {}
        ~Scope()
        {
            Add(m_Us, ToUs(std::chrono::steady_clock::now() - m_Start));
            if (m_Count >= 0)
                Add(static_cast<Counter>(m_Count), 1);
        }
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;

    private:
        Counter m_Us;
        int m_Count;
        std::chrono::steady_clock::time_point m_Start;
    };

    struct Sample
    {
        uint64_t frameUs = 0;
        uint64_t values[kCounterCount] = {};
    };

    // Takes (and clears) everything counted since the previous call.
    inline void Take(Sample& out)
    {
        for (int i = 0; i < kCounterCount; ++i)
            out.values[i] = g_Counters[i].exchange(0, std::memory_order_relaxed);
    }

    // Slow frame: longer than 3 display periods (and 50 ms at least, so a
    // 144 Hz headset does not report every 21 ms hitch); one line per second.
    class SlowFrameLog
    {
    public:
        bool ShouldLog(uint64_t frameUs, float displayHz, uint64_t nowMs)
        {
            const float hz = displayHz > 20.0f && displayHz < 1000.0f ? displayHz : 90.0f;
            uint64_t threshold = static_cast<uint64_t>(3.0e6 / hz);
            if (threshold < 50000u)
                threshold = 50000u;
            // A frame of more than 5 s is a load or a pause, not a slow frame.
            if (frameUs <= threshold || frameUs > 5000000u)
                return false;
            ++m_SlowSinceLog;
            if (m_LastLogMs != 0 && nowMs - m_LastLogMs < 1000u)
                return false;
            m_LastLogMs = nowMs == 0 ? 1 : nowMs;
            return true;
        }

        // Slow frames since the previous line (including the one being logged).
        uint32_t TakeSlowCount()
        {
            const uint32_t n = m_SlowSinceLog;
            m_SlowSinceLog = 0;
            return n;
        }

    private:
        uint64_t m_LastLogMs = 0;
        uint32_t m_SlowSinceLog = 0;
    };

    inline int Format(char* out, size_t size, const Sample& s, float displayHz, uint32_t slowFrames,
        bool thirdPerson, bool observer, bool incap)
    {
        const auto ms = [](uint64_t us) { return us / 1000.0; };
        const uint64_t* v = s.values;
        const uint64_t outside = s.frameUs > v[kRenderViewUs] ? s.frameUs - v[kRenderViewUs] : 0;
        const uint64_t drawOwn = v[kModelDrawUs] > v[kModelEngineUs] ? v[kModelDrawUs] - v[kModelEngineUs] : 0;
        return std::snprintf(out, size,
            "[VR][Perf] slow frame %.1f ms (display %.1f Hz, %u slow since the last line): renderView %.1f ms "
            "(eyes %.1f ms, %llu passes, extra passes %.1f ms) | models %llu: hook %.1f ms + engine %.1f ms | "
            "worldPose %.1f ms (%llu calls, %llu solves) weaponPose %.1f ms | outside renderView %.1f ms: "
            "VR::Update %.1f ms, CreateMove %.1f ms, HUD %.1f ms | log %llu lines %.1f ms | faults %llu | "
            "thirdPerson=%d observer=%d incap=%d",
            ms(s.frameUs), displayHz, slowFrames, ms(v[kRenderViewUs]), ms(v[kStereoUs]),
            (unsigned long long)v[kPasses], ms(v[kExtraPassUs]), (unsigned long long)v[kModelDraws],
            ms(drawOwn), ms(v[kModelEngineUs]), ms(v[kWorldPoseUs]), (unsigned long long)v[kWorldPoseCalls],
            (unsigned long long)v[kWorldPoseSolves], ms(v[kWeaponPoseUs]), ms(outside), ms(v[kVrUpdateUs]),
            ms(v[kCreateMoveUs]), ms(v[kHudUs]), (unsigned long long)v[kLogLines], ms(v[kLogUs]),
            (unsigned long long)v[kFaults], thirdPerson ? 1 : 0, observer ? 1 : 0, incap ? 1 : 0);
    }
}

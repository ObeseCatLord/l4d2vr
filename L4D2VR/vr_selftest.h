#pragma once
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Developer self-test (launch option "-l4d2vrselftest step,step,...").
// It scripts head/hand poses and button presses so every online mechanic can
// be exercised on a real server with a simulated headset. Only the launch
// option enables it; normal play never touches these overrides.
namespace l4d2vr_selftest
{
    // ---- Button overrides ---------------------------------------------------
    // Read by VR::GetDigitalActionData and l4d2vr_hand_input::ReadDigitalForHand.
    struct ButtonOverride
    {
        std::atomic<std::uint64_t> action{ 0u };
        std::atomic<bool> down{ false };
        std::atomic<std::uint32_t> edgeFrame{ 0u };
    };

    struct Registry
    {
        std::atomic<bool> active{ false };
        std::atomic<std::uint32_t> frame{ 1u };
        ButtonOverride buttons[8];
    };

    inline Registry& Get()
    {
        static Registry registry;
        return registry;
    }

    inline void BeginFrame()
    {
        Get().frame.fetch_add(1u, std::memory_order_acq_rel);
    }

    // Claims or updates the override for an action. An edge is reported for
    // the rest of the frame in which the state changed.
    inline void SetButton(std::uint64_t action, bool down)
    {
        Registry& r = Get();
        if (action == 0u)
            return;
        ButtonOverride* slot = nullptr;
        for (ButtonOverride& b : r.buttons)
        {
            if (b.action.load(std::memory_order_acquire) == action)
            {
                slot = &b;
                break;
            }
        }
        if (!slot)
        {
            for (ButtonOverride& b : r.buttons)
            {
                std::uint64_t expected = 0u;
                if (b.action.compare_exchange_strong(expected, action, std::memory_order_acq_rel))
                {
                    slot = &b;
                    break;
                }
            }
        }
        if (!slot)
            return;
        if (slot->down.exchange(down, std::memory_order_acq_rel) != down)
            slot->edgeFrame.store(r.frame.load(std::memory_order_acquire), std::memory_order_release);
    }

    inline void ReleaseAll()
    {
        for (ButtonOverride& b : Get().buttons)
            SetButton(b.action.load(std::memory_order_acquire), false);
    }

    inline bool Lookup(std::uint64_t action, bool& down, bool& changed)
    {
        Registry& r = Get();
        down = false;
        changed = false;
        if (!r.active.load(std::memory_order_acquire) || action == 0u)
            return false;
        for (const ButtonOverride& b : r.buttons)
        {
            if (b.action.load(std::memory_order_acquire) != action)
                continue;
            down = b.down.load(std::memory_order_acquire);
            changed = b.edgeFrame.load(std::memory_order_acquire) == r.frame.load(std::memory_order_acquire);
            return true;
        }
        return false;
    }

    // ---- Scripted paths -----------------------------------------------------
    // Positions are metres in the head-yaw frame: x forward, y toward the right
    // hand, z up.
    struct Vec3
    {
        float x = 0.0f, y = 0.0f, z = 0.0f;
    };

    inline Vec3 Lerp(const Vec3& a, const Vec3& b, float t)
    {
        return { a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t };
    }

    inline float SmoothStep(float t)
    {
        t = std::clamp(t, 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    }

    struct Key
    {
        float time = 0.0f;   // seconds from the start of the path
        Vec3 position{};
        bool smooth = true;  // ease in and out; false = constant speed
    };

    // Position at time t along consecutive keys (held before the first and
    // after the last).
    inline Vec3 Evaluate(const std::vector<Key>& keys, float t)
    {
        if (keys.empty())
            return {};
        if (t <= keys.front().time)
            return keys.front().position;
        for (std::size_t i = 1; i < keys.size(); ++i)
        {
            const Key& a = keys[i - 1];
            const Key& b = keys[i];
            if (t <= b.time)
            {
                const float span = std::max(1e-4f, b.time - a.time);
                float u = (t - a.time) / span;
                if (b.smooth)
                    u = SmoothStep(u);
                return Lerp(a.position, b.position, u);
            }
        }
        return keys.back().position;
    }

    inline float Distance(const Vec3& a, const Vec3& b)
    {
        const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }

    // Average speed in m/s of the segment ending at key i.
    inline float SegmentSpeed(const std::vector<Key>& keys, std::size_t i)
    {
        if (i == 0 || i >= keys.size())
            return 0.0f;
        const float span = std::max(1e-4f, keys[i].time - keys[i - 1].time);
        return Distance(keys[i - 1].position, keys[i].position) / span;
    }

    // Head-frame offset to tracking space (Source axes: x forward, y left, z up).
    inline Vec3 HeadLocalToTracking(const Vec3& head, float headYawDeg, const Vec3& local)
    {
        const float yaw = headYawDeg * 3.14159265358979f / 180.0f;
        const float c = std::cos(yaw), s = std::sin(yaw);
        // forward = (c, s, 0); right = (s, -c, 0)
        return { head.x + local.x * c + local.y * s,
                 head.y + local.x * s - local.y * c,
                 head.z + local.z };
    }

    // World offset from the head (game units) back to tracking metres, undoing
    // the stick-turn rotation offset applied around the head.
    inline Vec3 WorldOffsetToTracking(const Vec3& headTracking, const Vec3& worldOffset,
        float rotationOffsetDeg, float unitsPerMeter)
    {
        const float scale = unitsPerMeter > 1e-3f ? 1.0f / unitsPerMeter : 0.0f;
        const float a = -rotationOffsetDeg * 3.14159265358979f / 180.0f;
        const float c = std::cos(a), s = std::sin(a);
        const float x = worldOffset.x * scale, y = worldOffset.y * scale;
        return { headTracking.x + x * c - y * s, headTracking.y + x * s + y * c,
                 headTracking.z + worldOffset.z * scale };
    }

    // "throw,crouch , holster" -> {"throw","crouch","holster"}
    inline std::vector<std::string> ParseSteps(const std::string& text)
    {
        std::vector<std::string> steps;
        std::string current;
        auto flush = [&]()
        {
            std::size_t b = current.find_first_not_of(" \t");
            std::size_t e = current.find_last_not_of(" \t");
            if (b != std::string::npos)
            {
                std::string step = current.substr(b, e - b + 1);
                for (char& ch : step)
                    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                steps.push_back(step);
            }
            current.clear();
        };
        for (char ch : text)
        {
            if (ch == ',')
                flush();
            else
                current.push_back(ch);
        }
        flush();
        return steps;
    }
}

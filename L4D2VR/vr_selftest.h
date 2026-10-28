#pragma once
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
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

    // ---- Human body model ----------------------------------------------------
    // Where a standing adult's hands actually go (eyes ~1.65 m; metres from the
    // eyes in the head-yaw frame: x forward, y toward the right hand, z up).
    // Deliberately independent of the holster layout under test, so a self-test
    // reach succeeds only if the layout matches real bodies.
    namespace body
    {
        inline Vec3 V(float x, float y, float z) { Vec3 v; v.x = x; v.y = y; v.z = z; return v; }
        inline Vec3 RightReady() { return V(0.40f, 0.15f, -0.35f); }       // gun held out, ready
        inline Vec3 LeftReady() { return V(0.32f, -0.18f, -0.42f); }        // off hand relaxed in front
        inline Vec3 RightHip() { return V(-0.05f, 0.22f, -0.80f); }
        inline Vec3 OverRightShoulder() { return V(-0.10f, 0.15f, -0.05f); }
        inline Vec3 RightChest() { return V(0.10f, 0.12f, -0.35f); }
        inline Vec3 LeftChest() { return V(0.10f, -0.12f, -0.35f); }
        inline Vec3 CentreChest() { return V(0.12f, 0.0f, -0.40f); }      // sternum / belly pouch
        inline Vec3 OverLeftShoulder() { return V(-0.10f, -0.15f, -0.05f); }
        inline Vec3 LeftHip() { return V(-0.05f, -0.22f, -0.80f); }
        inline Vec3 Push() { return V(0.70f, 0.15f, -0.30f); }               // gun bash end point
    }

    inline float MinimumJerk(float u)
    {
        u = std::clamp(u, 0.0f, 1.0f);
        return u * u * u * (10.0f + u * (-15.0f + 6.0f * u));
    }

    // A human reach: minimum-jerk profile at averageSpeed m/s with ~2 mm of
    // tracking jitter, as keys starting at time 0. Angles (pitch, yaw, roll
    // relative to the head) turn along the same profile.
    inline void BuildReach(const Vec3& from, const Vec3& to, const Vec3& fromAngles, const Vec3& toAngles,
        float averageSpeed, unsigned seed, std::vector<Key>& position, std::vector<Key>& angles)
    {
        position.clear();
        angles.clear();
        const float duration = std::max(0.15f, Distance(from, to) / std::max(0.05f, averageSpeed));
        constexpr int kSteps = 16;
        for (int i = 0; i <= kSteps; ++i)
        {
            const float u = static_cast<float>(i) / kSteps;
            const float m = MinimumJerk(u);
            Vec3 p = Lerp(from, to, m);
            if (i > 0 && i < kSteps)
            {
                seed = seed * 1664525u + 1013904223u;
                p.x += ((seed >> 8 & 0xFF) / 255.0f - 0.5f) * 0.004f;
                p.y += ((seed >> 16 & 0xFF) / 255.0f - 0.5f) * 0.004f;
                p.z += ((seed >> 24 & 0xFF) / 255.0f - 0.5f) * 0.004f;
            }
            position.push_back(Key{ u * duration, p, false });
            angles.push_back(Key{ u * duration, Lerp(fromAngles, toAngles, m), false });
        }
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
        // "righthand=0.02,0.22,-0.7": numeric tokens continue the previous step.
        std::vector<std::string> merged;
        for (const std::string& step : steps)
        {
            const char first = step[0];
            const bool numeric = (first >= '0' && first <= '9') || first == '-' || first == '+' || first == '.';
            if (numeric && !merged.empty())
                merged.back() += "," + step;
            else
                merged.push_back(step);
        }
        return merged;
    }

    // "0.02,0.22,-0.7" -> {0.02, 0.22, -0.7}; false unless exactly three numbers.
    inline bool ParseVec3(const std::string& text, Vec3& out)
    {
        float values[3] = {};
        std::size_t begin = 0;
        for (int i = 0; i < 3; ++i)
        {
            const std::size_t end = text.find(',', begin);
            if ((i < 2) != (end != std::string::npos))
                return false;
            const std::string part = text.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
            char* stop = nullptr;
            values[i] = std::strtof(part.c_str(), &stop);
            if (part.empty() || !stop || *stop != '\0' || !std::isfinite(values[i]))
                return false;
            begin = end + 1;
        }
        out.x = values[0];
        out.y = values[1];
        out.z = values[2];
        return true;
    }

    // ---- Screenshots ----------------------------------------------------------
    // Minimal PNG writer (8-bit RGB, stored deflate blocks) for the self-test
    // eye captures: no compression library, any viewer can open the result.
    inline std::uint32_t PngCrc(const std::uint8_t* data, std::size_t size, std::uint32_t crc = 0xFFFFFFFFu)
    {
        for (std::size_t i = 0; i < size; ++i)
        {
            crc ^= data[i];
            for (int bit = 0; bit < 8; ++bit)
                crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
        return crc;
    }

    inline std::vector<std::uint8_t> EncodePngRgb(const std::uint8_t* rgb, int width, int height)
    {
        std::vector<std::uint8_t> png;
        if (!rgb || width <= 0 || height <= 0)
            return png;
        auto put32 = [](std::vector<std::uint8_t>& out, std::uint32_t v)
        {
            out.push_back(static_cast<std::uint8_t>(v >> 24));
            out.push_back(static_cast<std::uint8_t>(v >> 16));
            out.push_back(static_cast<std::uint8_t>(v >> 8));
            out.push_back(static_cast<std::uint8_t>(v));
        };
        auto chunk = [&](const char* type, const std::vector<std::uint8_t>& data)
        {
            put32(png, static_cast<std::uint32_t>(data.size()));
            const std::size_t start = png.size();
            png.insert(png.end(), type, type + 4);
            png.insert(png.end(), data.begin(), data.end());
            put32(png, PngCrc(png.data() + start, png.size() - start) ^ 0xFFFFFFFFu);
        };
        static const std::uint8_t kSignature[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
        png.insert(png.end(), kSignature, kSignature + 8);

        std::vector<std::uint8_t> header;
        put32(header, static_cast<std::uint32_t>(width));
        put32(header, static_cast<std::uint32_t>(height));
        header.insert(header.end(), { 8, 2, 0, 0, 0 }); // 8-bit RGB, no interlace
        chunk("IHDR", header);

        // Raw scanlines (filter byte 0) in stored deflate blocks of up to 65535 bytes.
        const std::size_t rowBytes = static_cast<std::size_t>(width) * 3u;
        std::vector<std::uint8_t> raw;
        raw.reserve((rowBytes + 1u) * static_cast<std::size_t>(height));
        for (int y = 0; y < height; ++y)
        {
            raw.push_back(0);
            const std::uint8_t* row = rgb + rowBytes * static_cast<std::size_t>(y);
            raw.insert(raw.end(), row, row + rowBytes);
        }
        std::vector<std::uint8_t> zlib = { 0x78, 0x01 };
        std::size_t offset = 0;
        do
        {
            const std::size_t block = (std::min)(raw.size() - offset, static_cast<std::size_t>(65535u));
            const bool last = offset + block >= raw.size();
            zlib.push_back(static_cast<std::uint8_t>(last ? 1u : 0u));
            zlib.push_back(static_cast<std::uint8_t>(block));
            zlib.push_back(static_cast<std::uint8_t>(block >> 8));
            zlib.push_back(static_cast<std::uint8_t>(~block));
            zlib.push_back(static_cast<std::uint8_t>(~block >> 8));
            zlib.insert(zlib.end(), raw.begin() + static_cast<std::ptrdiff_t>(offset),
                raw.begin() + static_cast<std::ptrdiff_t>(offset + block));
            offset += block;
        } while (offset < raw.size());
        std::uint32_t a = 1u, b = 0u;
        for (std::uint8_t byte : raw)
        {
            a = (a + byte) % 65521u;
            b = (b + a) % 65521u;
        }
        put32(zlib, (b << 16) | a);
        chunk("IDAT", zlib);
        chunk("IEND", {});
        return png;
    }
}

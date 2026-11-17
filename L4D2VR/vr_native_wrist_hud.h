#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

// Native wrist HUD layout (WristHudStyle=native). The wrist panels reuse the
// game's own HUD: VGUI paints the whole HUD into the HUD render target, and
// the wrist textures are built from scaled crops of that image, so fonts,
// bar graphics and colours are exactly the game's. Rectangles here are in
// pixels of the HUD render target; this header stays std-only so the layout
// can be unit tested.
namespace l4d2vr_native_wrist_hud
{
    struct Rect
    {
        int x = 0, y = 0, w = 0, h = 0;
        bool Empty() const { return w <= 0 || h <= 0; }
        int Right() const { return x + w; }
        int Bottom() const { return y + h; }
    };

    inline Rect Union(const Rect& a, const Rect& b)
    {
        if (a.Empty())
            return b;
        if (b.Empty())
            return a;
        const int x0 = (std::min)(a.x, b.x);
        const int y0 = (std::min)(a.y, b.y);
        const int x1 = (std::max)(a.Right(), b.Right());
        const int y1 = (std::max)(a.Bottom(), b.Bottom());
        return { x0, y0, x1 - x0, y1 - y0 };
    }

    inline Rect Clip(const Rect& r, int texW, int texH)
    {
        const int x0 = std::clamp(r.x, 0, (std::max)(0, texW));
        const int y0 = std::clamp(r.y, 0, (std::max)(0, texH));
        const int x1 = std::clamp(r.Right(), 0, (std::max)(0, texW));
        const int y1 = std::clamp(r.Bottom(), 0, (std::max)(0, texH));
        if (x1 <= x0 || y1 <= y0)
            return {};
        return { x0, y0, x1 - x0, y1 - y0 };
    }

    // Maps a VGUI screen rectangle into the HUD render target. VGUI lays the
    // HUD out for the root panel's size; the render target can differ.
    inline Rect ScreenToTexture(const Rect& r, int screenW, int screenH, int texW, int texH)
    {
        if (r.Empty() || screenW <= 0 || screenH <= 0 || texW <= 0 || texH <= 0)
            return {};
        const double sx = (double)texW / (double)screenW;
        const double sy = (double)texH / (double)screenH;
        const int x0 = (int)std::floor(r.x * sx);
        const int y0 = (int)std::floor(r.y * sy);
        const int x1 = (int)std::ceil(r.Right() * sx);
        const int y1 = (int)std::ceil(r.Bottom() * sy);
        return Clip({ x0, y0, x1 - x0, y1 - y0 }, texW, texH);
    }

    // Maps a panel rectangle through an ancestor that VGUI draws scaled: the
    // client root panels can be narrower than the viewport laid out inside
    // them (the HUD is then painted squeezed into the root's width).
    struct PanelMap
    {
        double sx = 1.0, sy = 1.0;
        double fromX = 0.0, fromY = 0.0;
        double toX = 0.0, toY = 0.0;

        static PanelMap Between(const Rect& laidOut, const Rect& drawn)
        {
            PanelMap m;
            if (laidOut.Empty() || drawn.Empty())
                return m;
            m.sx = (double)drawn.w / (double)laidOut.w;
            m.sy = (double)drawn.h / (double)laidOut.h;
            m.fromX = laidOut.x; m.fromY = laidOut.y;
            m.toX = drawn.x; m.toY = drawn.y;
            return m;
        }

        Rect Apply(const Rect& r) const
        {
            if (r.Empty())
                return {};
            const double x0 = (r.x - fromX) * sx + toX;
            const double y0 = (r.y - fromY) * sy + toY;
            const double x1 = (r.Right() - fromX) * sx + toX;
            const double y1 = (r.Bottom() - fromY) * sy + toY;
            const int ix0 = (int)std::floor(x0), iy0 = (int)std::floor(y0);
            return { ix0, iy0, (std::max)(1, (int)std::ceil(x1) - ix0), (std::max)(1, (int)std::ceil(y1) - iy0) };
        }
    };

    struct Blit
    {
        Rect src;
        Rect dst;
    };

    // A HUD element: where it is in the HUD texture (src) and its size as laid
    // out (w x h, in texture pixels before any squeeze), which the wrist uses
    // so the element keeps the game's intended proportions.
    struct Element
    {
        Rect src;
        int w = 0, h = 0;
        bool Empty() const { return src.Empty(); }
        int W() const { return w > 0 ? w : src.w; }
        int H() const { return h > 0 ? h : src.h; }
    };

    inline Element Union(const Element& a, const Element& b)
    {
        if (a.Empty())
            return b;
        if (b.Empty())
            return a;
        Element e;
        e.src = Union(a.src, b.src);
        // Scale of the union follows the parts (they share one squeeze).
        const double kx = (double)a.W() / (double)a.src.w;
        const double ky = (double)a.H() / (double)a.src.h;
        e.w = (int)std::lround(e.src.w * kx);
        e.h = (int)std::lround(e.src.h * ky);
        return e;
    }

    // One teammate: the native health bar and the name label under it.
    // Portraits are left out on the wrist.
    struct Teammate
    {
        Element bar;
        Element name;
    };

    struct Composition
    {
        std::vector<Blit> blits;
        int usedW = 0;
        int usedH = 0;
        bool Empty() const { return blits.empty(); }
    };

    struct LeftLayoutParams
    {
        int pad = 6;
        int gap = 8;          // between the local block and the teammate list
        int rowGap = 6;       // between teammates
        float mateScale = 0.62f; // teammate size relative to the local player's block
    };

    // Left wrist: the local player's health block (icon, number and bar) fills
    // the width at the top; teammates follow below, smaller, each as its bar
    // with the name under it like the native panel. Everything is scaled down
    // uniformly if it would not fit the texture height.
    inline Composition ComposeLeft(const Element& own, const std::vector<Teammate>& mates, int texW, int texH,
        const LeftLayoutParams& params = {})
    {
        Composition out;
        if (texW <= 2 * params.pad || texH <= 2 * params.pad)
            return out;

        int mateW = 0;
        int mateCount = 0;
        for (const Teammate& m : mates)
        {
            if (m.bar.Empty() && m.name.Empty())
                continue;
            mateW = (std::max)(mateW, (std::max)(m.bar.Empty() ? 0 : m.bar.W(), m.name.Empty() ? 0 : m.name.W()));
            ++mateCount;
        }
        if (own.Empty() && mateCount == 0)
            return out;

        const float avail = (float)(texW - 2 * params.pad);
        float ownScale = own.Empty() ? 0.0f : avail / (float)own.W();
        float mateScale = 0.0f;
        if (mateCount > 0)
        {
            const float fitMate = avail / (float)(std::max)(1, mateW);
            mateScale = ownScale > 0.0f ? (std::min)(fitMate, ownScale * params.mateScale) : fitMate * params.mateScale;
        }

        auto contentHeight = [&](float k) -> float
            {
                float h = 0.0f;
                if (!own.Empty())
                    h += own.H() * ownScale * k;
                int placed = 0;
                for (const Teammate& m : mates)
                {
                    if (m.bar.Empty() && m.name.Empty())
                        continue;
                    if (placed > 0)
                        h += (float)params.rowGap;
                    h += (m.bar.Empty() ? 0.0f : m.bar.H() * mateScale * k) + (m.name.Empty() ? 0.0f : m.name.H() * mateScale * k);
                    ++placed;
                }
                if (!own.Empty() && placed > 0)
                    h += (float)params.gap;
                return h;
            };

        const float availH = (float)(texH - 2 * params.pad);
        float k = 1.0f;
        const float full = contentHeight(1.0f);
        if (full > availH && full > 0.0f)
        {
            // Gaps do not scale; shrink the scaled part until it fits.
            const float fixed = contentHeight(0.0f);
            k = (std::max)(0.05f, (availH - fixed) / (std::max)(1.0f, full - fixed));
        }
        ownScale *= k;
        mateScale *= k;

        auto scaled = [](int v, float s) { return (std::max)(1, (int)std::lround(v * s)); };

        int y = params.pad;
        int right = params.pad;
        if (!own.Empty())
        {
            const Rect dst{ params.pad, y, scaled(own.W(), ownScale), scaled(own.H(), ownScale) };
            out.blits.push_back({ own.src, dst });
            y = dst.Bottom();
            right = (std::max)(right, dst.Right());
        }

        int placed = 0;
        for (const Teammate& m : mates)
        {
            if (m.bar.Empty() && m.name.Empty())
                continue;
            if (placed == 0 && !own.Empty())
                y += params.gap;
            else if (placed > 0)
                y += params.rowGap;
            if (!m.bar.Empty())
            {
                const Rect dst{ params.pad, y, scaled(m.bar.W(), mateScale), scaled(m.bar.H(), mateScale) };
                out.blits.push_back({ m.bar.src, dst });
                y = dst.Bottom();
                right = (std::max)(right, dst.Right());
            }
            if (!m.name.Empty())
            {
                const Rect dst{ params.pad, y, scaled(m.name.W(), mateScale), scaled(m.name.H(), mateScale) };
                out.blits.push_back({ m.name.src, dst });
                y = dst.Bottom();
                right = (std::max)(right, dst.Right());
            }
            ++placed;
        }

        out.usedW = (std::min)(texW, right + params.pad);
        out.usedH = (std::min)(texH, y + params.pad);
        for (Blit& b : out.blits)
            b.dst = Clip(b.dst, texW, texH);
        return out;
    }

    // Right wrist: the native weapon / inventory panel, scaled to the texture
    // width (or height, whichever limits first).
    inline Composition ComposeRight(const Element& panel, int texW, int texH, int pad = 4)
    {
        Composition out;
        if (panel.Empty() || texW <= 2 * pad || texH <= 2 * pad)
            return out;
        const float s = (std::min)((float)(texW - 2 * pad) / (float)panel.W(), (float)(texH - 2 * pad) / (float)panel.H());
        const Rect dst{ pad, pad, (std::max)(1, (int)std::lround(panel.W() * s)), (std::max)(1, (int)std::lround(panel.H() * s)) };
        out.blits.push_back({ panel.src, Clip(dst, texW, texH) });
        out.usedW = (std::min)(texW, dst.Right() + pad);
        out.usedH = (std::min)(texH, dst.Bottom() + pad);
        return out;
    }

    // Sub-rectangle of an element given as fractions of it (from the painted
    // content's alpha bounds). Keeps the element's laid-out scale.
    inline Element Trim(const Element& e, float u0, float v0, float u1, float v1)
    {
        if (e.Empty() || !(u1 > u0) || !(v1 > v0))
            return e;
        u0 = std::clamp(u0, 0.0f, 1.0f); u1 = std::clamp(u1, 0.0f, 1.0f);
        v0 = std::clamp(v0, 0.0f, 1.0f); v1 = std::clamp(v1, 0.0f, 1.0f);
        if (u1 - u0 < 0.02f || v1 - v0 < 0.02f)
            return e;
        Element out;
        const int x0 = e.src.x + (int)std::floor(u0 * e.src.w);
        const int y0 = e.src.y + (int)std::floor(v0 * e.src.h);
        const int x1 = e.src.x + (int)std::ceil(u1 * e.src.w);
        const int y1 = e.src.y + (int)std::ceil(v1 * e.src.h);
        out.src = { x0, y0, (std::max)(1, x1 - x0), (std::max)(1, y1 - y0) };
        out.w = (std::max)(1, (int)std::lround(e.W() * (double)out.src.w / e.src.w));
        out.h = (std::max)(1, (int)std::lround(e.H() * (double)out.src.h / e.src.h));
        return out;
    }

    // Alpha bounds of a BGRA/RGBA image (alpha in byte 3) as fractions, with a
    // margin in pixels. Returns false when nothing is painted.
    inline bool AlphaBounds(const uint8_t* pixels, int w, int h, int pitch, int threshold, int margin,
        float& u0, float& v0, float& u1, float& v1)
    {
        if (!pixels || w <= 0 || h <= 0)
            return false;
        int minX = w, minY = h, maxX = -1, maxY = -1;
        for (int y = 0; y < h; ++y)
        {
            const uint8_t* row = pixels + (size_t)y * (size_t)pitch;
            for (int x = 0; x < w; ++x)
            {
                if (row[x * 4 + 3] > threshold)
                {
                    minX = (std::min)(minX, x); maxX = (std::max)(maxX, x);
                    minY = (std::min)(minY, y); maxY = (std::max)(maxY, y);
                }
            }
        }
        if (maxX < 0)
            return false;
        minX = (std::max)(0, minX - margin); minY = (std::max)(0, minY - margin);
        maxX = (std::min)(w - 1, maxX + margin); maxY = (std::min)(h - 1, maxY + margin);
        u0 = (float)minX / w; v0 = (float)minY / h;
        u1 = (float)(maxX + 1) / w; v1 = (float)(maxY + 1) / h;
        return true;
    }

    // Wrist texture sizes (fixed, so the textures are never recreated while a
    // submit may still reference them; the used part is shown via texture bounds).
    constexpr int kLeftTexW = 512;
    constexpr int kLeftTexH = 512;
    constexpr int kRightTexW = 256;
    constexpr int kRightTexH = 512;

    // One frame's work for the D3D side: what to copy into each wrist texture
    // and which HUD areas to clear so the floating HUD does not repeat them.
    struct Job
    {
        uint32_t epoch = 0;
        Composition left;
        Composition right;
        std::vector<Rect> hide;
        Rect trimSrc; // weapon panel, untrimmed, for the alpha-bounds readback
        bool dump = false;
    };

    // Overlay quad height in metres for the used part of a wrist texture.
    inline float UsedHeightMeters(float widthMeters, const Composition& c)
    {
        if (c.usedW <= 0 || c.usedH <= 0 || !(widthMeters > 0.0f))
            return 0.0f;
        return widthMeters * (float)c.usedH / (float)c.usedW;
    }

    // ---- 2.0.12 (review s12 issue 16): native or custom, decided every frame ----
    //
    // 2.0.4-2.0.11 latched the custom panels for the whole process when no
    // native crop arrived within 4 s of the wrist wanting one. Those 4 s are
    // the join (spectator -> team -> spawn, MOTD with the cursor up), when no
    // crop can exist, so the native HUD never came back. Now each wrist shows
    // the native crop whenever a fresh one exists and the custom panel
    // otherwise; the timers below only decide when to *log*, and they only
    // run while a capture could succeed.
    struct SourceInputs
    {
        uint64_t nowMs = 0;
        bool cropFresh = false;   // a native crop for this wrist is recent enough to show
        bool playable = false;    // in game, alive on a team, no menu or cursor UI
        bool captureRan = false;  // the HUD walk ran recently (the capture is not gated)
    };

    struct SourceDecision
    {
        bool native = false;        // show the native crop (else the custom panel)
        bool logFallback = false;   // custom for kFallbackLogMs while the walk ran: say what it found
        bool logNotRunning = false; // custom for kNotRunningLogMs while the walk did not run: say why
        bool logRecovered = false;  // the native crop is back after a logged fallback
        uint64_t waitedMs = 0;      // capture-possible time without a crop (for the log)
    };

    class SourcePolicy
    {
    public:
        static constexpr uint64_t kFallbackLogMs = 4000;
        static constexpr uint64_t kNotRunningLogMs = 10000;
        // A hitch or a long menu must not count as waiting time.
        static constexpr uint64_t kMaxStepMs = 250;

        SourceDecision Update(const SourceInputs& in)
        {
            SourceDecision out;
            const uint64_t step = (m_LastMs != 0 && in.nowMs > m_LastMs) ? (std::min)(in.nowMs - m_LastMs, kMaxStepMs) : 0;
            m_LastMs = in.nowMs;
            if (in.cropFresh)
            {
                out.native = true;
                out.logRecovered = m_FallbackLogged || m_NotRunningLogged;
                m_FallbackLogged = false;
                m_NotRunningLogged = false;
                m_WaitCaptureMs = 0;
                m_WaitRunMs = 0;
                return out;
            }
            if (in.playable && in.captureRan)
            {
                m_WaitCaptureMs += step;
                m_WaitRunMs = 0;
                if (m_WaitCaptureMs >= kFallbackLogMs && !m_FallbackLogged)
                {
                    m_FallbackLogged = true;
                    out.logFallback = true;
                }
            }
            else if (in.playable)
            {
                m_WaitRunMs += step;
                if (m_WaitRunMs >= kNotRunningLogMs && !m_NotRunningLogged)
                {
                    m_NotRunningLogged = true;
                    out.logNotRunning = true;
                }
            }
            out.waitedMs = in.captureRan ? m_WaitCaptureMs : m_WaitRunMs;
            return out;
        }

        // Spawn, map change, a new capture gate reason: restart the timers.
        // A logged fallback stays logged, so its recovery is still reported.
        void Reset()
        {
            m_LastMs = 0;
            m_WaitCaptureMs = 0;
            m_WaitRunMs = 0;
        }

        bool FallbackLogged() const { return m_FallbackLogged || m_NotRunningLogged; }

    private:
        uint64_t m_LastMs = 0;
        uint64_t m_WaitCaptureMs = 0;
        uint64_t m_WaitRunMs = 0;
        bool m_FallbackLogged = false;
        bool m_NotRunningLogged = false;
    };

    // What one HUD walk found, for the "native capture:" log line.
    struct CaptureSummary
    {
        bool localPlayer = false;  // the LocalPlayer panel
        int healthParts = 0;       // of HealthIcon / HealthNumber / Health
        int teamPlayers = 0;       // TeamPlayer* panels with a bar or a name
        bool weapon = false;       // HudWeaponSelection
        int visited = 0;
        int leftW = 0, leftH = 0;  // composed wrist crops (0 = none)
        int rightW = 0, rightH = 0;

        bool AnyPanel() const { return localPlayer || teamPlayers > 0 || weapon; }
        // The parts that make a result "distinct" (sizes and counts of
        // visited panels change every frame and are left out).
        uint32_t Key() const
        {
            return (localPlayer ? 1u : 0u) | ((uint32_t)(healthParts & 3) << 1) |
                ((uint32_t)(teamPlayers < 15 ? teamPlayers : 15) << 3) | (weapon ? 0x80u : 0u) |
                (leftW > 0 ? 0x100u : 0u) | (rightW > 0 ? 0x200u : 0u);
        }
    };

    // Logs each distinct capture result once (plus the first), at most one
    // line per kMinIntervalMs; a result that flickers back and forth is
    // logged once per value, not once per frame.
    class CaptureReport
    {
    public:
        static constexpr uint64_t kMinIntervalMs = 2000;

        static constexpr uint32_t kCropBits = 0x300u; // CaptureSummary::Key: a left or right crop

        bool ShouldLog(uint32_t key, uint64_t nowMs)
        {
            if (m_HasLast && key == m_LastKey)
                return false;
            bool seen = false;
            for (int i = 0; i < m_SeenCount; ++i)
                seen = seen || m_Seen[i] == key;
            // A result seen before is only worth a line again when it brings
            // a crop back after a logged result without one (a recovery).
            const bool recovery = seen && (key & kCropBits) != 0 && (m_LastKey & kCropBits) == 0;
            if (seen && !recovery)
                return false;
            if (m_HasLast && nowMs - m_LastLogMs < kMinIntervalMs)
                return false;
            if (!seen && m_SeenCount < kMaxSeen)
                m_Seen[m_SeenCount++] = key;
            m_HasLast = true;
            m_LastKey = key;
            m_LastLogMs = nowMs;
            return true;
        }

        void Reset() { m_HasLast = false; m_SeenCount = 0; }

    private:
        static constexpr int kMaxSeen = 16;
        uint32_t m_Seen[kMaxSeen] = {};
        int m_SeenCount = 0;
        bool m_HasLast = false;
        uint32_t m_LastKey = 0;
        uint64_t m_LastLogMs = 0;
    };

    // Formats the capture summary (std-only so the wording is tested).
    inline int FormatCaptureSummary(char* out, size_t size, const CaptureSummary& s)
    {
        return std::snprintf(out, size,
            "visited=%d panels LocalPlayer=%s(health parts %d/3) TeamPlayer=%d weapon=HudWeaponSelection:%s -> left crop %s, right crop %s",
            s.visited, s.localPlayer ? "found" : "not found", s.healthParts, s.teamPlayers,
            s.weapon ? "found" : "not found",
            s.leftW > 0 ? (std::to_string(s.leftW) + "x" + std::to_string(s.leftH)).c_str() : "none",
            s.rightW > 0 ? (std::to_string(s.rightW) + "x" + std::to_string(s.rightH)).c_str() : "none");
    }
}

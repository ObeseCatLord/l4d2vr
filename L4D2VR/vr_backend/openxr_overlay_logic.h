#pragma once

// OpenXR overlays (OpenXR port M4): the runtime-free parts, unit-tested in
// tests/vr_openxr_overlay_tests.cpp.
//
// * Geometry: an OpenVR overlay (absolute transform, width in meters, texel
//   aspect, texture bounds, curvature) as an OpenXR quad or cylinder layer.
//   SteamVR semantics: the whole affine transform applies (axis scale
//   stretches the panel), height = width * visible texture aspect, a curved
//   overlay keeps its arc length = width and bends towards the viewer (+Z).
// * Layer planning: sort order, the layer budget (maxLayerCount) with a
//   priority order for what to drop, and the wineopenxr layer workaround
//   (Proton #10275: a projection at layers[0] followed by 2+ layers corrupts
//   the frame; a tiny invisible quad first avoids it).
// * Synthetic overlay mouse: SteamVR's laser mouse rebuilt from controller
//   rays and triggers. OpenVR mouse events (move/down/up/discrete scroll) in
//   the overlay's mouse scale, y up, with click capture for drags.
// * In-mod VR keyboard: layout, hit test and editing for
//   ShowKeyboardForOverlay (OpenXR has no system keyboard).

#include "../vr_overlay_pointer.h"
#include "openxr_pose_math.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace l4d2vr_openxr_overlay
{
    namespace op = l4d2vr_overlay_pointer;
    namespace xrm = l4d2vr_openxr;

    constexpr float kPi = 3.14159265358979323846f;
    constexpr float kMinCurvature = 0.001f;

    // ---- Geometry -----------------------------------------------------------------
    struct Bounds
    {
        float uMin = 0.0f, vMin = 0.0f, uMax = 1.0f, vMax = 1.0f;
    };

    // The texture sub-rectangle the bounds select (pixels, top-left origin)
    // and whether it is mirrored on each axis. OpenXR image rects cannot flip;
    // a flipped region is mirrored when the content is copied.
    struct Region
    {
        int32_t x = 0, y = 0, width = 0, height = 0;
        bool flipU = false, flipV = false;
    };

    inline Region RegionFromBounds(const Bounds& b, uint32_t texWidth, uint32_t texHeight)
    {
        Region r;
        if (texWidth == 0 || texHeight == 0)
            return r;
        vr::VRTextureBounds_t vb{ b.uMin, b.vMin, b.uMax, b.vMax };
        const xrm::Rect rect = xrm::ImageRectFromBounds(&vb, texWidth, texHeight);
        r.x = rect.x;
        r.y = rect.y;
        r.width = rect.width;
        r.height = rect.height;
        r.flipU = b.uMin > b.uMax;
        r.flipV = b.vMin > b.vMax;
        return r;
    }

    struct Shape
    {
        op::Mat34 transform;          // tracking space, OpenVR overlay convention (faces +Z)
        float widthMeters = 1.0f;
        float texelAspect = 1.0f;     // texel width / texel height
        float curvature = 0.0f;       // OpenVR: fraction of a full circle
        uint32_t texWidth = 0, texHeight = 0;
        Bounds bounds;
    };

    struct Geometry
    {
        bool valid = false;
        bool cylinder = false;
        xrm::Pose pose;               // quad: panel centre; cylinder: point on the axis (viewer side)
        float width = 0.0f;           // quad: meters; cylinder: arc length
        float height = 0.0f;
        float radius = 0.0f;          // cylinder only
        float centralAngle = 0.0f;    // cylinder only, radians
        float aspectRatio = 1.0f;     // cylinder only: width / height
        Region region;
        op::Panel panel;              // the same surface for hit tests (u right, v down)
    };

    inline xrm::Pose PoseFromBasis(const op::Vec3& right, const op::Vec3& up, const op::Vec3& normal, const op::Vec3& origin)
    {
        vr::HmdMatrix34_t m{};
        const op::Vec3 cols[4] = { right, up, normal, origin };
        for (int c = 0; c < 4; ++c)
        {
            m.m[0][c] = cols[c].x;
            m.m[1][c] = cols[c].y;
            m.m[2][c] = cols[c].z;
        }
        return xrm::FromMatrix(m);
    }

    inline bool ComputeGeometry(const Shape& s, bool cylinderSupported, Geometry& out)
    {
        out = Geometry{};
        if (!op::Finite(s.transform) || !std::isfinite(s.widthMeters) || s.widthMeters <= 0.0f ||
            s.texWidth == 0 || s.texHeight == 0)
            return false;
        out.region = RegionFromBounds(s.bounds, s.texWidth, s.texHeight);
        if (out.region.width <= 0 || out.region.height <= 0)
            return false;
        const float texel = (std::isfinite(s.texelAspect) && s.texelAspect > 0.0f) ? s.texelAspect : 1.0f;
        const float aspect = static_cast<float>(out.region.height) / (static_cast<float>(out.region.width) * texel);

        const op::Vec3 colX = op::Column(s.transform, 0);
        const op::Vec3 colY = op::Column(s.transform, 1);
        const op::Vec3 colZ = op::Column(s.transform, 2);
        op::Vec3 normal;
        if (!op::Normalize(colZ, normal) && !op::Normalize(op::Cross(colX, colY), normal))
            return false;
        op::Vec3 right;
        if (!op::Normalize(op::Sub(colX, op::Scale(normal, op::Dot(colX, normal))), right))
            return false;
        const op::Vec3 up = op::Cross(normal, right);
        const float sx = op::Length(colX);
        const float sy = op::Length(colY);
        const float width = s.widthMeters * (sx > 1e-6f ? sx : 1.0f);
        const float height = s.widthMeters * aspect * (sy > 1e-6f ? sy : 1.0f);
        if (!std::isfinite(width) || !std::isfinite(height) || width <= 0.0f || height <= 0.0f)
            return false;
        const op::Vec3 center = op::Column(s.transform, 3);

        float curvature = std::isfinite(s.curvature) ? s.curvature : 0.0f;
        if (curvature < kMinCurvature || !cylinderSupported)
            curvature = 0.0f;
        curvature = (std::min)(curvature, 1.0f);

        out.panel.center = center;
        out.panel.right = right;
        out.panel.up = up;
        out.panel.normal = normal;
        out.panel.width = width;
        out.panel.height = height;
        out.panel.curvature = curvature;
        out.width = width;
        out.height = height;
        if (curvature > 0.0f)
        {
            const float angle = 2.0f * kPi * curvature;
            const float radius = width / angle;
            out.cylinder = true;
            out.radius = radius;
            out.centralAngle = angle;
            out.aspectRatio = width / height;
            out.pose = PoseFromBasis(right, up, normal, op::Add(center, op::Scale(normal, radius)));
        }
        else
        {
            out.pose = PoseFromBasis(right, up, normal, center);
        }
        out.valid = std::isfinite(out.pose.position.x) && std::isfinite(out.pose.position.y) && std::isfinite(out.pose.position.z) &&
            std::isfinite(out.pose.orientation.w);
        return out.valid;
    }

    // ---- Layer planning ---------------------------------------------------------
    // What is dropped first when the runtime's layer limit is reached.
    enum class Priority : uint8_t
    {
        Low = 0,     // kill indicators
        Normal = 1,  // wrist panels, scope, mirror, intent HUD
        High = 2,    // menus, HUD, config panel, keyboard, laser
    };

    inline Priority PriorityForKey(const std::string& key)
    {
        if (key.rfind("KillIndicatorOverlayKey", 0) == 0)
            return Priority::Low;
        if (key == "LeftWristHudOverlayKey" || key == "RightAmmoHudOverlayKey" || key == "ScopeOverlayKey" ||
            key == "RearMirrorOverlayKey" || key == "SpecialInfectedIntentSenseHudOverlayKey")
            return Priority::Normal;
        return Priority::High;
    }

    struct Candidate
    {
        uint64_t id = 0;          // stable identity (overlay handle)
        uint32_t sortOrder = 0;   // OpenVR: higher draws on top
        uint64_t serial = 0;      // creation order, ties
        Priority priority = Priority::High;
    };

    enum class Workaround : uint8_t
    {
        Off,     // native runtimes
        Auto,    // wineopenxr: dummy first when a projection is followed by 2+ layers
        Always,  // wineopenxr: dummy first whenever a projection has any overlay after it
    };

    struct Plan
    {
        std::vector<size_t> order;    // indices into the candidates, back to front
        std::vector<size_t> dropped;  // over the budget
        bool dummyFirst = false;
        uint32_t layerCount = 0;      // total submitted (dummy + projection + overlays)
    };

    inline bool DummyNeeded(Workaround w, bool projection, size_t overlays)
    {
        if (!projection || w == Workaround::Off)
            return false;
        return w == Workaround::Always ? overlays >= 1 : overlays >= 2;
    }

    inline Plan PlanLayers(const std::vector<Candidate>& candidates, uint32_t maxLayers, bool projection, Workaround w)
    {
        Plan plan;
        // The spec guarantees at least 16; 0 means the runtime did not say.
        const uint32_t limit = maxLayers == 0 ? 16u : maxLayers;
        const size_t base = projection ? 1u : 0u;
        const size_t room = limit > base ? limit - base : 0u;
        size_t keep = (std::min)(candidates.size(), room);
        bool dummy = DummyNeeded(w, projection, keep);
        if (dummy && keep + 1 > room)
        {
            keep = room > 0 ? room - 1 : 0;
            dummy = DummyNeeded(w, projection, keep);
        }

        std::vector<size_t> ranked(candidates.size());
        for (size_t i = 0; i < ranked.size(); ++i)
            ranked[i] = i;
        std::stable_sort(ranked.begin(), ranked.end(), [&](size_t a, size_t b) {
            const Candidate& ca = candidates[a];
            const Candidate& cb = candidates[b];
            if (ca.priority != cb.priority)
                return ca.priority > cb.priority;
            if (ca.sortOrder != cb.sortOrder)
                return ca.sortOrder > cb.sortOrder;
            return ca.serial < cb.serial;
        });
        plan.order.assign(ranked.begin(), ranked.begin() + static_cast<std::ptrdiff_t>(keep));
        plan.dropped.assign(ranked.begin() + static_cast<std::ptrdiff_t>(keep), ranked.end());
        std::stable_sort(plan.order.begin(), plan.order.end(), [&](size_t a, size_t b) {
            const Candidate& ca = candidates[a];
            const Candidate& cb = candidates[b];
            if (ca.sortOrder != cb.sortOrder)
                return ca.sortOrder < cb.sortOrder;
            return ca.serial < cb.serial;
        });
        plan.dummyFirst = dummy;
        plan.layerCount = static_cast<uint32_t>(plan.order.size() + base + (dummy ? 1u : 0u));
        return plan;
    }

    // ---- Alpha ----------------------------------------------------------------------
    // SetOverlayAlpha without XR_KHR_composition_layer_color_scale_bias: the
    // alpha channel of the copied pixels is scaled (unpremultiplied blending).
    // An overlay that ignores its texture alpha fades as a whole.
    inline void ApplyAlpha(uint8_t* rgba, size_t pixels, float alpha, bool ignoreTextureAlpha)
    {
        const float a = std::isfinite(alpha) ? std::clamp(alpha, 0.0f, 1.0f) : 1.0f;
        const uint32_t scale = static_cast<uint32_t>(std::lround(a * 256.0f));
        for (size_t i = 0; i < pixels; ++i)
        {
            uint8_t& pa = rgba[i * 4 + 3];
            if (ignoreTextureAlpha)
                pa = static_cast<uint8_t>((std::min)(255u, scale * 255u >> 8));
            else
                pa = static_cast<uint8_t>((static_cast<uint32_t>(pa) * scale) >> 8);
        }
    }

    // Mirrors a tightly packed 4-byte-per-pixel image in place.
    inline void FlipPixels(uint8_t* px, uint32_t width, uint32_t height, bool flipU, bool flipV)
    {
        const size_t stride = static_cast<size_t>(width) * 4u;
        if (flipU)
        {
            for (uint32_t y = 0; y < height; ++y)
            {
                uint8_t* row = px + y * stride;
                for (uint32_t x = 0; x < width / 2; ++x)
                    for (int c = 0; c < 4; ++c)
                        std::swap(row[x * 4 + c], row[(width - 1 - x) * 4 + c]);
            }
        }
        if (flipV)
        {
            for (uint32_t y = 0; y < height / 2; ++y)
                std::swap_ranges(px + y * stride, px + (y + 1) * stride, px + (height - 1 - y) * stride);
        }
    }

    // ---- Aim override (tests) ---------------------------------------------------
    // Turns a controller pose about its tip so the tip ray (-Z) points at
    // `target` (tracking space). Used by the self-test to aim the real laser.
    inline op::Mat34 AimPoseAt(const op::Mat34& raw, const op::Mat34& tipLocal, const op::Vec3& target)
    {
        const op::Mat34 tipWorld = op::Multiply(raw, tipLocal);
        const op::Vec3 tipPos = op::Column(tipWorld, 3);
        op::Vec3 from, to;
        if (!op::Normalize(op::Scale(op::Column(tipWorld, 2), -1.0f), from) || !op::Normalize(op::Sub(target, tipPos), to))
            return raw;
        // Shortest-arc rotation from -> to (Rodrigues).
        const op::Vec3 axis = op::Cross(from, to);
        const float c = op::Dot(from, to);
        const float s = op::Length(axis);
        float R[3][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
        if (s > 1e-6f)
        {
            const op::Vec3 k = op::Scale(axis, 1.0f / s);
            const float K[3][3] = { { 0, -k.z, k.y }, { k.z, 0, -k.x }, { -k.y, k.x, 0 } };
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j)
                {
                    float kk = 0.0f;
                    for (int m = 0; m < 3; ++m)
                        kk += K[i][m] * K[m][j];
                    R[i][j] += s * K[i][j] + (1.0f - c) * kk;
                }
        }
        else if (c < 0.0f)
        {
            // Opposite: half turn about any axis perpendicular to `from`.
            op::Vec3 p;
            if (!op::Normalize(op::Cross(from, std::fabs(from.y) < 0.9f ? op::Vec3{ 0, 1, 0 } : op::Vec3{ 1, 0, 0 }), p))
                return raw;
            const float P[3] = { p.x, p.y, p.z };
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j)
                    R[i][j] = 2.0f * P[i] * P[j] - (i == j ? 1.0f : 0.0f);
        }
        op::Mat34 rot;
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                rot.m[i][j] = R[i][j];
        // Rotate about the tip position: p' = tip + R (p - tip).
        op::Mat34 about = rot;
        const op::Vec3 rt = { R[0][0] * tipPos.x + R[0][1] * tipPos.y + R[0][2] * tipPos.z,
            R[1][0] * tipPos.x + R[1][1] * tipPos.y + R[1][2] * tipPos.z,
            R[2][0] * tipPos.x + R[2][1] * tipPos.y + R[2][2] * tipPos.z };
        about.m[0][3] = tipPos.x - rt.x;
        about.m[1][3] = tipPos.y - rt.y;
        about.m[2][3] = tipPos.z - rt.z;
        return op::Multiply(about, raw);
    }

    // ---- Synthetic overlay mouse ------------------------------------------------
    struct Target
    {
        uint64_t id = 0;
        op::Panel panel;
        uint32_t sortOrder = 0;
        float mouseScaleX = 1.0f, mouseScaleY = 1.0f;
        bool scrollEvents = false;   // VROverlayFlags_SendVRDiscreteScrollEvents
    };

    struct HandInput
    {
        bool valid = false;
        op::Ray ray;
        float trigger = 0.0f;        // 0..1
        float stickY = 0.0f;         // -1..1, + = up
    };

    enum class EventType : uint8_t { Move, Down, Up, Scroll };

    struct Event
    {
        uint64_t overlay = 0;
        EventType type = EventType::Move;
        float x = 0.0f, y = 0.0f;    // OpenVR mouse coordinates (y up)
        float scrollY = 0.0f;        // +1 = up
        int hand = -1;
    };

    struct HandHit
    {
        bool hit = false;
        size_t target = 0;           // index into the targets
        op::Hit where;
    };

    struct PointerOutput
    {
        HandHit hands[2];
        int active = -1;             // hand driving the cursor
        bool pressed = false;        // button held (drag)
        bool engaged[2] = { false, false };  // the overlay owns this hand's trigger
        uint64_t captured = 0;       // overlay that received the button down
    };

    constexpr float kTriggerPress = 0.70f;
    constexpr float kTriggerRelease = 0.45f;
    constexpr float kScrollStart = 0.60f;
    constexpr float kScrollStop = 0.35f;
    constexpr uint64_t kScrollRepeatMs = 200;
    constexpr float kPointerMaxDistance = 20.0f;

    inline void MousePosition(const Target& t, const op::Hit& hit, float& x, float& y)
    {
        const float u = std::clamp(hit.u, 0.0f, 1.0f);
        const float v = std::clamp(hit.v, 0.0f, 1.0f);
        x = u * t.mouseScaleX;
        y = (1.0f - v) * t.mouseScaleY;
    }

    // Nearest target the ray hits (higher sort order on a tie).
    inline bool NearestHit(const std::vector<Target>& targets, const op::Ray& ray, HandHit& out)
    {
        out = HandHit{};
        float best = kPointerMaxDistance;
        uint32_t bestSort = 0;
        for (size_t i = 0; i < targets.size(); ++i)
        {
            op::Hit h;
            if (!op::Intersect(targets[i].panel, ray, kPointerMaxDistance, h))
                continue;
            const bool closer = h.distance < best - 1e-3f;
            const bool tie = std::fabs(h.distance - best) <= 1e-3f && targets[i].sortOrder > bestSort;
            if (!out.hit || closer || tie)
            {
                out.hit = true;
                out.target = i;
                out.where = h;
                best = h.distance;
                bestSort = targets[i].sortOrder;
            }
        }
        return out.hit;
    }

    class Pointer
    {
    public:
        // One step per input sync. Appends the OpenVR events to `events`.
        PointerOutput Step(const std::vector<Target>& targets, const HandInput hands[2], uint64_t nowMs, std::vector<Event>& events)
        {
            PointerOutput out;
            bool trigger[2] = { false, false };
            for (int h = 0; h < 2; ++h)
            {
                const float v = hands[h].valid ? hands[h].trigger : 0.0f;
                m_TriggerDown[h] = m_TriggerDown[h] ? v > kTriggerRelease : v >= kTriggerPress;
                trigger[h] = m_TriggerDown[h];
                if (hands[h].valid)
                    NearestHit(targets, hands[h].ray, out.hands[h]);
            }

            // A captured overlay that disappeared releases the button.
            if (m_Tracker.buttonDown && FindTarget(targets, m_Captured) < 0)
            {
                events.push_back(Event{ m_Captured, EventType::Up, m_LastX, m_LastY, 0.0f, m_Tracker.downHand });
                m_Tracker.buttonDown = false;
                m_Tracker.downHand = -1;
                m_Captured = 0;
            }

            op::PointerFrame frame;
            for (int h = 0; h < 2; ++h)
            {
                frame.hits[h] = out.hands[h].hit;
                frame.trigger[h] = trigger[h];
            }
            const bool wasDown = m_Tracker.buttonDown;
            const op::PointerDecision decision = op::StepTracker(m_Tracker, frame);
            out.active = decision.hand;

            if (decision.action == op::ClickAction::Press && decision.hand >= 0)
            {
                const HandHit& hh = out.hands[decision.hand];
                const Target& t = targets[hh.target];
                float x = 0.0f, y = 0.0f;
                MousePosition(t, hh.where, x, y);
                if (m_HoverId != t.id || x != m_LastX || y != m_LastY)
                    events.push_back(Event{ t.id, EventType::Move, x, y, 0.0f, decision.hand });
                events.push_back(Event{ t.id, EventType::Down, x, y, 0.0f, decision.hand });
                m_Captured = t.id;
                m_HoverId = t.id;
                m_LastX = x;
                m_LastY = y;
            }
            else if (decision.action == op::ClickAction::Release && wasDown)
            {
                // Drag: the last position on the captured overlay.
                const int h = decision.hand;
                const int ci = FindTarget(targets, m_Captured);
                op::Hit hit;
                if (h >= 0 && ci >= 0 && hands[h].valid &&
                    op::Intersect(targets[static_cast<size_t>(ci)].panel, hands[h].ray, kPointerMaxDistance, hit))
                {
                    float x = 0.0f, y = 0.0f;
                    MousePosition(targets[static_cast<size_t>(ci)], hit, x, y);
                    if (x != m_LastX || y != m_LastY)
                        events.push_back(Event{ m_Captured, EventType::Move, x, y, 0.0f, h });
                    m_LastX = x;
                    m_LastY = y;
                }
                events.push_back(Event{ m_Captured, EventType::Up, m_LastX, m_LastY, 0.0f, h });
                m_Captured = 0;
            }
            else if (m_Tracker.buttonDown && decision.hand >= 0 && hands[decision.hand].valid)
            {
                // Dragging: the captured overlay gets the moves wherever the ray
                // meets it, even under another panel (the config panel's two
                // copies swap while it is used).
                const int ci = FindTarget(targets, m_Captured);
                op::Hit hit;
                if (ci >= 0 && op::Intersect(targets[static_cast<size_t>(ci)].panel, hands[decision.hand].ray, kPointerMaxDistance, hit))
                {
                    float x = 0.0f, y = 0.0f;
                    MousePosition(targets[static_cast<size_t>(ci)], hit, x, y);
                    if (std::fabs(x - m_LastX) >= 0.5f || std::fabs(y - m_LastY) >= 0.5f)
                    {
                        events.push_back(Event{ m_Captured, EventType::Move, x, y, 0.0f, decision.hand });
                        m_LastX = x;
                        m_LastY = y;
                    }
                }
            }
            else if (decision.hand >= 0 && out.hands[decision.hand].hit)
            {
                const HandHit& hh = out.hands[decision.hand];
                const Target& t = targets[hh.target];
                float x = 0.0f, y = 0.0f;
                MousePosition(t, hh.where, x, y);
                if (m_HoverId != t.id || std::fabs(x - m_LastX) >= 0.5f || std::fabs(y - m_LastY) >= 0.5f)
                {
                    events.push_back(Event{ t.id, EventType::Move, x, y, 0.0f, decision.hand });
                    m_HoverId = t.id;
                    m_LastX = x;
                    m_LastY = y;
                }
            }
            if (decision.hand < 0 || !out.hands[decision.hand].hit)
            {
                if (!m_Tracker.buttonDown)
                    m_HoverId = 0;
            }

            // Discrete scroll from the pointing hand's stick.
            const int sh = decision.hand;
            bool scrolled = false;
            if (sh >= 0 && out.hands[sh].hit && targets[out.hands[sh].target].scrollEvents)
            {
                const float y = hands[sh].stickY;
                const float mag = std::fabs(y);
                const bool held = m_ScrollHeld ? mag > kScrollStop : mag >= kScrollStart;
                if (held)
                {
                    const bool first = !m_ScrollHeld;
                    if (first || nowMs - m_ScrollLastMs >= kScrollRepeatMs)
                    {
                        const Target& t = targets[out.hands[sh].target];
                        float x = 0.0f, yy = 0.0f;
                        MousePosition(t, out.hands[sh].where, x, yy);
                        events.push_back(Event{ t.id, EventType::Scroll, x, yy, y > 0.0f ? 1.0f : -1.0f, sh });
                        m_ScrollLastMs = nowMs;
                    }
                    scrolled = true;
                }
            }
            m_ScrollHeld = scrolled;

            out.pressed = m_Tracker.buttonDown;
            out.captured = m_Tracker.buttonDown ? m_Captured : 0;
            for (int h = 0; h < 2; ++h)
                out.engaged[h] = out.hands[h].hit || (m_Tracker.buttonDown && m_Tracker.downHand == h) ||
                    (m_TriggerDown[h] && m_EngagedHeld[h]);
            // A trigger that clicked an overlay stays the overlay's until released.
            for (int h = 0; h < 2; ++h)
                m_EngagedHeld[h] = m_TriggerDown[h] && out.engaged[h];
            return out;
        }

        // A trigger already held when the pointer appears must be pressed again.
        void Reset()
        {
            bool held[2] = { m_TriggerDown[0], m_TriggerDown[1] };
            op::ResetTracker(m_Tracker, held);
            m_Captured = 0;
            m_HoverId = 0;
            m_ScrollHeld = false;
        }

        bool ButtonDown() const { return m_Tracker.buttonDown; }

    private:
        static int FindTarget(const std::vector<Target>& targets, uint64_t id)
        {
            for (size_t i = 0; i < targets.size(); ++i)
                if (targets[i].id == id)
                    return static_cast<int>(i);
            return -1;
        }

        op::PointerTracker m_Tracker;
        bool m_TriggerDown[2] = { false, false };
        bool m_EngagedHeld[2] = { false, false };
        uint64_t m_Captured = 0;
        uint64_t m_HoverId = 0;
        float m_LastX = -1.0f, m_LastY = -1.0f;
        bool m_ScrollHeld = false;
        uint64_t m_ScrollLastMs = 0;
    };

    // ---- In-mod VR keyboard -----------------------------------------------------
    enum class KeyKind : uint8_t { Char, Shift, Backspace, Space, Clear, Cancel, Done };

    struct Key
    {
        KeyKind kind = KeyKind::Char;
        char lower = 0, upper = 0;
        int x = 0, y = 0, w = 0, h = 0;  // canvas pixels
        const char* label = "";
    };

    constexpr int kKeyboardWidth = 1200;
    constexpr int kKeyboardHeight = 520;
    constexpr int kKeyboardTextTop = 12;
    constexpr int kKeyboardTextHeight = 104;
    constexpr int kKeyboardKeysTop = 128;

    inline const std::vector<Key>& KeyboardLayout()
    {
        static const std::vector<Key> keys = []() {
            std::vector<Key> k;
            const int pitch = 86, kw = 80, kh = 72, rowPitch = 78, left = 20;
            const char* rows[4][2] = {
                { "1234567890-=", "!@#$%^&*()_+" },
                { "qwertyuiop[]", "QWERTYUIOP{}" },
                { "asdfghjkl;'", "ASDFGHJKL:\"" },
                { "zxcvbnm,./", "ZXCVBNM<>?" },
            };
            const int indent[4] = { 0, 30, 52, 117 };
            for (int r = 0; r < 4; ++r)
            {
                const size_t n = std::strlen(rows[r][0]);
                for (size_t i = 0; i < n; ++i)
                {
                    Key key;
                    key.kind = KeyKind::Char;
                    key.lower = rows[r][0][i];
                    key.upper = rows[r][1][i];
                    key.x = left + indent[r] + static_cast<int>(i) * pitch;
                    key.y = kKeyboardKeysTop + r * rowPitch;
                    key.w = kw;
                    key.h = kh;
                    k.push_back(key);
                }
            }
            const int x12 = left + 12 * pitch;  // right of the digit row
            k.push_back(Key{ KeyKind::Backspace, 0, 0, x12, kKeyboardKeysTop, kKeyboardWidth - 20 - x12, kh, "Bksp" });
            k.push_back(Key{ KeyKind::Shift, 0, 0, left, kKeyboardKeysTop + 3 * rowPitch, 107, kh, "Shift" });
            const int xClear = left + indent[3] + 10 * pitch;
            k.push_back(Key{ KeyKind::Clear, 0, 0, xClear, kKeyboardKeysTop + 3 * rowPitch, kKeyboardWidth - 20 - xClear, kh, "Clear" });
            const int bottom = kKeyboardKeysTop + 4 * rowPitch;
            k.push_back(Key{ KeyKind::Cancel, 0, 0, left, bottom, 220, kh, "Cancel" });
            k.push_back(Key{ KeyKind::Space, ' ', ' ', left + 232, bottom, 690, kh, "Space" });
            k.push_back(Key{ KeyKind::Done, 0, 0, left + 934, bottom, kKeyboardWidth - 20 - (left + 934), kh, "Done" });
            return k;
        }();
        return keys;
    }

    inline int KeyAt(int px, int py)
    {
        const std::vector<Key>& keys = KeyboardLayout();
        for (size_t i = 0; i < keys.size(); ++i)
        {
            const Key& k = keys[i];
            if (px >= k.x && px < k.x + k.w && py >= k.y && py < k.y + k.h)
                return static_cast<int>(i);
        }
        return -1;
    }

    // Centre of the key for a character (or a special key), as u,v (0..1, v down).
    inline bool KeyCentreUv(char ch, KeyKind kind, bool& needsShift, float& u, float& v)
    {
        needsShift = false;
        const std::vector<Key>& keys = KeyboardLayout();
        for (const Key& k : keys)
        {
            bool match = false;
            if (kind == KeyKind::Char)
            {
                if (ch == ' ' && k.kind == KeyKind::Space)
                    match = true;
                else if (k.kind == KeyKind::Char && (k.lower == ch || k.upper == ch))
                {
                    match = true;
                    needsShift = k.upper == ch && k.lower != ch;
                }
            }
            else
                match = k.kind == kind;
            if (match)
            {
                u = (static_cast<float>(k.x) + 0.5f * static_cast<float>(k.w)) / static_cast<float>(kKeyboardWidth);
                v = (static_cast<float>(k.y) + 0.5f * static_cast<float>(k.h)) / static_cast<float>(kKeyboardHeight);
                return true;
            }
        }
        return false;
    }

    class Keyboard
    {
    public:
        enum class Result : uint8_t { None, Typed, Done, Cancelled };

        void Open(const std::string& description, const std::string& existing, uint32_t charMax, bool multiLine, uint64_t userValue)
        {
            m_Active = true;
            m_Description = description;
            m_Text = existing;
            m_CharMax = charMax;
            m_MultiLine = multiLine;
            m_UserValue = userValue;
            m_Shift = false;
            if (m_CharMax && m_Text.size() > m_CharMax)
                m_Text.resize(m_CharMax);
            ++m_Revision;
        }
        void Close() { if (m_Active) { m_Active = false; ++m_Revision; } }

        // Presses key `index`; `typed` receives the character for a
        // KeyboardCharInput event (8 = backspace).
        Result Press(int index, char& typed)
        {
            typed = 0;
            const std::vector<Key>& keys = KeyboardLayout();
            if (!m_Active || index < 0 || index >= static_cast<int>(keys.size()))
                return Result::None;
            const Key& k = keys[static_cast<size_t>(index)];
            ++m_Revision;
            switch (k.kind)
            {
            case KeyKind::Char:
            case KeyKind::Space:
            {
                const char c = (m_Shift && k.kind == KeyKind::Char) ? k.upper : k.lower;
                m_Shift = false;
                if (m_CharMax && m_Text.size() >= m_CharMax)
                    return Result::None;
                m_Text.push_back(c);
                typed = c;
                return Result::Typed;
            }
            case KeyKind::Shift:
                m_Shift = !m_Shift;
                return Result::None;
            case KeyKind::Backspace:
                if (m_Text.empty())
                    return Result::None;
                m_Text.pop_back();
                typed = '\b';
                return Result::Typed;
            case KeyKind::Clear:
                m_Text.clear();
                return Result::None;
            case KeyKind::Cancel:
                m_Active = false;
                return Result::Cancelled;
            case KeyKind::Done:
                m_Active = false;
                return Result::Done;
            }
            return Result::None;
        }

        bool Active() const { return m_Active; }
        bool Shift() const { return m_Shift; }
        const std::string& Text() const { return m_Text; }
        const std::string& Description() const { return m_Description; }
        uint64_t UserValue() const { return m_UserValue; }
        uint64_t Revision() const { return m_Revision; }
        bool MultiLine() const { return m_MultiLine; }

    private:
        bool m_Active = false;
        bool m_Shift = false;
        bool m_MultiLine = false;
        std::string m_Description;
        std::string m_Text;
        uint32_t m_CharMax = 0;
        uint64_t m_UserValue = 0;
        uint64_t m_Revision = 0;
    };
}

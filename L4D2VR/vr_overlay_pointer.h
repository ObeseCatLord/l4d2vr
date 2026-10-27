#pragma once

// Controller laser pointer for the menu/VGUI overlays.
//
// Some OpenVR runtimes (XRizer over OpenXR) do not implement
// IVROverlay::ComputeOverlayIntersection or overlay mouse events, so the menu
// panel is hit-tested here from the transform, width and texture aspect the
// mod itself gave the overlay. Standard library only, so it is unit tested.

#include <cmath>
#include <cstdint>

namespace l4d2vr_overlay_pointer
{
    constexpr float kPi = 3.14159265358979323846f;

    struct Vec3
    {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
    };

    // Row-major 3x4 rigid transform, laid out like vr::HmdMatrix34_t.
    struct Mat34
    {
        float m[3][4] = {};
    };

    inline Vec3 Add(const Vec3& a, const Vec3& b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
    inline Vec3 Sub(const Vec3& a, const Vec3& b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
    inline Vec3 Scale(const Vec3& a, float s) { return { a.x * s, a.y * s, a.z * s }; }
    inline float Dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
    inline Vec3 Cross(const Vec3& a, const Vec3& b)
    {
        return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
    }
    inline float Length(const Vec3& a) { return std::sqrt(Dot(a, a)); }
    inline bool Finite(float v) { return std::isfinite(v); }
    inline bool Finite(const Vec3& v) { return Finite(v.x) && Finite(v.y) && Finite(v.z); }
    inline bool Finite(const Mat34& m)
    {
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 4; ++c)
                if (!Finite(m.m[r][c]))
                    return false;
        return true;
    }

    // Returns false (and leaves out untouched) for a zero or non-finite vector.
    inline bool Normalize(const Vec3& v, Vec3& out)
    {
        const float len = Length(v);
        if (!Finite(len) || len < 1.0e-6f)
            return false;
        out = Scale(v, 1.0f / len);
        return true;
    }

    inline Vec3 Column(const Mat34& m, int c) { return { m.m[0][c], m.m[1][c], m.m[2][c] }; }

    inline Mat34 FromBasis(const Vec3& x, const Vec3& y, const Vec3& z, const Vec3& origin)
    {
        Mat34 out;
        const Vec3 cols[4] = { x, y, z, origin };
        for (int c = 0; c < 4; ++c)
        {
            out.m[0][c] = cols[c].x;
            out.m[1][c] = cols[c].y;
            out.m[2][c] = cols[c].z;
        }
        return out;
    }

    // a * b for 3x4 affine transforms (implicit bottom row 0 0 0 1).
    inline Mat34 Multiply(const Mat34& a, const Mat34& b)
    {
        Mat34 out;
        for (int r = 0; r < 3; ++r)
        {
            for (int c = 0; c < 4; ++c)
            {
                float v = a.m[r][0] * b.m[0][c] + a.m[r][1] * b.m[1][c] + a.m[r][2] * b.m[2][c];
                if (c == 3)
                    v += a.m[r][3];
                out.m[r][c] = v;
            }
        }
        return out;
    }

    struct Ray
    {
        Vec3 origin;
        Vec3 direction; // unit length
    };

    // OpenVR convention: a controller points along -Z of its pose.
    inline bool RayFromPose(const Mat34& pose, Ray& out)
    {
        if (!Finite(pose))
            return false;
        Vec3 dir;
        if (!Normalize(Scale(Column(pose, 2), -1.0f), dir))
            return false;
        out.origin = Column(pose, 3);
        out.direction = dir;
        return true;
    }

    // How the runtime turns (transform, width, texture) into a visible panel.
    enum class RuntimeModel
    {
        // SteamVR compositor: applies the full affine transform, so axis scale
        // stretches the quad. Curved height follows the texture aspect.
        OpenVRCompositor,
        // XRizer: converts the transform to an OpenXR pose (rotation and
        // translation only, scale dropped). Flat layers are width x
        // width*aspect. Curved layers become XR_KHR_composition_layer_cylinder
        // with aspectRatio = texH/texW, which the OpenXR runtime renders as
        // height = arcLength / aspectRatio = width / aspect.
        OpenXRLayer,
    };

    // World-space panel. The OpenVR overlay faces +Z (towards the viewer).
    // For a curved panel the transform position is the centre of the arc and
    // the cylinder axis lies `radius` meters along +Z from it.
    struct Panel
    {
        Vec3 center;
        Vec3 right;  // unit, +U direction
        Vec3 up;     // unit, -V direction (V grows downwards like pixels)
        Vec3 normal; // unit, faces the viewer
        float width = 0.0f;     // meters (arc length when curved)
        float height = 0.0f;    // meters
        float curvature = 0.0f; // OpenVR curvature: fraction of a full circle, 0 = flat
    };

    constexpr float kMinCurvature = 0.001f; // matches L4D2VRSetOverlayCurvature

    // textureAspect = visible texture height / width (after texture bounds).
    inline bool BuildPanel(const Mat34& transform, float widthMeters, float textureAspect,
        float curvature, RuntimeModel model, Panel& out)
    {
        if (!Finite(transform) || !Finite(widthMeters) || !Finite(textureAspect) ||
            widthMeters <= 0.0f || textureAspect <= 0.0f)
            return false;
        if (!Finite(curvature) || curvature < kMinCurvature)
            curvature = 0.0f;
        if (curvature > 1.0f)
            curvature = 1.0f;

        const Vec3 colX = Column(transform, 0);
        const Vec3 colY = Column(transform, 1);
        const Vec3 colZ = Column(transform, 2);

        // Orthonormal frame: keep the normal, then make right perpendicular.
        Vec3 normal;
        if (!Normalize(colZ, normal))
        {
            if (!Normalize(Cross(colX, colY), normal))
                return false;
        }
        Vec3 right;
        if (!Normalize(Sub(colX, Scale(normal, Dot(colX, normal))), right))
            return false;
        const Vec3 up = Cross(normal, right);

        float width = widthMeters;
        float height = widthMeters * textureAspect;
        if (model == RuntimeModel::OpenVRCompositor)
        {
            width *= Length(colX);
            height *= Length(colY);
        }
        else if (curvature > 0.0f)
        {
            height = widthMeters / textureAspect;
        }
        if (!Finite(width) || !Finite(height) || width <= 0.0f || height <= 0.0f)
            return false;

        out.center = Column(transform, 3);
        out.right = right;
        out.up = up;
        out.normal = normal;
        out.width = width;
        out.height = height;
        out.curvature = curvature;
        return true;
    }

    struct Hit
    {
        float u = 0.0f; // 0 = left edge, 1 = right edge
        float v = 0.0f; // 0 = top edge, 1 = bottom edge
        float distance = 0.0f;
        Vec3 point;
        Vec3 normal; // surface normal at the hit, facing the ray origin
    };

    inline bool InRange01(float v) { return v >= 0.0f && v <= 1.0f; }

    inline bool IntersectFlat(const Panel& panel, const Ray& ray, float maxDistance, Hit& out)
    {
        const float denom = Dot(ray.direction, panel.normal);
        if (std::fabs(denom) < 1.0e-6f)
            return false;
        const float t = Dot(Sub(panel.center, ray.origin), panel.normal) / denom;
        if (!Finite(t) || t <= 0.0f || t > maxDistance)
            return false;
        const Vec3 p = Add(ray.origin, Scale(ray.direction, t));
        const Vec3 local = Sub(p, panel.center);
        const float u = Dot(local, panel.right) / panel.width + 0.5f;
        const float v = 0.5f - Dot(local, panel.up) / panel.height;
        if (!InRange01(u) || !InRange01(v))
            return false;
        out.u = u;
        out.v = v;
        out.distance = t;
        out.point = p;
        out.normal = denom < 0.0f ? panel.normal : Scale(panel.normal, -1.0f);
        return true;
    }

    inline bool IntersectCurved(const Panel& panel, const Ray& ray, float maxDistance, Hit& out)
    {
        const float angle = 2.0f * kPi * panel.curvature; // full central angle
        const float radius = panel.width / angle;
        const Vec3 axisPoint = Add(panel.center, Scale(panel.normal, radius));

        // Cylinder frame: x = right, y = up (axis), z = normal. The surface is
        // at (R sin a, y, -R cos a) relative to the axis point.
        const Vec3 rel = Sub(ray.origin, axisPoint);
        const float ox = Dot(rel, panel.right);
        const float oy = Dot(rel, panel.up);
        const float oz = Dot(rel, panel.normal);
        const float dx = Dot(ray.direction, panel.right);
        const float dy = Dot(ray.direction, panel.up);
        const float dz = Dot(ray.direction, panel.normal);

        const float a = dx * dx + dz * dz;
        if (a < 1.0e-9f)
            return false; // parallel to the cylinder axis
        const float b = 2.0f * (ox * dx + oz * dz);
        const float c = ox * ox + oz * oz - radius * radius;
        const float disc = b * b - 4.0f * a * c;
        if (!Finite(disc) || disc < 0.0f)
            return false;
        const float s = std::sqrt(disc);
        const float roots[2] = { (-b - s) / (2.0f * a), (-b + s) / (2.0f * a) };
        for (float t : roots)
        {
            if (!Finite(t) || t <= 0.0f || t > maxDistance)
                continue;
            const float px = ox + dx * t;
            const float py = oy + dy * t;
            const float pz = oz + dz * t;
            const float theta = std::atan2(px, -pz);
            const float u = theta / angle + 0.5f;
            const float v = 0.5f - py / panel.height;
            if (!InRange01(u) || !InRange01(v))
                continue;
            out.u = u;
            out.v = v;
            out.distance = t;
            out.point = Add(ray.origin, Scale(ray.direction, t));
            // Inward normal (towards the axis); flip if the ray hits the outside.
            Vec3 inward = Add(Scale(panel.right, -px / radius), Scale(panel.normal, -pz / radius));
            if (Dot(inward, ray.direction) > 0.0f)
                inward = Scale(inward, -1.0f);
            out.normal = inward;
            return true;
        }
        return false;
    }

    inline bool Intersect(const Panel& panel, const Ray& ray, float maxDistance, Hit& out)
    {
        if (!Finite(ray.origin) || !Finite(ray.direction) || panel.width <= 0.0f || panel.height <= 0.0f)
            return false;
        if (panel.curvature >= kMinCurvature)
            return IntersectCurved(panel, ray, maxDistance, out);
        return IntersectFlat(panel, ray, maxDistance, out);
    }

    struct Pixel
    {
        int x = 0;
        int y = 0;
    };

    // Maps a hit UV (top-left origin) to window pixels, clamped to the window.
    inline Pixel UvToPixel(float u, float v, int width, int height)
    {
        Pixel p;
        if (width <= 0 || height <= 0 || !Finite(u) || !Finite(v))
            return p;
        const auto toPixel = [](float t, int size)
            {
                const float clamped = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
                int px = static_cast<int>(std::floor(clamped * static_cast<float>(size)));
                if (px >= size)
                    px = size - 1;
                return px < 0 ? 0 : px;
            };
        p.x = toPixel(u, width);
        p.y = toPixel(v, height);
        return p;
    }

    // Transform for a small marker quad lying on the panel at the hit point,
    // lifted `lift` meters towards the viewer so it is not coplanar.
    inline bool MarkerTransform(const Panel& panel, const Hit& hit, float lift, Mat34& out)
    {
        Vec3 z;
        if (!Normalize(hit.normal, z))
            return false;
        Vec3 x;
        if (!Normalize(Cross(panel.up, z), x))
            return false;
        const Vec3 y = Cross(z, x);
        out = FromBasis(x, y, z, Add(hit.point, Scale(z, lift)));
        return Finite(out);
    }

    // Transform for a beam quad from `start` to `end`: +X runs along the beam
    // (the quad width is the beam length) and the face turns towards `viewer`.
    inline bool BeamTransform(const Vec3& start, const Vec3& end, const Vec3& viewer,
        Mat34& out, float& lengthOut)
    {
        const Vec3 span = Sub(end, start);
        const float length = Length(span);
        Vec3 x;
        if (!Normalize(span, x))
            return false;
        const Vec3 mid = Add(start, Scale(span, 0.5f));
        const Vec3 toViewer = Sub(viewer, mid);
        Vec3 z;
        if (!Normalize(Sub(toViewer, Scale(x, Dot(toViewer, x))), z))
        {
            // Viewer on the beam line: any perpendicular will do.
            const Vec3 helper = std::fabs(x.y) < 0.9f ? Vec3{ 0.0f, 1.0f, 0.0f } : Vec3{ 1.0f, 0.0f, 0.0f };
            if (!Normalize(Cross(x, helper), z))
                return false;
        }
        const Vec3 y = Cross(z, x);
        out = FromBasis(x, y, z, mid);
        lengthOut = length;
        return Finite(out);
    }

    enum class ClickAction
    {
        None,
        Press,
        Release,
    };

    constexpr int kLeft = 0;
    constexpr int kRight = 1;

    // Which hand drives the cursor and when the left button goes down/up.
    // Pressing a trigger on a hand whose ray is on the panel makes it the
    // pointer hand and presses the button; releasing that trigger releases
    // it, so click-and-drag works. A trigger already held when the pointer
    // appears must be released first.
    struct PointerTracker
    {
        bool prevTrigger[2] = { false, false };
        bool buttonDown = false;
        int downHand = -1;
        int preferredHand = -1;
    };

    struct PointerFrame
    {
        bool hits[2] = { false, false };
        bool trigger[2] = { false, false };
    };

    struct PointerDecision
    {
        int hand = -1; // hand whose ray drives the cursor, -1 = none
        ClickAction action = ClickAction::None;
    };

    inline void ResetTracker(PointerTracker& tracker, const bool triggerHeld[2])
    {
        const int preferred = tracker.preferredHand;
        tracker = PointerTracker{};
        tracker.preferredHand = preferred;
        tracker.prevTrigger[kLeft] = triggerHeld[kLeft];
        tracker.prevTrigger[kRight] = triggerHeld[kRight];
    }

    inline int SelectHand(const bool hits[2], int preferredHand)
    {
        if (preferredHand >= 0 && preferredHand < 2 && hits[preferredHand])
            return preferredHand;
        if (hits[kRight])
            return kRight;
        if (hits[kLeft])
            return kLeft;
        return -1;
    }

    inline PointerDecision StepTracker(PointerTracker& tracker, const PointerFrame& frame)
    {
        PointerDecision decision;
        bool pressed[2];
        for (int h = 0; h < 2; ++h)
            pressed[h] = frame.trigger[h] && !tracker.prevTrigger[h];

        if (tracker.buttonDown)
        {
            decision.hand = tracker.downHand;
            if (!frame.trigger[tracker.downHand])
            {
                decision.action = ClickAction::Release;
                tracker.buttonDown = false;
                tracker.downHand = -1;
            }
        }
        else
        {
            // Check the preferred hand first when both triggers go down together.
            const int first = tracker.preferredHand == kLeft ? kLeft : kRight;
            const int order[2] = { first, 1 - first };
            for (int h : order)
            {
                if (pressed[h] && frame.hits[h])
                {
                    tracker.preferredHand = h;
                    tracker.buttonDown = true;
                    tracker.downHand = h;
                    decision.action = ClickAction::Press;
                    break;
                }
            }
            decision.hand = tracker.buttonDown ? tracker.downHand : SelectHand(frame.hits, tracker.preferredHand);
        }

        tracker.prevTrigger[kLeft] = frame.trigger[kLeft];
        tracker.prevTrigger[kRight] = frame.trigger[kRight];
        return decision;
    }
}

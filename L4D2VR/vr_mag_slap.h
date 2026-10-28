#pragma once
#include <algorithm>
#include <cmath>

// Pavlov VR magazine slap (VRMagazine overlap handler 0x140beb420): while a
// magazine is seated, a held magazine of the same kind that strikes it fast
// enough, moving along the seated magazine's axis, knocks it out of the well.
// Pavlov measures the held magazine's velocity minus its owner's, so walking
// and turning never count; here the strike is the off hand's tracked velocity
// minus the gun hand's. Std-only so the tests can include it.
namespace l4d2vr_mag_slap
{
    struct Vec3
    {
        float x = 0.0f, y = 0.0f, z = 0.0f;
    };

    inline Vec3 Add(const Vec3& a, const Vec3& b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
    inline Vec3 Sub(const Vec3& a, const Vec3& b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
    inline Vec3 Scale(const Vec3& a, float s) { return { a.x * s, a.y * s, a.z * s }; }
    inline float Dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
    inline float Length(const Vec3& a) { return std::sqrt(Dot(a, a)); }
    inline bool Finite(const Vec3& a) { return std::isfinite(a.x) && std::isfinite(a.y) && std::isfinite(a.z); }

    inline bool Normalize(const Vec3& a, Vec3& out)
    {
        const float length = Length(a);
        if (!Finite(a) || !std::isfinite(length) || length < 0.0001f)
            return false;
        out = Scale(a, 1.0f / length);
        return true;
    }

    // An oriented box: unit axes and local bounds along them (the published
    // magazine box layout).
    struct Box
    {
        Vec3 origin{};
        Vec3 axisX{ 1.0f, 0.0f, 0.0f };
        Vec3 axisY{ 0.0f, 1.0f, 0.0f };
        Vec3 axisZ{ 0.0f, 0.0f, 1.0f };
        Vec3 mins{};
        Vec3 maxs{};
    };

    inline bool ValidBox(const Box& box)
    {
        return Finite(box.origin) && Finite(box.axisX) && Finite(box.axisY) && Finite(box.axisZ) &&
            Finite(box.mins) && Finite(box.maxs) && box.mins.x <= box.maxs.x && box.mins.y <= box.maxs.y &&
            box.mins.z <= box.maxs.z && Length(box.axisX) > 0.0001f && Length(box.axisY) > 0.0001f &&
            Length(box.axisZ) > 0.0001f;
    }

    inline Vec3 LocalToWorld(const Box& box, const Vec3& local)
    {
        return Add(box.origin, Add(Scale(box.axisX, local.x), Add(Scale(box.axisY, local.y), Scale(box.axisZ, local.z))));
    }

    inline Vec3 LocalCenter(const Box& box)
    {
        return Scale(Add(box.mins, box.maxs), 0.5f);
    }

    inline Vec3 Center(const Box& box)
    {
        return LocalToWorld(box, LocalCenter(box));
    }

    inline Box Translated(Box box, const Vec3& offset)
    {
        box.origin = Add(box.origin, offset);
        return box;
    }

    // Distance from a world point to the box (0 inside).
    inline float PointDistance(const Box& box, const Vec3& point)
    {
        if (!ValidBox(box) || !Finite(point))
            return INFINITY;
        const Vec3 delta = Sub(point, box.origin);
        const Vec3 local{ Dot(delta, box.axisX), Dot(delta, box.axisY), Dot(delta, box.axisZ) };
        const Vec3 outside{ local.x - std::clamp(local.x, box.mins.x, box.maxs.x),
            local.y - std::clamp(local.y, box.mins.y, box.maxs.y),
            local.z - std::clamp(local.z, box.mins.z, box.maxs.z) };
        return Length(outside);
    }

    // Smallest distance from the 27 lattice points (corners, edge and face
    // centres, centre) of either box to the other box: 0 when one holds a
    // lattice point of the other, otherwise about how far apart they are. Two
    // thin boxes crossing between lattice points read a small positive gap,
    // which the few-centimetre contact distance absorbs.
    inline float Gap(const Box& a, const Box& b)
    {
        if (!ValidBox(a) || !ValidBox(b))
            return INFINITY;
        float best = INFINITY;
        const Box* boxes[2] = { &a, &b };
        for (int pass = 0; pass < 2; ++pass)
        {
            const Box& from = *boxes[pass];
            const Box& to = *boxes[1 - pass];
            for (int ix = 0; ix < 3; ++ix)
                for (int iy = 0; iy < 3; ++iy)
                    for (int iz = 0; iz < 3; ++iz)
                    {
                        const Vec3 local{ from.mins.x + (from.maxs.x - from.mins.x) * 0.5f * static_cast<float>(ix),
                            from.mins.y + (from.maxs.y - from.mins.y) * 0.5f * static_cast<float>(iy),
                            from.mins.z + (from.maxs.z - from.mins.z) * 0.5f * static_cast<float>(iz) };
                        best = std::min(best, PointDistance(to, LocalToWorld(from, local)));
                        if (best <= 0.0f)
                            return 0.0f;
                    }
        }
        return best;
    }

    // The seated magazine's outward axis in world space: its insertion axis
    // (box local; L4D2 clip bones hang the magazine along -Y from the bone at the
    // top of the well) signed so it points from the well toward the magazine's
    // base, the way a magazine leaves the gun. For the AK-47 profile (y from
    // -9.53 to 1.34) that is the clip bone's -Y, straight down out of the well;
    // the forward-and-down strike of a real AK mag slap is 45 degrees off it.
    inline bool OutwardAxis(const Box& seated, const Vec3& insertionAxisLocal, Vec3& out)
    {
        Vec3 axisLocal{};
        if (!ValidBox(seated) || !Normalize(insertionAxisLocal, axisLocal))
            return false;
        if (Dot(LocalCenter(seated), axisLocal) < 0.0f)
            axisLocal = Scale(axisLocal, -1.0f);
        const Vec3 world = Add(Scale(seated.axisX, axisLocal.x),
            Add(Scale(seated.axisY, axisLocal.y), Scale(seated.axisZ, axisLocal.z)));
        return Normalize(world, out);
    }

    // Tracking space to world axes: the stick-turn offset rotates about up.
    inline Vec3 RotateYaw(const Vec3& v, float yawDeg)
    {
        const float radians = yawDeg * 3.14159265358979323846f / 180.0f;
        const float s = std::sin(radians), c = std::cos(radians);
        return { v.x * c - v.y * s, v.x * s + v.y * c, v.z };
    }

    // Strike velocity in world axes (m/s): the held magazine's hand relative to
    // the gun's hand. Both come from tracking space, so locomotion never enters.
    inline Vec3 RelativeVelocity(const Vec3& supportHandTracking, const Vec3& gunHandTracking, float yawDeg)
    {
        return RotateYaw(Sub(supportHandTracking, gunHandTracking), yawDeg);
    }

    constexpr float kContactMeters = 0.03f; // boxes this close count as touching

    struct Params
    {
        float speedMetersPerSecond = 0.9f;  // Pavlov SlapVelocityThreshold 90 cm/s
        float forwardDot = 0.68f;           // Pavlov SlapForwardThreshold
        float reinsertSeconds = 0.15f;      // Pavlov SlapReinsertTime
        float contactMeters = kContactMeters;
    };

    struct Result
    {
        bool hit = false;
        bool contact = false;
        float speed = 0.0f;
        float dot = -1.0f;
    };

    // Pavlov rejects a strike slower than the threshold or whose direction is
    // further than the dot threshold from the seated magazine's axis.
    inline Result Evaluate(const Params& params, const Vec3& relativeVelocity, const Vec3& outwardAxis, float gapMeters)
    {
        Result result{};
        result.contact = std::isfinite(gapMeters) && gapMeters <= params.contactMeters;
        result.speed = Finite(relativeVelocity) ? Length(relativeVelocity) : 0.0f;
        Vec3 direction{}, outward{};
        if (result.speed > 0.0001f && Normalize(relativeVelocity, direction) && Normalize(outwardAxis, outward))
            result.dot = Dot(direction, outward);
        result.hit = result.contact && result.speed >= params.speedMetersPerSecond && result.dot >= params.forwardDot;
        return result;
    }

    // The new magazine may go in SlapReinsertTime after the slap (negative: no slap).
    inline bool MayReinsert(float secondsSinceSlap, float reinsertSeconds)
    {
        return !(secondsSinceSlap >= 0.0f && secondsSinceSlap < reinsertSeconds);
    }
}

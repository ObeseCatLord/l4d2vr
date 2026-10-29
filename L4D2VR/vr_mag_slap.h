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

    // ---- Physical slap (what the game uses) ---------------------------------
    // The held fresh magazine and the seated one are oriented boxes from their
    // rendered bones. Every frame the held box is swept from where it was last
    // frame (relative to the seated one, so the gun's own motion never counts)
    // to where it is now, in small steps, so a fast strike cannot tunnel
    // through a 3 cm magazine between frames. The first touch gives a contact
    // normal (the face that was hit) and the strike's velocity relative to the
    // gun. The seated magazine can only leave its well along its release
    // directions (out of the well, rocked toward the muzzle, or between), so
    // the strike counts by the part of its impulse along those: the approach
    // speed into the face pushes along the face's inward normal, and friction
    // (up to kFriction of that) drags along the face. A slap is that release
    // speed reaching Pavlov's threshold (speed x dot, 80 % of it). A glancing
    // touch, a push toward the stock or into the well, or a slow push never
    // ejects; touching below the threshold keeps the held magazine on the
    // seated one's surface instead of passing through it.
    struct PhysicsParams
    {
        float releaseSpeedMetersPerSecond = 0.49f; // 0.9 m/s x 0.68 x 0.8
        float friction = 0.5f;                     // magazine on magazine
        float contactMeters = 0.015f;              // boxes this close are in contact
        float maxIntoWellDot = 0.35f;              // strikes this much into the well are inserts
        float maxSpeedMetersPerSecond = 6.0f;      // faster is a tracking jump, not a hand
        float lagSeconds = 0.1f;                   // a strike sample may be this old (velocity lag)
        int sweepSteps = 24;
    };

    // Pavlov's gate (|v| >= speed and dot >= forwardDot) needs at least
    // speed x dot along the magazine; 80 % of that is the release speed.
    inline PhysicsParams Physical(const Params& pavlov)
    {
        PhysicsParams out{};
        out.releaseSpeedMetersPerSecond = pavlov.speedMetersPerSecond * pavlov.forwardDot * 0.8f;
        return out;
    }

    // Separating-axis query: the axis along which the boxes are furthest apart
    // (separation > 0: apart by that much; <= 0: overlapping that deep), its
    // direction from a toward b. Works in any unit.
    struct SatResult
    {
        bool valid = false;
        float separation = 0.0f;
        Vec3 axis{};
    };

    inline SatResult SatQuery(const Box& a, const Box& b)
    {
        SatResult out{};
        if (!ValidBox(a) || !ValidBox(b))
            return out;
        Vec3 ax[3], bx[3];
        float ah[3], bh[3];
        const Vec3* aAxes[3] = { &a.axisX, &a.axisY, &a.axisZ };
        const Vec3* bAxes[3] = { &b.axisX, &b.axisY, &b.axisZ };
        const float aMin[3] = { a.mins.x, a.mins.y, a.mins.z }, aMax[3] = { a.maxs.x, a.maxs.y, a.maxs.z };
        const float bMin[3] = { b.mins.x, b.mins.y, b.mins.z }, bMax[3] = { b.maxs.x, b.maxs.y, b.maxs.z };
        for (int i = 0; i < 3; ++i)
        {
            const float la = Length(*aAxes[i]), lb = Length(*bAxes[i]);
            ax[i] = Scale(*aAxes[i], 1.0f / la);
            bx[i] = Scale(*bAxes[i], 1.0f / lb);
            ah[i] = 0.5f * (aMax[i] - aMin[i]) * la;
            bh[i] = 0.5f * (bMax[i] - bMin[i]) * lb;
        }
        const Vec3 d = Sub(Center(b), Center(a));
        float best = -INFINITY;
        auto test = [&](const Vec3& raw, bool face)
        {
            Vec3 axis{};
            if (!Normalize(raw, axis))
                return;
            float ra = 0.0f, rb = 0.0f;
            for (int i = 0; i < 3; ++i)
            {
                ra += std::fabs(Dot(ax[i], axis)) * ah[i];
                rb += std::fabs(Dot(bx[i], axis)) * bh[i];
            }
            const float along = Dot(d, axis);
            float separation = std::fabs(along) - (ra + rb);
            // Prefer a face over an edge pair at (nearly) the same distance:
            // its normal is the one a hand sees being hit.
            if (!face)
                separation -= 1e-3f * (ra + rb);
            if (separation > best)
            {
                best = separation;
                out.axis = along >= 0.0f ? axis : Scale(axis, -1.0f);
            }
        };
        for (int i = 0; i < 3; ++i)
        {
            test(ax[i], true);
            test(bx[i], true);
        }
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
            {
                const Vec3 c{ ax[i].y * bx[j].z - ax[i].z * bx[j].y, ax[i].z * bx[j].x - ax[i].x * bx[j].z,
                    ax[i].x * bx[j].y - ax[i].y * bx[j].x };
                if (Length(c) > 1e-3f)
                    test(c, false);
            }
        out.valid = std::isfinite(best);
        out.separation = best;
        return out;
    }

    // The held box's origin in the seated box's frame, so a sweep follows the
    // gun when both hands move.
    inline Vec3 RelativeOrigin(const Box& seated, const Box& held)
    {
        const Vec3 d = Sub(held.origin, seated.origin);
        return { Dot(d, seated.axisX), Dot(d, seated.axisY), Dot(d, seated.axisZ) };
    }

    inline Box AtRelativeOrigin(const Box& seated, Box held, const Vec3& relative)
    {
        held.origin = Add(seated.origin, Add(Scale(seated.axisX, relative.x),
            Add(Scale(seated.axisY, relative.y), Scale(seated.axisZ, relative.z))));
        return held;
    }

    // First contact along the straight path from the previous relative origin
    // to the current pose. started: in contact already at the start.
    struct SweepResult
    {
        bool hit = false;
        bool started = false;
        float fraction = 0.0f;
        Vec3 normal{};   // from the seated box toward the held one
    };

    inline SweepResult Sweep(const Box& seated, const Box& held, const Vec3* previousRelativeOrigin,
        float contactUnits, int steps)
    {
        SweepResult out{};
        const Vec3 now = RelativeOrigin(seated, held);
        const Vec3 from = previousRelativeOrigin && Finite(*previousRelativeOrigin) ? *previousRelativeOrigin : now;
        SatResult before = SatQuery(seated, AtRelativeOrigin(seated, held, from));
        if (!before.valid)
            return out;
        if (before.separation <= contactUnits)
        {
            out.started = true;
            out.normal = before.axis;
            return out;
        }
        steps = std::max(1, steps);
        for (int k = 1; k <= steps; ++k)
        {
            const float u = static_cast<float>(k) / static_cast<float>(steps);
            const SatResult at = SatQuery(seated, AtRelativeOrigin(seated, held, Add(from, Scale(Sub(now, from), u))));
            if (!at.valid)
                return out;
            if (at.separation <= contactUnits)
            {
                out.hit = true;
                out.fraction = u;
                out.normal = before.axis;   // the face that was still apart a step ago
                return out;
            }
            before = at;
        }
        return out;
    }

    // The release directions of a seated magazine: out of the well, rocked
    // toward the muzzle (gun forward square to the magazine), and halfway.
    inline int ReleaseDirections(const Vec3& outwardAxis, const Vec3& gunForward, Vec3 out[3])
    {
        int count = 0;
        Vec3 outward{};
        if (!Normalize(outwardAxis, outward))
            return 0;
        out[count++] = outward;
        Vec3 forward{};
        if (Finite(gunForward) && Normalize(Sub(gunForward, Scale(outward, Dot(gunForward, outward))), forward))
        {
            out[count++] = forward;
            Vec3 between{};
            if (Normalize(Add(outward, forward), between))
                out[count++] = between;
        }
        return count;
    }

    // What one strike velocity does to the seated magazine through a contact
    // with this normal (from seated toward held): m/s.
    struct Impulse
    {
        float approach = 0.0f;   // closing speed into the face
        float release = 0.0f;    // speed it gives the seated magazine along its best release direction
        float intoWell = 0.0f;   // the strike's direction along the insert axis
        Vec3 releaseDirection{};
    };

    inline Impulse StrikeImpulse(const PhysicsParams& p, const Vec3& velocity, const Vec3& normal,
        const Vec3& outwardAxis, const Vec3& gunForward)
    {
        Impulse out{};
        Vec3 n{}, outward{};
        if (!Finite(velocity) || !Normalize(normal, n) || !Normalize(outwardAxis, outward))
            return out;
        const float speed = Length(velocity);
        if (speed > 1e-4f)
            out.intoWell = -Dot(velocity, outward) / speed;
        out.approach = std::max(0.0f, -Dot(velocity, n));
        if (out.approach <= 0.0f)
            return out;
        const Vec3 tangential = Sub(velocity, Scale(n, Dot(velocity, n)));
        Vec3 dirs[3];
        const int count = ReleaseDirections(outward, gunForward, dirs);
        for (int i = 0; i < count; ++i)
        {
            const float push = out.approach * std::max(0.0f, -Dot(n, dirs[i]));
            const float drag = std::min(p.friction * out.approach, std::max(0.0f, Dot(tangential, dirs[i])));
            if (push + drag > out.release)
            {
                out.release = push + drag;
                out.releaseDirection = dirs[i];
            }
        }
        return out;
    }

    // Feed every frame while a fresh magazine is held for a slap.
    class Collider
    {
    public:
        struct Outcome
        {
            bool hit = false;          // slap: knock the seated magazine out
            bool contactBegan = false; // a new touch this frame (hit or not)
            bool touching = false;     // in contact now (hit or not)
            Impulse impulse{};         // the strongest strike sample at the touch
            Vec3 normal{};             // contact normal, seated toward held
            Vec3 clampOffset{};        // add to the held box so it rests on the surface (units)
        };

        void Reset()
        {
            m_InContact = false;
            m_Embedded = false;
            m_ContactFree = false;
            m_Count = 0;
            m_Head = 0;
            m_HavePrevious = false;
        }

        // velocities: strike estimates for this frame (m/s, world, held relative
        // to the gun); unitsPerMeter converts box units.
        Outcome Update(const PhysicsParams& p, float time, const Box& seated, const Box& held,
            const Vec3* velocities, int velocityCount, const Vec3& outwardAxis, const Vec3& gunForward,
            float unitsPerMeter)
        {
            Outcome o{};
            for (int i = 0; i < velocityCount; ++i)
                Push(time, velocities[i]);
            const float contactUnits = p.contactMeters * unitsPerMeter;
            const SweepResult sweep = Sweep(seated, held, m_HavePrevious ? &m_PreviousRelative : nullptr,
                contactUnits, p.sweepSteps);
            m_PreviousRelative = RelativeOrigin(seated, held);
            m_HavePrevious = Finite(m_PreviousRelative);
            const SatResult now = SatQuery(seated, held);
            if (m_Embedded)
            {
                if (now.valid && now.separation > contactUnits)
                {
                    m_Embedded = false;
                    m_ContactFree = true;
                }
                return o;
            }
            auto keepNormal = [&](const Vec3& normal)
            {
                m_InContact = true;
                m_NormalLocal = { Dot(normal, Unit(seated.axisX)), Dot(normal, Unit(seated.axisY)),
                    Dot(normal, Unit(seated.axisZ)) };
            };
            if (!m_InContact && sweep.started)
            {
                // Taken already overlapping the old magazine: nothing until it
                // has been apart once. Back within reach of it: resting contact.
                if (!m_ContactFree)
                {
                    m_Embedded = true;
                    return o;
                }
                keepNormal(sweep.normal);
            }
            else if (!m_InContact)
            {
                if (!sweep.hit)
                {
                    m_ContactFree = true;
                    return o;
                }
                o.contactBegan = true;
                o.normal = sweep.normal;
                for (int i = 0; i < m_Count; ++i)
                {
                    const Sample& s = m_Samples[(m_Head + kCapacity - 1 - i) % kCapacity];
                    if (s.time < time - p.lagSeconds - 1e-4f)
                        break;
                    const float speed = Length(s.velocity);
                    if (!std::isfinite(speed) || speed > p.maxSpeedMetersPerSecond)
                        continue;
                    const Impulse impulse = StrikeImpulse(p, s.velocity, sweep.normal, outwardAxis, gunForward);
                    if (impulse.release > o.impulse.release || o.impulse.approach <= 0.0f)
                        o.impulse = impulse;
                }
                o.hit = o.impulse.release >= p.releaseSpeedMetersPerSecond &&
                    o.impulse.intoWell <= p.maxIntoWellDot;
                if (o.hit)
                    return o;
                keepNormal(sweep.normal);
            }
            // Resting on the surface: in contact until the hand draws back.
            if (now.valid && now.separation > contactUnits * 2.0f)
            {
                m_InContact = false;
                return o;
            }
            o.touching = true;
            o.normal = Add(Scale(Unit(seated.axisX), m_NormalLocal.x),
                Add(Scale(Unit(seated.axisY), m_NormalLocal.y), Scale(Unit(seated.axisZ), m_NormalLocal.z)));
            const float depth = PenetrationAlong(seated, held, o.normal);
            if (depth > 0.0f)
                o.clampOffset = Scale(o.normal, depth);
            return o;
        }

        // How far b reaches into a along unit axis n (from a toward b), 0 when apart.
        static float PenetrationAlong(const Box& a, const Box& b, const Vec3& n)
        {
            const Vec3* aAxes[3] = { &a.axisX, &a.axisY, &a.axisZ };
            const Vec3* bAxes[3] = { &b.axisX, &b.axisY, &b.axisZ };
            const float aExt[3] = { a.maxs.x - a.mins.x, a.maxs.y - a.mins.y, a.maxs.z - a.mins.z };
            const float bExt[3] = { b.maxs.x - b.mins.x, b.maxs.y - b.mins.y, b.maxs.z - b.mins.z };
            float ra = 0.0f, rb = 0.0f;
            for (int i = 0; i < 3; ++i)
            {
                ra += std::fabs(Dot(*aAxes[i], n)) * 0.5f * aExt[i];
                rb += std::fabs(Dot(*bAxes[i], n)) * 0.5f * bExt[i];
            }
            const float depth = ra + rb - Dot(Sub(Center(b), Center(a)), n);
            return std::isfinite(depth) ? std::max(0.0f, depth) : 0.0f;
        }

    private:
        struct Sample
        {
            float time = 0.0f;
            Vec3 velocity{};
        };
        static constexpr int kCapacity = 32;
        static Vec3 Unit(const Vec3& v)
        {
            Vec3 out{};
            return Normalize(v, out) ? out : v;
        }
        void Push(float time, const Vec3& velocity)
        {
            m_Samples[m_Head] = { time, velocity };
            m_Head = (m_Head + 1) % kCapacity;
            if (m_Count < kCapacity)
                ++m_Count;
        }
        Sample m_Samples[kCapacity]{};
        int m_Count = 0;
        int m_Head = 0;
        bool m_InContact = false;
        bool m_Embedded = false;
        bool m_ContactFree = false;
        bool m_HavePrevious = false;
        Vec3 m_PreviousRelative{};
        Vec3 m_NormalLocal{};
    };

    // Letting go must last this long before the spare goes back, so a hand that
    // relaxes for a moment during the swing keeps it.
    constexpr float kReleaseGraceSeconds = 0.12f;
}

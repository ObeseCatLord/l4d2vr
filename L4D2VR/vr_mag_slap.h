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

    // ---- Forgiving slap (what the game uses) --------------------------------
    // Pavlov tests the held magazine's physics velocity at the instant its
    // collision starts overlapping the seated one. Our hand is a tracked point
    // and one game frame can be 1-3 cm of a fast swing, so a strict
    // same-frame test missed most real slaps (headset log 2026-10-28: the user
    // held the spare for 31 s of strikes before one counted; every slap that
    // counted was within 0.05 m/s or 0.04 of a threshold). Here:
    //  - contact is the held box overlapping the seated one, swept along the
    //    path since the last frame, so a fast pass cannot skip through between
    //    frames. A touch counts when the boxes come within a few centimetres
    //    or when they start to overlap, so a spare held close for a short
    //    wind-up still slaps;
    //  - the strike is the fastest qualifying sample from up to `window`
    //    seconds before the touch (the hand slows as it lands); samples faster
    //    than any hand (tracking jumps) never count;
    //  - a slap may run along the magazine out of the well (straight down on a
    //    level gun) or knock it toward the muzzle (how an AK magazine rocks
    //    out, and what Pavlov's magazine forward axis means), or anything in
    //    between; moving into the well never counts, so a slow or a fast
    //    insert can never knock the old magazine out.
    struct ForgivingParams
    {
        float speedMetersPerSecond = 0.72f; // 80 % of Pavlov's 90 cm/s
        float dot = 0.58f;                  // Pavlov's forward dot less 0.1
        float windowSeconds = 0.15f;        // strike up to this long before the touch
        float lateSeconds = 0.06f;          // or this long after it began (velocity lag)
        float contactMeters = 0.04f;        // boxes this close touch
        float overlapMeters = 0.005f;       // ... and this close overlap
        float maxIntoWellDot = 0.35f;       // more than this along the insert axis is an insert
        float maxSpeedMetersPerSecond = 6.0f; // faster is a tracking jump, not a hand
    };

    inline ForgivingParams Forgiving(const Params& pavlov)
    {
        ForgivingParams out{};
        out.speedMetersPerSecond = pavlov.speedMetersPerSecond * 0.8f;
        out.dot = pavlov.forwardDot - 0.1f;
        return out;
    }

    // How well a strike direction matches a slap: the best of out of the well,
    // toward the muzzle (gun forward, square to the magazine) and halfway.
    // intoWell is its component along the insert direction.
    struct DirectionMatch
    {
        float dot = -1.0f;
        float intoWell = 0.0f;
    };

    inline DirectionMatch MatchDirection(const Vec3& strike, const Vec3& outwardAxis, const Vec3& gunForward)
    {
        DirectionMatch m{};
        Vec3 dir{}, out{};
        if (!Normalize(strike, dir) || !Normalize(outwardAxis, out))
            return m;
        m.intoWell = -Dot(dir, out);
        m.dot = Dot(dir, out);
        Vec3 forward{};
        if (Finite(gunForward) && Normalize(Sub(gunForward, Scale(out, Dot(gunForward, out))), forward))
        {
            Vec3 between{};
            m.dot = std::max(m.dot, Dot(dir, forward));
            if (Normalize(Add(out, forward), between))
                m.dot = std::max(m.dot, Dot(dir, between));
        }
        return m;
    }

    inline bool StrikeQualifies(const ForgivingParams& p, float speed, const DirectionMatch& m)
    {
        return std::isfinite(speed) && speed >= p.speedMetersPerSecond && speed <= p.maxSpeedMetersPerSecond &&
            m.dot >= p.dot && m.intoWell <= p.maxIntoWellDot;
    }

    // Oriented box overlap (separating axis test) with `margin` added to a's
    // half extents. Works in any unit.
    inline bool OverlapWithMargin(const Box& a, const Box& b, float margin)
    {
        if (!ValidBox(a) || !ValidBox(b) || !std::isfinite(margin))
            return false;
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
            ah[i] = 0.5f * (aMax[i] - aMin[i]) * la + std::max(0.0f, margin);
            bh[i] = 0.5f * (bMax[i] - bMin[i]) * lb;
        }
        const Vec3 d = Sub(Center(b), Center(a));
        float r[3][3], absR[3][3];
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
            {
                r[i][j] = Dot(ax[i], bx[j]);
                absR[i][j] = std::fabs(r[i][j]) + 1e-5f;
            }
        const float t[3] = { Dot(d, ax[0]), Dot(d, ax[1]), Dot(d, ax[2]) };
        for (int i = 0; i < 3; ++i)
            if (std::fabs(t[i]) > ah[i] + bh[0] * absR[i][0] + bh[1] * absR[i][1] + bh[2] * absR[i][2])
                return false;
        for (int j = 0; j < 3; ++j)
            if (std::fabs(t[0] * r[0][j] + t[1] * r[1][j] + t[2] * r[2][j]) >
                ah[0] * absR[0][j] + ah[1] * absR[1][j] + ah[2] * absR[2][j] + bh[j])
                return false;
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
            {
                const int i1 = (i + 1) % 3, i2 = (i + 2) % 3, j1 = (j + 1) % 3, j2 = (j + 2) % 3;
                const float ra = ah[i1] * absR[i2][j] + ah[i2] * absR[i1][j];
                const float rb = bh[j1] * absR[i][j2] + bh[j2] * absR[i][j1];
                if (std::fabs(t[i2] * r[i1][j] - t[i1] * r[i2][j]) > ra + rb)
                    return false;
            }
        return true;
    }

    // The held box's origin in the seated box's frame, so a sweep follows the
    // gun when both hands move.
    inline Vec3 RelativeOrigin(const Box& seated, const Box& held)
    {
        const Vec3 d = Sub(held.origin, seated.origin);
        return { Dot(d, seated.axisX), Dot(d, seated.axisY), Dot(d, seated.axisZ) };
    }

    // Touching now, or anywhere on the straight path from where the held box
    // was last frame (relative to the seated one).
    inline bool TouchingSwept(const Box& seated, const Box& held, const Vec3* previousRelativeOrigin,
        float marginUnits, int steps = 6)
    {
        if (OverlapWithMargin(seated, held, marginUnits))
            return true;
        if (!previousRelativeOrigin || !Finite(*previousRelativeOrigin))
            return false;
        const Vec3 now = RelativeOrigin(seated, held);
        for (int k = 0; k < steps; ++k)
        {
            const float u = static_cast<float>(k) / static_cast<float>(steps);
            const Vec3 local = Add(*previousRelativeOrigin, Scale(Sub(now, *previousRelativeOrigin), u));
            Box at = held;
            at.origin = Add(seated.origin, Add(Scale(seated.axisX, local.x),
                Add(Scale(seated.axisY, local.y), Scale(seated.axisZ, local.z))));
            if (OverlapWithMargin(seated, at, marginUnits))
                return true;
        }
        return false;
    }

    // Frame-by-frame slap detector: feed every frame while a fresh magazine is
    // held for a slap.
    class Detector
    {
    public:
        struct Sample
        {
            float time = 0.0f;
            float speed = 0.0f;
            DirectionMatch direction{};
            bool qualifies = false;
        };
        struct Outcome
        {
            bool hit = false;
            bool touchBegan = false;   // a new touch this frame
            bool nearMiss = false;     // a touch began with no qualifying strike
            Sample strike{};           // the qualifying strike (hit) or the fastest recent one
        };

        void Reset()
        {
            m_Count = 0;
            m_Head = 0;
            m_WasTouching = false;
            m_WasOverlapping = false;
            m_TouchBeganAt = -1e9f;
        }

        // strikes: one or more velocity estimates for this frame (m/s, world).
        // touching: within contactMeters; overlapping: within overlapMeters.
        Outcome Update(const ForgivingParams& p, float time, const Vec3* strikes, int strikeCount,
            const Vec3& outwardAxis, const Vec3& gunForward, bool touching, bool overlapping = false)
        {
            Outcome o{};
            Sample best{};
            best.time = time;
            for (int i = 0; i < strikeCount; ++i)
            {
                Sample s{};
                s.time = time;
                s.speed = Finite(strikes[i]) ? Length(strikes[i]) : 0.0f;
                s.direction = MatchDirection(strikes[i], outwardAxis, gunForward);
                s.qualifies = StrikeQualifies(p, s.speed, s.direction);
                if (i == 0 || (s.qualifies && !best.qualifies) ||
                    (s.qualifies == best.qualifies && s.speed > best.speed))
                    best = s;
            }
            Push(best);
            overlapping = overlapping && touching;
            o.touchBegan = touching && !m_WasTouching;
            const bool overlapBegan = overlapping && !m_WasOverlapping;
            m_WasTouching = touching;
            m_WasOverlapping = overlapping;
            const bool began = o.touchBegan || overlapBegan;
            if (began)
                m_TouchBeganAt = time;
            // A touch just began: any qualifying strike within the window before it.
            // A qualifying strike now: a touch that began a moment ago.
            const bool touchRecent = touching && time - m_TouchBeganAt <= p.lateSeconds + 1e-4f;
            if (began || (touchRecent && best.qualifies))
            {
                const Sample* found = nullptr;
                const Sample* fastest = nullptr;
                for (int i = 0; i < m_Count; ++i)
                {
                    const Sample& s = m_Samples[(m_Head + kCapacity - 1 - i) % kCapacity];
                    if (s.time < m_TouchBeganAt - p.windowSeconds - 1e-4f)
                        break;
                    if (!fastest || s.speed > fastest->speed)
                        fastest = &s;
                    if (s.qualifies && (!found || s.speed > found->speed))
                        found = &s;
                }
                if (found)
                {
                    o.hit = true;
                    o.strike = *found;
                }
                else if (began)
                {
                    o.nearMiss = true;
                    if (fastest)
                        o.strike = *fastest;
                }
            }
            else
            {
                o.strike = best;
            }
            return o;
        }

    private:
        static constexpr int kCapacity = 48;
        void Push(const Sample& s)
        {
            m_Samples[m_Head] = s;
            m_Head = (m_Head + 1) % kCapacity;
            if (m_Count < kCapacity)
                ++m_Count;
        }
        Sample m_Samples[kCapacity]{};
        int m_Count = 0;
        int m_Head = 0;
        bool m_WasTouching = false;
        bool m_WasOverlapping = false;
        float m_TouchBeganAt = -1e9f;
    };

    // Letting go must last this long before the spare goes back, so a hand that
    // relaxes for a moment during the swing keeps it.
    constexpr float kReleaseGraceSeconds = 0.12f;
}

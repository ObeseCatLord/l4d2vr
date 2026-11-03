#pragma once

// First-person full-body IK helpers (std-only, unit tested in
// tests/vr_body_ik_tests.cpp). Ported in spirit from eduke32_vr's solveIkBody
// (vr.cpp): the torso turns after the head with a deadband, the arms are
// analytic two-bone chains from the body's own shoulders to the tracked
// hands with the elbow on a body-relative pole, and an arm that cannot reach
// stretches a little before anything else gives.
//
//  * BodyYaw*: the torso's yaw in tracking space (degrees). Inside the
//    deadband the torso stays put while the head looks around; past it the
//    torso turns smoothly until the head is nearly centred again; it never
//    lags more than MaxLagDeg; a hand held far out to the side turns it at
//    once (fast follow) so the shoulder line stays under the head and the arm
//    never has to wrap behind the torso. Snap/smooth turning is applied by
//    the caller (world yaw = turn yaw + tracking yaw), so it is instant.
//  * SolveTwoBoneArm: shoulder -> elbow -> wrist, law of cosines. Out of reach
//    the two segments stretch together up to MaxStretch; past that the
//    shoulder slides toward the hand by up to MaxShoulderShift; anything left
//    stretches further, so the wrist always lands on the hand (the drawn hand
//    never detaches from the arm).
//  * ArmElbowPole: where the elbow points, in the body frame: down, a little
//    out and back; out to the side as the hand rises above the shoulder; out
//    and back when the hand goes behind the torso.
//
// Vectors are in any right-handed world frame; the body frame is given as
// unit forward / right / up axes in that frame. Lengths are in any unit
// (game units in the mod); thresholds that are distances take that unit's
// metres-per-unit via the caller's "unitsPerMeter".

#include <algorithm>
#include <cmath>

namespace l4d2vr_body_ik
{
    constexpr float kPi = 3.14159265358979323846f;

    struct Vec3
    {
        float x = 0.0f, y = 0.0f, z = 0.0f;
    };

    inline Vec3 operator+(Vec3 a, Vec3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
    inline Vec3 operator-(Vec3 a, Vec3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
    inline Vec3 operator-(Vec3 a) { return { -a.x, -a.y, -a.z }; }
    inline Vec3 operator*(Vec3 a, float s) { return { a.x * s, a.y * s, a.z * s }; }
    inline Vec3 operator*(float s, Vec3 a) { return a * s; }
    inline float Dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
    inline Vec3 Cross(Vec3 a, Vec3 b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
    inline float Length(Vec3 a) { return std::sqrt(Dot(a, a)); }
    inline bool Finite(Vec3 a) { return std::isfinite(a.x) && std::isfinite(a.y) && std::isfinite(a.z); }
    inline Vec3 Normalized(Vec3 a, Vec3 fallback = { 1.0f, 0.0f, 0.0f })
    {
        const float l = Length(a);
        return (std::isfinite(l) && l > 1e-8f) ? a * (1.0f / l) : fallback;
    }
    inline float Smoothstep(float edge0, float edge1, float x)
    {
        if (!(edge1 > edge0))
            return x >= edge1 ? 1.0f : 0.0f;
        const float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    }
    inline float WrapDeg(float a)
    {
        if (!std::isfinite(a))
            return 0.0f;
        a = std::fmod(a + 180.0f, 360.0f);
        if (a < 0.0f)
            a += 360.0f;
        return a - 180.0f;
    }

    // ---- Torso yaw ------------------------------------------------------------

    struct BodyYawConfig
    {
        float deadbandDeg = 30.0f;        // head may look this far off the torso before it turns
        float releaseDeg = 4.0f;          // a turn continues until the head is this close
        float maxLagDeg = 60.0f;          // hard limit: the torso is never further behind the head
        float followSeconds = 0.30f;      // time constant of the smooth turn
        float maxTurnDegPerSecond = 360.0f;
        // Hands: a hand this far out (horizontal, from the chest) and between
        // these yaws relative to the torso turns the torso at once.
        float handSideStartDeg = 70.0f;   // 90 = straight out to the side
        float handSideEndDeg = 140.0f;    // beyond this the hand is behind the back: no turn
        float handNearMeters = 0.30f;     // closer than this (e.g. a holster reach) never turns
        float handFarMeters = 0.50f;      // full effect from here
        float handFollowSeconds = 0.06f;
    };

    struct BodyYawState
    {
        bool valid = false;
        bool turning = false;
        float bodyYawDeg = 0.0f;          // tracking space
    };

    // One hand for the torso yaw: its horizontal offset from the chest
    // centre in the torso's own frame (forward, right), in metres.
    struct BodyYawHand
    {
        bool valid = false;
        float forwardMeters = 0.0f;
        float rightMeters = 0.0f;
    };

    // How far (degrees, signed: + = toward the torso's right, i.e. a yaw
    // decrease in Source's yaw convention is NOT assumed: callers pass yaw in
    // their own convention and the hand's side in the torso frame) the hands
    // ask the torso to turn. The sign follows "right" = positive.
    inline float HandsTurnRequestDeg(const BodyYawConfig& c, const BodyYawHand* hands, int count)
    {
        float request = 0.0f;
        for (int i = 0; hands && i < count; ++i)
        {
            const BodyYawHand& h = hands[i];
            if (!h.valid || !std::isfinite(h.forwardMeters) || !std::isfinite(h.rightMeters))
                continue;
            const float dist = std::sqrt(h.forwardMeters * h.forwardMeters + h.rightMeters * h.rightMeters);
            const float reachWeight = Smoothstep(c.handNearMeters, c.handFarMeters, dist);
            if (reachWeight <= 0.0f)
                continue;
            // 0 = straight ahead, +90 = out to the right, +-180 = behind.
            const float handYaw = std::atan2(h.rightMeters, h.forwardMeters) * (180.0f / kPi);
            const float a = std::fabs(handYaw);
            if (a <= c.handSideStartDeg || a >= c.handSideEndDeg)
                continue;
            // Fade out toward the back so a reach behind the shoulder blends to 0.
            const float backFade = 1.0f - Smoothstep(c.handSideEndDeg - 25.0f, c.handSideEndDeg, a);
            request += std::copysign(a - c.handSideStartDeg, handYaw) * reachWeight * backFade;
        }
        return request;
    }

    // Advances the torso yaw (tracking space, degrees, same convention as
    // headYawDeg) by dt seconds. turnSign converts the hands' "right =
    // positive" request into the caller's yaw convention (Source: yaw grows
    // to the left, so pass -1).
    inline float UpdateBodyYaw(BodyYawState& s, const BodyYawConfig& c, float headYawDeg, float dt,
        const BodyYawHand* hands = nullptr, int handCount = 0, float turnSign = -1.0f)
    {
        if (!std::isfinite(headYawDeg))
            return s.valid ? s.bodyYawDeg : 0.0f;
        headYawDeg = WrapDeg(headYawDeg);
        if (!s.valid || !std::isfinite(s.bodyYawDeg))
        {
            s.valid = true;
            s.turning = false;
            s.bodyYawDeg = headYawDeg;
            return s.bodyYawDeg;
        }
        dt = std::isfinite(dt) ? std::clamp(dt, 0.0f, 0.25f) : 0.0f;
        const float deadband = std::clamp(c.deadbandDeg, 0.0f, 89.0f);
        const float maxLag = std::clamp(c.maxLagDeg, deadband, 179.0f);
        const float release = std::clamp(c.releaseDeg, 0.0f, deadband);

        float body = s.bodyYawDeg;
        float err = WrapDeg(headYawDeg - body);
        if (!s.turning && std::fabs(err) > deadband)
            s.turning = true;
        if (s.turning && dt > 0.0f)
        {
            const float goal = err - std::copysign(release * 0.5f, err);
            float step = goal * (1.0f - std::exp(-dt / std::max(0.01f, c.followSeconds)));
            const float maxStep = std::max(0.0f, c.maxTurnDegPerSecond) * dt;
            step = std::clamp(step, -maxStep, maxStep);
            body = WrapDeg(body + step);
            err = WrapDeg(headYawDeg - body);
            if (std::fabs(err) <= release)
                s.turning = false;
        }

        // Hands far out to the side pull the torso round quickly.
        const float request = HandsTurnRequestDeg(c, hands, handCount) * turnSign;
        if (request != 0.0f && dt > 0.0f)
        {
            body = WrapDeg(body + request * (1.0f - std::exp(-dt / std::max(0.005f, c.handFollowSeconds))));
            err = WrapDeg(headYawDeg - body);
        }

        // Never further behind the head than maxLag.
        if (err > maxLag)
            body = WrapDeg(headYawDeg - maxLag);
        else if (err < -maxLag)
            body = WrapDeg(headYawDeg + maxLag);
        s.bodyYawDeg = body;
        return body;
    }

    // ---- Arms -------------------------------------------------------------------

    struct BodyFrame
    {
        Vec3 forward{ 1.0f, 0.0f, 0.0f };
        Vec3 right{ 0.0f, -1.0f, 0.0f };
        Vec3 up{ 0.0f, 0.0f, 1.0f };
    };

    // side: -1 left arm, +1 right arm. Returns a unit direction the elbow
    // should point toward (perpendicular-ised by the solver).
    inline Vec3 ArmElbowPole(const BodyFrame& f, int side, Vec3 shoulder, Vec3 target, float unitsPerMeter)
    {
        const float upm = (std::isfinite(unitsPerMeter) && unitsPerMeter > 0.0f) ? unitsPerMeter : 1.0f;
        const Vec3 out = f.right * static_cast<float>(side >= 0 ? 1 : -1);
        const Vec3 d = target - shoulder;
        const float above = Dot(d, f.up) / upm;          // metres above the shoulder
        const float ahead = Dot(d, f.forward) / upm;     // metres in front of the shoulder
        const float raise = Smoothstep(-0.10f, 0.25f, above);
        const float behind = Smoothstep(0.0f, -0.20f, ahead);
        // Down, slightly out and back at rest; out as the hand rises; out and
        // back (the elbow "wing") when the hand goes behind the torso.
        const float downW = 1.0f - 0.75f * raise;
        const float outW = 0.35f + 0.75f * raise + 0.45f * behind;
        const float backW = 0.20f + 0.35f * behind;
        return Normalized(-f.up * downW + out * outW - f.forward * backW, -f.up);
    }

    struct ArmConfig
    {
        float maxStretch = 1.12f;          // both segments together, before the shoulder moves
        float maxShoulderShiftMeters = 0.06f;
        float hardMaxStretch = 2.0f;       // beyond this the solve is refused (bad input)
    };

    struct ArmSolve
    {
        bool valid = false;
        Vec3 shoulder{};                   // possibly shifted toward the hand
        Vec3 elbow{};
        Vec3 wrist{};                      // == target unless the input was degenerate
        float stretch = 1.0f;              // segment length scale applied to both bones
        float shoulderShift = 0.0f;        // in the input unit
        bool overStretched = false;        // needed more than maxStretch + shoulder shift
    };

    // Two-bone IK from the shoulder to the target. pole: direction the elbow
    // should bend toward (any length; the component along the reach is
    // removed). fallbackPole is used when the pole is (nearly) along the reach.
    inline ArmSolve SolveTwoBoneArm(Vec3 shoulder, Vec3 target, float upperLen, float lowerLen,
        Vec3 pole, Vec3 fallbackPole, const ArmConfig& c, float unitsPerMeter)
    {
        ArmSolve r{};
        if (!Finite(shoulder) || !Finite(target) || !Finite(pole) || !std::isfinite(upperLen) ||
            !std::isfinite(lowerLen) || upperLen <= 1e-4f || lowerLen <= 1e-4f)
        {
            return r;
        }
        const float upm = (std::isfinite(unitsPerMeter) && unitsPerMeter > 0.0f) ? unitsPerMeter : 1.0f;
        float L1 = upperLen, L2 = lowerLen;
        Vec3 s = shoulder;
        Vec3 d = target - s;
        float dist = Length(d);
        if (!(dist > 1e-5f))
            return r;
        const float reach = (L1 + L2) * 0.999f;
        float k = 1.0f;
        if (dist > reach)
        {
            const float maxStretch = std::max(1.0f, c.maxStretch);
            k = std::min(maxStretch, dist / reach);
            float remaining = dist - reach * k;
            if (remaining > 0.0f)
            {
                const float shift = std::min(remaining, std::max(0.0f, c.maxShoulderShiftMeters) * upm);
                s = s + Normalized(d) * shift;
                r.shoulderShift = shift;
                remaining -= shift;
                d = target - s;
                dist = Length(d);
            }
            if (remaining > 1e-5f)
            {
                k = dist / reach;
                r.overStretched = true;
                if (k > std::max(c.hardMaxStretch, std::max(1.0f, c.maxStretch)))
                    return ArmSolve{};
            }
            L1 *= k;
            L2 *= k;
        }
        const Vec3 dir = Normalized(d);
        // Keep the triangle valid (hand nearly on the shoulder: fold fully).
        const float solveDist = std::clamp(dist, std::fabs(L1 - L2) + 1e-4f, L1 + L2 - 1e-4f);
        const float cosA = std::clamp((L1 * L1 + solveDist * solveDist - L2 * L2) / (2.0f * L1 * solveDist), -1.0f, 1.0f);
        const float sinA = std::sqrt(std::max(0.0f, 1.0f - cosA * cosA));

        auto perpOf = [&](Vec3 p) { return p - dir * Dot(p, dir); };
        Vec3 perp = perpOf(Normalized(pole, -dir));
        float pl = Length(perp);
        // Blend toward the fallback pole as the main pole lines up with the
        // reach, so the elbow never flips through the degenerate case.
        if (pl < 0.35f)
        {
            const Vec3 fb = perpOf(Normalized(fallbackPole, Vec3{ 0.0f, 0.0f, -1.0f }));
            const float w = 1.0f - pl / 0.35f;
            perp = perp * (1.0f - w) + Normalized(fb) * (0.35f * w);
            pl = Length(perp);
        }
        if (pl < 1e-5f)
        {
            perp = std::fabs(dir.z) < 0.9f ? Cross(dir, Vec3{ 0.0f, 0.0f, 1.0f }) : Cross(dir, Vec3{ 1.0f, 0.0f, 0.0f });
            pl = Length(perp);
        }
        perp = perp * (1.0f / pl);
        r.valid = true;
        r.shoulder = s;
        r.elbow = s + dir * (L1 * cosA) + perp * (L1 * sinA);
        r.wrist = s + dir * solveDist;
        if (solveDist == dist)
            r.wrist = target;
        r.stretch = k;
        return r;
    }

    // Rotation (as a 3x3 row-major matrix, column vectors) taking unit
    // direction a onto unit direction b along the shortest arc.
    inline void RotationBetween(Vec3 a, Vec3 b, float m[3][3])
    {
        a = Normalized(a);
        b = Normalized(b);
        const float c = std::clamp(Dot(a, b), -1.0f, 1.0f);
        Vec3 axis = Cross(a, b);
        float s = Length(axis);
        const float angle = std::atan2(s, c);
        if (s < 1e-6f)
        {
            if (c > 0.0f)
            {
                m[0][0] = 1; m[0][1] = 0; m[0][2] = 0;
                m[1][0] = 0; m[1][1] = 1; m[1][2] = 0;
                m[2][0] = 0; m[2][1] = 0; m[2][2] = 1;
                return;
            }
            axis = std::fabs(a.x) < 0.9f ? Cross(a, Vec3{ 1.0f, 0.0f, 0.0f }) : Cross(a, Vec3{ 0.0f, 1.0f, 0.0f });
            s = Length(axis);
        }
        axis = axis * (1.0f / s);
        const float cs = std::cos(angle), sn = std::sin(angle), t = 1.0f - cs;
        const float x = axis.x, y = axis.y, z = axis.z;
        m[0][0] = t * x * x + cs;     m[0][1] = t * x * y - sn * z; m[0][2] = t * x * z + sn * y;
        m[1][0] = t * x * y + sn * z; m[1][1] = t * y * y + cs;     m[1][2] = t * y * z - sn * x;
        m[2][0] = t * x * z - sn * y; m[2][1] = t * y * z + sn * x; m[2][2] = t * z * z + cs;
    }

    inline Vec3 Apply(const float m[3][3], Vec3 v)
    {
        return { m[0][0] * v.x + m[0][1] * v.y + m[0][2] * v.z,
                 m[1][0] * v.x + m[1][1] * v.y + m[1][2] * v.z,
                 m[2][0] * v.x + m[2][1] * v.y + m[2][2] * v.z };
    }
}

#pragma once

// Lower-body ("physical crouch") stage for remote VR world-model poses.
//
// The observer-side world-pose renderer (hooks_world_pose.inl) layers a VR
// head / chest-lean / two-arm solve over Source's final, already animated
// bone matrices. This header adds the std-only math for the missing lower
// body, using the same rules as the eduke32_vr VRIK port (vr_vrik_solver.h):
//
//  * Standing eye height is calibrated per player from the relayed HMD
//    height above the player origin: a decaying maximum (fast rise, slow
//    multiplicative decay), reset on respawn / teleport / silence.
//  * The pelvis drops by 90% of the physical crouch (standing - current eye
//    height), measured against Source's own animated crouch: only the part
//    that the native duck animation does not already provide is added, and
//    the total stays within a fraction of the leg length.
//  * Feet stay planted at their animated positions with an analytic two-bone
//    leg IK (thigh -> calf -> foot), knees bending forward relative to the
//    visible body yaw (biased by the animated knee direction).
//  * The spine bends forward (bounded) for the head drop the pelvis did not
//    cover.
//
// Every position is in one world-aligned space (Source: +X/+Y horizontal,
// +Z up) in game units. Rotations are quaternions from vr_vrik_solver.h.

#include "vr_vrik_solver.h"

#include <algorithm>
#include <cmath>

namespace l4d2vr_vrik
{
namespace lower
{
    struct Config
    {
        float pelvisDrop = 0.9f;                  // share of the physical crouch taken by the pelvis
        float maxDropLegFraction = 0.7f;          // total pelvis drop (animated + added) <= this * leg length
        float standingDecayPerSecond = 0.01f;     // decaying-max calibration, fraction per second
        float standingRiseTauSeconds = 0.2f;      // rise filter; rejects one-frame HMD spikes
        float minEyeHeight = 12.0f;               // game units; samples outside are ignored
        float maxEyeHeight = 120.0f;
        float animatedReferenceTauSeconds = 0.5f; // animated standing pelvis height filter
        float animatedStandingRatio = 0.85f;      // lower animated pelvis samples are crouch poses
        float maxSpineBendDegrees = 30.0f;        // total forward spine bend for the head remainder
        float teleportDistance = 96.0f;           // origin jump that restarts the calibration
        float silenceResetSeconds = 2.0f;         // no solve for this long restarts the calibration
    };

    inline float ClampDt(float dtSeconds)
    {
        return std::isfinite(dtSeconds) ? std::clamp(dtSeconds, 0.0f, 0.25f) : 0.0f;
    }

    // Standing eye height above the player origin (decaying maximum).
    struct HeightCalibration
    {
        bool valid = false;
        float standing = 0.0f;

        void Reset() { *this = HeightCalibration{}; }

        // Returns false when the sample is unusable (the state is unchanged).
        bool Update(float eyeHeight, float dtSeconds, const Config& config)
        {
            if (!std::isfinite(eyeHeight) ||
                eyeHeight < config.minEyeHeight ||
                eyeHeight > config.maxEyeHeight)
            {
                return false;
            }
            const float dt = ClampDt(dtSeconds);
            if (!valid || !std::isfinite(standing))
            {
                standing = eyeHeight;
                valid = true;
                return true;
            }
            const float decay = std::clamp(config.standingDecayPerSecond, 0.0f, 1.0f);
            float next = standing * std::exp(-decay * dt);
            if (eyeHeight > next)
            {
                const float tau = std::max(1e-4f, config.standingRiseTauSeconds);
                const float rise = 1.0f - std::exp(-dt / tau);
                // Never let the rise filter undercut the decayed maximum.
                next = std::max(next, standing + (eyeHeight - standing) * rise);
                next = std::min(next, eyeHeight);
            }
            standing = std::max(next, config.minEyeHeight);
            return true;
        }
    };

    // Source's own (animated) standing pelvis height above the player origin,
    // sampled only while the native duck state is off. Samples well below the
    // reference are crouch/transition poses even without the duck flag.
    struct AnimatedReference
    {
        bool valid = false;
        float height = 0.0f;

        void Reset() { *this = AnimatedReference{}; }

        void Update(float animatedPelvisHeight, float dtSeconds, bool nativeCrouched, const Config& config)
        {
            if (nativeCrouched || !std::isfinite(animatedPelvisHeight) || animatedPelvisHeight <= 0.0f)
                return;
            if (!valid)
            {
                height = animatedPelvisHeight;
                valid = true;
                return;
            }
            if (animatedPelvisHeight < height * std::clamp(config.animatedStandingRatio, 0.0f, 1.0f))
                return;
            const float tau = std::max(1e-4f, config.animatedReferenceTauSeconds);
            height += (animatedPelvisHeight - height) * (1.0f - std::exp(-ClampDt(dtSeconds) / tau));
        }
    };

    // Persistent per-player state.
    struct State
    {
        HeightCalibration eye;
        AnimatedReference pelvis;
        bool haveOrigin = false;
        Vec3 lastOrigin{};
        std::uint64_t lastTickMs = 0u;

        void Reset() { *this = State{}; }

        // Restarts the calibration after a respawn/teleport (origin jump) or a
        // long gap without solves. Returns the clamped dt in seconds.
        float Begin(Vec3 origin, std::uint64_t nowMs, const Config& config)
        {
            float dt = 0.0f;
            if (lastTickMs != 0u && nowMs >= lastTickMs)
                dt = static_cast<float>(nowMs - lastTickMs) * 0.001f;
            const bool silent = lastTickMs != 0u &&
                (nowMs < lastTickMs || dt > std::max(0.1f, config.silenceResetSeconds));
            const bool teleported = haveOrigin && Finite(origin) &&
                Length(origin - lastOrigin) > std::max(1.0f, config.teleportDistance);
            if (silent || teleported || !Finite(origin))
            {
                Reset();
                dt = 0.0f;
            }
            if (Finite(origin))
            {
                lastOrigin = origin;
                haveOrigin = true;
            }
            lastTickMs = nowMs;
            return ClampDt(dt);
        }
    };

    struct CrouchResult
    {
        bool valid = false;
        float physicalCrouch = 0.0f;     // standing eye height - current eye height (>= 0)
        float desiredPelvisDrop = 0.0f;  // pelvisDrop * physicalCrouch, clamped to the leg budget
        float animatedDrop = 0.0f;       // drop already present in Source's animation
        float addedDrop = 0.0f;          // extra pelvis drop to apply on top of the animation
        float headRemainder = 0.0f;      // head drop still missing after the pelvis (spine bend)
    };

    inline CrouchResult ComputeCrouch(
        float standingEyeHeight,
        float eyeHeight,
        float animatedReferenceHeight,
        float animatedPelvisHeight,
        float legLength,
        const Config& config)
    {
        CrouchResult result{};
        if (!std::isfinite(standingEyeHeight) || !std::isfinite(eyeHeight) ||
            !std::isfinite(animatedReferenceHeight) || !std::isfinite(animatedPelvisHeight) ||
            !std::isfinite(legLength) || standingEyeHeight <= 0.0f || legLength <= 0.0f)
        {
            return result;
        }
        result.valid = true;
        result.physicalCrouch = std::clamp(standingEyeHeight - eyeHeight, 0.0f, standingEyeHeight);
        const float budget = std::clamp(config.maxDropLegFraction, 0.0f, 0.95f) * legLength;
        result.desiredPelvisDrop = std::min(
            result.physicalCrouch * std::clamp(config.pelvisDrop, 0.0f, 1.0f),
            budget);
        result.animatedDrop = std::max(0.0f, animatedReferenceHeight - animatedPelvisHeight);
        // Never double-drop: the native crouch animation already lowered the
        // pelvis by animatedDrop. Never raise either: a native duck deeper than
        // the physical crouch stays authoritative.
        result.addedDrop = std::max(0.0f, result.desiredPelvisDrop - result.animatedDrop);
        result.headRemainder = std::max(
            0.0f,
            result.physicalCrouch - (result.animatedDrop + result.addedDrop));
        return result;
    }

    // Forward pitch (radians, >= 0) about axis Cross(up, forward) that lowers
    // the tip of `segment` (pivot -> tip) by `drop`, clamped to maxRadians.
    // Rotating by +angle takes `up` toward `forward`.
    inline float SpineBendForDrop(Vec3 segment, Vec3 up, Vec3 forward, float drop, float maxRadians)
    {
        if (!Finite(segment) || !std::isfinite(drop) || !(drop > 0.0f) || !(maxRadians > 0.0f))
            return 0.0f;
        const Vec3 u = Normalized(up, { 0.0f, 0.0f, 1.0f });
        Vec3 f = forward - u * Dot(forward, u);
        f = Normalized(f, { 1.0f, 0.0f, 0.0f });
        const float a = Dot(segment, u);
        const float b = Dot(segment, f);
        const float r = std::sqrt(a * a + b * b);
        if (!(r > 1e-3f))
            return 0.0f;
        // a*cos(t) - b*sin(t) = r*cos(t + phi), phi = atan2(b, a).
        const float phi = std::atan2(b, a);
        const float c = std::clamp((a - drop) / r, -1.0f, 1.0f);
        const float angle = std::acos(c) - phi;
        if (!std::isfinite(angle) || angle <= 0.0f)
            return 0.0f;
        return std::min(angle, maxRadians);
    }

    inline Vec3 SpineBendAxis(Vec3 up, Vec3 forward)
    {
        return Normalized(Cross(Normalized(up, { 0.0f, 0.0f, 1.0f }), forward), { 0.0f, 1.0f, 0.0f });
    }

    // ---- Rigid helpers for Source-style 3x4 bone matrices -----------------

    // p' = rotation * p + translation
    struct Rigid
    {
        Quat rotation{};
        Vec3 translation{};
    };

    inline Rigid RotationAbout(Quat rotation, Vec3 pivot)
    {
        rotation = Normalize(rotation);
        return { rotation, pivot - Rotate(rotation, pivot) };
    }

    inline Rigid Translation(Vec3 offset) { return { Quat{}, offset }; }

    inline Vec3 Apply(const Rigid& rigid, Vec3 point) { return Rotate(rigid.rotation, point) + rigid.translation; }

    // Rigid map taking a frame (fromRotation at fromPosition) onto another.
    inline Rigid MapFrame(Quat fromRotation, Vec3 fromPosition, Quat toRotation, Vec3 toPosition)
    {
        const Quat rotation = Normalize(Normalize(toRotation) * Conjugate(Normalize(fromRotation)));
        return { rotation, toPosition - Rotate(rotation, fromPosition) };
    }

    // m[row][col], columns 0..2 are the rotated local axes, column 3 the origin.
    inline void ToMatrix34(const Rigid& rigid, float out[3][4])
    {
        float r[3][3];
        ToMatrix(rigid.rotation, r);
        const float t[3] = { rigid.translation.x, rigid.translation.y, rigid.translation.z };
        for (int row = 0; row < 3; ++row)
        {
            for (int col = 0; col < 3; ++col)
                out[row][col] = r[row][col];
            out[row][3] = t[row];
        }
    }

    inline Vec3 MatrixOrigin(const float m[3][4]) { return { m[0][3], m[1][3], m[2][3] }; }

    // Orthonormal rotation of a (possibly scaled) bone matrix.
    inline bool MatrixRotation(const float m[3][4], Quat& out)
    {
        const Vec3 x{ m[0][0], m[1][0], m[2][0] };
        const Vec3 y{ m[0][1], m[1][1], m[2][1] };
        if (!Finite(x) || !Finite(y) || !(Length(x) > 1e-6f) || !(Length(y) > 1e-6f))
            return false;
        const Vec3 fx = Normalized(x);
        Vec3 fy = y - fx * Dot(y, fx);
        if (!(Length(fy) > 1e-6f))
            return false;
        fy = Normalized(fy);
        out = FromAxes(fx, fy, Cross(fx, fy));
        return Finite(out);
    }

    // ---- Planted-foot two-bone leg IK ------------------------------------

    struct LegSolution
    {
        bool valid = false;
        bool reached = false;
        Quat thighRotation{};   // world rotation about the hip (thigh branch)
        Vec3 knee{};            // solved knee position
        Quat calfRotation{};    // world rotation about the solved knee (calf branch), after thighRotation
        Vec3 ankle{};           // solved ankle position
    };

    // hip/knee/ankle: current joint positions (the hip already lowered).
    // target: where the ankle must stay planted. forward/up: visible body
    // frame; the knee bends toward forward (+20% up), biased by the current
    // animated knee direction when it agrees.
    inline LegSolution SolvePlantedLeg(Vec3 hip, Vec3 knee, Vec3 ankle, Vec3 target, Vec3 forward, Vec3 up)
    {
        LegSolution solution{};
        if (!Finite(hip) || !Finite(knee) || !Finite(ankle) || !Finite(target) ||
            !Finite(forward) || !Finite(up))
        {
            return solution;
        }
        const Vec3 upperBone = knee - hip;
        const Vec3 lowerBone = ankle - knee;
        const float l1 = Length(upperBone);
        const float l2 = Length(lowerBone);
        const Vec3 d = target - hip;
        const float rawDist = Length(d);
        if (!(l1 > 1e-3f) || !(l2 > 1e-3f) || !(rawDist > 1e-4f))
            return solution;

        const Vec3 dir = d * (1.0f / rawDist);
        const float dist = std::clamp(rawDist, std::fabs(l1 - l2) + 1e-3f, l1 + l2 - 1e-3f);
        const float cosA = std::clamp((l1 * l1 + dist * dist - l2 * l2) / (2.0f * l1 * dist), -1.0f, 1.0f);
        const float sinA = std::sqrt(std::max(0.0f, 1.0f - cosA * cosA));

        const Vec3 u = Normalized(up, { 0.0f, 0.0f, 1.0f });
        const Vec3 forwardPole = Normalized(forward, { 1.0f, 0.0f, 0.0f }) + u * 0.2f;
        Vec3 forwardPerp = forwardPole - dir * Dot(forwardPole, dir);
        Vec3 animatedPerp = upperBone - dir * Dot(upperBone, dir);
        Vec3 perp{};
        const bool forwardUsable = Length(forwardPerp) > 1e-4f;
        const bool animatedUsable = Length(animatedPerp) > 0.05f * l1;
        if (forwardUsable)
        {
            perp = Normalized(forwardPerp);
            // Keep the animated bend plane only when it points the knee forward.
            if (animatedUsable && Dot(Normalized(animatedPerp), perp) > 0.0f)
                perp = perp + Normalized(animatedPerp) * 0.5f;
        }
        else if (animatedUsable)
        {
            perp = animatedPerp;
        }
        else
        {
            perp = std::fabs(dir.z) < 0.9f ? Cross(dir, { 0.0f, 0.0f, 1.0f }) : Cross(dir, { 1.0f, 0.0f, 0.0f });
        }
        perp = perp - dir * Dot(perp, dir);
        if (!(Length(perp) > 1e-6f))
            return solution;
        perp = Normalized(perp);

        const Vec3 newKnee = hip + dir * (l1 * cosA) + perp * (l1 * sinA);
        const Vec3 end = hip + dir * dist;
        solution.thighRotation = Between(upperBone, newKnee - hip);
        const Vec3 ankleAfterThigh = newKnee + Rotate(solution.thighRotation, lowerBone);
        solution.calfRotation = Between(ankleAfterThigh - newKnee, end - newKnee);
        solution.knee = newKnee;
        solution.ankle = newKnee + Rotate(solution.calfRotation, ankleAfterThigh - newKnee);
        solution.reached = Length(solution.ankle - target) < 0.01f * (l1 + l2) + 1e-3f;
        solution.valid = Finite(solution.thighRotation) && Finite(solution.calfRotation) &&
            Finite(solution.knee) && Finite(solution.ankle);
        return solution;
    }
}
}

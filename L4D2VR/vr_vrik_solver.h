#pragma once

// Full-body VR IK for a Source survivor skeleton, driven by the HMD and two
// tracked wrists. Port of the eduke32_vr solveIkBody algorithm (vr.cpp):
//
//  * Facing follows the head yaw with a +-45 degree deadzone and eases onto
//    the head (tau 80 ms) while walking.
//  * Uniform height scale = clamp(standing eye height / bind eye height,
//    0.8, 1.25).
//  * The root (model origin, on the floor between the feet) is placed so the
//    bind head plus the eye offset lands on the tracked eye, pushed 4 cm back.
//  * The pelvis follows 35% of the head's horizontal offset and drops by 90%
//    of the crouch depth; spine and chest each take half the rotation that
//    swings the neck toward the head target; the neck takes 40% of the head's
//    rotation relative to the chest; the head takes the HMD orientation.
//  * Arms: analytic two-bone IK (law of cosines) with the elbow pole
//    (-0.4, +-0.8, -1) projected perpendicular to the reach; tracked arms
//    stretch up to 1.6x when out of reach; untracked arms hang.
//  * Legs: procedural stride from the walk speed (phase advances
//    speed * dt / 1.4 m, 0.18 m step, 0.08 m lift, half-cycle offset between
//    the legs, knee pole forward). Feet stay flat (bind orientation).
//
// Spaces:
//  * Player frame: origin at the player origin (feet), axes aligned with the
//    player's view yaw: +X forward, +Y left, +Z up, game units. All inputs.
//  * Model space: Source model axes (+X forward, +Y left, +Z up) in bind
//    units (game units at scale 1). All bone outputs.
//    player = root.position + rotate(root.rotation, scale * model).
// Orientations map local axes into the containing space. A Source QAngle
// matrix (columns forward, left, up) converts with Quat::FromAxes.
//
// L4D2 survivors have an extra Spine4 between Spine2 and Neck1; supply its
// bind transform as Spine2 (Bip01_Spine2 then follows Spine1 rigidly).

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace l4d2vr_vrik
{
    constexpr float kPi = 3.14159265358979323846f;

    // ---- Minimal vector / quaternion toolkit --------------------------------

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
        return l > 1e-8f ? a * (1.0f / l) : fallback;
    }

    struct Quat
    {
        float x = 0.0f, y = 0.0f, z = 0.0f, w = 1.0f;
    };

    inline bool Finite(Quat q) { return std::isfinite(q.x) && std::isfinite(q.y) && std::isfinite(q.z) && std::isfinite(q.w); }
    inline Quat Conjugate(Quat q) { return { -q.x, -q.y, -q.z, q.w }; }
    inline Quat Normalize(Quat q)
    {
        const float l = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
        return l > 1e-8f ? Quat{ q.x / l, q.y / l, q.z / l, q.w / l } : Quat{};
    }
    // a * b: apply b, then a.
    inline Quat Multiply(Quat a, Quat b)
    {
        return {
            a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
            a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z };
    }
    inline Quat operator*(Quat a, Quat b) { return Multiply(a, b); }
    inline Vec3 Rotate(Quat q, Vec3 v)
    {
        const Vec3 u{ q.x, q.y, q.z };
        const Vec3 t = Cross(u, v) * 2.0f;
        return v + t * q.w + Cross(u, t);
    }
    inline Quat FromAxisAngle(Vec3 axis, float radians)
    {
        const Vec3 n = Normalized(axis);
        const float s = std::sin(radians * 0.5f);
        return { n.x * s, n.y * s, n.z * s, std::cos(radians * 0.5f) };
    }
    inline Quat FromYaw(float radians) { return { 0.0f, 0.0f, std::sin(radians * 0.5f), std::cos(radians * 0.5f) }; }
    // Rotation whose columns (images of local X, Y, Z) are x, y, z.
    inline Quat FromAxes(Vec3 x, Vec3 y, Vec3 z)
    {
        const float trace = x.x + y.y + z.z;
        Quat q;
        if (trace > 0.0f)
        {
            const float s = std::sqrt(trace + 1.0f) * 2.0f;
            q = { (y.z - z.y) / s, (z.x - x.z) / s, (x.y - y.x) / s, 0.25f * s };
        }
        else if (x.x > y.y && x.x > z.z)
        {
            const float s = std::sqrt(1.0f + x.x - y.y - z.z) * 2.0f;
            q = { 0.25f * s, (y.x + x.y) / s, (z.x + x.z) / s, (y.z - z.y) / s };
        }
        else if (y.y > z.z)
        {
            const float s = std::sqrt(1.0f + y.y - x.x - z.z) * 2.0f;
            q = { (y.x + x.y) / s, 0.25f * s, (z.y + y.z) / s, (z.x - x.z) / s };
        }
        else
        {
            const float s = std::sqrt(1.0f + z.z - x.x - y.y) * 2.0f;
            q = { (z.x + x.z) / s, (z.y + y.z) / s, 0.25f * s, (x.y - y.x) / s };
        }
        return Normalize(q);
    }
    // Rotation from forward and (approximate) up, Source axes (+X fwd, +Y left, +Z up).
    inline Quat FromForwardUp(Vec3 forward, Vec3 up)
    {
        const Vec3 f = Normalized(forward);
        const Vec3 l = Normalized(Cross(up, f), Normalized(Cross({ 0.0f, 0.0f, 1.0f }, f), { 0.0f, 1.0f, 0.0f }));
        return FromAxes(f, l, Cross(f, l));
    }
    // Shortest rotation taking direction a onto direction b.
    inline Quat Between(Vec3 a, Vec3 b)
    {
        const float la = Length(a), lb = Length(b);
        if (!(la > 1e-6f) || !(lb > 1e-6f))
            return {};
        a = a * (1.0f / la);
        b = b * (1.0f / lb);
        const float c = Dot(a, b);
        if (c < -0.9999f)
        {
            Vec3 axis = Cross(a, { 1.0f, 0.0f, 0.0f });
            if (Dot(axis, axis) < 1e-6f)
                axis = Cross(a, { 0.0f, 1.0f, 0.0f });
            axis = Normalized(axis);
            return { axis.x, axis.y, axis.z, 0.0f };
        }
        const Vec3 axis = Cross(a, b);
        return Normalize({ axis.x, axis.y, axis.z, 1.0f + c });
    }
    // The rotation by fraction t of q's angle (about the same axis).
    inline Quat ScaleAngle(Quat q, float t)
    {
        q = Normalize(q);
        if (q.w < 0.0f)
            q = { -q.x, -q.y, -q.z, -q.w };
        const float angle = 2.0f * std::acos(std::min(1.0f, q.w)) * t;
        const float s = std::sqrt(std::max(0.0f, 1.0f - q.w * q.w));
        if (s < 1e-6f)
            return {};
        const float h = std::sin(angle * 0.5f) / s;
        return { q.x * h, q.y * h, q.z * h, std::cos(angle * 0.5f) };
    }
    // Row-major 3x3, m[row][col]; columns are the rotated local axes.
    inline void ToMatrix(Quat q, float m[3][3])
    {
        q = Normalize(q);
        const float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
        const float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
        const float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
        m[0][0] = 1.0f - 2.0f * (yy + zz); m[0][1] = 2.0f * (xy - wz); m[0][2] = 2.0f * (xz + wy);
        m[1][0] = 2.0f * (xy + wz); m[1][1] = 1.0f - 2.0f * (xx + zz); m[1][2] = 2.0f * (yz - wx);
        m[2][0] = 2.0f * (xz - wy); m[2][1] = 2.0f * (yz + wx); m[2][2] = 1.0f - 2.0f * (xx + yy);
    }
    inline float WrapRadians(float a)
    {
        if (!std::isfinite(a))
            return 0.0f;
        return a - 2.0f * kPi * std::floor((a + kPi) / (2.0f * kPi));
    }

    struct Transform
    {
        Quat rotation{};
        Vec3 position{};
    };

    inline Transform Compose(const Transform& parent, const Transform& local)
    {
        return { Normalize(parent.rotation * local.rotation), parent.position + Rotate(parent.rotation, local.position) };
    }
    inline Transform Inverse(const Transform& t)
    {
        const Quat inv = Conjugate(t.rotation);
        return { inv, Rotate(inv, -t.position) };
    }

    // ---- Skeleton ---------------------------------------------------------------

    enum Bone : int
    {
        Pelvis, Spine, Spine1, Spine2, Neck, Head,
        ClavicleL, UpperArmL, ForearmL, HandL,
        ClavicleR, UpperArmR, ForearmR, HandR,
        ThighL, CalfL, FootL,
        ThighR, CalfR, FootR,
        kBoneCount
    };

    constexpr std::array<int, kBoneCount> kDefaultParents = {
        -1, Pelvis, Spine, Spine1, Spine2, Neck,
        Spine2, ClavicleL, UpperArmL, ForearmL,
        Spine2, ClavicleR, UpperArmR, ForearmR,
        Pelvis, ThighL, CalfL,
        Pelvis, ThighR, CalfR,
    };

    // Source (ValveBiped) names for binding the enum to a studio model.
    constexpr const char* kSourceBoneNames[kBoneCount] = {
        "ValveBiped.Bip01_Pelvis", "ValveBiped.Bip01_Spine", "ValveBiped.Bip01_Spine1",
        "ValveBiped.Bip01_Spine2", "ValveBiped.Bip01_Neck1", "ValveBiped.Bip01_Head1",
        "ValveBiped.Bip01_L_Clavicle", "ValveBiped.Bip01_L_UpperArm", "ValveBiped.Bip01_L_Forearm",
        "ValveBiped.Bip01_L_Hand",
        "ValveBiped.Bip01_R_Clavicle", "ValveBiped.Bip01_R_UpperArm", "ValveBiped.Bip01_R_Forearm",
        "ValveBiped.Bip01_R_Hand",
        "ValveBiped.Bip01_L_Thigh", "ValveBiped.Bip01_L_Calf", "ValveBiped.Bip01_L_Foot",
        "ValveBiped.Bip01_R_Thigh", "ValveBiped.Bip01_R_Calf", "ValveBiped.Bip01_R_Foot",
    };

    struct Skeleton
    {
        std::array<int, kBoneCount> parents = kDefaultParents;   // parent index < bone index, -1 for roots
        std::array<Transform, kBoneCount> local{};               // relative to parent (roots: model space)
        float eyeForward = 0.0f;   // eye offset from the head bone along the HMD forward, model units
        float eyeUp = 0.0f;        // ... and along the HMD up
    };

    // Model-space bind transforms. False for a malformed skeleton.
    inline bool ComputeModelSpace(const Skeleton& skeleton, std::array<Transform, kBoneCount>& model)
    {
        for (int b = 0; b < kBoneCount; ++b)
        {
            const int parent = skeleton.parents[static_cast<std::size_t>(b)];
            const Transform& local = skeleton.local[static_cast<std::size_t>(b)];
            if (parent >= b || parent < -1 || !Finite(local.position) || !Finite(local.rotation))
                return false;
            const Transform normalized{ Normalize(local.rotation), local.position };
            model[static_cast<std::size_t>(b)] = parent < 0
                ? normalized
                : Compose(model[static_cast<std::size_t>(parent)], normalized);
        }
        return std::isfinite(skeleton.eyeForward) && std::isfinite(skeleton.eyeUp);
    }

    // Builds parent-relative locals from model-space bind transforms.
    inline bool MakeSkeletonFromModelSpace(const std::array<Transform, kBoneCount>& model,
        const std::array<int, kBoneCount>& parents, float eyeForward, float eyeUp, Skeleton& out)
    {
        Skeleton result;
        result.parents = parents;
        result.eyeForward = eyeForward;
        result.eyeUp = eyeUp;
        for (int b = 0; b < kBoneCount; ++b)
        {
            const int parent = parents[static_cast<std::size_t>(b)];
            if (parent >= b || parent < -1)
                return false;
            const Transform bone{ Normalize(model[static_cast<std::size_t>(b)].rotation), model[static_cast<std::size_t>(b)].position };
            result.local[static_cast<std::size_t>(b)] = parent < 0
                ? bone
                : Compose(Inverse(model[static_cast<std::size_t>(parent)]), bone);
        }
        out = result;
        return true;
    }

    // ---- Solver -------------------------------------------------------------

    struct Config
    {
        float unitsPerMeter = 39.37f;            // L4D2: one game unit is one inch
        bool anchorRootToOrigin = false;         // true: feet stay at the player origin, the body leans instead
        float yawDeadzoneDegrees = 45.0f;
        float walkYawTauSeconds = 0.08f;
        float walkAlignAmount = 0.25f;           // walk amount above which the body lines up with the head
        float walkSpeedTauSeconds = 0.12f;
        float fullStrideSpeedMps = 1.2f;
        float strideCycleMeters = 1.4f;
        float stepLengthMeters = 0.18f;
        float stepLiftMeters = 0.08f;
        float settleBackMeters = 0.04f;
        float pelvisFollow = 0.35f;
        float pelvisDrop = 0.9f;
        float neckShare = 0.4f;
        float maxArmStretch = 1.6f;
        float minScale = 0.8f;
        float maxScale = 1.25f;
        Vec3 elbowPole{ -0.4f, 0.8f, -1.0f };    // y mirrored for the right arm
        Vec3 kneePole{ 1.0f, 0.0f, 0.2f };
        Vec3 hangOffsetMeters{ 0.0625f, 0.094f, -0.625f };   // untracked wrist target from the shoulder; y mirrored
    };

    struct TrackedTarget
    {
        Vec3 position{};      // player frame, game units
        Quat orientation{};   // player frame
        bool valid = false;
    };

    struct Input
    {
        TrackedTarget head;       // eye position and HMD orientation (local +X forward, +Z up); required
        TrackedTarget hands[2];   // [0] left, [1] right: wrist position and hand-bone orientation
        Vec3 walkVelocity{};      // player frame, game units / s (z ignored)
        float dt = 0.0f;          // seconds since the previous solve (clamped to 0..0.1)
        float standingEyeHeight = 0.0f;   // calibrated, game units above the floor; <= 0: no scaling
    };

    // Persistent per-body state (one per rendered player).
    struct State
    {
        float bodyYaw = 0.0f;     // radians, player frame
        float stride = 0.0f;      // 0..1 stride phase
        float walkSpeed = 0.0f;   // smoothed, game units / s
        bool initialized = false;
    };

    struct Output
    {
        std::array<Transform, kBoneCount> bones{};   // model space
        // Stretch applied along each bone toward its child (1: none); a skinner
        // scales vertex offsets along that bone axis by it, as eduke32_vr does.
        std::array<float, kBoneCount> stretch{};
        Transform root{};          // model -> player frame (yaw rotation + floor position)
        float scale = 1.0f;        // uniform model scale
        float bodyYawDegrees = 0.0f;
        float pelvisDropUnits = 0.0f;   // model units
        float walkAmount = 0.0f;   // 0..1
        bool valid = false;
    };

    inline Transform BoneInPlayerFrame(const Output& output, int bone)
    {
        const Transform& b = output.bones[static_cast<std::size_t>(bone)];
        return { Normalize(output.root.rotation * b.rotation),
                 output.root.position + Rotate(output.root.rotation, b.position * output.scale) };
    }

    // Source matrix3x4_t layout (m[row][col], column 3 translation) for a bone
    // in the player frame, including the uniform model scale.
    inline void BoneToPlayerMatrix(const Output& output, int bone, float out[3][4])
    {
        const Transform t = BoneInPlayerFrame(output, bone);
        float r[3][3];
        ToMatrix(t.rotation, r);
        const Vec3 p = t.position;
        const float pos[3] = { p.x, p.y, p.z };
        for (int row = 0; row < 3; ++row)
        {
            for (int col = 0; col < 3; ++col)
                out[row][col] = r[row][col] * output.scale;
            out[row][3] = pos[row];
        }
    }

    namespace detail
    {
        struct Pose
        {
            std::array<Transform, kBoneCount> local{};
            std::array<Transform, kBoneCount> world{};
            std::array<bool, kBoneCount> overridden{};   // world rotation set explicitly
            std::array<bool, kBoneCount> placed{};       // world position set explicitly
            std::array<float, kBoneCount> stretch{};
            const std::array<int, kBoneCount>* parents = nullptr;

            Transform& W(int b) { return world[static_cast<std::size_t>(b)]; }
            Transform& L(int b) { return local[static_cast<std::size_t>(b)]; }

            void Fk(int from)
            {
                for (int b = std::max(0, from); b < kBoneCount; ++b)
                {
                    const std::size_t i = static_cast<std::size_t>(b);
                    const int parent = (*parents)[i];
                    if (parent < 0)
                    {
                        if (!overridden[i]) world[i].rotation = local[i].rotation;
                        if (!placed[i]) world[i].position = local[i].position;
                        continue;
                    }
                    const Transform& p = W(parent);
                    if (!overridden[i]) world[i].rotation = Normalize(p.rotation * local[i].rotation);
                    if (!placed[i]) world[i].position = p.position + Rotate(p.rotation, local[i].position);
                }
            }

            // Root bone b0 at its position, mid bone b1, end bone b2 reaching t; bend toward pole.
            void TwoBone(int b0, int b1, int b2, Vec3 t, Vec3 pole, bool canStretch, float maxStretch)
            {
                const Vec3 a = W(b0).position;
                float l1 = Length(L(b1).position), l2 = Length(L(b2).position);
                const Vec3 d = t - a;
                float dist = Length(d);
                if (!(dist > 1e-4f) || !(l1 > 1e-4f) || !(l2 > 1e-4f) || !Finite(t))
                    return;
                const float reach = (l1 + l2) * 0.995f;
                if (canStretch && dist > reach)
                {
                    const float k = std::min(maxStretch, dist / reach);
                    stretch[static_cast<std::size_t>(b0)] = k;
                    stretch[static_cast<std::size_t>(b1)] = k;
                    L(b1).position = L(b1).position * k;
                    L(b2).position = L(b2).position * k;
                    l1 *= k;
                    l2 *= k;
                    Fk(b0 + 1);
                }
                const Vec3 dir = d * (1.0f / dist);
                dist = std::max(std::fabs(l1 - l2) + 1e-3f, std::min(l1 + l2 - 1e-3f, dist));
                const float cosA = std::clamp((l1 * l1 + dist * dist - l2 * l2) / (2.0f * l1 * dist), -1.0f, 1.0f);
                const float sinA = std::sqrt(std::max(0.0f, 1.0f - cosA * cosA));
                Vec3 perp = pole - dir * Dot(pole, dir);
                const float pl = Length(perp);
                if (pl < 1e-5f)
                    perp = Normalized(std::fabs(dir.z) < 0.9f ? Cross(dir, { 0.0f, 0.0f, 1.0f }) : Cross(dir, { 1.0f, 0.0f, 0.0f }));
                else
                    perp = perp * (1.0f / pl);
                const Vec3 elbow = a + dir * (l1 * cosA) + perp * (l1 * sinA);
                const Vec3 end = a + dir * dist;
                // Swing each bone from its parent-carried direction onto the solved one.
                const Vec3 current1 = Rotate(W(b0).rotation, L(b1).position);
                W(b0).rotation = Normalize(Between(current1, elbow - a) * W(b0).rotation);
                overridden[static_cast<std::size_t>(b0)] = true;
                Fk(b0 + 1);
                const Vec3 current2 = Rotate(W(b1).rotation, L(b2).position);
                W(b1).rotation = Normalize(Between(current2, end - W(b1).position) * W(b1).rotation);
                overridden[static_cast<std::size_t>(b1)] = true;
                Fk(b1 + 1);
            }
        };
    }

    inline bool Solve(const Skeleton& skeleton, const Config& config, const Input& input, State& state, Output& output)
    {
        output = Output{};
        output.stretch.fill(1.0f);
        std::array<Transform, kBoneCount> bind{};
        const float upm = config.unitsPerMeter;
        if (!ComputeModelSpace(skeleton, bind) || !std::isfinite(upm) || upm <= 0.0f ||
            !input.head.valid || !Finite(input.head.position) || !Finite(input.head.orientation))
            return false;

        const float dt = std::isfinite(input.dt) ? std::clamp(input.dt, 0.0f, 0.1f) : 0.0f;
        const Quat headRotation = Normalize(input.head.orientation);
        const Vec3 headForward = Rotate(headRotation, { 1.0f, 0.0f, 0.0f });
        const Vec3 headUp = Rotate(headRotation, { 0.0f, 0.0f, 1.0f });

        // Facing: robust yaw even when looking straight up or down (the up axis
        // then points along the facing).
        const float yawX = headForward.x - headForward.z * headUp.x;
        const float yawY = headForward.y - headForward.z * headUp.y;
        const bool yawUsable = std::hypot(yawX, yawY) > 1e-3f;
        float headYaw = yawUsable ? std::atan2(yawY, yawX) : state.bodyYaw;
        if (!std::isfinite(headYaw))
            headYaw = 0.0f;

        if (!state.initialized || !std::isfinite(state.bodyYaw) || !std::isfinite(state.stride) ||
            !std::isfinite(state.walkSpeed))
        {
            state = State{};
            state.bodyYaw = headYaw;
            state.initialized = true;
        }

        // Smoothed walk speed (the game moves players in ticks, so velocity is lumpy).
        float targetSpeed = std::hypot(input.walkVelocity.x, input.walkVelocity.y);
        if (!std::isfinite(targetSpeed) || targetSpeed > 10.0f * upm)
            targetSpeed = 0.0f;   // a teleport is not a step
        if (dt > 0.0f)
        {
            const float tau = std::max(1e-4f, config.walkSpeedTauSeconds);
            state.walkSpeed += (targetSpeed - state.walkSpeed) * (1.0f - std::exp(-dt / tau));
            if (state.walkSpeed < 0.05f * upm && targetSpeed == 0.0f)
                state.walkSpeed = 0.0f;
        }
        const float walk = std::min(1.0f, state.walkSpeed / (std::max(1e-3f, config.fullStrideSpeedMps) * upm));

        const float limit = config.yawDeadzoneDegrees * (kPi / 180.0f);
        const float lag = WrapRadians(headYaw - state.bodyYaw);
        if (lag > limit) state.bodyYaw = headYaw - limit;
        else if (lag < -limit) state.bodyYaw = headYaw + limit;
        if (walk > config.walkAlignAmount && dt > 0.0f)
        {
            const float tau = std::max(1e-4f, config.walkYawTauSeconds);
            state.bodyYaw += WrapRadians(headYaw - state.bodyYaw) * (1.0f - std::exp(-dt / tau));
        }
        state.bodyYaw = WrapRadians(state.bodyYaw);
        state.stride += state.walkSpeed * dt / (std::max(1e-3f, config.strideCycleMeters) * upm);
        state.stride -= std::floor(state.stride);

        // Uniform scale to the standing eye height.
        const Vec3 bindHead = bind[Head].position;
        const float bindEye = bindHead.z + skeleton.eyeUp;
        float scale = 1.0f;
        if (std::isfinite(input.standingEyeHeight) && input.standingEyeHeight > 0.0f)
            scale = std::clamp(input.standingEyeHeight / std::max(0.5f * upm, bindEye), config.minScale, config.maxScale);

        // Model frame and the head target.
        const Quat bodyRotation = FromYaw(state.bodyYaw);
        const Quat toModelRotation = Conjugate(bodyRotation);
        const Vec3 headForwardM = Rotate(toModelRotation, headForward);
        const Vec3 headUpM = Rotate(toModelRotation, headUp);
        const Vec3 headFromEye = -(headForwardM * skeleton.eyeForward + headUpM * skeleton.eyeUp);
        const float settle = config.settleBackMeters * upm;
        Vec3 root{};
        if (!config.anchorRootToOrigin)
        {
            const Vec3 offset{ bindHead.x + settle - headFromEye.x, bindHead.y - headFromEye.y, 0.0f };
            root = input.head.position - Rotate(bodyRotation, offset) * scale;
            root.z = 0.0f;
        }
        const auto toModel = [&](Vec3 p) { return Rotate(toModelRotation, p - root) * (1.0f / scale); };
        const Vec3 eyeM = toModel(input.head.position);
        const Vec3 headTarget = eyeM + headFromEye - Vec3{ settle, 0.0f, 0.0f };

        detail::Pose pose;
        pose.parents = &skeleton.parents;
        for (int b = 0; b < kBoneCount; ++b)
        {
            const std::size_t i = static_cast<std::size_t>(b);
            pose.local[i] = { Normalize(skeleton.local[i].rotation), skeleton.local[i].position };
            pose.stretch[i] = 1.0f;
        }

        // Pelvis: drops when the head is lower than standing, follows the head horizontally.
        const float drop = std::max(0.0f, bindHead.z - headTarget.z);
        pose.W(Pelvis).position = {
            bind[Pelvis].position.x + (headTarget.x - bindHead.x) * config.pelvisFollow,
            bind[Pelvis].position.y + (headTarget.y - bindHead.y) * config.pelvisFollow,
            bind[Pelvis].position.z - drop * config.pelvisDrop };
        pose.W(Pelvis).rotation = bind[Pelvis].rotation;
        pose.overridden[Pelvis] = pose.placed[Pelvis] = true;
        pose.Fk(0);

        // Spine and chest bend so the neck heads for the head target.
        {
            const Vec3 s0 = pose.W(Spine).position;
            const Vec3 current = pose.W(Neck).position - s0;
            const Vec3 wanted = headTarget - s0 - Vec3{ 0.0f, 0.0f, bindHead.z - bind[Neck].position.z };
            const Quat bend = ScaleAngle(Between(current, wanted), 0.5f);
            pose.W(Spine).rotation = Normalize(bend * pose.W(Spine).rotation);
            pose.overridden[Spine] = true;
            pose.Fk(Spine + 1);
            pose.W(Spine2).rotation = Normalize(bend * pose.W(Spine2).rotation);
            pose.overridden[Spine2] = true;
            pose.Fk(Spine2 + 1);
        }

        // Neck and head follow the headset.
        {
            const Quat target = FromAxes(headForwardM, Cross(headUpM, headForwardM), headUpM);
            const Quat chestDelta = Normalize(pose.W(Spine2).rotation * Conjugate(bind[Spine2].rotation));
            const Quat relative = Normalize(target * Conjugate(chestDelta));
            pose.W(Neck).rotation = Normalize(ScaleAngle(relative, config.neckShare) * chestDelta * bind[Neck].rotation);
            pose.overridden[Neck] = true;
            pose.Fk(Neck + 1);
            pose.W(Head).rotation = Normalize(target * bind[Head].rotation);
            pose.overridden[Head] = true;
            pose.Fk(Head + 1);
        }

        // Arms to the tracked wrists, hanging when untracked.
        const int upper[2] = { UpperArmL, UpperArmR }, fore[2] = { ForearmL, ForearmR }, hand[2] = { HandL, HandR };
        for (int side = 0; side < 2; ++side)
        {
            const TrackedTarget& tracked = input.hands[side];
            const bool valid = tracked.valid && Finite(tracked.position) && Finite(tracked.orientation);
            const float out = bind[static_cast<std::size_t>(upper[side])].position.y > 0.0f ? 1.0f : -1.0f;
            const Vec3 target = valid
                ? toModel(tracked.position)
                : pose.W(upper[side]).position + Vec3{ config.hangOffsetMeters.x * upm,
                    out * config.hangOffsetMeters.y * upm, config.hangOffsetMeters.z * upm };
            const Vec3 pole{ config.elbowPole.x, out * config.elbowPole.y, config.elbowPole.z };
            pose.TwoBone(upper[side], fore[side], hand[side], target, pole, valid, std::max(1.0f, config.maxArmStretch));
            if (valid)
            {
                pose.W(hand[side]).rotation = Normalize(toModelRotation * tracked.orientation);
                pose.overridden[static_cast<std::size_t>(hand[side])] = true;
                pose.Fk(hand[side] + 1);
            }
        }

        // Legs: stride cycle while moving, knees forward, feet flat.
        const int thigh[2] = { ThighL, ThighR }, calf[2] = { CalfL, CalfR }, foot[2] = { FootL, FootR };
        for (int side = 0; side < 2; ++side)
        {
            const float phase = (state.stride + (side ? 0.5f : 0.0f)) * 2.0f * kPi;
            Vec3 target = bind[static_cast<std::size_t>(foot[side])].position;
            target.x += std::sin(phase) * config.stepLengthMeters * upm * walk;
            target.z += std::max(0.0f, std::cos(phase)) * config.stepLiftMeters * upm * walk;
            pose.TwoBone(thigh[side], calf[side], foot[side], target, config.kneePole, false, 1.0f);
            pose.W(foot[side]).rotation = bind[static_cast<std::size_t>(foot[side])].rotation;
            pose.overridden[static_cast<std::size_t>(foot[side])] = true;
        }

        output.bones = pose.world;
        output.stretch = pose.stretch;
        output.root = { bodyRotation, root };
        output.scale = scale;
        output.bodyYawDegrees = state.bodyYaw * (180.0f / kPi);
        output.pelvisDropUnits = drop * config.pelvisDrop;
        output.walkAmount = walk;
        for (const Transform& t : output.bones)
            if (!Finite(t.position) || !Finite(t.rotation))
            {
                output = Output{};
                return false;
            }
        output.valid = true;
        return true;
    }
}

#pragma once

// Hand skeletons for the OpenXR backend (OpenXR port M5). Std-only and
// runtime-free so it is unit-tested (tests/vr_openxr_hand_tests.cpp).
//
//   * The OpenVR hand skeleton the mod's skeletal reads expect: 31 bones (root,
//     wrist, 4 thumb, 5 per finger, 5 aux) with SteamVR's names, hierarchy and
//     reference poses (bind, open hand, fist, grip limit). The right hand's
//     poses come from SteamVR's own files (openxr_hand_reference.h); the left
//     hand follows the mirror rule SteamVR's left data obeys (positions mirror x
//     in the controller frame; finger bones keep their local rotation; the
//     wrist's children turn 180 degrees about x).
//   * XR_EXT_hand_tracking (26 joints, located in the raw controller frame =
//     the OpenVR skeleton's model space) -> OpenVR bones in model or parent space.
//     Bone positions are the joint positions; bone axes follow SteamVR's
//     convention (fingers: x along the bone, z = the curl axis; the wrist: a
//     fixed rotation measured on SteamVR's open hand); aux bones = the distal
//     bone in model space (left: turned 180 degrees about x), as SteamVR's data.
//   * Curls (0 = SteamVR's open hand, 1 = its fist, linear in the summed joint
//     angles, the way the mod renders curls) and splay from joint positions.
//   * Motion range: "with controller" stops each finger at SteamVR's grip-limit
//     curl (runtimes without XR_EXT_hand_joints_motion_range, or where Wine
//     drops the request, report the unobstructed hand).
//   * The controller fallback when there is no hand tracking: curls from the
//     trigger, grip, touch and proximity sensors of each profile, smoothed like
//     XRizer's estimated skeleton, and bones blended from the open hand toward
//     the fist (without controller) or the grip limit (with controller).

#include "openxr_input_logic.h"
#include "openxr_hand_reference.h"

#include "openvr.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>

namespace l4d2vr_openxr_hands
{
    namespace xrm = l4d2vr_openxr;
    namespace xi = l4d2vr_openxr_input;
    using Vec3 = xrm::Vec3;
    using Quat = xrm::Quat;
    using Pose = xrm::Pose;

    constexpr int kBoneCount = 31;    // OpenVR hand skeleton
    constexpr int kJointCount = 26;   // XR_HAND_JOINT_COUNT_EXT
    constexpr int kFingerCount = 5;   // thumb, index, middle, ring, pinky (OpenVR order)
    constexpr int kLeft = 0, kRight = 1;

    using Bones = std::array<Pose, kBoneCount>;
    using Joints = std::array<Pose, kJointCount>;

    // OpenVR bone indices (EHandSkeletonBone). Bones 1..25 sit at the OpenXR
    // joints with the same number (XrHandJointEXT: wrist 1, thumb metacarpal 2 ..
    // little tip 25); joint 0 is the palm, bone 0 the root.
    enum Bone : int
    {
        kRoot = 0, kWrist = 1,
        kThumb0 = 2, kThumb1, kThumb2, kThumb3,
        kIndex0 = 6, kIndex1, kIndex2, kIndex3, kIndex4,
        kMiddle0 = 11, kMiddle1, kMiddle2, kMiddle3, kMiddle4,
        kRing0 = 16, kRing1, kRing2, kRing3, kRing4,
        kPinky0 = 21, kPinky1, kPinky2, kPinky3, kPinky4,
        kAuxThumb = 26, kAuxIndex, kAuxMiddle, kAuxRing, kAuxPinky,
    };
    enum JointIndex : int { kJointPalm = 0, kJointWrist = 1 };

    // ---- Small math ----------------------------------------------------------------
    inline Vec3 Add(const Vec3& a, const Vec3& b) { return Vec3{ a.x + b.x, a.y + b.y, a.z + b.z }; }
    inline Vec3 Sub(const Vec3& a, const Vec3& b) { return Vec3{ a.x - b.x, a.y - b.y, a.z - b.z }; }
    inline Vec3 Scale(const Vec3& a, float s) { return Vec3{ a.x * s, a.y * s, a.z * s }; }
    inline float Dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
    inline float Length(const Vec3& a) { return std::sqrt(Dot(a, a)); }
    inline Vec3 Normalize(const Vec3& a)
    {
        const float l = Length(a);
        return l > 1e-9f ? Scale(a, 1.0f / l) : Vec3{ 0.0f, 0.0f, 0.0f };
    }
    inline float QuatDot(const Quat& a, const Quat& b) { return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w; }
    inline Quat Identity() { return Quat{ 0.0f, 0.0f, 0.0f, 1.0f }; }

    // Angle between two directions (0..pi); 0 when either is degenerate.
    inline float AngleBetween(const Vec3& a, const Vec3& b)
    {
        const float la = Length(a), lb = Length(b);
        if (la <= 1e-9f || lb <= 1e-9f)
            return 0.0f;
        const float c = std::clamp(Dot(a, b) / (la * lb), -1.0f, 1.0f);
        return std::acos(c);
    }

    // Rotation matrix (rows) -> quaternion.
    inline Quat QuatFromRows(const Vec3& r0, const Vec3& r1, const Vec3& r2)
    {
        const float m00 = r0.x, m01 = r0.y, m02 = r0.z;
        const float m10 = r1.x, m11 = r1.y, m12 = r1.z;
        const float m20 = r2.x, m21 = r2.y, m22 = r2.z;
        Quat q;
        const float trace = m00 + m11 + m22;
        if (trace > 0.0f)
        {
            const float s = std::sqrt(trace + 1.0f) * 2.0f;
            q.w = 0.25f * s; q.x = (m21 - m12) / s; q.y = (m02 - m20) / s; q.z = (m10 - m01) / s;
        }
        else if (m00 > m11 && m00 > m22)
        {
            const float s = std::sqrt(1.0f + m00 - m11 - m22) * 2.0f;
            q.w = (m21 - m12) / s; q.x = 0.25f * s; q.y = (m01 + m10) / s; q.z = (m02 + m20) / s;
        }
        else if (m11 > m22)
        {
            const float s = std::sqrt(1.0f + m11 - m00 - m22) * 2.0f;
            q.w = (m02 - m20) / s; q.x = (m01 + m10) / s; q.y = 0.25f * s; q.z = (m12 + m21) / s;
        }
        else
        {
            const float s = std::sqrt(1.0f + m22 - m00 - m11) * 2.0f;
            q.w = (m10 - m01) / s; q.x = (m02 + m20) / s; q.y = (m12 + m21) / s; q.z = 0.25f * s;
        }
        return xrm::Normalized(q);
    }

    // A frame from its axes (columns) -> quaternion.
    inline Quat QuatFromAxes(const Vec3& x, const Vec3& y, const Vec3& z)
    {
        return QuatFromRows(Vec3{ x.x, y.x, z.x }, Vec3{ x.y, y.y, z.y }, Vec3{ x.z, y.z, z.z });
    }

    // Spherical interpolation along the shorter arc.
    inline Quat Slerp(const Quat& a, Quat b, float t)
    {
        float d = QuatDot(a, b);
        if (d < 0.0f)
        {
            b = Quat{ -b.x, -b.y, -b.z, -b.w };
            d = -d;
        }
        if (d > 0.9995f)
            return xrm::Normalized(Quat{ a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t });
        const float theta = std::acos(std::clamp(d, -1.0f, 1.0f));
        const float s = std::sin(theta);
        const float wa = std::sin((1.0f - t) * theta) / s, wb = std::sin(t * theta) / s;
        return xrm::Normalized(Quat{ a.x * wa + b.x * wb, a.y * wa + b.y * wb, a.z * wa + b.z * wb, a.w * wa + b.w * wb });
    }

    inline bool Finite(const Pose& p)
    {
        const float v[7] = { p.position.x, p.position.y, p.position.z, p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w };
        for (float c : v)
            if (!std::isfinite(c))
                return false;
        return QuatDot(p.orientation, p.orientation) > 1e-6f;
    }

    // ---- Skeleton layout --------------------------------------------------------------
    inline int Parent(int b)
    {
        if (b <= kRoot || b >= kBoneCount) return -1;
        if (b == kWrist || b >= kAuxThumb) return kRoot;
        if (b == kThumb0 || b == kIndex0 || b == kMiddle0 || b == kRing0 || b == kPinky0) return kWrist;
        return b - 1;
    }

    // Finger (0 thumb .. 4 pinky) a bone belongs to; -1 for the root and wrist.
    inline int FingerOfBone(int b)
    {
        if (b >= kAuxThumb && b < kBoneCount) return b - kAuxThumb;
        if (b >= kThumb0 && b <= kThumb3) return 0;
        if (b >= kIndex0 && b <= kPinky4) return 1 + (b - kIndex0) / 5;
        return -1;
    }
    inline int FirstBone(int finger) { return finger == 0 ? kThumb0 : kIndex0 + 5 * (finger - 1); }
    inline int TipBone(int finger) { return finger == 0 ? kThumb3 : FirstBone(finger) + 4; }
    inline int DistalBone(int finger) { return TipBone(finger) - 1; }

    // SteamVR's bone names (GetBoneName; the vr_glove GLB joints carry the same).
    inline std::string BoneName(int hand, int b)
    {
        static const char* kFinger[kFingerCount] = { "thumb", "index", "middle", "ring", "pinky" };
        const char side = hand == kLeft ? 'l' : 'r';
        if (b == kRoot) return "Root";
        if (b == kWrist) return std::string("wrist_") + side;
        if (b < 0 || b >= kBoneCount) return std::string();
        const int f = FingerOfBone(b);
        if (b >= kAuxThumb) return std::string("finger_") + kFinger[f] + "_" + side + "_aux";
        if (b == TipBone(f)) return std::string("finger_") + kFinger[f] + "_" + side + "_end";
        const int k = b - FirstBone(f);
        if (f == 0) return std::string("finger_thumb_") + static_cast<char>('0' + k) + "_" + side;
        if (k == 0) return std::string("finger_") + kFinger[f] + "_meta_" + side;
        return std::string("finger_") + kFinger[f] + "_" + static_cast<char>('0' + (k - 1)) + "_" + side;
    }

    // ---- Spaces ------------------------------------------------------------------------
    inline Bones ToModel(const Bones& parentSpace)
    {
        Bones out;
        for (int b = 0; b < kBoneCount; ++b)
        {
            const int p = Parent(b);
            out[static_cast<size_t>(b)] = p < 0 ? parentSpace[static_cast<size_t>(b)]
                : xi::Compose(out[static_cast<size_t>(p)], parentSpace[static_cast<size_t>(b)]);
        }
        return out;
    }

    inline Bones ToParent(const Bones& modelSpace)
    {
        Bones out;
        for (int b = 0; b < kBoneCount; ++b)
        {
            const int p = Parent(b);
            out[static_cast<size_t>(b)] = p < 0 ? modelSpace[static_cast<size_t>(b)]
                : xi::Compose(xi::Inverse(modelSpace[static_cast<size_t>(p)]), modelSpace[static_cast<size_t>(b)]);
        }
        return out;
    }

    // 180 degrees about x (quaternion (1, 0, 0, 0)).
    inline Quat TurnX180(const Quat& q) { return xi::Mul(q, Quat{ 1.0f, 0.0f, 0.0f, 0.0f }); }

    // Aux bones (model space): the distal bone of each finger; the left hand's
    // are turned 180 degrees about x (SteamVR's left grip-limit data).
    inline void SetAuxFromDistal(int hand, Bones& model)
    {
        for (int f = 0; f < kFingerCount; ++f)
        {
            Pose aux = model[static_cast<size_t>(DistalBone(f))];
            if (hand == kLeft)
                aux.orientation = TurnX180(aux.orientation);
            model[static_cast<size_t>(kAuxThumb + f)] = aux;
        }
    }

    // ---- Reference poses -----------------------------------------------------------------
    enum class Reference : int { Bind = 0, Open = 1, Fist = 2, GripLimit = 3 };

    namespace detail
    {
        inline Bones FromTable(const float (*t)[7])
        {
            Bones out;
            for (int b = 0; b < kBoneCount; ++b)
            {
                Pose& p = out[static_cast<size_t>(b)];
                p.position = Vec3{ t[b][0], t[b][1], t[b][2] };
                p.orientation = xrm::Normalized(Quat{ t[b][3], t[b][4], t[b][5], t[b][6] });
            }
            return out;
        }

        // Right -> left in parent space (the rule SteamVR's left data follows).
        inline Bones MirrorToLeft(const Bones& right, bool bindPose)
        {
            Bones out = right;
            for (int b = 1; b < kBoneCount; ++b)
            {
                const Pose& r = right[static_cast<size_t>(b)];
                Pose& l = out[static_cast<size_t>(b)];
                const Quat& q = r.orientation;
                if (b == kWrist || (b >= kAuxThumb && !bindPose))
                {
                    // Child of the root: reflect through the controller's x = 0 plane.
                    l.position = Vec3{ -r.position.x, r.position.y, r.position.z };
                    l.orientation = Quat{ q.x, -q.y, -q.z, q.w };
                }
                else if (Parent(b) == kWrist || b >= kAuxThumb)
                {
                    // Metacarpals / thumb base (and the bind pose's aux bones).
                    l.position = Vec3{ -r.position.x, r.position.y, r.position.z };
                    l.orientation = xi::Mul(Quat{ 1.0f, 0.0f, 0.0f, 0.0f }, q);
                }
                else
                {
                    l.position = Vec3{ -r.position.x, -r.position.y, -r.position.z };
                    l.orientation = q;
                }
            }
            return out;
        }

        inline Bones WithAux(int hand, const Bones& parentSpace)
        {
            Bones model = ToModel(parentSpace);
            SetAuxFromDistal(hand, model);
            return ToParent(model);
        }

        struct ReferenceSet
        {
            Bones pose[2][4];  // [hand][Reference], parent space
            ReferenceSet()
            {
                const float (*tables[4])[7] = { reference::kRightBind, reference::kRightOpen, reference::kRightFist, reference::kRightGripLimit };
                for (int r = 0; r < 4; ++r)
                {
                    const Bones right = FromTable(tables[r]);
                    const bool bind = r == static_cast<int>(Reference::Bind);
                    pose[kRight][r] = bind ? right : WithAux(kRight, right);
                    const Bones left = MirrorToLeft(right, bind);
                    pose[kLeft][r] = bind ? left : WithAux(kLeft, left);
                }
            }
        };
        inline const ReferenceSet& References()
        {
            static const ReferenceSet set;
            return set;
        }
    }

    // SteamVR's reference pose (GetSkeletalReferenceTransforms), parent space.
    inline const Bones& ReferencePose(int hand, Reference which)
    {
        return detail::References().pose[hand == kLeft ? kLeft : kRight][static_cast<int>(which)];
    }

    // ---- Axis conventions ---------------------------------------------------------------
    // OpenXR joints: +z points back along the bone (away from the tip), +y out of
    // the back of the finger/hand. OpenVR finger bones: x along the bone (right
    // hand: tip at -x; left: +x), z = the curl axis. bone = joint * convention.
    inline Quat FingerConvention(int hand)
    {
        constexpr float s = 0.70710678f;
        return hand == kLeft ? Quat{ s, 0.0f, -s, 0.0f } : Quat{ 0.0f, -s, 0.0f, s };
    }

    // The OpenXR wrist frame of a hand from joint positions (the spec's
    // definition): +z from the middle metacarpal joint back to the wrist, +y out
    // of the back of the hand (from the index/little knuckle line).
    inline Quat XrWristFrame(int hand, const Vec3& wrist, const Vec3& middleMetacarpal, const Vec3& indexProximal, const Vec3& littleProximal)
    {
        const Vec3 z = Normalize(Sub(wrist, middleMetacarpal));
        const Vec3 forward = Scale(z, -1.0f);
        const Vec3 across = Sub(indexProximal, littleProximal);
        Vec3 dorsal = hand == kLeft ? xi::Cross(across, forward) : xi::Cross(forward, across);
        dorsal = Normalize(Sub(dorsal, Scale(z, Dot(dorsal, z))));
        const Vec3 x = xi::Cross(dorsal, z);
        return QuatFromAxes(x, dorsal, z);
    }

    namespace detail
    {
        struct WristConventions
        {
            Quat q[2];
            WristConventions()
            {
                for (int h = 0; h < 2; ++h)
                {
                    const Bones m = ToModel(ReferencePose(h, Reference::Open));
                    const Quat xr = XrWristFrame(h, m[kWrist].position, m[kMiddle0].position, m[kIndex1].position, m[kPinky1].position);
                    q[h] = xrm::Normalized(xi::Mul(xi::Conjugate(xr), m[kWrist].orientation));
                }
            }
        };
    }

    // SteamVR's wrist bone relative to the OpenXR wrist joint (bone = joint * this),
    // measured on SteamVR's open hand.
    inline Quat WristConvention(int hand)
    {
        static const detail::WristConventions conventions;
        return conventions.q[hand == kLeft ? kLeft : kRight];
    }

    // ---- Retarget ----------------------------------------------------------------------
    // XR joints in the OpenVR skeleton's model space (the raw controller frame)
    // -> OpenVR bones in model space.
    inline Bones ModelBonesFromJoints(int hand, const Joints& joints)
    {
        Bones m;
        m[kRoot] = Pose{};
        m[kWrist].position = joints[kJointWrist].position;
        m[kWrist].orientation = xrm::Normalized(xi::Mul(joints[kJointWrist].orientation, WristConvention(hand)));
        const Quat k = FingerConvention(hand);
        for (int b = kThumb0; b <= kPinky4; ++b)
        {
            m[static_cast<size_t>(b)].position = joints[static_cast<size_t>(b)].position;
            m[static_cast<size_t>(b)].orientation = xrm::Normalized(xi::Mul(joints[static_cast<size_t>(b)].orientation, k));
        }
        SetAuxFromDistal(hand, m);
        return m;
    }

    // The inverse (model-space bones -> XR joints): synthetic hands for tests and
    // the self-test's fake hand tracking. The palm sits halfway along the middle
    // metacarpal with that bone's orientation.
    inline Joints JointsFromModelBones(int hand, const Bones& m)
    {
        Joints j;
        j[kJointWrist].position = m[kWrist].position;
        j[kJointWrist].orientation = xrm::Normalized(xi::Mul(m[kWrist].orientation, xi::Conjugate(WristConvention(hand))));
        const Quat kInv = xi::Conjugate(FingerConvention(hand));
        for (int b = kThumb0; b <= kPinky4; ++b)
        {
            j[static_cast<size_t>(b)].position = m[static_cast<size_t>(b)].position;
            j[static_cast<size_t>(b)].orientation = xrm::Normalized(xi::Mul(m[static_cast<size_t>(b)].orientation, kInv));
        }
        j[kJointPalm].position = Scale(Add(m[kMiddle0].position, m[kMiddle1].position), 0.5f);
        j[kJointPalm].orientation = j[kMiddle0].orientation;
        return j;
    }

    // ---- Curls and splay -----------------------------------------------------------------
    struct Summary
    {
        float curl[kFingerCount] = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };   // thumb .. pinky, 0..1
        float splay[kFingerCount - 1] = { 0.0f, 0.0f, 0.0f, 0.0f };    // thumb-index .. ring-pinky, 0..1
    };

    // How far one finger is bent, from joint positions: for the fingers the
    // summed angles (radians) at the knuckle, middle and last joint; for the
    // thumb, whose SteamVR fist lays it straight across the fingers, how close
    // its tip comes to the middle finger's knuckle (minus the distance in hand
    // lengths, so it grows as the thumb closes).
    inline float FingerBend(int finger, const std::array<Vec3, kJointCount>& p)
    {
        auto bone = [&](int from, int to) { return Sub(p[static_cast<size_t>(to)], p[static_cast<size_t>(from)]); };
        if (finger == 0)
        {
            const float hand = Length(bone(kWrist, kMiddle1));
            if (!(hand > 1e-4f))
                return 0.0f;
            return -Length(bone(kMiddle1, kThumb3)) / hand;
        }
        const int b = FirstBone(finger);
        const Vec3 meta = bone(b, b + 1), prox = bone(b + 1, b + 2), mid = bone(b + 2, b + 3), dist = bone(b + 3, b + 4);
        return AngleBetween(meta, prox) + AngleBetween(prox, mid) + AngleBetween(mid, dist);
    }

    inline std::array<Vec3, kJointCount> Positions(const Joints& j)
    {
        std::array<Vec3, kJointCount> p{};
        for (int i = 0; i < kJointCount; ++i)
            p[static_cast<size_t>(i)] = j[static_cast<size_t>(i)].position;
        return p;
    }
    inline std::array<Vec3, kJointCount> Positions(const Bones& model)
    {
        std::array<Vec3, kJointCount> p{};
        for (int b = kWrist; b <= kPinky4; ++b)
            p[static_cast<size_t>(b)] = model[static_cast<size_t>(b)].position;
        p[kJointPalm] = Scale(Add(model[kMiddle0].position, model[kMiddle1].position), 0.5f);
        return p;
    }

    // Splay of adjacent fingers: the angle between their proximal bones seen
    // from the back of the hand, 0 = together; 1 at 25 degrees (fingers) or 90
    // degrees (thumb to index).
    inline void Splay(int hand, const std::array<Vec3, kJointCount>& p, float out[kFingerCount - 1])
    {
        const Vec3 forward = Normalize(Sub(p[kMiddle1], p[kWrist]));
        const Vec3 across = Sub(p[kIndex1], p[kPinky1]);
        const Vec3 normal = Normalize(hand == kLeft ? xi::Cross(across, forward) : xi::Cross(forward, across));
        auto dir = [&](int finger) {
            const int b = FirstBone(finger);
            const Vec3 d = finger == 0 ? Sub(p[kThumb2], p[kThumb1]) : Sub(p[static_cast<size_t>(b + 2)], p[static_cast<size_t>(b + 1)]);
            return Sub(d, Scale(normal, Dot(d, normal)));
        };
        constexpr float kFingerSplayMax = 25.0f * 3.14159265f / 180.0f;
        constexpr float kThumbSplayMax = 90.0f * 3.14159265f / 180.0f;
        for (int i = 0; i < kFingerCount - 1; ++i)
        {
            const float a = AngleBetween(dir(i), dir(i + 1));
            out[i] = std::clamp(a / (i == 0 ? kThumbSplayMax : kFingerSplayMax), 0.0f, 1.0f);
        }
    }

    namespace detail
    {
        struct BendRange
        {
            float open[kFingerCount]{}, fist[kFingerCount]{}, grip[kFingerCount]{};
            BendRange()
            {
                const auto po = Positions(ToModel(ReferencePose(kRight, Reference::Open)));
                const auto pf = Positions(ToModel(ReferencePose(kRight, Reference::Fist)));
                const auto pg = Positions(ToModel(ReferencePose(kRight, Reference::GripLimit)));
                for (int f = 0; f < kFingerCount; ++f)
                {
                    open[f] = FingerBend(f, po);
                    fist[f] = FingerBend(f, pf);
                    grip[f] = FingerBend(f, pg);
                }
            }
        };
        inline const BendRange& Bends()
        {
            static const BendRange range;
            return range;
        }
    }

    // Curl of one finger from its bend: 0 at SteamVR's open hand, 1 at its fist.
    inline float CurlFromBend(int finger, float bend)
    {
        const detail::BendRange& r = detail::Bends();
        const float span = r.fist[finger] - r.open[finger];
        if (!(span > 1e-4f) || !std::isfinite(bend))
            return 0.0f;
        return std::clamp((bend - r.open[finger]) / span, 0.0f, 1.0f);
    }

    // The grip-limit pose's curl per finger (how far a finger closes around a controller).
    inline float GripLimitCurl(int finger)
    {
        return CurlFromBend(finger, detail::Bends().grip[finger]);
    }

    inline Summary SummaryFromPositions(int hand, const std::array<Vec3, kJointCount>& p)
    {
        Summary s;
        for (int f = 0; f < kFingerCount; ++f)
            s.curl[f] = CurlFromBend(f, FingerBend(f, p));
        Splay(hand, p, s.splay);
        return s;
    }
    inline Summary SummaryFromJoints(int hand, const Joints& j) { return SummaryFromPositions(hand, Positions(j)); }

    // ---- Motion range ---------------------------------------------------------------------
    // "With controller": a finger closed beyond SteamVR's grip-limit curl is
    // turned back toward the open hand by the ratio, joint by joint (parent space).
    inline void LimitToController(int hand, Bones& parentSpace, const float curl[kFingerCount])
    {
        const Bones& open = ReferencePose(hand, Reference::Open);
        bool changed = false;
        for (int f = 0; f < kFingerCount; ++f)
        {
            const float limit = GripLimitCurl(f);
            if (!(curl[f] > limit) || curl[f] <= 1e-4f)
                continue;
            const float k = std::clamp(limit / curl[f], 0.0f, 1.0f);
            const int first = FirstBone(f);
            const int last = DistalBone(f);
            for (int b = first + (f == 0 ? 0 : 1); b <= last; ++b)
            {
                Pose& pose = parentSpace[static_cast<size_t>(b)];
                pose.orientation = Slerp(open[static_cast<size_t>(b)].orientation, pose.orientation, k);
            }
            changed = true;
        }
        if (changed)
        {
            Bones model = ToModel(parentSpace);
            SetAuxFromDistal(hand, model);
            parentSpace = ToParent(model);
        }
    }

    // Tracked joints -> bones in the requested space and motion range.
    inline Bones BonesFromJoints(int hand, const Joints& joints, bool modelSpace, bool withController, const Summary& summary)
    {
        const Bones model = ModelBonesFromJoints(hand, joints);
        if (!withController)
            return modelSpace ? model : ToParent(model);
        Bones parent = ToParent(model);
        LimitToController(hand, parent, summary.curl);
        return modelSpace ? ToModel(parent) : parent;
    }

    // ---- Controller fallback --------------------------------------------------------------
    // Bones for curls without hand tracking: each finger's bones go from the
    // open hand toward the fist (no controller) or the grip limit (with
    // controller) by its curl. Parent space.
    inline Bones BlendedBones(int hand, const float curl[kFingerCount], bool withController)
    {
        const Bones& open = ReferencePose(hand, Reference::Open);
        const Bones& closed = ReferencePose(hand, withController ? Reference::GripLimit : Reference::Fist);
        Bones out = open;
        for (int b = kThumb0; b <= kPinky4; ++b)
        {
            const int f = FingerOfBone(b);
            const float t = std::isfinite(curl[f]) ? std::clamp(curl[f], 0.0f, 1.0f) : 0.0f;
            const Pose& a = open[static_cast<size_t>(b)];
            const Pose& c = closed[static_cast<size_t>(b)];
            Pose& o = out[static_cast<size_t>(b)];
            o.position = Add(a.position, Scale(Sub(c.position, a.position), t));
            o.orientation = Slerp(a.orientation, c.orientation, t);
        }
        Bones model = ToModel(out);
        SetAuxFromDistal(hand, model);
        return ToParent(model);
    }

    // What a controller's sensors say about each finger (the profile decides
    // which of them exist; see openxr_input_profiles.h curl sources).
    struct CurlSensors
    {
        float trigger = 0.0f;          // trigger value 0..1
        bool triggerTouched = false;   // capacitive trigger (Index, Touch, Pico)
        float indexProximity = -1.0f;  // Quest Pro / Quest 3 trigger curl 0..1 (finger near the trigger); < 0 = none
        float squeeze = 0.0f;          // grip value 0..1 (a grip button: 0 or 1)
        float squeezeForce = -1.0f;    // Index grip force 0..1; < 0 = none
        bool thumbTouched = false;     // thumb on a stick, pad, face button or thumb rest
    };

    // A finger resting on a capacitive trigger (XRizer's estimated skeleton uses
    // the same 0.3), and how much the free fingers follow the index finger.
    constexpr float kIndexTouchCurl = 0.3f;
    constexpr float kFollowIndex[kFingerCount] = { 0.0f, 1.0f, 1.0f / 2.0f, 1.0f / 4.0f, 1.0f / 6.0f };
    constexpr float kCurlSmoothingRate = 24.0f;  // per second (XRizer's finger smoothing speed)

    inline float Clamp01(float v) { return std::isfinite(v) ? std::clamp(v, 0.0f, 1.0f) : 0.0f; }

    // Target curls (thumb .. pinky) for the sensors, before smoothing:
    //   index  = the trigger value, at least kIndexTouchCurl while the finger is
    //            on the trigger; a proximity sensor curls it up to that point as
    //            the finger approaches;
    //   middle, ring, pinky = the grip (value, plus the remaining range by the
    //            Index's grip force), and a little of the index finger's curl;
    //   thumb  = down (1) while it touches anything.
    inline void TargetCurls(const CurlSensors& s, float out[kFingerCount])
    {
        const float trigger = Clamp01(s.trigger);
        float index = 0.0f;
        if (s.triggerTouched || trigger > 0.0f)
            index = (std::max)(trigger, kIndexTouchCurl);
        else if (s.indexProximity >= 0.0f)
            index = Clamp01(s.indexProximity) * kIndexTouchCurl;
        float grip = Clamp01(s.squeeze);
        if (s.squeezeForce >= 0.0f)
            grip = grip + (1.0f - grip) * Clamp01(s.squeezeForce);
        out[0] = s.thumbTouched ? 1.0f : 0.0f;
        out[1] = index;
        for (int f = 2; f < kFingerCount; ++f)
            out[f] = (std::max)(grip, index * kFollowIndex[f]);
    }

    // Moves the curls toward the target (binary buttons animate instead of
    // snapping). dt in seconds; dt <= 0 or a huge gap jumps to the target.
    inline void SmoothCurls(float state[kFingerCount], const float target[kFingerCount], float dtSeconds)
    {
        const float t = (dtSeconds > 0.0f && dtSeconds < 1.0f) ? (std::min)(dtSeconds * kCurlSmoothingRate, 1.0f) : 1.0f;
        for (int f = 0; f < kFingerCount; ++f)
        {
            const float cur = std::isfinite(state[f]) ? state[f] : 0.0f;
            state[f] = Clamp01(cur + (Clamp01(target[f]) - cur) * t);
        }
    }

    // Splay of the blended fallback hand (from the same bones the bone data returns).
    inline Summary SummaryForCurls(int hand, const float curl[kFingerCount])
    {
        Summary s;
        for (int f = 0; f < kFingerCount; ++f)
            s.curl[f] = Clamp01(curl[f]);
        Splay(hand, Positions(ToModel(BlendedBones(hand, s.curl, false))), s.splay);
        return s;
    }

    // ---- OpenVR output ----------------------------------------------------------------------
    inline vr::VRBoneTransform_t ToBoneTransform(const Pose& p)
    {
        vr::VRBoneTransform_t t{};
        t.position.v[0] = p.position.x;
        t.position.v[1] = p.position.y;
        t.position.v[2] = p.position.z;
        t.position.v[3] = 1.0f;
        t.orientation.w = p.orientation.w;
        t.orientation.x = p.orientation.x;
        t.orientation.y = p.orientation.y;
        t.orientation.z = p.orientation.z;
        return t;
    }

    inline void CopyBones(const Bones& bones, vr::VRBoneTransform_t* out, uint32_t count)
    {
        for (uint32_t i = 0; i < count && i < static_cast<uint32_t>(kBoneCount); ++i)
            out[i] = ToBoneTransform(bones[i]);
    }

    inline bool JointsUsable(const Joints& j)
    {
        for (const Pose& p : j)
            if (!Finite(p))
                return false;
        // A collapsed hand (every joint at one point) is not a hand.
        return Length(Sub(j[kMiddle4].position, j[kJointWrist].position)) > 0.03f;
    }
}

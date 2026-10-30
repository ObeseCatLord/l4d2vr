// OpenXR hand skeletons (OpenXR port M5): L4D2VR/vr_backend/openxr_hand_logic.h.
// Golden poses are SteamVR's own reference poses (openxr_hand_reference.h).
#include "../L4D2VR/vr_backend/openxr_hand_logic.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

static int checks = 0;
static void Check(bool passed, const std::string& contract)
{
    ++checks;
    if (!passed)
    {
        std::fprintf(stderr, "FAIL: %s\n", contract.c_str());
        std::exit(1);
    }
}

namespace xh = l4d2vr_openxr_hands;
namespace xi = l4d2vr_openxr_input;
using xh::Bones;
using xh::Joints;
using xh::Pose;
using xh::Quat;
using xh::Vec3;
using xh::Reference;

static bool Near(float a, float b, float eps) { return std::fabs(a - b) <= eps; }
static bool NearV(const Vec3& a, const Vec3& b, float eps) { return xh::Length(xh::Sub(a, b)) <= eps; }
// Same rotation (q and -q are the same rotation).
static bool NearQ(const Quat& a, const Quat& b, float eps) { return 1.0f - std::fabs(xh::QuatDot(a, b)) <= eps; }
static float AngleDeg(const Quat& a, const Quat& b)
{
    const float d = std::fabs(xh::QuatDot(l4d2vr_openxr::Normalized(a), l4d2vr_openxr::Normalized(b)));
    return 2.0f * std::acos(std::fmin(1.0f, d)) * 57.29578f;
}

static Vec3 Axis(const Quat& q, int axis)
{
    const Vec3 unit[3] = { { 1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f } };
    return xi::Rotate(q, unit[axis]);
}

static const char* RefName(Reference r)
{
    switch (r)
    {
    case Reference::Bind: return "bind";
    case Reference::Open: return "open";
    case Reference::Fist: return "fist";
    default: return "griplimit";
    }
}

int main()
{
    // ---- Layout -------------------------------------------------------------------
    {
        const char* kRight[xh::kBoneCount] = {
            "Root", "wrist_r", "finger_thumb_0_r", "finger_thumb_1_r", "finger_thumb_2_r", "finger_thumb_r_end",
            "finger_index_meta_r", "finger_index_0_r", "finger_index_1_r", "finger_index_2_r", "finger_index_r_end",
            "finger_middle_meta_r", "finger_middle_0_r", "finger_middle_1_r", "finger_middle_2_r", "finger_middle_r_end",
            "finger_ring_meta_r", "finger_ring_0_r", "finger_ring_1_r", "finger_ring_2_r", "finger_ring_r_end",
            "finger_pinky_meta_r", "finger_pinky_0_r", "finger_pinky_1_r", "finger_pinky_2_r", "finger_pinky_r_end",
            "finger_thumb_r_aux", "finger_index_r_aux", "finger_middle_r_aux", "finger_ring_r_aux", "finger_pinky_r_aux" };
        const int kParents[xh::kBoneCount] = { -1, 0, 1, 2, 3, 4, 1, 6, 7, 8, 9, 1, 11, 12, 13, 14, 1, 16, 17, 18, 19,
            1, 21, 22, 23, 24, 0, 0, 0, 0, 0 };
        for (int b = 0; b < xh::kBoneCount; ++b)
        {
            Check(xh::BoneName(xh::kRight, b) == kRight[b], std::string("right bone name ") + kRight[b]);
            std::string left = kRight[b];
            if (b > 0) left.replace(left.rfind("_r"), 2, "_l");
            Check(xh::BoneName(xh::kLeft, b) == left, "left bone name " + left);
            Check(xh::Parent(b) == kParents[b], "parent of " + std::string(kRight[b]));
        }
        Check(xh::DistalBone(0) == xh::kThumb2 && xh::DistalBone(1) == xh::kIndex3 && xh::DistalBone(4) == xh::kPinky3, "distal bones");
        Check(xh::FingerOfBone(xh::kAuxRing) == 3 && xh::FingerOfBone(xh::kMiddle2) == 2 && xh::FingerOfBone(xh::kWrist) == -1, "finger of bone");
    }

    // ---- Reference poses (SteamVR) ---------------------------------------------------
    {
        // Spot values from SteamVR's open hand (right): wrist, index knuckle.
        const Bones& open = xh::ReferencePose(xh::kRight, Reference::Open);
        Check(NearV(open[xh::kWrist].position, Vec3{ 0.03404f, 0.0365f, 0.16472f }, 1e-4f), "open wrist position");
        Check(NearQ(open[xh::kWrist].orientation, Quat{ -0.07861f, 0.92028f, -0.3793f, -0.05515f }, 1e-4f), "open wrist rotation");
        Check(NearV(open[xh::kIndex1].position, Vec3{ -0.0742f, 0.005f, -0.00023f }, 1e-4f), "open index knuckle");
        Check(NearQ(open[xh::kIndex1].orientation, Quat{ 0.00705f, -0.04129f, 0.09301f, 0.99478f }, 1e-4f), "open index knuckle rotation");
        const Bones& grip = xh::ReferencePose(xh::kRight, Reference::GripLimit);
        Check(NearQ(grip[xh::kIndex1].orientation, Quat{ -0.03201f, -0.11501f, 0.66494f, 0.73729f }, 1e-4f), "grip-limit index knuckle");
        // Left = SteamVR's left data (spot values) = the mirror rule.
        const Bones& lopen = xh::ReferencePose(xh::kLeft, Reference::Open);
        Check(NearV(lopen[xh::kWrist].position, Vec3{ -0.03404f, 0.0365f, 0.16472f }, 1e-4f), "left open wrist position");
        Check(NearQ(lopen[xh::kWrist].orientation, Quat{ -0.07861f, -0.92028f, 0.3793f, -0.05515f }, 1e-4f), "left open wrist rotation");
        Check(NearV(lopen[xh::kThumb1].position, Vec3{ 0.04041f, 0.0f, 0.0f }, 1e-4f), "left thumb bone points +x");
        const Bones& lbind = xh::ReferencePose(xh::kLeft, Reference::Bind);
        Check(NearQ(lbind[xh::kThumb0].orientation, Quat{ 0.54119f, 0.18203f, 0.77304f, 0.27639f }, 1e-4f), "left bind thumb base (SteamVR left GLB)");
        Check(NearQ(lbind[xh::kAuxThumb].orientation, Quat{ 0.81959f, -0.04861f, -0.56911f, 0.04504f }, 1e-4f), "left bind thumb aux");
        Check(NearV(lbind[xh::kAuxThumb].position, Vec3{ 0.00039f, 0.0006f, -0.00084f }, 2e-5f), "left bind thumb aux position");

        for (int r = 0; r < 4; ++r)
        {
            const Reference ref = static_cast<Reference>(r);
            const Bones mr = xh::ToModel(xh::ReferencePose(xh::kRight, ref));
            const Bones ml = xh::ToModel(xh::ReferencePose(xh::kLeft, ref));
            for (int b = 0; b <= xh::kPinky4; ++b)
            {
                const Vec3 p = mr[static_cast<size_t>(b)].position;
                Check(NearV(ml[static_cast<size_t>(b)].position, Vec3{ -p.x, p.y, p.z }, 1e-5f),
                    std::string("left = mirror of right in model space: ") + RefName(ref) + " " + xh::BoneName(1, b));
            }
            // Parent/model round trip.
            const Bones back = xh::ToParent(mr);
            for (int b = 0; b < xh::kBoneCount; ++b)
                Check(NearV(back[static_cast<size_t>(b)].position, xh::ReferencePose(xh::kRight, ref)[static_cast<size_t>(b)].position, 1e-6f) &&
                    NearQ(back[static_cast<size_t>(b)].orientation, xh::ReferencePose(xh::kRight, ref)[static_cast<size_t>(b)].orientation, 1e-6f),
                    std::string("parent <-> model round trip ") + RefName(ref));
            if (ref == Reference::Bind)
                continue;
            // Aux = distal bone in model space (left: turned 180 degrees about x).
            for (int f = 0; f < xh::kFingerCount; ++f)
            {
                const Pose& d = mr[static_cast<size_t>(xh::DistalBone(f))];
                const Pose& a = mr[static_cast<size_t>(xh::kAuxThumb + f)];
                Check(NearV(a.position, d.position, 1e-6f) && NearQ(a.orientation, d.orientation, 1e-6f), "right aux = distal");
                const Pose& dl = ml[static_cast<size_t>(xh::DistalBone(f))];
                const Pose& al = ml[static_cast<size_t>(xh::kAuxThumb + f)];
                Check(NearV(al.position, dl.position, 1e-6f) && NearQ(al.orientation, xh::TurnX180(dl.orientation), 1e-6f), "left aux = distal turned");
            }
        }
        // SteamVR's grip-limit aux bones (right, model space).
        const Bones mg = xh::ToModel(grip);
        Check(NearV(mg[xh::kAuxIndex].position, Vec3{ -0.00017f, 0.01647f, 0.09652f }, 1e-4f) &&
            NearQ(mg[xh::kAuxIndex].orientation, Quat{ 0.02275f, 0.93293f, 0.35929f, -0.00646f }, 1e-4f), "grip-limit index aux");
    }

    // ---- Axis conventions against the hand's geometry ----------------------------------
    for (int h = 0; h < 2; ++h)
    {
        const Bones m = xh::ToModel(xh::ReferencePose(h, Reference::Open));
        const Vec3 forward = xh::Normalize(xh::Sub(m[xh::kMiddle1].position, m[xh::kWrist].position));
        const Vec3 across = xh::Sub(m[xh::kIndex1].position, m[xh::kPinky1].position);
        const Vec3 dorsal = xh::Normalize(h == xh::kLeft ? xi::Cross(across, forward) : xi::Cross(forward, across));
        const Quat kInv = xi::Conjugate(xh::FingerConvention(h));
        for (int b = xh::kThumb0; b <= xh::kPinky4; ++b)
        {
            const int f = xh::FingerOfBone(b);
            if (b == xh::TipBone(f))
                continue;
            const Quat xr = xi::Mul(m[static_cast<size_t>(b)].orientation, kInv);
            const Vec3 toChild = xh::Normalize(xh::Sub(m[static_cast<size_t>(b + 1)].position, m[static_cast<size_t>(b)].position));
            const float alongDeg = xh::AngleBetween(xh::Scale(Axis(xr, 2), -1.0f), toChild) * 57.29578f;
            Check(alongDeg < 5.0f, "XR -z of " + xh::BoneName(h, b) + " runs along the bone (" + std::to_string(alongDeg) + " deg)");
            if (f != 0)
            {
                const float upDeg = xh::AngleBetween(Axis(xr, 1), dorsal) * 57.29578f;
                Check(upDeg < 25.0f, "XR +y of " + xh::BoneName(h, b) + " is the back of the open hand (" + std::to_string(upDeg) + " deg)");
            }
        }
        // The wrist joint frame the spec defines.
        const Quat xrWrist = xi::Mul(m[xh::kWrist].orientation, xi::Conjugate(xh::WristConvention(h)));
        const Vec3 toMeta = xh::Normalize(xh::Sub(m[xh::kMiddle0].position, m[xh::kWrist].position));
        Check(xh::AngleBetween(xh::Scale(Axis(xrWrist, 2), -1.0f), toMeta) * 57.29578f < 0.5f, "wrist -z toward the middle metacarpal");
        Check(xh::AngleBetween(Axis(xrWrist, 1), dorsal) * 57.29578f < 20.0f, "wrist +y = back of the hand");
    }
    {
        // Left wrist convention = right's mirrored: W_L = D W_R D, D = diag(-1, 1, 1).
        const Quat wr = xh::WristConvention(xh::kRight), wl = xh::WristConvention(xh::kLeft);
        const Quat mirrored{ wr.x, -wr.y, -wr.z, wr.w };
        Check(AngleDeg(wl, mirrored) < 0.5f, "left wrist convention mirrors the right (" + std::to_string(AngleDeg(wl, mirrored)) + " deg)");
    }

    // ---- Golden retarget: SteamVR's poses as XR joints and back ------------------------
    for (int h = 0; h < 2; ++h)
    {
        for (Reference ref : { Reference::Open, Reference::Fist, Reference::GripLimit })
        {
            const Bones& golden = xh::ReferencePose(h, ref);
            const Joints joints = xh::JointsFromModelBones(h, xh::ToModel(golden));
            Check(xh::JointsUsable(joints), "joints usable");
            const xh::Summary s = xh::SummaryFromJoints(h, joints);
            const Bones parent = xh::BonesFromJoints(h, joints, false, false, s);
            const Bones model = xh::BonesFromJoints(h, joints, true, false, s);
            const Bones goldenModel = xh::ToModel(golden);
            for (int b = 0; b < xh::kBoneCount; ++b)
            {
                Check(NearV(parent[static_cast<size_t>(b)].position, golden[static_cast<size_t>(b)].position, 1e-5f) &&
                    NearQ(parent[static_cast<size_t>(b)].orientation, golden[static_cast<size_t>(b)].orientation, 1e-5f),
                    std::string("retarget parent space ") + RefName(ref) + " " + xh::BoneName(h, b));
                Check(NearV(model[static_cast<size_t>(b)].position, goldenModel[static_cast<size_t>(b)].position, 1e-5f) &&
                    NearQ(model[static_cast<size_t>(b)].orientation, goldenModel[static_cast<size_t>(b)].orientation, 1e-5f),
                    std::string("retarget model space ") + RefName(ref) + " " + xh::BoneName(h, b));
            }
            for (int f = 0; f < xh::kFingerCount; ++f)
            {
                if (ref == Reference::Open)
                    Check(Near(s.curl[f], 0.0f, 1e-4f), "open hand curl 0");
                else if (ref == Reference::Fist)
                    Check(Near(s.curl[f], 1.0f, 1e-4f), "fist curl 1");
                else
                    Check(s.curl[f] > 0.3f && s.curl[f] <= 1.0f, "grip limit curls between: " + std::to_string(s.curl[f]));
                Check(Near(s.curl[f], xh::SummaryFromJoints(1 - h, xh::JointsFromModelBones(1 - h, xh::ToModel(xh::ReferencePose(1 - h, ref)))).curl[f], 1e-4f),
                    "left and right curls agree");
            }
            for (int i = 0; i < 4; ++i)
                Check(s.splay[i] >= 0.0f && s.splay[i] <= 1.0f, "splay in range");
        }
        // Open hand splay: fingers a little apart, thumb well apart.
        const xh::Summary open = xh::SummaryFromJoints(h, xh::JointsFromModelBones(h, xh::ToModel(xh::ReferencePose(h, Reference::Open))));
        Check(open.splay[0] > 0.2f, "open thumb splay " + std::to_string(open.splay[0]));
        std::printf("hand %d open splay %.3f %.3f %.3f %.3f; grip-limit curls %.3f %.3f %.3f %.3f %.3f\n", h,
            open.splay[0], open.splay[1], open.splay[2], open.splay[3],
            xh::GripLimitCurl(0), xh::GripLimitCurl(1), xh::GripLimitCurl(2), xh::GripLimitCurl(3), xh::GripLimitCurl(4));
    }

    // ---- An independent hand: one finger bent by known angles ----------------------------
    {
        // Straight index finger along -z from the wrist, then bend each joint by
        // a known angle about +x (flexion toward the palm, -y). FingerBend must
        // return the sum.
        std::array<Vec3, xh::kJointCount> p{};
        const float lengths[4] = { 0.07f, 0.045f, 0.028f, 0.022f };
        const float bends[3] = { 0.5f, 0.8f, 0.35f };
        Vec3 at{ 0.0f, 0.0f, 0.0f };
        Vec3 dir{ 0.0f, 0.0f, -1.0f };
        p[xh::kIndex0] = at;
        for (int i = 0; i < 4; ++i)
        {
            at = xh::Add(at, xh::Scale(dir, lengths[i]));
            p[static_cast<size_t>(xh::kIndex1 + i)] = at;
            if (i < 3)
            {
                const Quat r = xi::AxisAngle(1.0f, 0.0f, 0.0f, -bends[i]);
                dir = xi::Rotate(r, dir);
            }
        }
        Check(Near(xh::FingerBend(1, p), bends[0] + bends[1] + bends[2], 1e-4f), "finger bend = summed joint angles");
        const float c = xh::CurlFromBend(1, bends[0] + bends[1] + bends[2]);
        Check(c > 0.0f && c < 1.0f, "a half-bent finger curls part way");
    }

    // ---- Fallback bones are consistent with the curl measure -----------------------------
    for (int h = 0; h < 2; ++h)
    {
        for (float t : { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f })
        {
            const float curls[5] = { t, t, t, t, t };
            const Bones parent = xh::BlendedBones(h, curls, false);
            const xh::Summary s = xh::SummaryFromPositions(h, xh::Positions(xh::ToModel(parent)));
            for (int f = 0; f < xh::kFingerCount; ++f)
                Check(Near(s.curl[f], t, 0.1f), "blended bones at curl " + std::to_string(t) + " measure " + std::to_string(s.curl[f]) +
                    " (finger " + std::to_string(f) + ", hand " + std::to_string(h) + ")");
            const Bones grip = xh::BlendedBones(h, curls, true);
            const xh::Summary sg = xh::SummaryFromPositions(h, xh::Positions(xh::ToModel(grip)));
            for (int f = 0; f < xh::kFingerCount; ++f)
                Check(sg.curl[f] <= xh::GripLimitCurl(f) + 0.08f, "with controller the blend stops at the grip limit");
        }
        const float open[5] = { 0, 0, 0, 0, 0 };
        const Bones o = xh::BlendedBones(h, open, true);
        for (int b = 0; b < xh::kBoneCount; ++b)
            Check(NearQ(o[static_cast<size_t>(b)].orientation, xh::ReferencePose(h, Reference::Open)[static_cast<size_t>(b)].orientation, 1e-5f),
                "curl 0 = SteamVR's open hand");
        const float fist[5] = { 1, 1, 1, 1, 1 };
        const Bones c1 = xh::BlendedBones(h, fist, false);
        const Bones g1 = xh::BlendedBones(h, fist, true);
        for (int b = 0; b < xh::kBoneCount; ++b)
        {
            Check(NearQ(c1[static_cast<size_t>(b)].orientation, xh::ReferencePose(h, Reference::Fist)[static_cast<size_t>(b)].orientation, 1e-5f),
                "curl 1 without controller = SteamVR's fist");
            Check(NearQ(g1[static_cast<size_t>(b)].orientation, xh::ReferencePose(h, Reference::GripLimit)[static_cast<size_t>(b)].orientation, 1e-5f),
                "curl 1 with controller = SteamVR's grip limit");
        }
    }

    // ---- Motion range on tracked joints ---------------------------------------------------
    for (int h = 0; h < 2; ++h)
    {
        const Joints fist = xh::JointsFromModelBones(h, xh::ToModel(xh::ReferencePose(h, Reference::Fist)));
        const xh::Summary s = xh::SummaryFromJoints(h, fist);
        const Bones limited = xh::BonesFromJoints(h, fist, true, true, s);
        const xh::Summary sl = xh::SummaryFromPositions(h, xh::Positions(limited));
        for (int f = 0; f < xh::kFingerCount; ++f)
            Check(Near(sl.curl[f], xh::GripLimitCurl(f), 0.12f), "a tracked fist with controller closes to the grip limit: " +
                std::to_string(sl.curl[f]) + " vs " + std::to_string(xh::GripLimitCurl(f)));
        const Joints open = xh::JointsFromModelBones(h, xh::ToModel(xh::ReferencePose(h, Reference::Open)));
        const xh::Summary so = xh::SummaryFromJoints(h, open);
        const Bones a = xh::BonesFromJoints(h, open, false, true, so);
        for (int b = 0; b < xh::kBoneCount; ++b)
            Check(NearQ(a[static_cast<size_t>(b)].orientation, xh::ReferencePose(h, Reference::Open)[static_cast<size_t>(b)].orientation, 1e-5f),
                "an open tracked hand is unchanged with controller");
        // Model-space aux bones stay the distal bones after the limit.
        for (int f = 0; f < xh::kFingerCount; ++f)
            Check(NearV(limited[static_cast<size_t>(xh::kAuxThumb + f)].position, limited[static_cast<size_t>(xh::DistalBone(f))].position, 1e-5f),
                "aux follows the limited finger");
    }

    // ---- A tracked hand that is not SteamVR's: other bone lengths, same angles -------------
    {
        Bones scaled = xh::ToModel(xh::ReferencePose(xh::kRight, Reference::Fist));
        Bones parent = xh::ReferencePose(xh::kRight, Reference::Fist);
        for (int b = xh::kThumb1; b <= xh::kPinky4; ++b)
            if (xh::Parent(b) != xh::kWrist)
                parent[static_cast<size_t>(b)].position = xh::Scale(parent[static_cast<size_t>(b)].position, 1.15f);
        scaled = xh::ToModel(parent);
        const xh::Summary s = xh::SummaryFromJoints(xh::kRight, xh::JointsFromModelBones(xh::kRight, scaled));
        for (int f = 0; f < xh::kFingerCount; ++f)
            Check(Near(s.curl[f], 1.0f, 0.02f), "a bigger hand's fist still curls 1");
        const Bones model = xh::BonesFromJoints(xh::kRight, xh::JointsFromModelBones(xh::kRight, scaled), true, false, s);
        Check(NearV(model[xh::kIndex4].position, scaled[xh::kIndex4].position, 1e-5f), "bone positions are the tracked joints");
    }

    // ---- Controller sensors -> curls (per profile style) ----------------------------------
    {
        float c[5];
        xh::CurlSensors s;
        xh::TargetCurls(s, c);
        Check(c[0] == 0.0f && c[1] == 0.0f && c[2] == 0.0f && c[4] == 0.0f, "nothing touched: open hand");
        // Index / Touch: finger resting on the capacitive trigger.
        s.triggerTouched = true;
        xh::TargetCurls(s, c);
        Check(Near(c[1], xh::kIndexTouchCurl, 1e-6f) && Near(c[2], xh::kIndexTouchCurl / 2.0f, 1e-6f) &&
            Near(c[3], xh::kIndexTouchCurl / 4.0f, 1e-6f) && Near(c[4], xh::kIndexTouchCurl / 6.0f, 1e-6f), "resting index; others follow a little");
        s.trigger = 0.8f;
        xh::TargetCurls(s, c);
        Check(Near(c[1], 0.8f, 1e-6f) && Near(c[2], 0.4f, 1e-6f), "trigger pull curls the index");
        s.squeeze = 0.6f; s.thumbTouched = true;
        xh::TargetCurls(s, c);
        Check(Near(c[2], 0.6f, 1e-6f) && Near(c[3], 0.6f, 1e-6f) && Near(c[4], 0.6f, 1e-6f) && c[0] == 1.0f, "grip value, thumb down");
        // Index grip force closes the rest of the way.
        s.squeezeForce = 0.5f;
        xh::TargetCurls(s, c);
        Check(Near(c[2], 0.8f, 1e-6f), "Index grip force");
        // A trigger pull without a touch sensor (WMR, Vive) still curls.
        xh::CurlSensors w;
        w.trigger = 0.1f;
        xh::TargetCurls(w, c);
        Check(Near(c[1], xh::kIndexTouchCurl, 1e-6f), "a light pull curls at least to the resting curl");
        // Quest Pro / Quest 3: the finger approaching the trigger.
        xh::CurlSensors q;
        q.indexProximity = 0.5f;
        xh::TargetCurls(q, c);
        Check(Near(c[1], 0.5f * xh::kIndexTouchCurl, 1e-6f), "proximity curls toward the resting curl");
        // Binary grip button (WMR, Vive wand, Cosmos).
        xh::CurlSensors b;
        b.squeeze = 1.0f;
        xh::TargetCurls(b, c);
        Check(c[2] == 1.0f && c[3] == 1.0f && c[4] == 1.0f && c[1] == 0.0f, "grip button = fist of the grip fingers");
        // Garbage in.
        xh::CurlSensors n;
        n.trigger = NAN; n.squeeze = 9.0f;
        xh::TargetCurls(n, c);
        Check(c[1] == 0.0f && c[2] == 1.0f, "non-finite / out of range clamp");
    }
    {
        float state[5] = { 0, 0, 0, 0, 0 };
        const float target[5] = { 1, 1, 1, 1, 1 };
        xh::SmoothCurls(state, target, 1.0f / 90.0f);
        Check(Near(state[2], 24.0f / 90.0f, 1e-5f), "smoothing moves 24/s");
        for (int i = 0; i < 40; ++i)
            xh::SmoothCurls(state, target, 1.0f / 90.0f);
        Check(state[2] > 0.99f, "smoothing converges in half a second");
        float jump[5] = { 0, 0, 0, 0, 0 };
        xh::SmoothCurls(jump, target, 0.0f);
        Check(jump[0] == 1.0f, "no time base: jump to the target");
    }
    {
        // Fallback summary carries the curls unchanged and a plausible splay.
        const float curls[5] = { 0.2f, 0.4f, 0.6f, 0.8f, 1.0f };
        const xh::Summary s = xh::SummaryForCurls(xh::kRight, curls);
        for (int f = 0; f < 5; ++f)
            Check(s.curl[f] == curls[f], "fallback summary = curls");
        for (int i = 0; i < 4; ++i)
            Check(s.splay[i] >= 0.0f && s.splay[i] <= 1.0f, "fallback splay range");
    }

    // ---- OpenVR output --------------------------------------------------------------------
    {
        vr::VRBoneTransform_t out[xh::kBoneCount]{};
        xh::CopyBones(xh::ReferencePose(xh::kRight, Reference::Open), out, xh::kBoneCount);
        Check(Near(out[xh::kWrist].position.v[2], 0.16472f, 1e-4f) && Near(std::fabs(out[xh::kWrist].orientation.y), 0.92028f, 1e-3f) &&
            out[xh::kWrist].position.v[3] == 1.0f, "VRBoneTransform_t layout");
        Joints bad = xh::JointsFromModelBones(xh::kRight, xh::ToModel(xh::ReferencePose(xh::kRight, Reference::Open)));
        bad[7].position.x = NAN;
        Check(!xh::JointsUsable(bad), "non-finite joints rejected");
        Joints collapsed{};
        Check(!xh::JointsUsable(collapsed), "collapsed joints rejected");
    }

    std::printf("vr_openxr_hand_tests: %d checks passed\n", checks);
    return 0;
}

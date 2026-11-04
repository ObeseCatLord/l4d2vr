#pragma once
// The free off hand's orientation in the controller frame (std-only, unit
// tested in tests/vr_offhand_pose_tests.cpp).
//
// The survivor's free (support) hand follows its controller: the arm IK sets
// the ValveBiped hand bone to
//   controller * local(NativeViewmodelLeftHandPoseOffsetMeters,
//                      NativeViewmodelLeftHandPoseRotationOffsetDeg)
// (hooks_misc.inl HooksNativeViewmodelHandsOnlyBuildLeftControllerTargetAnchor),
// where the controller matrix has the columns (right, up, back) of the
// calibrated controller. Bip01_L_Hand's own axes (measured with the `handdump`
// self-test on the survivor arms): +x runs from the wrist along the fingers,
// +z toward the index finger's side, -y toward the thumb and the palm.
//
// With a zero rotation offset the bone's +x (the fingers) therefore lies along
// the controller's RIGHT axis: the free left hand is bent 90 degrees in
// toward the body even with the controller held straight. With the mirrored
// left hand (ItemHandMirror, LeftHanded) the free right hand is the reflection
// of the same pose, so it bends in the same way. The calibrated pose is
// (-90, 90, 0): fingers forward along the controller, the index knuckle up and
// the palm facing the other hand, as on the developer's tuned install (where
// it came from an upstream defaults tuning that never reached config.txt).
#include <cmath>
#include <cstdlib>
#include <string>

namespace l4d2vr_offhand_pose
{
    // Defaults (config.txt, vr.h, vr_config_overlay_embedded.inl sample).
    constexpr float kDefaultRotationDeg[3] = { -90.0f, 90.0f, 0.0f };
    constexpr float kDefaultOffsetMeters[3] = { 0.0f, -0.08f, 0.1f };
    constexpr const char* kDefaultRotationText = "-90,90,0";
    constexpr const char* kDefaultOffsetText = "0,-0.08,0.1";
    // The free right hand (and the gun hand, GunHandOnController) mirrors the
    // left: the same place relative to its controller (x = 0, so the mirror
    // leaves it unchanged). Until 2.0.6 it was 0,0,0, which put the right
    // wrist on the controller's raw origin, 10 cm in front of and 8 cm above
    // where the left hand's wrist sits (headset session 7: "the hands are
    // very offset from the controllers").
    constexpr float kDefaultRightOffsetMeters[3] = { 0.0f, -0.08f, 0.1f };
    constexpr const char* kDefaultRightOffsetText = "0,-0.08,0.1";

    // Local rotation of NativeViewmodel*HandPoseRotationOffsetDeg (x, y, z in
    // degrees): R = Rz * Ry * Rx, row-major. The exact matrix
    // HooksNativeViewmodelHandsOnlyBuildLocalTransform applies.
    inline void LocalRotation(const float deg[3], float out[3][3])
    {
        constexpr float kDegToRad = 3.14159265358979323846f / 180.0f;
        const float rx = deg[0] * kDegToRad, ry = deg[1] * kDegToRad, rz = deg[2] * kDegToRad;
        const float sx = std::sin(rx), cx = std::cos(rx);
        const float sy = std::sin(ry), cy = std::cos(ry);
        const float sz = std::sin(rz), cz = std::cos(rz);
        out[0][0] = cz * cy; out[0][1] = cz * sy * sx - sz * cx; out[0][2] = cz * sy * cx + sz * sx;
        out[1][0] = sz * cy; out[1][1] = sz * sy * sx + cz * cx; out[1][2] = sz * sy * cx - cz * sx;
        out[2][0] = -sy;     out[2][1] = cy * sx;                out[2][2] = cy * cx;
    }

    // Parses "x,y,z" (spaces allowed). False for anything else.
    inline bool ParseVector3(const std::string& text, float out[3])
    {
        std::string s;
        for (char c : text)
            if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
                s.push_back(c);
        size_t start = 0;
        for (int i = 0; i < 3; ++i)
        {
            const size_t comma = s.find(',', start);
            if ((i < 2) != (comma != std::string::npos))
                return false;
            const std::string part = s.substr(start, i < 2 ? comma - start : std::string::npos);
            if (part.empty())
                return false;
            char* end = nullptr;
            out[i] = std::strtof(part.c_str(), &end);
            if (!end || *end != '\0' || !std::isfinite(out[i]))
                return false;
            start = comma + 1;
        }
        return true;
    }

    // One-shot migration rule: only a config that still has the old shipped
    // value (0,0,0, or the key missing) gets the calibrated default; a value the
    // player tuned is kept.
    inline bool ShouldMigrateLegacyZero(bool keyPresent, const std::string& value)
    {
        if (!keyPresent)
            return true;
        float v[3];
        if (!ParseVector3(value, v))
            return false;
        return std::fabs(v[0]) < 1e-4f && std::fabs(v[1]) < 1e-4f && std::fabs(v[2]) < 1e-4f;
    }
}

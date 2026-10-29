#pragma once

// Pure conversions between OpenXR poses/FOVs and the OpenVR value types the mod
// uses (OpenXR port M2). No OpenXR headers: XrPosef and XrFovf have the same
// layout as the Pose/Fov structs here, and both APIs share the right-handed,
// +Y up, -Z forward, metres convention, so no axis changes are needed.

#include "openvr.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace l4d2vr_openxr
{
    struct Quat { float x = 0.0f, y = 0.0f, z = 0.0f, w = 1.0f; };
    struct Vec3 { float x = 0.0f, y = 0.0f, z = 0.0f; };
    struct Pose { Quat orientation; Vec3 position; };  // XrPosef layout
    struct Fov { float angleLeft = 0.0f, angleRight = 0.0f, angleUp = 0.0f, angleDown = 0.0f; };  // XrFovf layout
    struct Rect { int32_t x = 0, y = 0, width = 0, height = 0; };  // XrRect2Di layout

    inline Quat Normalized(Quat q)
    {
        const float n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
        if (!(n > 1e-12f))
            return Quat{};
        return Quat{ q.x / n, q.y / n, q.z / n, q.w / n };
    }

    inline vr::HmdMatrix34_t ToMatrix(const Pose& pose)
    {
        const Quat q = Normalized(pose.orientation);
        const float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
        const float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
        const float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
        vr::HmdMatrix34_t m{};
        m.m[0][0] = 1.0f - 2.0f * (yy + zz); m.m[0][1] = 2.0f * (xy - wz);        m.m[0][2] = 2.0f * (xz + wy);
        m.m[1][0] = 2.0f * (xy + wz);        m.m[1][1] = 1.0f - 2.0f * (xx + zz); m.m[1][2] = 2.0f * (yz - wx);
        m.m[2][0] = 2.0f * (xz - wy);        m.m[2][1] = 2.0f * (yz + wx);        m.m[2][2] = 1.0f - 2.0f * (xx + yy);
        m.m[0][3] = pose.position.x;
        m.m[1][3] = pose.position.y;
        m.m[2][3] = pose.position.z;
        return m;
    }

    // Rotation part of a 3x4 rigid transform to a unit quaternion (Shepperd).
    inline Pose FromMatrix(const vr::HmdMatrix34_t& m)
    {
        Pose pose;
        const float trace = m.m[0][0] + m.m[1][1] + m.m[2][2];
        Quat q;
        if (trace > 0.0f)
        {
            const float s = std::sqrt(trace + 1.0f) * 2.0f;
            q.w = 0.25f * s;
            q.x = (m.m[2][1] - m.m[1][2]) / s;
            q.y = (m.m[0][2] - m.m[2][0]) / s;
            q.z = (m.m[1][0] - m.m[0][1]) / s;
        }
        else if (m.m[0][0] > m.m[1][1] && m.m[0][0] > m.m[2][2])
        {
            const float s = std::sqrt(1.0f + m.m[0][0] - m.m[1][1] - m.m[2][2]) * 2.0f;
            q.w = (m.m[2][1] - m.m[1][2]) / s;
            q.x = 0.25f * s;
            q.y = (m.m[0][1] + m.m[1][0]) / s;
            q.z = (m.m[0][2] + m.m[2][0]) / s;
        }
        else if (m.m[1][1] > m.m[2][2])
        {
            const float s = std::sqrt(1.0f + m.m[1][1] - m.m[0][0] - m.m[2][2]) * 2.0f;
            q.w = (m.m[0][2] - m.m[2][0]) / s;
            q.x = (m.m[0][1] + m.m[1][0]) / s;
            q.y = 0.25f * s;
            q.z = (m.m[1][2] + m.m[2][1]) / s;
        }
        else
        {
            const float s = std::sqrt(1.0f + m.m[2][2] - m.m[0][0] - m.m[1][1]) * 2.0f;
            q.w = (m.m[1][0] - m.m[0][1]) / s;
            q.x = (m.m[0][2] + m.m[2][0]) / s;
            q.y = (m.m[1][2] + m.m[2][1]) / s;
            q.z = 0.25f * s;
        }
        if (q.w < 0.0f)
            q = Quat{ -q.x, -q.y, -q.z, -q.w };
        pose.orientation = Normalized(q);
        pose.position = Vec3{ m.m[0][3], m.m[1][3], m.m[2][3] };
        return pose;
    }

    inline vr::HmdMatrix34_t Multiply(const vr::HmdMatrix34_t& a, const vr::HmdMatrix34_t& b)
    {
        vr::HmdMatrix34_t r{};
        for (int i = 0; i < 3; ++i)
        {
            for (int j = 0; j < 4; ++j)
            {
                float v = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j];
                if (j == 3)
                    v += a.m[i][3];
                r.m[i][j] = v;
            }
        }
        return r;
    }

    // The pose a projection view must carry for an eye the game rendered from
    // `head` (tracking space) with the eye offset it read from
    // GetEyeToHeadTransform. The mod renders with the eye *position* only
    // (VR::GetViewParameters), so the layer gets exactly that camera.
    inline Pose EyePoseFromHead(const vr::HmdMatrix34_t& head, const vr::HmdMatrix34_t& eyeToHead, bool translationOnly)
    {
        vr::HmdMatrix34_t eye = eyeToHead;
        if (translationOnly)
        {
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j)
                    eye.m[i][j] = (i == j) ? 1.0f : 0.0f;
        }
        return FromMatrix(Multiply(head, eye));
    }

    // OpenVR GetProjectionRaw <-> XrFovf, with XRizer's convention (the mod
    // computes its eye texture bounds from these): left/right are the tangents
    // of angleLeft/angleRight, and "bottom"/"top" are the tangents of
    // angleUp/angleDown (OpenVR's raw projection has y pointing down).
    struct RawProjection { float left = 0.0f, right = 0.0f, top = 0.0f, bottom = 0.0f; };

    inline RawProjection ToRawProjection(const Fov& fov)
    {
        RawProjection raw;
        raw.left = std::tan(fov.angleLeft);
        raw.right = std::tan(fov.angleRight);
        raw.bottom = std::tan(fov.angleUp);
        raw.top = std::tan(fov.angleDown);
        return raw;
    }

    inline Fov FromRawProjection(const RawProjection& raw)
    {
        Fov fov;
        fov.angleLeft = std::atan(raw.left);
        fov.angleRight = std::atan(raw.right);
        fov.angleUp = std::atan(raw.bottom);
        fov.angleDown = std::atan(raw.top);
        return fov;
    }

    inline bool FovUsable(const Fov& fov)
    {
        auto finite = [](float v) { return std::isfinite(v); };
        return finite(fov.angleLeft) && finite(fov.angleRight) && finite(fov.angleUp) && finite(fov.angleDown) &&
            fov.angleRight > fov.angleLeft && fov.angleUp > fov.angleDown &&
            fov.angleLeft > -1.5f && fov.angleRight < 1.5f && fov.angleDown > -1.5f && fov.angleUp < 1.5f;
    }

    // The sub-rectangle of a width x height eye image that OpenVR texture
    // bounds select (u right, v down, 0..1). OpenXR image rects cannot flip,
    // so a flipped axis is reported and its extent used as is.
    inline Rect ImageRectFromBounds(const vr::VRTextureBounds_t* bounds, uint32_t width, uint32_t height, bool* flipped = nullptr)
    {
        float u0 = 0.0f, u1 = 1.0f, v0 = 0.0f, v1 = 1.0f;
        if (bounds)
        {
            u0 = bounds->uMin; u1 = bounds->uMax;
            v0 = bounds->vMin; v1 = bounds->vMax;
        }
        const bool flip = u0 > u1 || v0 > v1;
        if (flipped)
            *flipped = flip;
        if (u0 > u1) std::swap(u0, u1);
        if (v0 > v1) std::swap(v0, v1);
        auto clamp01 = [](float v) { return std::isfinite(v) ? std::clamp(v, 0.0f, 1.0f) : 0.0f; };
        u0 = clamp01(u0); u1 = clamp01(u1); v0 = clamp01(v0); v1 = clamp01(v1);
        const int32_t x0 = static_cast<int32_t>(std::lround(u0 * static_cast<float>(width)));
        const int32_t x1 = static_cast<int32_t>(std::lround(u1 * static_cast<float>(width)));
        const int32_t y0 = static_cast<int32_t>(std::lround(v0 * static_cast<float>(height)));
        const int32_t y1 = static_cast<int32_t>(std::lround(v1 * static_cast<float>(height)));
        Rect rect;
        rect.x = x0;
        rect.y = y0;
        rect.width = std::max<int32_t>(1, x1 - x0);
        rect.height = std::max<int32_t>(1, y1 - y0);
        if (rect.x + rect.width > static_cast<int32_t>(width))
            rect.x = std::max<int32_t>(0, static_cast<int32_t>(width) - rect.width);
        if (rect.y + rect.height > static_cast<int32_t>(height))
            rect.y = std::max<int32_t>(0, static_cast<int32_t>(height) - rect.height);
        return rect;
    }

    // OpenVR pose from an OpenXR space location. `flags` uses the XrSpaceLocationFlags
    // bits (1 orientation valid, 2 position valid, 4 orientation tracked, 8 position tracked).
    inline vr::TrackedDevicePose_t ToTrackedPose(const Pose& pose, uint64_t flags,
        const Vec3& linearVelocity, const Vec3& angularVelocity, bool haveVelocity)
    {
        vr::TrackedDevicePose_t out{};
        const bool orientationValid = (flags & 0x1u) != 0;
        const bool positionValid = (flags & 0x2u) != 0;
        const bool tracked = (flags & 0x4u) != 0 && (flags & 0x8u) != 0;
        out.bDeviceIsConnected = true;
        out.bPoseIsValid = orientationValid && positionValid;
        out.eTrackingResult = out.bPoseIsValid
            ? (tracked ? vr::TrackingResult_Running_OK : vr::TrackingResult_Fallback_RotationOnly)
            : vr::TrackingResult_Running_OutOfRange;
        if (out.bPoseIsValid)
            out.mDeviceToAbsoluteTracking = ToMatrix(pose);
        if (haveVelocity)
        {
            out.vVelocity.v[0] = linearVelocity.x; out.vVelocity.v[1] = linearVelocity.y; out.vVelocity.v[2] = linearVelocity.z;
            out.vAngularVelocity.v[0] = angularVelocity.x; out.vAngularVelocity.v[1] = angularVelocity.y; out.vAngularVelocity.v[2] = angularVelocity.z;
        }
        return out;
    }

    inline float DisplayHzFromPeriod(int64_t periodNs, float fallbackHz)
    {
        if (periodNs <= 0)
            return fallbackHz;
        const double hz = 1e9 / static_cast<double>(periodNs);
        return (hz >= 20.0 && hz <= 1000.0) ? static_cast<float>(hz) : fallbackHz;
    }
}

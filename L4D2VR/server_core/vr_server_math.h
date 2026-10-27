#pragma once

// Minimal Source-compatible vector math for the dedicated-server core.
// std-only; no SDK headers. Conventions match Source: QAngle(pitch, yaw, roll)
// in degrees, +X forward, +Y left, +Z up, AngleVectors returns forward/right/up.

#include <algorithm>
#include <cmath>

namespace l4d2vr_server
{
    constexpr float kPi = 3.14159265358979323846f;
    constexpr float kDegToRad = kPi / 180.0f;
    constexpr float kRadToDeg = 180.0f / kPi;

    struct Vec3
    {
        float x = 0.0f, y = 0.0f, z = 0.0f;

        constexpr Vec3() = default;
        constexpr Vec3(float xv, float yv, float zv) : x(xv), y(yv), z(zv) {}

        Vec3 operator+(const Vec3& o) const { return { x + o.x, y + o.y, z + o.z }; }
        Vec3 operator-(const Vec3& o) const { return { x - o.x, y - o.y, z - o.z }; }
        Vec3 operator*(float s) const { return { x * s, y * s, z * s }; }
        Vec3 operator/(float s) const { return { x / s, y / s, z / s }; }
        Vec3& operator+=(const Vec3& o) { x += o.x; y += o.y; z += o.z; return *this; }
        Vec3& operator*=(float s) { x *= s; y *= s; z *= s; return *this; }
        bool operator==(const Vec3& o) const { return x == o.x && y == o.y && z == o.z; }
        float LengthSqr() const { return x * x + y * y + z * z; }
        float Length() const { return std::sqrt(LengthSqr()); }
        bool Finite() const { return std::isfinite(x) && std::isfinite(y) && std::isfinite(z); }
    };

    // Pitch, yaw, roll in degrees.
    using Angles = Vec3;

    inline float Dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

    inline Vec3 Cross(const Vec3& a, const Vec3& b)
    {
        return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
    }

    // Normalizes in place and returns the previous length (Source semantics).
    inline float Normalize(Vec3& v)
    {
        const float length = v.Length();
        if (std::isfinite(length) && length > 0.000001f)
            v *= 1.0f / length;
        return length;
    }

    inline void AngleVectors(const Angles& angles, Vec3* forward, Vec3* right, Vec3* up)
    {
        const float sp = std::sin(angles.x * kDegToRad), cp = std::cos(angles.x * kDegToRad);
        const float sy = std::sin(angles.y * kDegToRad), cy = std::cos(angles.y * kDegToRad);
        const float sr = std::sin(angles.z * kDegToRad), cr = std::cos(angles.z * kDegToRad);
        if (forward) *forward = { cp * cy, cp * sy, -sp };
        if (right) *right = { (-sr * sp * cy) + (-cr * -sy), (-sr * sp * sy) + (-cr * cy), -sr * cp };
        if (up) *up = { (cr * sp * cy) + (-sr * -sy), (cr * sp * sy) + (-sr * cy), cr * cp };
    }

    // Source VectorAngles (forward only; roll = 0).
    inline Angles VectorAngles(const Vec3& forward)
    {
        float yaw = 0.0f, pitch = 0.0f;
        if (forward.y == 0.0f && forward.x == 0.0f)
        {
            yaw = 0.0f;
            pitch = forward.z > 0.0f ? 270.0f : 90.0f;
        }
        else
        {
            yaw = std::atan2(forward.y, forward.x) * kRadToDeg;
            if (yaw < 0.0f) yaw += 360.0f;
            const float tmp = std::sqrt(forward.x * forward.x + forward.y * forward.y);
            pitch = std::atan2(-forward.z, tmp) * kRadToDeg;
            if (pitch < 0.0f) pitch += 360.0f;
        }
        return { pitch, yaw, 0.0f };
    }

    // Rodrigues rotation of `value` around `axis` by `degrees`.
    inline Vec3 RotateAroundAxis(const Vec3& value, const Vec3& axis, float degrees)
    {
        Vec3 n = axis;
        if (Normalize(n) <= 0.000001f)
            return value;
        const float c = std::cos(degrees * kDegToRad), s = std::sin(degrees * kDegToRad);
        return value * c + Cross(n, value) * s + n * (Dot(n, value) * (1.0f - c));
    }

    inline float NormalizeDegrees(float degrees)
    {
        if (!std::isfinite(degrees))
            return 0.0f;
        degrees = std::fmod(degrees, 360.0f);
        if (degrees > 180.0f) degrees -= 360.0f;
        if (degrees < -180.0f) degrees += 360.0f;
        return degrees;
    }

    // Shortest signed difference a - b in degrees.
    inline float AngleDelta(float a, float b) { return NormalizeDegrees(a - b); }

    // Same contract as the client's NormalizeAndClampViewAngles: wrap pitch and
    // yaw into [-180, 180], clamp pitch to +-89 and drop roll.
    inline Angles NormalizeAndClampViewAngles(Angles angles)
    {
        angles.x = std::clamp(NormalizeDegrees(angles.x), -89.0f, 89.0f);
        angles.y = NormalizeDegrees(angles.y);
        angles.z = 0.0f;
        return angles;
    }

    inline void ClampMagnitude(Vec3& value, float maximum)
    {
        if (!value.Finite() || !(maximum > 0.0f))
        {
            value = {};
            return;
        }
        const float length = value.Length();
        if (std::isfinite(length) && length > maximum && length > 0.0001f)
            value *= maximum / length;
    }
}

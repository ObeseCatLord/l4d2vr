#pragma once
#include "vr_hands/vr_hand_math.h"

namespace l4d2vr_ammo_grip
{
    struct Pose
    {
        VrHandMatrix4 orientationLocal{};
        Vector centerOffsetLocal{0.0f, 0.0f, 0.0f};
    };
    inline Vector Origin(const VrHandMatrix4& matrix)
    {
        return Vector(VrHandMath::Get(matrix, 0, 3), VrHandMath::Get(matrix, 1, 3), VrHandMath::Get(matrix, 2, 3));
    }
    inline Vector Axis(const VrHandMatrix4& matrix, int column)
    {
        return Vector(VrHandMath::Get(matrix, 0, column), VrHandMath::Get(matrix, 1, column), VrHandMath::Get(matrix, 2, column));
    }
    inline bool Finite(const Vector& value)
    {
        return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
    }
    inline bool Rigid(const VrHandMatrix4& matrix)
    {
        for (float value : matrix.m) if (!std::isfinite(value)) return false;
        const Vector x = Axis(matrix, 0), y = Axis(matrix, 1), z = Axis(matrix, 2);
        Vector cross; CrossProduct(x, y, cross);
        return std::fabs(x.LengthSqr() - 1.0f) < 0.002f && std::fabs(y.LengthSqr() - 1.0f) < 0.002f &&
            std::fabs(z.LengthSqr() - 1.0f) < 0.002f && std::fabs(VrHandMath::Dot(x, y)) < 0.002f &&
            std::fabs(VrHandMath::Dot(x, z)) < 0.002f && std::fabs(VrHandMath::Dot(y, z)) < 0.002f &&
            VrHandMath::Dot(cross, z) > 0.998f &&
            std::fabs(VrHandMath::Get(matrix, 3, 0)) < 0.0001f &&
            std::fabs(VrHandMath::Get(matrix, 3, 1)) < 0.0001f &&
            std::fabs(VrHandMath::Get(matrix, 3, 2)) < 0.0001f &&
            std::fabs(VrHandMath::Get(matrix, 3, 3) - 1.0f) < 0.0001f;
    }
    inline Vector Rotate(const VrHandMatrix4& matrix, const Vector& value)
    {
        return Axis(matrix, 0) * value.x + Axis(matrix, 1) * value.y + Axis(matrix, 2) * value.z;
    }
    inline VrHandMatrix4 Calibration(const Vector& degrees)
    {
        constexpr float radians = 3.14159265358979323846f / 180.0f;
        const float sx = std::sin(degrees.x * radians), cx = std::cos(degrees.x * radians);
        const float sy = std::sin(degrees.y * radians), cy = std::cos(degrees.y * radians);
        const float sz = std::sin(degrees.z * radians), cz = std::cos(degrees.z * radians);
        VrHandMatrix4 matrix = VrHandMath::Identity();
        VrHandMath::Set(matrix, 0, 0, cz * cy);
        VrHandMath::Set(matrix, 0, 1, cz * sy * sx - sz * cx);
        VrHandMath::Set(matrix, 0, 2, cz * sy * cx + sz * sx);
        VrHandMath::Set(matrix, 1, 0, sz * cy);
        VrHandMath::Set(matrix, 1, 1, sz * sy * sx + cz * cx);
        VrHandMath::Set(matrix, 1, 2, sz * sy * cx - cz * sx);
        VrHandMath::Set(matrix, 2, 0, -sy);
        VrHandMath::Set(matrix, 2, 1, cy * sx);
        VrHandMath::Set(matrix, 2, 2, cy * cx);
        return matrix;
    }
    // The rotation part of a pose with its axes made unit length and square
    // (Gram-Schmidt on x then y); false when it is degenerate or mirrored.
    inline bool RotationOf(const VrHandMatrix4& matrix, VrHandMatrix4& result)
    {
        for (int column = 0; column < 3; ++column)
            if (!Finite(Axis(matrix, column))) return false;
        Vector x = Axis(matrix, 0), y = Axis(matrix, 1);
        const Vector z0 = Axis(matrix, 2);
        if (x.Length() < 0.0001f || y.Length() < 0.0001f || z0.Length() < 0.0001f) return false;
        x = x * (1.0f / x.Length());
        y = y - x * VrHandMath::Dot(x, y);
        if (y.Length() < 0.0001f) return false;
        y = y * (1.0f / y.Length());
        Vector z; CrossProduct(x, y, z);
        if (VrHandMath::Dot(z, z0) <= 0.0f) return false;
        result = VrHandMath::Identity();
        for (int row = 0; row < 3; ++row)
        {
            VrHandMath::Set(result, row, 0, x[row]);
            VrHandMath::Set(result, row, 1, y[row]);
            VrHandMath::Set(result, row, 2, z[row]);
        }
        return Rigid(result);
    }
    // a^T * b for rotations: b expressed in a's frame.
    inline VrHandMatrix4 Relative(const VrHandMatrix4& a, const VrHandMatrix4& b)
    {
        VrHandMatrix4 result = VrHandMath::Identity();
        for (int row = 0; row < 3; ++row)
            for (int column = 0; column < 3; ++column)
                VrHandMath::Set(result, row, column, VrHandMath::Dot(Axis(a, row), Axis(b, column)));
        return result;
    }
    // Angle of the rotation that turns a into b, in degrees.
    inline float AngleBetweenDeg(const VrHandMatrix4& a, const VrHandMatrix4& b)
    {
        const float trace = VrHandMath::Dot(Axis(a, 0), Axis(b, 0)) +
            VrHandMath::Dot(Axis(a, 1), Axis(b, 1)) + VrHandMath::Dot(Axis(a, 2), Axis(b, 2));
        const float c = std::fmin(1.0f, std::fmax(-1.0f, (trace - 1.0f) * 0.5f));
        return std::acos(c) * 180.0f / 3.14159265358979323846f;
    }
    // Calibration() angles of a rotation (x, then y, then z, as Calibration
    // composes them).
    inline Vector CalibrationDegrees(const VrHandMatrix4& rotation)
    {
        constexpr float degrees = 180.0f / 3.14159265358979323846f;
        const float sy = std::fmin(1.0f, std::fmax(-1.0f, -VrHandMath::Get(rotation, 2, 0)));
        return Vector(std::atan2(VrHandMath::Get(rotation, 2, 1), VrHandMath::Get(rotation, 2, 2)) * degrees,
            std::asin(sy) * degrees,
            std::atan2(VrHandMath::Get(rotation, 1, 0), VrHandMath::Get(rotation, 0, 0)) * degrees);
    }
    // How the off hand holds a spare magazine or shell (controller frame:
    // x right, y up, z back). The natural hold is the hand turned as the gun
    // hand holds the gun, then raised `tiltDeg` nose-up (Pavlov's magazine
    // lean): there the spare sits exactly as it sits in the gun, the same
    // relation to the gun hand as `socketWorld` has now. `offsetDeg` turns it
    // further in the hand (ManualReloadMagazineHandRotationOffsetDeg).
    inline bool NaturalHold(const VrHandMatrix4& gunController, const VrHandMatrix4& socketWorld,
        float tiltDeg, const Vector& offsetDeg, VrHandMatrix4& result)
    {
        VrHandMatrix4 gun{}, socket{};
        if (!std::isfinite(tiltDeg) || !Finite(offsetDeg) ||
            !RotationOf(gunController, gun) || !RotationOf(socketWorld, socket)) return false;
        // Natural hand = gun hand * Rx(tilt); the spare in that hand = socket.
        const VrHandMatrix4 inGunHand = Relative(gun, socket);
        const VrHandMatrix4 inNaturalHand = VrHandMath::Multiply(Calibration(Vector(-tiltDeg, 0.0f, 0.0f)), inGunHand);
        VrHandMatrix4 hold = VrHandMath::Multiply(Calibration(offsetDeg), inNaturalHand);
        if (!RotationOf(hold, hold)) return false;
        result = hold; return true;
    }
    // A held magazine fits its well only facing the way it goes in: every
    // axis within the capture angle, with its sign (a magazine turned 180
    // degrees, backwards or upside down, does not seat).
    inline bool AxesAgree(const VrHandMatrix4& held, const VrHandMatrix4& socket, float minDot)
    {
        for (int column = 0; column < 3; ++column)
        {
            Vector a = Axis(held, column), b = Axis(socket, column);
            const float la = a.Length(), lb = b.Length();
            if (!(la > 0.0001f) || !(lb > 0.0001f)) return false;
            if (VrHandMath::Dot(a, b) / (la * lb) < minDot) return false;
        }
        return true;
    }
    inline bool Capture(const VrHandMatrix4& controller, const VrHandMatrix4& orientationLocal,
        const Vector& palmWorld, const Vector& boxCenterLocal, const Vector& gripPointLocal, Pose& result)
    {
        if (!Rigid(controller) || !Rigid(orientationLocal) || !Finite(palmWorld) ||
            !Finite(boxCenterLocal) || !Finite(gripPointLocal)) return false;
        Pose pose{};
        if (!RotationOf(orientationLocal, pose.orientationLocal)) return false;
        const auto orientation = VrHandMath::Multiply(controller, pose.orientationLocal);
        const Vector center = palmWorld + Rotate(orientation, boxCenterLocal - gripPointLocal);
        const Vector delta = center - Origin(controller);
        pose.centerOffsetLocal = Vector(VrHandMath::Dot(delta, Axis(controller, 0)),
            VrHandMath::Dot(delta, Axis(controller, 1)), VrHandMath::Dot(delta, Axis(controller, 2)));
        if (!Rigid(pose.orientationLocal) || !Finite(pose.centerOffsetLocal)) return false;
        result = pose; return true;
    }
    inline bool Capture(const VrHandMatrix4& controller, const Vector& rotationDegrees,
        const Vector& palmWorld, const Vector& boxCenterLocal, const Vector& gripPointLocal, Pose& result)
    {
        if (!Rigid(controller) || !Finite(rotationDegrees) || !Finite(palmWorld) ||
            !Finite(boxCenterLocal) || !Finite(gripPointLocal)) return false;
        Pose pose{};
        pose.orientationLocal = Calibration(rotationDegrees);
        const auto orientation = VrHandMath::Multiply(controller, pose.orientationLocal);
        const Vector center = palmWorld + Rotate(orientation, boxCenterLocal - gripPointLocal);
        const Vector delta = center - Origin(controller);
        pose.centerOffsetLocal = Vector(VrHandMath::Dot(delta, Axis(controller, 0)),
            VrHandMath::Dot(delta, Axis(controller, 1)), VrHandMath::Dot(delta, Axis(controller, 2)));
        if (!Rigid(pose.orientationLocal) || !Finite(pose.centerOffsetLocal)) return false;
        result = pose; return true;
    }
    inline bool Follow(const VrHandMatrix4& controller, const Pose& pose,
        const Vector& boxCenterLocal, VrHandMatrix4& result)
    {
        if (!Rigid(controller) || !Rigid(pose.orientationLocal) ||
            !Finite(pose.centerOffsetLocal) || !Finite(boxCenterLocal)) return false;
        auto world = VrHandMath::Multiply(controller, pose.orientationLocal);
        const Vector center = Origin(controller) + Rotate(controller, pose.centerOffsetLocal);
        const Vector origin = center - Rotate(world, boxCenterLocal);
        if (!Finite(origin)) return false;
        VrHandMath::Set(world, 0, 3, origin.x);
        VrHandMath::Set(world, 1, 3, origin.y);
        VrHandMath::Set(world, 2, 3, origin.z);
        result = world; return true;
    }
    inline bool BodyWorld(const Vector& forward, const Vector& right,
        const Vector& centerWorld, const Vector& boxCenterLocal, VrHandMatrix4& result)
    {
        if (!Finite(forward) || !Finite(right) || !Finite(centerWorld) || !Finite(boxCenterLocal)) return false;
        auto world = VrHandMath::Identity();
        // Source's forward/right/up frame has negative handedness. Model
        // vertices need forward/left/up so the mesh and its normals are intact.
        for (int row = 0; row < 3; ++row)
        {
            VrHandMath::Set(world, row, 0, forward[row]);
            VrHandMath::Set(world, row, 1, -right[row]);
        }
        if (!Rigid(world)) return false;
        const Vector origin = centerWorld - Rotate(world, boxCenterLocal);
        if (!Finite(origin)) return false;
        VrHandMath::Set(world, 0, 3, origin.x);
        VrHandMath::Set(world, 1, 3, origin.y);
        VrHandMath::Set(world, 2, 3, origin.z);
        result = world; return true;
    }
}

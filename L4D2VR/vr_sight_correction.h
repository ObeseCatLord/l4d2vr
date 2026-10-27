#pragma once
#include "vr_interaction_geometry.h"
#include <cmath>

// Crosshair-to-sights alignment. The viewmodel is drawn at the controller pose
// rotated by a per-weapon angle offset, while the aim ray (crosshair, aim line,
// bullets) follows the controller. Stock viewmodels point their bore slightly
// inward, so the sights and the crosshair disagree. Given the bore direction
// measured on the drawn model (weapon_bone +Z, in the viewmodel's own frame),
// solve the extra pitch/yaw offset that makes the drawn bore parallel to the
// aim ray. The sights sit just above the bore, so they then line up with the
// crosshair at range.
namespace l4d2vr_sight_correction
{
    struct Basis
    {
        Vector forward{ 1.0f, 0.0f, 0.0f };
        Vector right{ 0.0f, -1.0f, 0.0f };
        Vector up{ 0.0f, 0.0f, 1.0f };
    };

    // Same rotation order as VR::UpdateTracking applies m_ViewmodelAngOffset:
    // yaw about up, then pitch about the yawed right, then roll about forward.
    inline Basis RotateBasis(const Basis& controller, const QAngle& offset)
    {
        Basis b = controller;
        b.forward = VectorRotate(b.forward, b.up, offset.y);
        b.right = VectorRotate(b.right, b.up, offset.y);
        b.forward = VectorRotate(b.forward, b.right, offset.x);
        b.up = VectorRotate(b.up, b.right, offset.x);
        b.right = VectorRotate(b.right, b.forward, offset.z);
        b.up = VectorRotate(b.up, b.forward, offset.z);
        return b;
    }

    // boreLocal components are in the viewmodel's frame: x forward, y left, z up.
    inline Vector BoreWorld(const Basis& viewmodel, const Vector& boreLocal)
    {
        return viewmodel.forward * boreLocal.x - viewmodel.right * boreLocal.y + viewmodel.up * boreLocal.z;
    }

    inline Vector ToLocal(const Basis& frame, const Vector& world)
    {
        return Vector(DotProduct(world, frame.forward), -DotProduct(world, frame.right), DotProduct(world, frame.up));
    }

    inline Vector Unit(const Vector& v)
    {
        const float length = std::sqrt(DotProduct(v, v));
        if (!std::isfinite(length) || length < 1.0e-6f)
            return Vector(0.0f, 0.0f, 0.0f);
        return v * (1.0f / length);
    }

    inline float AngleBetweenDeg(const Vector& a, const Vector& b)
    {
        const Vector ua = Unit(a), ub = Unit(b);
        const float d = std::fmax(-1.0f, std::fmin(1.0f, DotProduct(ua, ub)));
        return std::acos(d) * 57.29577951308232f;
    }

    // Returns the extra (pitch, yaw) to add to baseOffset so the bore drawn with
    // the corrected offset points along aimLocal (aim direction expressed in the
    // controller frame: x forward, y left, z up). Roll is left unchanged.
    inline bool Solve(const QAngle& baseOffset, const Vector& boreLocal, const Vector& aimLocal,
        QAngle& correction, float maxCorrectionDeg = 20.0f)
    {
        correction = QAngle(0.0f, 0.0f, 0.0f);
        const Vector bore = Unit(boreLocal);
        const Vector aim = Unit(aimLocal);
        if (DotProduct(bore, bore) < 0.5f || DotProduct(aim, aim) < 0.5f)
            return false;
        const Basis controller{};
        auto residual = [&](float pitch, float yaw, float out[3])
        {
            const QAngle offset(baseOffset.x + pitch, baseOffset.y + yaw, baseOffset.z);
            const Vector world = Unit(BoreWorld(RotateBasis(controller, offset), bore));
            // controller basis is the identity in its own frame (right = -y)
            out[0] = world.x - aim.x;
            out[1] = world.y - aim.y;
            out[2] = world.z - aim.z;
        };
        float pitch = 0.0f, yaw = 0.0f;
        for (int iteration = 0; iteration < 12; ++iteration)
        {
            float r[3], rp[3], ry[3];
            residual(pitch, yaw, r);
            const float h = 0.01f;
            residual(pitch + h, yaw, rp);
            residual(pitch, yaw + h, ry);
            float jp[3], jy[3];
            for (int i = 0; i < 3; ++i)
            {
                jp[i] = (rp[i] - r[i]) / h;
                jy[i] = (ry[i] - r[i]) / h;
            }
            // Normal equations for the 2-parameter least squares step.
            const float a11 = jp[0] * jp[0] + jp[1] * jp[1] + jp[2] * jp[2];
            const float a12 = jp[0] * jy[0] + jp[1] * jy[1] + jp[2] * jy[2];
            const float a22 = jy[0] * jy[0] + jy[1] * jy[1] + jy[2] * jy[2];
            const float g1 = jp[0] * r[0] + jp[1] * r[1] + jp[2] * r[2];
            const float g2 = jy[0] * r[0] + jy[1] * r[1] + jy[2] * r[2];
            const float det = a11 * a22 - a12 * a12;
            if (!std::isfinite(det) || std::fabs(det) < 1.0e-12f)
                return false;
            const float dp = -(a22 * g1 - a12 * g2) / det;
            const float dy = -(a11 * g2 - a12 * g1) / det;
            pitch += std::fmax(-10.0f, std::fmin(10.0f, dp));
            yaw += std::fmax(-10.0f, std::fmin(10.0f, dy));
            if (std::fabs(dp) < 1.0e-4f && std::fabs(dy) < 1.0e-4f)
                break;
        }
        if (!std::isfinite(pitch) || !std::isfinite(yaw) ||
            std::fabs(pitch) > maxCorrectionDeg || std::fabs(yaw) > maxCorrectionDeg)
            return false;
        float r[3];
        residual(pitch, yaw, r);
        if (std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]) > 0.01f)
            return false;
        correction = QAngle(pitch, yaw, 0.0f);
        return true;
    }

    // Running mean of bore samples gathered while the weapon is settled.
    struct BoreAverage
    {
        Vector sum{ 0.0f, 0.0f, 0.0f };
        int count = 0;
        void Add(const Vector& bore)
        {
            const Vector u = Unit(bore);
            if (DotProduct(u, u) < 0.5f)
                return;
            sum += u;
            ++count;
        }
        Vector Mean() const { return Unit(sum); }
        void Reset() { sum = Vector(0.0f, 0.0f, 0.0f); count = 0; }
    };
}

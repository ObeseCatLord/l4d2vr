#pragma once
#include "sdk/vector.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>

// Per-weapon iron/optic sight line expressed in the weapon root bone's local
// frame, plus the world-space helpers that turn it into a sight ray, a bullet
// ray converging at the zero distance, an aim-down-sights test and an in-VR
// calibration solve. All positions are game units unless a name says meters.
namespace l4d2vr_sight
{
    inline constexpr float kPi = 3.14159265358979323846f;
    inline constexpr float kGameUnitsPerMeter = 39.3700787f; // 1 unit = 1 inch
    inline constexpr float kEpsilon = 0.0001f;

    inline bool Finite(const Vector& v)
    { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

    inline bool Normalized(const Vector& v, Vector& out)
    {
        if (!Finite(v)) return false;
        const float length = VectorLength(v);
        if (!std::isfinite(length) || !(length > kEpsilon)) return false;
        out = v * (1.0f / length);
        return Finite(out);
    }

    // local -> world: origin + x*axisX + y*axisY + z*axisZ (a 3x4 affine; axes
    // need not be unit length or orthogonal, but must be linearly independent).
    struct Affine
    {
        Vector origin{0.0f, 0.0f, 0.0f};
        Vector axisX{1.0f, 0.0f, 0.0f};
        Vector axisY{0.0f, 1.0f, 0.0f};
        Vector axisZ{0.0f, 0.0f, 1.0f};
    };

    inline float Determinant(const Affine& a)
    { return DotProduct(a.axisX, CrossProduct(a.axisY, a.axisZ)); }

    inline bool Valid(const Affine& a)
    {
        if (!Finite(a.origin) || !Finite(a.axisX) || !Finite(a.axisY) || !Finite(a.axisZ)) return false;
        const float scale = VectorLength(a.axisX) * VectorLength(a.axisY) * VectorLength(a.axisZ);
        const float det = Determinant(a);
        return std::isfinite(det) && std::isfinite(scale) && scale > kEpsilon && std::fabs(det) > 0.001f * scale;
    }

    inline bool TransformPoint(const Affine& a, const Vector& local, Vector& world)
    {
        if (!Valid(a) || !Finite(local)) return false;
        const Vector result = a.origin + a.axisX * local.x + a.axisY * local.y + a.axisZ * local.z;
        if (!Finite(result)) return false;
        world = result;
        return true;
    }

    inline bool TransformDirection(const Affine& a, const Vector& local, Vector& world)
    {
        if (!Valid(a) || !Finite(local)) return false;
        const Vector result = a.axisX * local.x + a.axisY * local.y + a.axisZ * local.z;
        if (!Finite(result)) return false;
        world = result;
        return true;
    }

    // world -> local via the inverse of the 3x3 part (Cramer's rule).
    inline bool InverseTransformPoint(const Affine& a, const Vector& world, Vector& local)
    {
        if (!Valid(a) || !Finite(world)) return false;
        const float det = Determinant(a);
        const Vector d = world - a.origin;
        const Vector result(DotProduct(d, CrossProduct(a.axisY, a.axisZ)) / det,
            DotProduct(a.axisX, CrossProduct(d, a.axisZ)) / det,
            DotProduct(a.axisX, CrossProduct(a.axisY, d)) / det);
        if (!Finite(result)) return false;
        local = result;
        return true;
    }

    struct SightAxis
    {
        Vector rearLocal{0.0f, 0.0f, 0.0f};
        Vector frontLocal{0.0f, 0.0f, 0.0f};
        float zeroDistance = 0.0f; // game units from the rear sight
        bool valid = false;
    };

    inline bool Valid(const SightAxis& s)
    {
        if (!s.valid || !Finite(s.rearLocal) || !Finite(s.frontLocal)) return false;
        if (!std::isfinite(s.zeroDistance) || !(s.zeroDistance > 0.0f)) return false;
        const float length = VectorLength(s.frontLocal - s.rearLocal);
        return std::isfinite(length) && length > kEpsilon;
    }

    struct SightRay
    {
        Vector rear{0.0f, 0.0f, 0.0f};
        Vector front{0.0f, 0.0f, 0.0f};
        Vector direction{1.0f, 0.0f, 0.0f}; // unit, rear -> front
        float length = 0.0f;                // rear -> front distance
        bool valid = false;
    };

    struct Ray
    {
        Vector origin{0.0f, 0.0f, 0.0f};
        Vector direction{1.0f, 0.0f, 0.0f}; // unit
        bool valid = false;
    };

    inline SightRay MakeSightRay(const Vector& rearWorld, const Vector& frontWorld)
    {
        SightRay ray{};
        Vector direction(0.0f, 0.0f, 0.0f);
        if (!Finite(rearWorld) || !Normalized(frontWorld - rearWorld, direction)) return ray;
        ray.rear = rearWorld;
        ray.front = frontWorld;
        ray.direction = direction;
        ray.length = VectorLength(frontWorld - rearWorld);
        ray.valid = std::isfinite(ray.length);
        return ray;
    }

    inline SightRay WorldSightRay(const SightAxis& sight, const Affine& rootWorld)
    {
        Vector rear(0.0f, 0.0f, 0.0f), front(0.0f, 0.0f, 0.0f);
        if (!Valid(sight) || !TransformPoint(rootWorld, sight.rearLocal, rear) ||
            !TransformPoint(rootWorld, sight.frontLocal, front))
            return SightRay{};
        return MakeSightRay(rear, front);
    }

    // Bullet ray from the muzzle that crosses the sight line zeroDistance units
    // beyond the rear sight. An infinite zeroDistance gives a ray parallel to
    // the sight line. Fails when the muzzle sits on the zero point.
    inline Ray AimRayFromSight(const SightRay& sight, const Vector& muzzleWorld, float zeroDistance)
    {
        Ray ray{};
        if (!sight.valid || !Finite(muzzleWorld) || std::isnan(zeroDistance) || !(zeroDistance > 0.0f))
            return ray;
        Vector direction = sight.direction;
        if (std::isfinite(zeroDistance))
        {
            const Vector zeroPoint = sight.rear + sight.direction * zeroDistance;
            if (!Normalized(zeroPoint - muzzleWorld, direction)) return ray;
            if (DotProduct(direction, sight.direction) <= 0.0f) return ray; // zero point behind the muzzle
        }
        ray.origin = muzzleWorld;
        ray.direction = direction;
        ray.valid = true;
        return ray;
    }

    // Point on the sight line hitDistance units beyond the rear sight (where a
    // crosshair or laser dot should sit for a hit at that distance).
    inline bool CrosshairPointOnSight(const SightRay& sight, float hitDistance, Vector& point)
    {
        if (!sight.valid || !std::isfinite(hitDistance) || hitDistance < 0.0f) return false;
        const Vector result = sight.rear + sight.direction * hitDistance;
        if (!Finite(result)) return false;
        point = result;
        return true;
    }

    // Angle between two directions in degrees; NaN when either is degenerate.
    inline float AngularErrorDeg(const Vector& a, const Vector& b)
    {
        Vector na(0.0f, 0.0f, 0.0f), nb(0.0f, 0.0f, 0.0f);
        if (!Normalized(a, na) || !Normalized(b, nb)) return std::numeric_limits<float>::quiet_NaN();
        // atan2 of |cross| and dot is accurate for tiny and near-180 angles alike.
        const float s = VectorLength(CrossProduct(na, nb));
        const float c = DotProduct(na, nb);
        return std::atan2(s, c) * (180.0f / kPi);
    }

    struct SightAlignment
    {
        float behind = 0.0f;   // eye distance behind the rear sight along the axis (>0 = behind)
        float offset = 0.0f;   // perpendicular eye distance from the sight line (game units)
        float angleDeg = 0.0f; // angle between the eye->rear view line and the sight axis
        bool valid = false;
    };

    inline SightAlignment EvaluateSightAlignment(const Vector& eyeWorld, const SightRay& sight)
    {
        SightAlignment result{};
        if (!sight.valid || !Finite(eyeWorld)) return result;
        const Vector delta = eyeWorld - sight.rear;
        const float along = DotProduct(delta, sight.direction);
        const float offset = VectorLength(delta - sight.direction * along);
        const float angle = AngularErrorDeg(sight.rear - eyeWorld, sight.direction);
        if (!std::isfinite(along) || !std::isfinite(offset)) return result;
        result.behind = -along;
        result.offset = offset;
        result.angleDeg = std::isfinite(angle) ? angle : 180.0f;
        result.valid = true;
        return result;
    }

    // Eye is behind the rear sight, within maxOffsetMeters of the sight line
    // and looking along it within maxAngleDeg.
    inline bool IsAimingDownSights(const Vector& eyeWorld, const SightRay& sight,
        float maxOffsetMeters = 0.06f, float maxAngleDeg = 6.0f, float unitsPerMeter = kGameUnitsPerMeter)
    {
        if (!std::isfinite(maxOffsetMeters) || maxOffsetMeters < 0.0f || !std::isfinite(maxAngleDeg) ||
            maxAngleDeg < 0.0f || !std::isfinite(unitsPerMeter) || !(unitsPerMeter > 0.0f))
            return false;
        const SightAlignment a = EvaluateSightAlignment(eyeWorld, sight);
        return a.valid && a.behind > 0.0f && a.offset <= maxOffsetMeters * unitsPerMeter &&
            a.angleDeg <= maxAngleDeg;
    }

    struct CalibrationOptions
    {
        float sightLength = 8.0f;          // rear -> solved front distance (game units)
        float minTargetDistance = 120.0f;  // target must be at least this far beyond the rear sight
        float maxRearOffset = 3.0f;        // rear sight must lie this close to the eye->target line
    };

    // In-VR calibration: the user lines the sights up on a far target. The
    // eye->target line, moved to pass through the (trusted) rear sight, is the
    // corrected sight axis; returns a frontLocal on it sightLength units ahead.
    inline bool SolveFrontFromAlignment(const Vector& rearLocal, const Affine& rootWorld,
        const Vector& eyeWorld, const Vector& targetWorld, Vector& frontLocal,
        const CalibrationOptions& options = CalibrationOptions{})
    {
        if (!std::isfinite(options.sightLength) || !(options.sightLength > kEpsilon) ||
            !std::isfinite(options.minTargetDistance) || !std::isfinite(options.maxRearOffset) ||
            options.maxRearOffset < 0.0f)
            return false;
        Vector rearWorld(0.0f, 0.0f, 0.0f), direction(0.0f, 0.0f, 0.0f);
        if (!Finite(eyeWorld) || !Finite(targetWorld) || !TransformPoint(rootWorld, rearLocal, rearWorld) ||
            !Normalized(targetWorld - eyeWorld, direction))
            return false;
        const Vector eyeToRear = rearWorld - eyeWorld;
        const float rearAlong = DotProduct(eyeToRear, direction);
        if (!(rearAlong > 0.0f)) return false; // rear sight not in front of the eye
        if (VectorLength(eyeToRear - direction * rearAlong) > options.maxRearOffset) return false;
        if (!(DotProduct(targetWorld - rearWorld, direction) >= (std::max)(options.minTargetDistance, kEpsilon)))
            return false;
        Vector local(0.0f, 0.0f, 0.0f);
        if (!InverseTransformPoint(rootWorld, rearWorld + direction * options.sightLength, local)) return false;
        if (!(VectorLength(local - rearLocal) > kEpsilon)) return false;
        frontLocal = local;
        return true;
    }

    // Weapon id -> sight axis. Starts empty; the integrator fills it from
    // measured/calibrated data. Ids outside [0, kMaxWeaponId) are rejected.
    class SightTable
    {
    public:
        static constexpr int kMaxWeaponId = 128;

        bool Set(int weaponId, const SightAxis& sight)
        {
            if (!InRange(weaponId) || !Valid(sight)) return false;
            m_Entries[static_cast<std::size_t>(weaponId)] = sight;
            return true;
        }
        bool Remove(int weaponId)
        {
            if (!InRange(weaponId) || !m_Entries[static_cast<std::size_t>(weaponId)].valid) return false;
            m_Entries[static_cast<std::size_t>(weaponId)] = SightAxis{};
            return true;
        }
        const SightAxis* Find(int weaponId) const
        {
            if (!InRange(weaponId)) return nullptr;
            const SightAxis& entry = m_Entries[static_cast<std::size_t>(weaponId)];
            return entry.valid ? &entry : nullptr;
        }
        std::size_t Size() const
        {
            return static_cast<std::size_t>(std::count_if(m_Entries.begin(), m_Entries.end(),
                [](const SightAxis& s) { return s.valid; }));
        }
        void Clear() { m_Entries.fill(SightAxis{}); }

    private:
        static bool InRange(int weaponId) { return weaponId >= 0 && weaponId < kMaxWeaponId; }
        std::array<SightAxis, kMaxWeaponId> m_Entries{};
    };
}

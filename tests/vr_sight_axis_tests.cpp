#include <limits>
#if defined(VECTOR_PARANOIA) && !defined(VEC_T_NAN)
#define VEC_T_NAN std::numeric_limits<float>::quiet_NaN()
#endif
#ifdef _MSC_VER
#pragma warning(push, 0)
#endif
#include "../L4D2VR/sdk/vector.h"
#ifdef _MSC_VER
#pragma warning(pop)
#endif
#include "../L4D2VR/vr_sight_axis.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#define CHECK(v) do { if (!(v)) { std::fprintf(stderr, "Sight axis failed at %d\n", __LINE__); std::abort(); } } while(false)
static bool Near(float a, float b, float eps = 0.001f) { return std::fabs(a - b) <= eps; }
static bool Near(const Vector& a, const Vector& b, float eps = 0.001f)
{ return Near(a.x, b.x, eps) && Near(a.y, b.y, eps) && Near(a.z, b.z, eps); }
int main()
{
    using namespace l4d2vr_sight;
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();

    // 90 degree yaw: local +X maps to world +Y.
    Affine root{};
    root.origin = Vector(100.0f, 200.0f, 50.0f);
    root.axisX = Vector(0.0f, 1.0f, 0.0f);
    root.axisY = Vector(-1.0f, 0.0f, 0.0f);
    root.axisZ = Vector(0.0f, 0.0f, 1.0f);
    CHECK(Valid(root));
    Vector world(0.0f, 0.0f, 0.0f), local(0.0f, 0.0f, 0.0f);
    CHECK(TransformPoint(root, Vector(5.0f, 2.0f, 3.0f), world) && Near(world, Vector(98.0f, 205.0f, 53.0f)));
    CHECK(TransformDirection(root, Vector(1.0f, 0.0f, 0.0f), world) && Near(world, Vector(0.0f, 1.0f, 0.0f)));
    CHECK(InverseTransformPoint(root, Vector(98.0f, 205.0f, 53.0f), local) && Near(local, Vector(5.0f, 2.0f, 3.0f)));

    // Non-orthonormal (scaled, sheared) frame still round-trips.
    Affine skew{};
    skew.origin = Vector(-3.0f, 7.0f, 1.0f);
    skew.axisX = Vector(2.0f, 0.0f, 0.0f);
    skew.axisY = Vector(0.5f, 1.5f, 0.0f);
    skew.axisZ = Vector(0.0f, 0.3f, 0.8f);
    CHECK(TransformPoint(skew, Vector(1.0f, -2.0f, 4.0f), world));
    CHECK(InverseTransformPoint(skew, world, local) && Near(local, Vector(1.0f, -2.0f, 4.0f)));

    // Degenerate frames are rejected.
    Affine flat = root;
    flat.axisZ = Vector(0.0f, 0.0f, 0.0f);
    CHECK(!Valid(flat) && !TransformPoint(flat, Vector(1.0f, 0.0f, 0.0f), world));
    flat = root;
    flat.axisY = root.axisX * 2.0f;
    CHECK(!Valid(flat) && !InverseTransformPoint(flat, Vector(1.0f, 0.0f, 0.0f), local));
    flat = root;
    flat.origin.x = nan;
    CHECK(!Valid(flat));
    CHECK(!TransformPoint(root, Vector(nan, 0.0f, 0.0f), world));

    // Sight axis -> world sight ray.
    SightAxis sight{};
    sight.rearLocal = Vector(5.0f, 0.0f, 3.0f);
    sight.frontLocal = Vector(25.0f, 0.0f, 3.0f);
    sight.zeroDistance = 1000.0f;
    sight.valid = true;
    CHECK(Valid(sight));
    const SightRay ray = WorldSightRay(sight, root);
    CHECK(ray.valid && Near(ray.rear, Vector(100.0f, 205.0f, 53.0f)) && Near(ray.front, Vector(100.0f, 225.0f, 53.0f)));
    CHECK(Near(ray.direction, Vector(0.0f, 1.0f, 0.0f)) && Near(ray.length, 20.0f));

    SightAxis badSight = sight;
    badSight.frontLocal = badSight.rearLocal;
    CHECK(!Valid(badSight) && !WorldSightRay(badSight, root).valid);
    badSight = sight;
    badSight.zeroDistance = 0.0f;
    CHECK(!Valid(badSight));
    badSight = sight;
    badSight.valid = false;
    CHECK(!Valid(badSight));
    CHECK(!WorldSightRay(sight, flat).valid);

    // Bullet ray from a muzzle below the sight converges at the zero distance.
    const Vector muzzle = ray.rear + Vector(0.0f, 10.0f, -1.5f);
    const Ray bullet = AimRayFromSight(ray, muzzle, sight.zeroDistance);
    CHECK(bullet.valid && Near(VectorLength(bullet.direction), 1.0f));
    const Vector zeroPoint = ray.rear + ray.direction * sight.zeroDistance;
    const float toZero = VectorLength(zeroPoint - muzzle);
    CHECK(Near(bullet.origin + bullet.direction * toZero, zeroPoint, 0.01f));
    // Below the line before the zero, above it after.
    CHECK((bullet.origin + bullet.direction * (toZero * 0.5f)).z < ray.rear.z);
    CHECK((bullet.origin + bullet.direction * (toZero * 2.0f)).z > ray.rear.z);
    // Infinite zero is a parallel ray.
    const Ray parallel = AimRayFromSight(ray, muzzle, inf);
    CHECK(parallel.valid && Near(parallel.direction, ray.direction));
    CHECK(!AimRayFromSight(ray, muzzle, 0.0f).valid && !AimRayFromSight(ray, muzzle, nan).valid);
    CHECK(!AimRayFromSight(ray, zeroPoint, sight.zeroDistance).valid);  // muzzle on the zero point
    CHECK(!AimRayFromSight(ray, zeroPoint + ray.direction * 5.0f, sight.zeroDistance).valid); // zero behind muzzle
    CHECK(!AimRayFromSight(SightRay{}, muzzle, 1000.0f).valid);

    Vector point(0.0f, 0.0f, 0.0f);
    CHECK(CrosshairPointOnSight(ray, 500.0f, point) && Near(point, Vector(100.0f, 705.0f, 53.0f)));
    CHECK(!CrosshairPointOnSight(ray, -1.0f, point) && !CrosshairPointOnSight(ray, nan, point));

    // Aiming down sights: behind the rear sight, near the line, looking along it.
    CHECK(IsAimingDownSights(ray.rear - ray.direction * 3.0f, ray));
    CHECK(IsAimingDownSights(ray.rear - ray.direction * 30.0f + Vector(1.5f, 0.0f, 0.0f), ray));
    CHECK(!IsAimingDownSights(ray.rear - ray.direction * 60.0f + Vector(0.0f, 0.0f, 3.0f), ray)); // offset > 6 cm
    CHECK(!IsAimingDownSights(ray.rear - ray.direction * 10.0f + Vector(2.0f, 0.0f, 0.0f), ray)); // angle > 6 deg
    CHECK(!IsAimingDownSights(ray.rear + ray.direction * 3.0f, ray));                              // in front of rear
    CHECK(IsAimingDownSights(ray.rear - ray.direction * 10.0f + Vector(2.0f, 0.0f, 0.0f), ray, 0.06f, 15.0f));
    CHECK(!IsAimingDownSights(Vector(nan, 0.0f, 0.0f), ray) && !IsAimingDownSights(ray.rear, SightRay{}));
    CHECK(!IsAimingDownSights(ray.rear - ray.direction * 3.0f, ray, nan));
    const SightAlignment alignment = EvaluateSightAlignment(ray.rear - ray.direction * 30.0f + Vector(1.5f, 0.0f, 0.0f), ray);
    CHECK(alignment.valid && Near(alignment.behind, 30.0f) && Near(alignment.offset, 1.5f));
    CHECK(Near(alignment.angleDeg, std::atan2(1.5f, 30.0f) * 180.0f / kPi, 0.01f));

    // Angular error.
    CHECK(Near(AngularErrorDeg(Vector(1.0f, 0.0f, 0.0f), Vector(0.0f, 2.0f, 0.0f)), 90.0f));
    CHECK(Near(AngularErrorDeg(Vector(1.0f, 0.0f, 0.0f), Vector(-3.0f, 0.0f, 0.0f)), 180.0f));
    CHECK(Near(AngularErrorDeg(Vector(1.0f, 1.0f, 0.0f), Vector(1.0f, 1.0f, 0.0f)), 0.0f));
    CHECK(std::isnan(AngularErrorDeg(Vector(0.0f, 0.0f, 0.0f), Vector(1.0f, 0.0f, 0.0f))));

    // Calibration: an arbitrarily rotated root and a known (tilted) sight axis.
    const Vector k(0.267261f, 0.534522f, 0.801784f);
    Affine rotated{};
    rotated.origin = Vector(-500.0f, 1200.0f, 64.0f);
    rotated.axisX = VectorRotate(Vector(1.0f, 0.0f, 0.0f), k, 37.0f);
    rotated.axisY = VectorRotate(Vector(0.0f, 1.0f, 0.0f), k, 37.0f);
    rotated.axisZ = VectorRotate(Vector(0.0f, 0.0f, 1.0f), k, 37.0f);
    CHECK(Valid(rotated));
    SightAxis truth{};
    truth.rearLocal = Vector(4.0f, 0.2f, 3.5f);
    truth.frontLocal = Vector(22.0f, -0.3f, 3.9f);
    truth.zeroDistance = 900.0f;
    truth.valid = true;
    const SightRay trueRay = WorldSightRay(truth, rotated);
    CHECK(trueRay.valid);
    const Vector eye = trueRay.rear - trueRay.direction * 4.0f;
    const Vector target = trueRay.rear + trueRay.direction * 3000.0f;
    Vector solvedFront(0.0f, 0.0f, 0.0f);
    CHECK(SolveFrontFromAlignment(truth.rearLocal, rotated, eye, target, solvedFront));
    CHECK(AngularErrorDeg(solvedFront - truth.rearLocal, truth.frontLocal - truth.rearLocal) < 0.05f);
    CHECK(Near(VectorLength(solvedFront - truth.rearLocal), CalibrationOptions{}.sightLength, 0.01f));
    // A misaligned initial guess is corrected: start from a front 2 degrees off.
    SightAxis guess = truth;
    guess.frontLocal = truth.rearLocal + VectorRotate(truth.frontLocal - truth.rearLocal, Vector(0.0f, 0.0f, 1.0f), 2.0f);
    CHECK(AngularErrorDeg(WorldSightRay(guess, rotated).direction, trueRay.direction) > 1.9f);
    guess.frontLocal = solvedFront;
    CHECK(AngularErrorDeg(WorldSightRay(guess, rotated).direction, trueRay.direction) < 0.05f);
    // Eye slightly off the rear (head-tracking noise) still yields the eye->target line.
    const Vector noisyEye = eye + Vector(0.0f, 0.0f, 0.5f);
    CHECK(SolveFrontFromAlignment(truth.rearLocal, rotated, noisyEye, target, solvedFront));
    Vector solvedFrontWorld(0.0f, 0.0f, 0.0f), rearWorld(0.0f, 0.0f, 0.0f);
    CHECK(TransformPoint(rotated, solvedFront, solvedFrontWorld) && TransformPoint(rotated, truth.rearLocal, rearWorld));
    CHECK(AngularErrorDeg(solvedFrontWorld - rearWorld, target - noisyEye) < 0.01f);

    // Degenerate calibration input is rejected.
    CHECK(!SolveFrontFromAlignment(truth.rearLocal, rotated, eye, eye, solvedFront));            // no direction
    CHECK(!SolveFrontFromAlignment(truth.rearLocal, rotated, eye, trueRay.rear + trueRay.direction * 50.0f, solvedFront)); // target too close
    CHECK(!SolveFrontFromAlignment(truth.rearLocal, rotated, eye, eye - trueRay.direction * 3000.0f, solvedFront));        // target behind
    CHECK(!SolveFrontFromAlignment(truth.rearLocal, rotated, eye + Vector(0.0f, 0.0f, 20.0f),
        target + Vector(0.0f, 0.0f, 20.0f), solvedFront));                                       // rear off the line
    CHECK(!SolveFrontFromAlignment(truth.rearLocal, flat, eye, target, solvedFront));
    CHECK(!SolveFrontFromAlignment(Vector(nan, 0.0f, 0.0f), rotated, eye, target, solvedFront));
    CHECK(!SolveFrontFromAlignment(truth.rearLocal, rotated, Vector(inf, 0.0f, 0.0f), target, solvedFront));
    CalibrationOptions badOptions{};
    badOptions.sightLength = 0.0f;
    CHECK(!SolveFrontFromAlignment(truth.rearLocal, rotated, eye, target, solvedFront, badOptions));

    // Weapon id table starts empty and validates entries.
    SightTable table;
    CHECK(table.Size() == 0u && table.Find(35) == nullptr);
    CHECK(table.Set(35, sight) && table.Find(35) != nullptr && table.Size() == 1u);
    CHECK(Near(table.Find(35)->frontLocal, sight.frontLocal));
    CHECK(!table.Set(36, badSight) && table.Find(36) == nullptr);
    CHECK(!table.Set(-1, sight) && !table.Set(SightTable::kMaxWeaponId, sight));
    CHECK(table.Find(-1) == nullptr && table.Find(1000) == nullptr);
    CHECK(table.Remove(35) && !table.Remove(35) && table.Size() == 0u);
    CHECK(table.Set(6, sight) && table.Set(10, sight));
    table.Clear();
    CHECK(table.Size() == 0u);
    std::puts("vr_sight_axis tests passed");
    return 0;
}

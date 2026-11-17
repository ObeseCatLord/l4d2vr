// 2.0.12 (review s12 issue 5): shells are pinched between the thumb and index
// tips, the same in both hands (Pavlov grip SPEC 3.1 / 7.7).
#include <limits>
#if defined(VECTOR_PARANOIA) && !defined(VEC_T_NAN)
#define VEC_T_NAN std::numeric_limits<float>::quiet_NaN()
#endif
#ifdef _MSC_VER
#pragma warning(push, 0)
#endif
#include "../L4D2VR/vr_hands/vr_hand_math.h"
#ifdef _MSC_VER
#pragma warning(pop)
#endif
#include "../L4D2VR/vr_ammo_grip.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>

static int g_checks = 0;
static void CheckImpl(bool ok, int line)
{
    ++g_checks;
    if (!ok) { std::fprintf(stderr, "vr_shell_pinch_tests failed at line %d\n", line); std::exit(1); }
}
#define Check(value) CheckImpl((value), __LINE__)
static bool Near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }

using namespace l4d2vr_ammo_grip;

static PinchBones RightHand()
{
    // A right hand (x right, y up, z back; units = cm): thumb on the left of
    // the index, both reaching forward (-z).
    PinchBones h{};
    h.thumb[0] = Vector(-2.0f, -1.0f, -3.0f); h.thumb[1] = Vector(-2.5f, 0.0f, -6.0f); h.thumb[2] = Vector(-2.0f, 0.5f, -8.0f);
    h.index[0] = Vector(1.0f, 1.0f, -8.0f); h.index[1] = Vector(0.5f, 1.0f, -11.0f); h.index[2] = Vector(0.0f, 0.5f, -13.0f);
    h.thumbValid = h.indexValid = true;
    return h;
}

static PinchBones Mirror(const PinchBones& r)
{
    PinchBones l = r;
    for (int i = 0; i < 3; ++i)
    {
        l.thumb[i].x = -r.thumb[i].x;
        l.index[i].x = -r.index[i].x;
    }
    return l;
}

int main()
{
    // Pinch points of mirrored hands are mirror images (|x| equal, sign flipped).
    Vector pr{}, ar{}, pl{}, al{};
    Check(PinchPoint(RightHand(), 0.6f, pr, ar));
    Check(PinchPoint(Mirror(RightHand()), 0.6f, pl, al));
    Check(Near(pr.x, -pl.x) && Near(pr.y, pl.y) && Near(pr.z, pl.z));
    Check(Near(ar.x, -al.x) && Near(ar.y, al.y) && Near(ar.z, al.z));
    // Between the extrapolated tips, ahead of the last joints.
    Check(pr.z < -8.0f && pr.z > -16.0f && pr.x > -2.5f && pr.x < 0.5f);
    PinchBones missing = RightHand();
    missing.indexValid = false;
    Check(!PinchPoint(missing, 0.6f, pr, ar));

    // The hold: the shell's long axis has no lateral component (identical for
    // the left and the right hand), crimp forward and 5 deg down, brass back.
    for (const Vector& outward : { Vector(0.0f, -1.0f, 0.0f), Vector(0.0f, 0.0f, 1.0f), Vector(-1.0f, 0.0f, 0.0f),
             Vector(0.0f, 0.2f, 0.9f) })
    {
        VrHandMatrix4 hold{};
        Check(ShellPinchHold(outward, 5.0f, hold));
        Check(Rigid(hold));
        const float ax = std::fabs(outward.x), ay = std::fabs(outward.y), az = std::fabs(outward.z);
        const int axis = (ay >= ax && ay >= az) ? 1 : (az >= ax && az >= ay) ? 2 : 0;
        Vector o(0.0f, 0.0f, 0.0f);
        o[axis] = (axis == 0 ? outward.x : axis == 1 ? outward.y : outward.z) < 0.0f ? -1.0f : 1.0f;
        const Vector brass = Rotate(hold, o);           // controller frame (x right, y up, z back)
        Check(Near(brass.x, 0.0f));
        Check(brass.z > 0.99f);                          // toward the palm
        Check(Near(std::asin(brass.y) * 180.0f / 3.14159265f, 5.0f, 0.01f)); // crimp 5 deg below forward
    }
    // Follow puts the shell centre exactly at the pinch, in either hand.
    {
        VrHandMatrix4 hold{};
        Check(ShellPinchHold(Vector(0.0f, -1.0f, 0.0f), 5.0f, hold));
        for (float side : { 1.0f, -1.0f })
        {
            VrHandMatrix4 controller = VrHandMath::Identity();
            VrHandMath::Set(controller, 0, 3, 20.0f * side);
            VrHandMath::Set(controller, 1, 3, 5.0f);
            VrHandMath::Set(controller, 2, 3, 40.0f);
            const Vector pinch(20.0f * side - 0.8f * side, 5.4f, 40.0f - 11.0f);
            const Vector centerLocal(0.0f, -3.5f, 0.0f);
            Pose pose{};
            Check(Capture(controller, hold, pinch, centerLocal, centerLocal, pose));
            VrHandMatrix4 world{};
            Check(Follow(controller, pose, centerLocal, world));
            const Vector centre = Origin(world) + Rotate(world, centerLocal);
            Check(Near(centre.x, pinch.x, 1e-3f) && Near(centre.y, pinch.y, 1e-3f) && Near(centre.z, pinch.z, 1e-3f));
        }
    }
    std::printf("vr_shell_pinch_tests: %d checks passed\n", g_checks);
    return 0;
}

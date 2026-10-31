#pragma once
#include <cmath>

// Mirrored viewmodel hands for an item held in the physical LEFT hand
// (ItemHandMirror; ambidextrous item hands or LeftHanded). The survivor
// viewmodel only has a right gun hand, so without this the left controller
// shows a right hand around the gun and the free right controller shows the
// model's left hand: the hands look swapped and the arms cross.
//
// The fix renders the mirror image of a normal right-handed viewmodel:
//   M  = reflection across the body's sagittal plane (through the eyes),
//   Pg = reflection across the gun's own centre plane (through the grip),
//   Ps = reflection across the free (right) controller's centre plane.
// The arm IK solves a normal right-handed pose in a mirrored world whose gun
// hand target is M*Pg*T (T = the gun hand target the game produced at the left
// controller) and whose free hand target is M*Ps*T'. Every bone of the result is
// then reflected by M and drawn with the triangle winding flipped. The rendered
// gun hand ends up at M*M*Pg*T = Pg*T: the same grip on the same gun, mirrored
// into a LEFT hand; the free hand ends up at Ps*T': a RIGHT hand around the
// right controller. Finger poses are bone-local and so come out mirrored too.
// The gun model itself is drawn reflected by Pg (it stays where it is).
//
// std-only so the algebra is unit-tested offline.
namespace l4d2vr_hand_mirror
{
    // Same layout as Source's matrix3x4_t / vr_vm_stabilize::Mat3x4:
    // rows are x/y/z, columns 0..2 the basis, column 3 the origin.
    struct Mat34
    {
        float m[3][4];
    };

    inline Mat34 Identity()
    {
        Mat34 out{};
        out.m[0][0] = out.m[1][1] = out.m[2][2] = 1.0f;
        return out;
    }

    // Reflection across the plane through `point` with normal `normal`
    // (need not be unit length). False when the normal is degenerate.
    inline bool Reflection(const float point[3], const float normal[3], Mat34& out)
    {
        const double len = std::sqrt(static_cast<double>(normal[0]) * normal[0] +
            static_cast<double>(normal[1]) * normal[1] + static_cast<double>(normal[2]) * normal[2]);
        if (!std::isfinite(len) || len < 1.0e-6)
            return false;
        for (int i = 0; i < 3; ++i)
            if (!std::isfinite(point[i]))
                return false;
        const double n[3] = { normal[0] / len, normal[1] / len, normal[2] / len };
        const double d = n[0] * point[0] + n[1] * point[1] + n[2] * point[2];
        for (int r = 0; r < 3; ++r)
        {
            for (int c = 0; c < 3; ++c)
                out.m[r][c] = static_cast<float>((r == c ? 1.0 : 0.0) - 2.0 * n[r] * n[c]);
            out.m[r][3] = static_cast<float>(2.0 * d * n[r]);
        }
        return true;
    }

    inline Mat34 Mul(const Mat34& a, const Mat34& b)
    {
        Mat34 out{};
        for (int r = 0; r < 3; ++r)
        {
            for (int c = 0; c < 4; ++c)
            {
                float v = a.m[r][0] * b.m[0][c] + a.m[r][1] * b.m[1][c] + a.m[r][2] * b.m[2][c];
                if (c == 3)
                    v += a.m[r][3];
                out.m[r][c] = v;
            }
        }
        return out;
    }

    inline float Determinant(const Mat34& a)
    {
        return a.m[0][0] * (a.m[1][1] * a.m[2][2] - a.m[1][2] * a.m[2][1]) -
            a.m[0][1] * (a.m[1][0] * a.m[2][2] - a.m[1][2] * a.m[2][0]) +
            a.m[0][2] * (a.m[1][0] * a.m[2][1] - a.m[1][1] * a.m[2][0]);
    }

    inline void TransformPoint(const Mat34& a, const float in[3], float out[3])
    {
        for (int r = 0; r < 3; ++r)
            out[r] = a.m[r][0] * in[0] + a.m[r][1] * in[1] + a.m[r][2] * in[2] + a.m[r][3];
    }

    // The mirrored-world IK target for a hand: body mirror * local mirror * target.
    // Proper (det +1) when the target is proper, so the solver can use it as is.
    inline Mat34 MirroredWorldTarget(const Mat34& bodyMirror, const Mat34& localMirror, const Mat34& target)
    {
        return Mul(bodyMirror, Mul(localMirror, target));
    }

    // Which plane the item hand is mirrored across.
    //   Weapon: the drawn weapon's own centre plane. Guns keep their sights on
    //     the aim ray and melee weapons keep the blade the server sweeps; the
    //     weapon itself does not move, only the hand changes side.
    //   Controller: the item-hand controller's centre plane. The whole
    //     right-handed hold is mirrored, its turn in the hand included (a
    //     grenade's hold correction, pills, a medkit; also empty hands).
    enum class Plane { Weapon, Controller };
    inline Plane PlaneForItem(bool isGun, bool isMelee, bool emptyHands)
    {
        return !emptyHands && (isGun || isMelee) ? Plane::Weapon : Plane::Controller;
    }

    // When to draw the mirrored hands.
    struct MirrorInput
    {
        bool enabled = true;            // ItemHandMirror
        bool vrActive = false;          // VR on, not mouse mode, first-person control
        bool itemInPhysicalLeft = false;
        bool dualPistols = false;       // akimbo: one model holds both guns, a single gun plane cannot mirror it
        bool fullArmIk = false;         // the arm IK path draws the arms (needed to re-solve the arms)
    };

    inline bool ShouldMirror(const MirrorInput& in)
    {
        return in.enabled && in.vrActive && in.itemInPhysicalLeft && !in.dualPistols && in.fullArmIk;
    }
}

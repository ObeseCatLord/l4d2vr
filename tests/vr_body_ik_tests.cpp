#include "../L4D2VR/vr_body_ik.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace
{
    using namespace l4d2vr_body_ik;

    int checks = 0;
    void Require(bool condition, const char* message)
    {
        ++checks;
        if (!condition)
        {
            std::fprintf(stderr, "FAILED: %s\n", message);
            std::exit(1);
        }
    }

    constexpr float kUpm = 39.37f;            // game units per metre
    constexpr float kUpper = 0.29f * kUpm;    // survivor-like segment lengths
    constexpr float kLower = 0.27f * kUpm;

    // Source axes: +X forward, +Y left, +Z up; right = -Y.
    BodyFrame SourceFrame()
    {
        BodyFrame f{};
        f.forward = { 1.0f, 0.0f, 0.0f };
        f.right = { 0.0f, -1.0f, 0.0f };
        f.up = { 0.0f, 0.0f, 1.0f };
        return f;
    }

    float Dist(Vec3 a, Vec3 b) { return Length(a - b); }

    ArmSolve Solve(int side, Vec3 shoulder, Vec3 target, const ArmConfig& c = ArmConfig{})
    {
        const BodyFrame f = SourceFrame();
        const Vec3 pole = ArmElbowPole(f, side, shoulder, target, kUpm);
        const Vec3 fallback = Normalized(f.right * static_cast<float>(side) - f.forward * 0.3f);
        return SolveTwoBoneArm(shoulder, target, kUpper, kLower, pole, fallback, c, kUpm);
    }

    void TestReach()
    {
        const Vec3 shoulder{ 0.0f, -0.18f * kUpm, 1.45f * kUpm };
        const Vec3 targets[] = {
            { 0.35f * kUpm, -0.20f * kUpm, 1.20f * kUpm },   // gun raised
            { 0.10f * kUpm, -0.25f * kUpm, 0.95f * kUpm },   // hanging
            { 0.45f * kUpm, -0.05f * kUpm, 1.55f * kUpm },   // above the shoulder
            { -0.20f * kUpm, -0.25f * kUpm, 1.00f * kUpm },  // behind the back
            { 0.05f * kUpm, -0.15f * kUpm, 1.30f * kUpm },   // close to the chest
        };
        for (const Vec3& t : targets)
        {
            const ArmSolve r = Solve(1, shoulder, t);
            Require(r.valid, "reachable target solves");
            Require(Dist(r.wrist, t) < 0.001f * kUpm, "wrist lands on a reachable target");
            Require(std::fabs(Dist(r.shoulder, r.elbow) - kUpper) < 0.001f * kUpm, "upper arm keeps its length");
            Require(std::fabs(Dist(r.elbow, r.wrist) - kLower) < 0.001f * kUpm, "forearm keeps its length");
            Require(r.stretch == 1.0f && !r.overStretched && r.shoulderShift == 0.0f, "no stretch within reach");
        }
    }

    void TestElbowPole()
    {
        const BodyFrame f = SourceFrame();
        const Vec3 shoulderR{ 0.0f, -0.18f * kUpm, 1.45f * kUpm };
        const Vec3 shoulderL{ 0.0f, 0.18f * kUpm, 1.45f * kUpm };
        // Gun raised in front: elbows below the reach line and outside the shoulders.
        const Vec3 tR{ 0.38f * kUpm, -0.12f * kUpm, 1.25f * kUpm };
        const Vec3 tL{ 0.38f * kUpm, 0.12f * kUpm, 1.25f * kUpm };
        const ArmSolve r = Solve(1, shoulderR, tR);
        const ArmSolve l = Solve(-1, shoulderL, tL);
        Require(r.valid && l.valid, "front reach solves");
        const Vec3 midR = (shoulderR + tR) * 0.5f, midL = (shoulderL + tL) * 0.5f;
        Require(Dot(r.elbow - midR, f.up) < 0.0f, "right elbow points down");
        Require(Dot(l.elbow - midL, f.up) < 0.0f, "left elbow points down");
        Require(Dot(r.elbow - midR, f.right) > 0.0f, "right elbow points out (right)");
        Require(Dot(l.elbow - midL, f.right) < 0.0f, "left elbow points out (left)");
        // Mirror symmetry of the two sides.
        Require(std::fabs(r.elbow.x - l.elbow.x) < 1e-3f && std::fabs(r.elbow.y + l.elbow.y) < 1e-3f &&
                std::fabs(r.elbow.z - l.elbow.z) < 1e-3f, "left and right elbows mirror each other");

        // Hand raised above the head: the elbow goes out to the side, not down.
        const Vec3 up{ 0.15f * kUpm, -0.22f * kUpm, 1.85f * kUpm };
        const ArmSolve u = Solve(1, shoulderR, up);
        Require(u.valid, "overhead reach solves");
        const Vec3 midU = (shoulderR + up) * 0.5f;
        Require(Dot(u.elbow - midU, f.right) > 0.0f, "overhead elbow points out");

        // Behind the back: elbow out and back.
        const Vec3 back{ -0.22f * kUpm, -0.20f * kUpm, 1.05f * kUpm };
        const ArmSolve b = Solve(1, shoulderR, back);
        Require(b.valid, "behind-the-back reach solves");
        Require(Dot(b.elbow - shoulderR, f.right) > 0.0f, "behind-back elbow stays outside the shoulder");

        // Hand straight below the shoulder (pole along the reach): continuous,
        // no flip between neighbouring targets.
        const Vec3 below1{ 0.0f, -0.18f * kUpm, 1.0f * kUpm };
        const Vec3 below2{ 0.01f * kUpm, -0.18f * kUpm, 1.0f * kUpm };
        const ArmSolve d1 = Solve(1, shoulderR, below1);
        const ArmSolve d2 = Solve(1, shoulderR, below2);
        Require(d1.valid && d2.valid, "straight-down reach solves");
        Require(Dist(d1.elbow, d2.elbow) < 0.03f * kUpm, "elbow is continuous through the degenerate pole");
    }

    void TestStretchClamp()
    {
        const Vec3 shoulder{ 0.0f, -0.18f * kUpm, 1.45f * kUpm };
        const float reach = (kUpper + kLower);
        ArmConfig c{};
        // 5% beyond reach: stretch only.
        {
            const Vec3 t = shoulder + Vec3{ reach * 1.05f, 0.0f, 0.0f };
            const ArmSolve r = Solve(1, shoulder, t, c);
            Require(r.valid && Dist(r.wrist, t) < 0.001f * kUpm, "slightly out of reach: wrist still on the hand");
            Require(r.stretch > 1.0f && r.stretch <= c.maxStretch + 1e-4f, "stretch within the limit");
            Require(r.shoulderShift == 0.0f && !r.overStretched, "no shoulder shift for a small overreach");
        }
        // Past the stretch limit: shoulder shifts, then the arm stretches more, never detaching.
        {
            const Vec3 t = shoulder + Vec3{ reach * 1.30f, 0.0f, 0.0f };
            const ArmSolve r = Solve(1, shoulder, t, c);
            Require(r.valid, "far out of reach still solves");
            Require(Dist(r.wrist, t) < 0.001f * kUpm, "far out of reach: the wrist never detaches from the hand");
            Require(r.shoulderShift > 0.0f && r.shoulderShift <= c.maxShoulderShiftMeters * kUpm + 1e-3f,
                "shoulder shift is bounded");
            Require(r.overStretched, "flags the over-stretch");
            Require(std::fabs(Dist(r.shoulder, r.elbow) / kUpper - r.stretch) < 1e-3f, "both segments scale together");
        }
        // Within stretch + shift: no over-stretch flag.
        {
            const Vec3 t = shoulder + Vec3{ reach * c.maxStretch + 0.03f * kUpm, 0.0f, 0.0f };
            const ArmSolve r = Solve(1, shoulder, t, c);
            Require(r.valid && !r.overStretched && r.shoulderShift > 0.0f, "shoulder shift absorbs the rest");
            Require(std::fabs(r.stretch - c.maxStretch) < 1e-4f, "stretch saturates at the limit");
            Require(Dist(r.wrist, t) < 0.001f * kUpm, "wrist on the hand with shoulder shift");
        }
        // Absurd target (bad tracking): refused.
        {
            const Vec3 t = shoulder + Vec3{ reach * 3.0f, 0.0f, 0.0f };
            Require(!Solve(1, shoulder, t, c).valid, "absurd reach is refused");
        }
        // Degenerate input.
        Require(!SolveTwoBoneArm(shoulder, shoulder, kUpper, kLower, { 0, 0, -1 }, { 0, -1, 0 }, c, kUpm).valid,
            "zero-distance target is refused");
        Require(!SolveTwoBoneArm(shoulder, shoulder + Vec3{ 5, 0, 0 }, 0.0f, kLower, { 0, 0, -1 }, { 0, -1, 0 }, c, kUpm).valid,
            "zero-length bone is refused");
    }

    void TestYawFollow()
    {
        BodyYawConfig c{};
        BodyYawState s{};
        const float dt = 1.0f / 90.0f;
        Require(UpdateBodyYaw(s, c, 10.0f, dt) == 10.0f, "first update snaps to the head");
        // Inside the deadband the torso stays put.
        for (int i = 0; i < 180; ++i)
            UpdateBodyYaw(s, c, 10.0f + c.deadbandDeg - 2.0f, dt);
        Require(std::fabs(s.bodyYawDeg - 10.0f) < 1e-4f, "torso holds inside the deadband");
        // Past the deadband the torso turns smoothly until the head is centred.
        float prev = s.bodyYawDeg;
        const float head = 10.0f + c.deadbandDeg + 15.0f;
        bool monotonic = true;
        float firstStep = -1.0f;
        for (int i = 0; i < 270; ++i)
        {
            const float y = UpdateBodyYaw(s, c, head, dt);
            if (y < prev - 1e-4f)
                monotonic = false;
            if (firstStep < 0.0f)
                firstStep = y - prev;
            prev = y;
        }
        Require(monotonic, "smooth follow never overshoots backwards");
        Require(firstStep > 0.0f && firstStep < 5.0f, "smooth follow, not a snap");
        Require(std::fabs(WrapDeg(head - s.bodyYawDeg)) <= c.releaseDeg + 0.01f, "torso ends under the head");
        Require(!s.turning, "turn ends once centred");
        // A fast head turn never leaves the torso more than maxLag behind.
        BodyYawState f{};
        UpdateBodyYaw(f, c, 0.0f, dt);
        UpdateBodyYaw(f, c, 120.0f, dt);
        Require(std::fabs(WrapDeg(120.0f - f.bodyYawDeg)) <= c.maxLagDeg + 1e-3f, "hard lag limit");
        // Wrap-around at +-180.
        BodyYawState w{};
        UpdateBodyYaw(w, c, 170.0f, dt);
        for (int i = 0; i < 270; ++i)
            UpdateBodyYaw(w, c, -150.0f, dt);
        Require(std::fabs(WrapDeg(-150.0f - w.bodyYawDeg)) <= c.releaseDeg + 0.01f, "follows across the +-180 seam");

        // A hand straight out to the right turns the torso right at once
        // (Source yaw: right = smaller yaw), even with the head inside the deadband.
        BodyYawState h{};
        UpdateBodyYaw(h, c, 0.0f, dt);
        BodyYawHand right[2]{};
        right[1] = { true, 0.0f, 0.65f };
        for (int i = 0; i < 9; ++i)   // 0.1 s
            UpdateBodyYaw(h, c, 0.0f, dt, right, 2);
        Require(h.bodyYawDeg < -10.0f, "hand out to the side turns the torso quickly");
        Require(h.bodyYawDeg >= -c.maxLagDeg - 1e-3f, "hand turn respects the lag limit");
        // Both hands out to opposite sides cancel; a holster reach does not turn.
        BodyYawState t{};
        UpdateBodyYaw(t, c, 0.0f, dt);
        BodyYawHand both[2] = { { true, 0.0f, -0.65f }, { true, 0.0f, 0.65f } };
        for (int i = 0; i < 30; ++i)
            UpdateBodyYaw(t, c, 0.0f, dt, both, 2);
        Require(std::fabs(t.bodyYawDeg) < 1e-3f, "symmetric arms out do not turn the torso");
        BodyYawHand holster[2] = { { false }, { true, -0.05f, 0.22f } };
        for (int i = 0; i < 30; ++i)
            UpdateBodyYaw(t, c, 0.0f, dt, holster, 2);
        Require(std::fabs(t.bodyYawDeg) < 1e-3f, "hip holster reach does not turn the torso");
        BodyYawHand behind[2] = { { false }, { true, -0.55f, 0.15f } };
        for (int i = 0; i < 30; ++i)
            UpdateBodyYaw(t, c, 0.0f, dt, behind, 2);
        Require(std::fabs(t.bodyYawDeg) < 1e-3f, "reach behind the back does not turn the torso");
    }

    void TestRotationBetween()
    {
        {
            // Half of a 90 degree swing is 45 degrees about the same axis.
            float m[3][3];
            RotationBetween({ 1, 0, 0 }, { 0, 1, 0 }, m, 0.5f);
            const Vec3 r = Apply(m, { 1, 0, 0 });
            Require(Dist(r, Normalized(Vec3{ 1, 1, 0 })) < 1e-4f, "fractional rotation turns half way");
        }
        const Vec3 pairs[][2] = {
            { { 1, 0, 0 }, { 0, 1, 0 } },
            { { 0, 0, 1 }, { 0.3f, -0.4f, 0.866f } },
            { { 1, 0, 0 }, { -1, 0, 0 } },
            { { 0.2f, 0.9f, 0.1f }, { 0.2f, 0.9f, 0.1f } },
        };
        for (const auto& p : pairs)
        {
            float m[3][3];
            RotationBetween(p[0], p[1], m);
            const Vec3 r = Apply(m, Normalized(p[0]));
            Require(Dist(r, Normalized(p[1])) < 1e-4f, "rotation maps a onto b");
            // Orthonormal.
            const Vec3 x = Apply(m, { 1, 0, 0 }), y = Apply(m, { 0, 1, 0 });
            Require(std::fabs(Length(x) - 1.0f) < 1e-4f && std::fabs(Dot(x, y)) < 1e-4f, "rotation is orthonormal");
        }
    }
}

int main()
{
    TestReach();
    TestElbowPole();
    TestStretchClamp();
    TestYawFollow();
    TestRotationBetween();
    std::printf("vr_body_ik_tests: %d checks passed\n", checks);
    return 0;
}

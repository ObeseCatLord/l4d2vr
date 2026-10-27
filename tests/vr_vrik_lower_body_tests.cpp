#include "../L4D2VR/vr_vrik_lower_body.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace
{
    using namespace l4d2vr_vrik;
    namespace lower = l4d2vr_vrik::lower;
    using lower::Config;
    using lower::State;
    using lower::HeightCalibration;
    using lower::AnimatedReference;
    using lower::CrouchResult;
    using lower::ComputeCrouch;
    using lower::SpineBendForDrop;
    using lower::SpineBendAxis;
    using lower::Rigid;
    using lower::RotationAbout;
    using lower::Translation;
    using lower::Apply;
    using lower::MapFrame;
    using lower::ToMatrix34;
    using lower::MatrixOrigin;
    using lower::MatrixRotation;
    using lower::LegSolution;
    using lower::SolvePlantedLeg;

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

    bool Near(float a, float b, float tolerance) { return std::fabs(a - b) <= tolerance; }
    bool Near(Vec3 a, Vec3 b, float tolerance) { return Length(a - b) <= tolerance; }

    void TestHeightCalibration()
    {
        const Config config{};
        HeightCalibration calibration;
        Require(!calibration.Update(5.0f, 0.01f, config) && !calibration.valid, "Too-low eye sample is ignored");
        Require(!calibration.Update(NAN, 0.01f, config) && !calibration.valid, "Non-finite eye sample is ignored");
        Require(calibration.Update(64.0f, 0.01f, config) && calibration.valid &&
            Near(calibration.standing, 64.0f, 1e-4f), "First sample calibrates instantly");

        for (int i = 0; i < 200; ++i)
            calibration.Update(64.0f, 0.011f, config);
        Require(Near(calibration.standing, 64.0f, 0.05f), "Standing still keeps the calibration");

        // One-frame upward spike is filtered, not latched.
        calibration.Update(100.0f, 0.011f, config);
        Require(calibration.standing > 64.0f && calibration.standing < 67.0f, "Single upward spike is attenuated");
        for (int i = 0; i < 400; ++i)
            calibration.Update(64.0f, 0.011f, config);
        Require(Near(calibration.standing, 64.0f, 0.2f), "Calibration recovers after a spike");

        // A real height increase (e.g. standing up straighter) is adopted.
        for (int i = 0; i < 200; ++i)
            calibration.Update(70.0f, 0.011f, config);
        Require(Near(calibration.standing, 70.0f, 0.05f), "Sustained rise is adopted");

        // Crouching only decays the maximum slowly.
        HeightCalibration crouch;
        crouch.Update(64.0f, 0.0f, config);
        for (int i = 0; i < 100; ++i)
            crouch.Update(40.0f, 0.01f, config);
        Require(Near(crouch.standing, 64.0f * std::exp(-0.01f), 0.02f), "One second of crouch decays ~1%");
        for (int i = 0; i < 2900; ++i)
            crouch.Update(40.0f, 0.01f, config);
        Require(Near(crouch.standing, 64.0f * std::exp(-0.3f), 0.1f), "Thirty seconds of crouch decays ~26%");
        for (int i = 0; i < 10000; ++i)
            crouch.Update(40.0f, 0.01f, config);
        Require(Near(crouch.standing, 40.0f, 1e-3f), "The decayed maximum never drops below the current eye");

        Config noDecay = config;
        noDecay.standingDecayPerSecond = 0.0f;
        HeightCalibration held;
        held.Update(64.0f, 0.0f, noDecay);
        for (int i = 0; i < 1000; ++i)
            held.Update(30.0f, 0.01f, noDecay);
        Require(Near(held.standing, 64.0f, 1e-4f), "Zero decay holds the maximum");
    }

    void TestStateResets()
    {
        const Config config{};
        State state;
        Require(state.Begin({ 0.0f, 0.0f, 0.0f }, 1000u, config) == 0.0f, "First solve has no dt");
        state.eye.Update(64.0f, 0.0f, config);
        state.pelvis.Update(38.0f, 0.0f, false, config);
        Require(Near(state.Begin({ 2.0f, 0.0f, 0.0f }, 1011u, config), 0.011f, 1e-5f) && state.eye.valid,
            "Walking keeps the calibration and reports dt");
        Require(state.Begin({ 500.0f, 0.0f, 0.0f }, 1022u, config) == 0.0f && !state.eye.valid && !state.pelvis.valid,
            "Teleport/respawn resets the calibration");
        state.eye.Update(64.0f, 0.0f, config);
        Require(state.Begin({ 500.0f, 0.0f, 0.0f }, 5000u, config) == 0.0f && !state.eye.valid,
            "A long silence resets the calibration");
        state.eye.Update(64.0f, 0.0f, config);
        Require(state.Begin({ 500.0f, 0.0f, 0.0f }, 4000u, config) == 0.0f && !state.eye.valid,
            "A backwards clock resets the calibration");
        Require(state.Begin({ NAN, 0.0f, 0.0f }, 4010u, config) == 0.0f && !state.haveOrigin,
            "Non-finite origin resets and forgets the origin");
        Require(Finite(state.lastOrigin), "Last origin stays finite");
    }

    void TestAnimatedReference()
    {
        const Config config{};
        AnimatedReference reference;
        reference.Update(20.0f, 0.01f, true, config);
        Require(!reference.valid, "Native crouch never seeds the standing reference");
        reference.Update(38.0f, 0.01f, false, config);
        Require(reference.valid && Near(reference.height, 38.0f, 1e-4f), "First standing sample seeds the reference");
        for (int i = 0; i < 100; ++i)
            reference.Update(20.0f, 0.01f, true, config);
        Require(Near(reference.height, 38.0f, 1e-4f), "Native crouch frames leave the reference untouched");
        for (int i = 0; i < 100; ++i)
            reference.Update(24.0f, 0.01f, false, config);
        Require(Near(reference.height, 38.0f, 1e-4f), "Crouch-depth samples without the duck flag are rejected");
        for (int i = 0; i < 300; ++i)
            reference.Update(37.0f, 0.01f, false, config);
        Require(Near(reference.height, 37.0f, 0.01f), "Reference follows the standing animation");
    }

    void TestCrouchDrop()
    {
        const Config config{};
        const float leg = 34.0f;
        CrouchResult r = ComputeCrouch(64.0f, 64.0f, 38.0f, 38.0f, leg, config);
        Require(r.valid && r.physicalCrouch == 0.0f && r.addedDrop == 0.0f && r.headRemainder == 0.0f,
            "Standing adds nothing");

        r = ComputeCrouch(64.0f, 44.0f, 38.0f, 38.0f, leg, config);
        Require(Near(r.physicalCrouch, 20.0f, 1e-4f) && Near(r.desiredPelvisDrop, 18.0f, 1e-4f) &&
            Near(r.addedDrop, 18.0f, 1e-4f) && Near(r.headRemainder, 2.0f, 1e-4f),
            "Pelvis drops 90% of the physical crouch, the spine covers the rest");

        r = ComputeCrouch(64.0f, 44.0f, 38.0f, 26.0f, leg, config);
        Require(Near(r.animatedDrop, 12.0f, 1e-4f) && Near(r.addedDrop, 6.0f, 1e-4f) &&
            Near(r.headRemainder, 2.0f, 1e-4f),
            "Native crouch animation is not dropped twice");

        r = ComputeCrouch(64.0f, 60.0f, 38.0f, 13.0f, leg, config);
        Require(r.addedDrop == 0.0f && r.headRemainder == 0.0f,
            "A deeper native crouch is never raised");

        r = ComputeCrouch(64.0f, 14.0f, 38.0f, 38.0f, leg, config);
        Require(Near(r.desiredPelvisDrop, 0.7f * leg, 1e-4f) && Near(r.addedDrop, 0.7f * leg, 1e-4f) &&
            Near(r.headRemainder, 50.0f - 0.7f * leg, 1e-4f),
            "Pelvis drop is clamped to 70% of the leg length");

        r = ComputeCrouch(64.0f, 70.0f, 38.0f, 38.0f, leg, config);
        Require(r.physicalCrouch == 0.0f && r.addedDrop == 0.0f, "Above the calibration is not a crouch");

        Require(!ComputeCrouch(NAN, 44.0f, 38.0f, 38.0f, leg, config).valid, "Non-finite standing height is rejected");
        Require(!ComputeCrouch(64.0f, 44.0f, 38.0f, 38.0f, 0.0f, config).valid, "Zero leg length is rejected");
    }

    void TestSpineBend()
    {
        const Vec3 up{ 0.0f, 0.0f, 1.0f };
        const Vec3 forward{ 1.0f, 0.0f, 0.0f };
        const float maxBend = 30.0f * kPi / 180.0f;
        Require(SpineBendForDrop({ 0.0f, 0.0f, 20.0f }, up, forward, 0.0f, maxBend) == 0.0f, "No remainder, no bend");
        const float angle = SpineBendForDrop({ 0.0f, 0.0f, 20.0f }, up, forward, 2.0f, maxBend);
        Require(Near(angle, std::acos(18.0f / 20.0f), 1e-4f), "Upright spine bend matches the drop");
        const Quat bend = FromAxisAngle(SpineBendAxis(up, forward), angle);
        const Vec3 tip = Rotate(bend, { 0.0f, 0.0f, 20.0f });
        Require(Near(tip.z, 18.0f, 1e-3f) && tip.x > 0.0f, "Bend lowers the head and leans it forward");

        const Vec3 leaning{ 5.0f, 1.0f, 19.0f };
        const float leanAngle = SpineBendForDrop(leaning, up, forward, 1.5f, maxBend);
        const Vec3 leaningTip = Rotate(FromAxisAngle(SpineBendAxis(up, forward), leanAngle), leaning);
        Require(Near(leaningTip.z, 17.5f, 1e-3f) && Near(leaningTip.y, 1.0f, 1e-4f),
            "Pre-leaned spine bend lowers by exactly the remainder");
        Require(Near(SpineBendForDrop({ 0.0f, 0.0f, 20.0f }, up, forward, 15.0f, maxBend), maxBend, 1e-6f),
            "Spine bend is bounded");

        // Body facing +Y: the bend leans toward +Y.
        const Vec3 left{ 0.0f, 1.0f, 0.0f };
        const float yawAngle = SpineBendForDrop({ 0.0f, 0.0f, 20.0f }, up, left, 2.0f, maxBend);
        const Vec3 yawTip = Rotate(FromAxisAngle(SpineBendAxis(up, left), yawAngle), { 0.0f, 0.0f, 20.0f });
        Require(Near(yawTip.z, 18.0f, 1e-3f) && yawTip.y > 0.0f && Near(yawTip.x, 0.0f, 1e-4f),
            "Bend follows the body yaw");
    }

    struct Mat
    {
        float m[3][4];
    };

    Mat MakeMatrix(Quat q, Vec3 p)
    {
        Mat out{};
        ToMatrix34({ q, p }, out.m);
        return out;
    }

    Mat Mul(const Mat& a, const Mat& b)
    {
        Mat out{};
        for (int r = 0; r < 3; ++r)
        {
            for (int c = 0; c < 4; ++c)
            {
                out.m[r][c] = a.m[r][0] * b.m[0][c] + a.m[r][1] * b.m[1][c] + a.m[r][2] * b.m[2][c];
            }
            out.m[r][3] += a.m[r][3];
        }
        return out;
    }

    Mat Delta(const Rigid& rigid)
    {
        Mat out{};
        ToMatrix34(rigid, out.m);
        return out;
    }

    bool SameMatrix(const Mat& a, const Mat& b, float tolerance)
    {
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 4; ++c)
                if (std::fabs(a.m[r][c] - b.m[r][c]) > tolerance)
                    return false;
        return true;
    }

    // Applies the hook's order: pelvis drop (translation), thigh swing about
    // the hip, calf swing about the knee, foot restored to its animated frame.
    void SolveAndCheckLeg(float yawRadians, float drop, const char* label)
    {
        const Quat yaw = FromYaw(yawRadians);
        const Vec3 forward = Rotate(yaw, { 1.0f, 0.0f, 0.0f });
        const Vec3 up{ 0.0f, 0.0f, 1.0f };
        Mat thigh = MakeMatrix(yaw * FromAxisAngle({ 0.2f, 1.0f, 0.1f }, 1.4f), Rotate(yaw, { 0.0f, 4.0f, 37.0f }));
        Mat calf = MakeMatrix(yaw * FromAxisAngle({ 0.1f, 1.0f, 0.3f }, 1.7f), Rotate(yaw, { 0.6f, 4.0f, 20.0f }));
        Mat foot = MakeMatrix(yaw * FromAxisAngle({ 1.0f, 0.2f, 0.0f }, 0.4f), Rotate(yaw, { 0.0f, 4.0f, 3.0f }));
        Mat toe = MakeMatrix(yaw * FromAxisAngle({ 0.0f, 1.0f, 0.0f }, 0.2f), Rotate(yaw, { 5.0f, 4.0f, 0.5f }));
        const Mat originalFoot = foot;
        const Mat originalToe = toe;
        const float thighLength = Length(MatrixOrigin(calf.m) - MatrixOrigin(thigh.m));
        const float calfLength = Length(MatrixOrigin(foot.m) - MatrixOrigin(calf.m));

        const Mat lower = Delta(Translation({ 0.0f, 0.0f, -drop }));
        thigh = Mul(lower, thigh);
        calf = Mul(lower, calf);
        foot = Mul(lower, foot);
        toe = Mul(lower, toe);

        const LegSolution leg = SolvePlantedLeg(
            MatrixOrigin(thigh.m), MatrixOrigin(calf.m), MatrixOrigin(foot.m),
            MatrixOrigin(originalFoot.m), forward, up);
        Require(leg.valid && leg.reached, label);

        const Mat thighDelta = Delta(RotationAbout(leg.thighRotation, MatrixOrigin(thigh.m)));
        thigh = Mul(thighDelta, thigh);
        calf = Mul(thighDelta, calf);
        foot = Mul(thighDelta, foot);
        toe = Mul(thighDelta, toe);
        Require(Near(MatrixOrigin(calf.m), leg.knee, 1e-3f), "Thigh swing puts the knee on the solution");

        const Mat calfDelta = Delta(RotationAbout(leg.calfRotation, MatrixOrigin(calf.m)));
        calf = Mul(calfDelta, calf);
        foot = Mul(calfDelta, foot);
        toe = Mul(calfDelta, toe);
        Require(Near(MatrixOrigin(foot.m), MatrixOrigin(originalFoot.m), 1e-3f), "Foot stays planted");
        Require(Near(Length(MatrixOrigin(calf.m) - MatrixOrigin(thigh.m)), thighLength, 1e-3f) &&
            Near(Length(MatrixOrigin(foot.m) - MatrixOrigin(calf.m)), calfLength, 1e-3f),
            "Leg IK preserves bone lengths");

        // Knee bends forward relative to the body yaw.
        const Vec3 hip = MatrixOrigin(thigh.m);
        const Vec3 ankle = MatrixOrigin(foot.m);
        const Vec3 kneeOffset = MatrixOrigin(calf.m) - (hip + (ankle - hip) * 0.5f);
        Require(Dot(kneeOffset, forward) > 0.25f * drop, "Knee points forward");

        Quat currentFoot{}, animatedFoot{};
        Require(MatrixRotation(foot.m, currentFoot) && MatrixRotation(originalFoot.m, animatedFoot),
            "Foot rotations are readable");
        const Mat restore = Delta(MapFrame(currentFoot, MatrixOrigin(foot.m), animatedFoot, MatrixOrigin(originalFoot.m)));
        foot = Mul(restore, foot);
        toe = Mul(restore, toe);
        Require(SameMatrix(foot, originalFoot, 2e-3f) && SameMatrix(toe, originalToe, 2e-3f),
            "Foot branch keeps its animated world transform");
    }

    void TestLegIk()
    {
        SolveAndCheckLeg(0.0f, 15.0f, "Planted leg solves (yaw 0, 15u)");
        SolveAndCheckLeg(kPi * 0.5f, 15.0f, "Planted leg solves (yaw 90, 15u)");
        SolveAndCheckLeg(-2.3f, 23.8f, "Planted leg solves (yaw -132, deep)");
        SolveAndCheckLeg(1.0f, 3.0f, "Planted leg solves (shallow)");

        // Out of reach: clamps along the target direction.
        const LegSolution far = SolvePlantedLeg(
            { 0.0f, 0.0f, 37.0f }, { 0.6f, 0.0f, 20.0f }, { 0.0f, 0.0f, 3.0f },
            { 0.0f, 0.0f, -40.0f }, { 1.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 1.0f });
        Require(far.valid && !far.reached && far.ankle.z > -1.0f && far.ankle.z < 3.5f,
            "Unreachable target stretches no further than the leg");

        // A backward-bent animated knee still solves knee-forward.
        const LegSolution backward = SolvePlantedLeg(
            { 0.0f, 0.0f, 25.0f }, { -2.0f, 0.0f, 8.5f }, { 0.0f, 0.0f, -9.0f },
            { 0.0f, 0.0f, 3.0f }, { 1.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 1.0f });
        Require(backward.valid && backward.reached && backward.knee.x > 0.0f, "Backward animated knee is flipped forward");

        Require(!SolvePlantedLeg({ NAN, 0.0f, 0.0f }, {}, {}, {}, { 1.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 1.0f }).valid,
            "Non-finite leg is rejected");
        Require(!SolvePlantedLeg({ 0.0f, 0.0f, 10.0f }, { 0.0f, 0.0f, 10.0f }, { 0.0f, 0.0f, 0.0f },
            { 0.0f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 1.0f }).valid,
            "Zero-length thigh is rejected");
    }

    void TestMatrixHelpers()
    {
        const Quat q = Normalize(FromAxisAngle({ 0.3f, -0.5f, 0.8f }, 1.1f));
        float m[3][4];
        ToMatrix34({ q, { 1.0f, 2.0f, 3.0f } }, m);
        // Uniform model scale must not leak into the rotation.
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c)
                m[r][c] *= 1.2f;
        Quat back{};
        Require(MatrixRotation(m, back), "Scaled matrix rotation is readable");
        const Vec3 v{ 0.4f, -1.0f, 2.0f };
        Require(Near(Rotate(back, v), Rotate(q, v), 1e-4f), "Matrix rotation round-trips");
        Require(Near(MatrixOrigin(m), { 1.0f, 2.0f, 3.0f }, 1e-6f), "Matrix origin reads column 3");
        const Rigid about = RotationAbout(q, { 5.0f, 0.0f, 0.0f });
        Require(Near(Apply(about, { 5.0f, 0.0f, 0.0f }), { 5.0f, 0.0f, 0.0f }, 1e-5f), "Rotation keeps its pivot fixed");
        float zero[3][4]{};
        Require(!MatrixRotation(zero, back), "Degenerate matrix is rejected");
    }
}

int main()
{
    TestHeightCalibration();
    TestStateResets();
    TestAnimatedReference();
    TestCrouchDrop();
    TestSpineBend();
    TestMatrixHelpers();
    TestLegIk();
    std::printf("vr_vrik_lower_body_tests: %d checks passed\n", checks);
    return 0;
}

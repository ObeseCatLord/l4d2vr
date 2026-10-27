#include "../L4D2VR/vr_vrik_solver.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace
{
    using namespace l4d2vr_vrik;

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

    constexpr float kMillimetre = 39.37f / 1000.0f;   // game units

    // Survivor-like bind pose in model space (game units, +X forward, +Y left,
    // +Z up). Every bone gets an arbitrary bind orientation so the solver
    // cannot rely on identity bind frames.
    Skeleton MakeSkeleton()
    {
        const Vec3 positions[kBoneCount] = {
            { 0.0f, 0.0f, 38.0f },   // Pelvis
            { 0.0f, 0.0f, 41.0f },   // Spine
            { 0.0f, 0.0f, 46.0f },   // Spine1
            { 0.0f, 0.0f, 51.0f },   // Spine2
            { 0.0f, 0.0f, 58.0f },   // Neck
            { 0.5f, 0.0f, 61.0f },   // Head
            { 0.0f, 2.0f, 56.0f },   // ClavicleL
            { -0.5f, 7.0f, 56.0f },  // UpperArmL
            { -0.5f, 7.0f, 44.0f },  // ForearmL
            { -0.5f, 7.0f, 33.0f },  // HandL
            { 0.0f, -2.0f, 56.0f },  // ClavicleR
            { -0.5f, -7.0f, 56.0f }, // UpperArmR
            { -0.5f, -7.0f, 44.0f }, // ForearmR
            { -0.5f, -7.0f, 33.0f }, // HandR
            { 0.0f, 4.0f, 37.0f },   // ThighL
            { 0.6f, 4.0f, 20.0f },   // CalfL
            { 0.0f, 4.0f, 3.0f },    // FootL
            { 0.0f, -4.0f, 37.0f },  // ThighR
            { 0.6f, -4.0f, 20.0f },  // CalfR
            { 0.0f, -4.0f, 3.0f },   // FootR
        };
        std::array<Transform, kBoneCount> model{};
        for (int b = 0; b < kBoneCount; ++b)
        {
            const float fb = static_cast<float>(b);
            model[static_cast<std::size_t>(b)] = {
                FromAxisAngle({ 0.3f + fb, 1.0f - 0.1f * fb, 0.5f }, 0.2f + 0.37f * fb),
                positions[b] };
        }
        Skeleton skeleton;
        Require(MakeSkeletonFromModelSpace(model, kDefaultParents, 3.5f, 3.5f, skeleton), "Skeleton builds");
        std::array<Transform, kBoneCount> check{};
        Require(ComputeModelSpace(skeleton, check), "Skeleton model space computes");
        for (int b = 0; b < kBoneCount; ++b)
            Require(Length(check[static_cast<std::size_t>(b)].position - positions[b]) < 1e-4f,
                "Local transforms reproduce the bind pose");
        return skeleton;
    }

    Input StandingInput(float headYawDegrees = 0.0f, float eyeHeight = 64.5f)
    {
        Input input;
        input.head.valid = true;
        input.head.position = { 0.0f, 0.0f, eyeHeight };
        input.head.orientation = FromYaw(headYawDegrees * kPi / 180.0f);
        input.dt = 1.0f / 90.0f;
        input.standingEyeHeight = 64.5f;
        return input;
    }

    Vec3 PlayerPosition(const Output& output, int bone) { return BoneInPlayerFrame(output, bone).position; }

    void TestReach()
    {
        const Skeleton skeleton = MakeSkeleton();
        const Config config;
        State state;
        Output output;
        Input input = StandingInput();
        Require(Solve(skeleton, config, input, state, output) && output.valid, "Standing solve");
        Require(std::fabs(output.scale - 1.0f) < 1e-4f, "Bind eye height gives unit scale");
        const Vec3 shoulderL = PlayerPosition(output, UpperArmL);
        const Vec3 shoulderR = PlayerPosition(output, UpperArmR);

        // Untracked arms hang below the shoulders.
        Require(PlayerPosition(output, HandL).z < shoulderL.z - 15.0f &&
            PlayerPosition(output, HandR).z < shoulderR.z - 15.0f, "Untracked arms hang");
        Require(output.stretch[UpperArmL] == 1.0f && output.stretch[ForearmR] == 1.0f, "Hanging arms do not stretch");

        const Vec3 targetL = shoulderL + Vec3{ 15.0f, 3.0f, -5.0f };
        const Vec3 targetR = shoulderR + Vec3{ 12.0f, -2.0f, 6.0f };
        const Quat handRotation = FromAxisAngle({ 0.2f, 1.0f, -0.4f }, 0.9f);
        input.hands[0] = { targetL, handRotation, true };
        input.hands[1] = { targetR, handRotation, true };
        State reachState;
        Require(Solve(skeleton, config, input, reachState, output), "Reach solve");
        Require(Length(PlayerPosition(output, UpperArmL) - shoulderL) < 1e-3f, "Arms do not move the shoulders");
        Require(Length(PlayerPosition(output, HandL) - targetL) < kMillimetre, "Left wrist within 1 mm of target");
        Require(Length(PlayerPosition(output, HandR) - targetR) < kMillimetre, "Right wrist within 1 mm of target");
        const Quat solvedHand = BoneInPlayerFrame(output, HandL).rotation;
        const float alignment = std::fabs(solvedHand.x * handRotation.x + solvedHand.y * handRotation.y +
            solvedHand.z * handRotation.z + solvedHand.w * handRotation.w);
        Require(alignment > 0.99999f, "Hand bone takes the tracked orientation");
        Require(output.stretch[UpperArmL] == 1.0f, "Reachable target does not stretch");

        // Bone lengths are preserved.
        const float upper = Length(PlayerPosition(output, ForearmL) - PlayerPosition(output, UpperArmL));
        const float lower = Length(PlayerPosition(output, HandL) - PlayerPosition(output, ForearmL));
        Require(std::fabs(upper - 12.0f) < 1e-3f && std::fabs(lower - 11.0f) < 1e-3f, "Arm bone lengths preserved");

        // Elbow on the pole side: back, outward and down of the shoulder-wrist line.
        for (int side = 0; side < 2; ++side)
        {
            const int up = side ? UpperArmR : UpperArmL, fo = side ? ForearmR : ForearmL, ha = side ? HandR : HandL;
            const Vec3 a = output.bones[static_cast<std::size_t>(up)].position;
            const Vec3 e = output.bones[static_cast<std::size_t>(fo)].position;
            const Vec3 h = output.bones[static_cast<std::size_t>(ha)].position;
            const Vec3 dir = Normalized(h - a);
            const Vec3 offset = (e - a) - dir * Dot(e - a, dir);
            const float out = side ? -1.0f : 1.0f;
            const Vec3 pole{ -0.4f, 0.8f * out, -1.0f };
            const Vec3 polePerp = pole - dir * Dot(pole, dir);
            Require(Dot(offset, polePerp) > 0.0f && offset.y * out > 0.0f && offset.z < 0.0f,
                "Elbow bends toward the pole (out and down)");
        }

        // Out of reach but within the stretch limit: the arm stretches to the target.
        input.hands[0] = { shoulderL + Vec3{ 30.0f, 0.0f, 0.0f }, handRotation, true };
        input.hands[1].valid = false;
        State stretchState;
        Require(Solve(skeleton, config, input, stretchState, output), "Stretch solve");
        Require(output.stretch[UpperArmL] > 1.25f && output.stretch[UpperArmL] < 1.4f &&
            output.stretch[ForearmL] == output.stretch[UpperArmL], "Both arm bones stretch equally");
        Require(Length(PlayerPosition(output, HandL) - input.hands[0].position) < kMillimetre,
            "Stretched wrist reaches the target");
        Require(output.stretch[UpperArmR] == 1.0f, "Untracked arm never stretches");

        // Beyond the stretch limit: clamped at 1.6x along the reach direction.
        const Vec3 far = shoulderL + Vec3{ 50.0f, 10.0f, 0.0f };
        input.hands[0].position = far;
        State clampState;
        Require(Solve(skeleton, config, input, clampState, output), "Clamp solve");
        const Vec3 reach = PlayerPosition(output, HandL) - shoulderL;
        Require(std::fabs(output.stretch[UpperArmL] - 1.6f) < 1e-5f, "Stretch clamps at 1.6x");
        Require(std::fabs(Length(reach) - 1.6f * 23.0f) < 0.01f, "Clamped reach is 1.6x the arm length");
        Require(Dot(Normalized(reach), Normalized(far - shoulderL)) > 0.99999f, "Clamped wrist points at the target");
    }

    void TestCrouch()
    {
        const Skeleton skeleton = MakeSkeleton();
        const Config config;
        State standingState, crouchState;
        Output standing, crouched;
        Require(Solve(skeleton, config, StandingInput(), standingState, standing), "Standing solve");
        Require(Solve(skeleton, config, StandingInput(0.0f, 44.5f), crouchState, crouched), "Crouched solve");
        Require(standing.pelvisDropUnits < 1e-3f, "Standing does not drop the pelvis");
        Require(std::fabs(crouched.pelvisDropUnits - 18.0f) < 0.01f, "Pelvis drops 90% of the crouch depth");
        Require(PlayerPosition(crouched, Pelvis).z < PlayerPosition(standing, Pelvis).z - 17.0f, "Crouch lowers the pelvis");
        for (int side = 0; side < 2; ++side)
        {
            const int thigh = side ? ThighR : ThighL, calf = side ? CalfR : CalfL, foot = side ? FootR : FootL;
            const Vec3 hip = crouched.bones[static_cast<std::size_t>(thigh)].position;
            const Vec3 knee = crouched.bones[static_cast<std::size_t>(calf)].position;
            const Vec3 ankle = crouched.bones[static_cast<std::size_t>(foot)].position;
            Require(knee.x > hip.x + 5.0f && knee.x > ankle.x + 5.0f, "Knees bend forward when crouching");
            Require(std::fabs(ankle.z - 3.0f) < 0.05f, "Feet stay on the floor when crouching");
        }
        Require(PlayerPosition(crouched, Head).z < PlayerPosition(standing, Head).z - 15.0f, "Crouch lowers the head bone");
        Require(std::fabs(PlayerPosition(standing, Head).z - 61.0f) < 0.05f, "Standing head bone sits at the bind height");

        // Height scale follows the calibrated standing height, clamped.
        Input tall = StandingInput();
        tall.standingEyeHeight = 64.5f * 1.1f;
        State s1;
        Output o1;
        Require(Solve(skeleton, config, tall, s1, o1) && std::fabs(o1.scale - 1.1f) < 1e-4f, "Height scale");
        tall.standingEyeHeight = 200.0f;
        State s2;
        Require(Solve(skeleton, config, tall, s2, o1) && std::fabs(o1.scale - 1.25f) < 1e-6f, "Height scale clamps high");
        tall.standingEyeHeight = 10.0f;
        State s3;
        Require(Solve(skeleton, config, tall, s3, o1) && std::fabs(o1.scale - 0.8f) < 1e-6f, "Height scale clamps low");
    }

    void TestRootPlacement()
    {
        const Skeleton skeleton = MakeSkeleton();
        Config config;
        State state;
        Output output;
        Input input = StandingInput(0.0f);
        input.head.position = { 20.0f, -6.0f, 64.5f };
        Require(Solve(skeleton, config, input, state, output), "Offset head solve");
        // Head bone at bind x (0.5) relative to the root; eye = head + 3.5 fwd; plus 4 cm settle.
        Require(std::fabs(output.root.position.x - (20.0f - 0.5f - 3.5f - 0.04f * 39.37f)) < 1e-3f &&
            std::fabs(output.root.position.y + 6.0f) < 1e-3f && output.root.position.z == 0.0f,
            "Root on the floor under the tracked eye, pushed back 4 cm");

        config.anchorRootToOrigin = true;
        State anchored;
        Require(Solve(skeleton, config, input, anchored, output), "Anchored solve");
        Require(Length(output.root.position) == 0.0f, "Anchored root stays at the player origin");
        Require(output.bones[Pelvis].position.x > 4.0f, "Anchored pelvis follows the head horizontally");
        Require(PlayerPosition(output, Neck).x > PlayerPosition(output, Pelvis).x + 2.0f, "Anchored spine leans toward the head");
    }

    void TestYaw()
    {
        const Skeleton skeleton = MakeSkeleton();
        const Config config;
        State state;
        Output output;
        Require(Solve(skeleton, config, StandingInput(0.0f), state, output) && std::fabs(output.bodyYawDegrees) < 1e-4f,
            "First solve faces the head");
        Require(Solve(skeleton, config, StandingInput(30.0f), state, output) && std::fabs(output.bodyYawDegrees) < 1e-4f,
            "Head turn inside the deadzone keeps the body");
        Require(Solve(skeleton, config, StandingInput(60.0f), state, output) &&
            std::fabs(output.bodyYawDegrees - 15.0f) < 1e-3f, "Head turn beyond 45 degrees drags the body");
        Require(Solve(skeleton, config, StandingInput(-10.0f), state, output) &&
            std::fabs(output.bodyYawDegrees - 15.0f) < 1e-3f, "Turning back inside the deadzone keeps the body");
        Require(Solve(skeleton, config, StandingInput(-170.0f), state, output) &&
            std::fabs(output.bodyYawDegrees - 145.0f) < 1e-3f, "Deadzone wraps around +-180 (shortest turn)");

        // Head bone takes the HMD orientation; neck takes part of it.
        State fresh;
        Input pitched = StandingInput(0.0f);
        pitched.head.orientation = FromAxisAngle({ 0.0f, 1.0f, 0.0f }, 0.6f);   // looking down
        Require(Solve(skeleton, config, pitched, fresh, output), "Pitched solve");
        std::array<Transform, kBoneCount> bind{};
        ComputeModelSpace(skeleton, bind);
        const Quat headDelta = output.bones[Head].rotation * Conjugate(bind[Head].rotation);
        const Vec3 forward = Rotate(headDelta, { 1.0f, 0.0f, 0.0f });
        Require(std::fabs(forward.z + std::sin(0.6f)) < 1e-4f, "Head follows the HMD pitch");
        Require(std::fabs(output.bodyYawDegrees) < 1e-3f, "Looking down keeps the facing");

        // Walking lines the body up with the head (tau 80 ms).
        State walking;
        Input walk = StandingInput(0.0f);
        Require(Solve(skeleton, config, walk, walking, output), "Walk init");
        walk = StandingInput(40.0f);
        walk.walkVelocity = { 1.2f * 39.37f, 0.0f, 0.0f };
        for (int frame = 0; frame < 90; ++frame)
            Solve(skeleton, config, walk, walking, output);
        Require(std::fabs(output.bodyYawDegrees - 40.0f) < 0.1f, "Walking aligns the body with the head");
    }

    void TestLegs()
    {
        const Skeleton skeleton = MakeSkeleton();
        const Config config;
        State state;
        Output output;
        Input input = StandingInput(0.0f);
        Require(Solve(skeleton, config, input, state, output), "Leg init");
        Require(std::fabs(output.bones[FootL].position.x) < 1e-3f && std::fabs(output.bones[FootR].position.x) < 1e-3f,
            "Standing feet stay under the hips");

        input.walkVelocity = { 1.2f * 39.37f, 0.0f, 0.0f };
        int leftAhead = 0, rightAhead = 0, signChanges = 0, previousSign = 0;
        float maxLiftL = 0.0f, maxLiftR = 0.0f;
        bool bothLifted = false;
        for (int frame = 0; frame < 270; ++frame)
        {
            Require(Solve(skeleton, config, input, state, output), "Walking solve");
            if (frame < 45)
                continue;   // walk speed ramp-up
            const Vec3 l = output.bones[FootL].position, r = output.bones[FootR].position;
            const float diff = l.x - r.x;
            const int sign = diff > 0.5f ? 1 : diff < -0.5f ? -1 : 0;
            if (sign > 0) ++leftAhead;
            if (sign < 0) ++rightAhead;
            if (sign != 0 && previousSign != 0 && sign != previousSign) ++signChanges;
            if (sign != 0) previousSign = sign;
            const float liftL = l.z - 3.0f, liftR = r.z - 3.0f;
            maxLiftL = std::max(maxLiftL, liftL);
            maxLiftR = std::max(maxLiftR, liftR);
            if (liftL > 1.2f && liftR > 1.2f) bothLifted = true;   // clamped reach alone lifts < 1 unit
        }
        Require(leftAhead > 20 && rightAhead > 20 && signChanges >= 3, "Legs alternate while walking");
        Require(maxLiftL > 1.5f && maxLiftR > 1.5f, "Each foot lifts during its swing");
        Require(!bothLifted, "Feet never lift together");
        Require(output.walkAmount > 0.95f, "Full stride at 1.2 m/s");

        input.walkVelocity = {};
        for (int frame = 0; frame < 180; ++frame)
            Solve(skeleton, config, input, state, output);
        Require(output.walkAmount == 0.0f && state.walkSpeed == 0.0f, "Stopping settles the walk");
    }

    void TestDeterminismAndValidation()
    {
        const Skeleton skeleton = MakeSkeleton();
        const Config config;
        State a, b;
        Output oa, ob;
        for (int frame = 0; frame < 120; ++frame)
        {
            Input input = StandingInput(static_cast<float>(frame) * 1.5f, 60.0f + static_cast<float>(frame % 7));
            input.walkVelocity = { 30.0f, 10.0f, 0.0f };
            input.hands[0] = { { 15.0f, 10.0f, 45.0f + static_cast<float>(frame % 5) }, FromYaw(0.3f), frame % 3 != 0 };
            input.hands[1] = { { 20.0f, -9.0f, 50.0f }, FromAxisAngle({ 1.0f, 0.0f, 0.0f }, 0.5f), true };
            Require(Solve(skeleton, config, input, a, oa) && Solve(skeleton, config, input, b, ob), "Determinism solves");
            Require(std::memcmp(&oa.bones, &ob.bones, sizeof(oa.bones)) == 0 &&
                std::memcmp(&oa.root, &ob.root, sizeof(oa.root)) == 0 &&
                a.bodyYaw == b.bodyYaw && a.stride == b.stride && a.walkSpeed == b.walkSpeed, "Identical inputs give identical outputs");
        }

        Input input = StandingInput();
        input.head.valid = false;
        State s;
        Output o;
        Require(!Solve(skeleton, config, input, s, o) && !o.valid, "Untracked head fails the solve");
        input = StandingInput();
        input.head.position.x = NAN;
        Require(!Solve(skeleton, config, input, s, o), "Non-finite head fails the solve");
        input = StandingInput();
        input.hands[0] = { { NAN, 0.0f, 0.0f }, Quat{}, true };
        Require(Solve(skeleton, config, input, s, o) && o.valid, "Non-finite hand is treated as untracked");
        Skeleton broken = skeleton;
        broken.parents[Spine] = Head;
        Require(!Solve(broken, config, StandingInput(), s, o), "Mis-ordered skeleton is rejected");
        input = StandingInput();
        input.head.orientation = FromAxisAngle({ 0.0f, 1.0f, 0.0f }, kPi * 0.5f);   // straight down
        State down;
        Require(Solve(skeleton, config, input, down, o) && std::fabs(o.bodyYawDegrees) < 0.01f,
            "Looking straight down keeps a usable facing");
    }
}

int main()
{
    TestReach();
    TestCrouch();
    TestRootPlacement();
    TestYaw();
    TestLegs();
    TestDeterminismAndValidation();
    std::printf("vr_vrik_solver_tests: %d checks passed\n", checks);
    return 0;
}

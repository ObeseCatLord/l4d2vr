#include "../L4D2VR/vr_hand_pose_library.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>

// Survivor hand pose library: data sanity, curl mapping, trigger finger,
// activity mapping and the per-hand blender.
static void CheckImpl(bool result, int line, const char* what)
{
    if (!result) { std::fprintf(stderr, "Check failed at line %d: %s\n", line, what); std::abort(); }
}
#define Check(result) CheckImpl((result), __LINE__, #result)

using namespace l4d2vr_handpose;

static bool Near(float a, float b, float tol = 0.01f) { return std::fabs(a - b) <= tol; }

static bool SamePose(const Pose& a, const Pose& b, float tol = 0.01f)
{
    for (int f = 0; f < kFingers; ++f)
    {
        for (int j = 0; j < kJoints; ++j)
            if (!Near(a.flex[f][j], b.flex[f][j], tol)) return false;
        if (!Near(a.splay[f], b.splay[f], tol)) return false;
    }
    for (int k = 0; k < 3; ++k)
        if (!Near(a.thumb[k], b.thumb[k], tol)) return false;
    return true;
}

static void TestData()
{
    for (int p = 0; p < kPoseCount; ++p)
    {
        const Pose& pose = Get(static_cast<PoseId>(p));
        for (int f = 0; f < kFingers; ++f)
        {
            for (int j = 0; j < kJoints; ++j)
            {
                Check(std::isfinite(pose.flex[f][j]));
                Check(pose.flex[f][j] >= -40.0f && pose.flex[f][j] <= 130.0f);
            }
            Check(std::isfinite(pose.splay[f]) && std::fabs(pose.splay[f]) <= 40.0f);
        }
        for (float a : pose.thumb)
            Check(std::isfinite(a) && std::fabs(a) <= 90.0f);
    }
    // A fist is more closed than a relaxed hand at every finger joint (thumb tip joints too).
    const Pose& relaxed = Get(PoseId::Relaxed);
    const Pose& fist = Get(PoseId::Fist);
    for (int f = 1; f < kFingers; ++f)
        for (int j = 0; j < kJoints; ++j)
            Check(fist.flex[f][j] > relaxed.flex[f][j] + 15.0f);
    // Pointing: index straighter than in the fist, others closed.
    const Pose& point = Get(PoseId::Point);
    for (int j = 0; j < kJoints; ++j)
        Check(point.flex[1][j] < fist.flex[1][j] - 30.0f);
    for (int f = 2; f < kFingers; ++f)
        Check(point.flex[f][1] > relaxed.flex[f][1] + 15.0f);
    // Holding something: the long fingers close more than a relaxed hand.
    for (PoseId id : { PoseId::MagazineHold, PoseId::PistolMagHold, PoseId::Foregrip, PoseId::PumpGrip,
             PoseId::GrenadeHold, PoseId::PistolGrip, PoseId::RifleGrip })
    {
        // (Pavlov's magazine and shell holds leave the ring and pinky loose, so count index..ring.)
        const Pose& p = Get(id);
        float held = 0.0f, rest = 0.0f;
        for (int f = 1; f <= 3; ++f)
            for (int j = 0; j < 2; ++j) { held += p.flex[f][j]; rest += relaxed.flex[f][j]; }
        Check(held > rest + 40.0f);
    }
    // Out-of-range ids clamp.
    Check(&Get(static_cast<PoseId>(-3)) == &Get(PoseId::Relaxed));
    Check(&Get(static_cast<PoseId>(999)) == &Get(static_cast<PoseId>(kPoseCount - 1)));
}

static void TestCurls()
{
    const float open[5] = { 0, 0, 0, 0, 0 };
    const float closed[5] = { 1, 1, 1, 1, 1 };
    Check(SamePose(FromCurls(open), Get(PoseId::Relaxed)));
    Check(SamePose(FromCurls(closed), Get(PoseId::Fist)));
    // Grip held, index off the trigger: the index straightens toward pointing.
    const float point[5] = { 1, 0, 1, 1, 1 };
    const Pose p = FromCurls(point);
    for (int j = 0; j < kJoints; ++j)
        Check(Near(p.flex[1][j], Get(PoseId::Point).flex[1][j]));
    for (int f = 2; f < kFingers; ++f)
        for (int j = 0; j < kJoints; ++j)
            Check(Near(p.flex[f][j], Get(PoseId::Fist).flex[f][j]));
    // Half curls lie between, NaN counts as open.
    const float half[5] = { 0.5f, 0.5f, 0.5f, 0.5f, 0.5f };
    const Pose h = FromCurls(half);
    for (int f = 2; f < kFingers; ++f)
        Check(h.flex[f][1] > Get(PoseId::Relaxed).flex[f][1] && h.flex[f][1] < Get(PoseId::Fist).flex[f][1]);
    const float bad[5] = { NAN, NAN, NAN, NAN, NAN };
    Check(SamePose(FromCurls(bad), Get(PoseId::Relaxed)));
}

static void TestTrigger()
{
    float d[kJoints];
    TriggerIndexDelta(0.0f, d);
    for (int j = 0; j < kJoints; ++j) Check(Near(d[j], kTriggerOff[j]));
    TriggerIndexDelta(kTriggerTouchCurl, d);
    for (int j = 0; j < kJoints; ++j) Check(Near(d[j], 0.0f));
    TriggerIndexDelta(1.0f, d);
    for (int j = 0; j < kJoints; ++j) Check(Near(d[j], kTriggerPull[j]));
    // Monotonic: more trigger, more curl.
    float prev = -1000.0f;
    for (int i = 0; i <= 20; ++i)
    {
        TriggerIndexDelta(static_cast<float>(i) / 20.0f, d);
        const float sum = d[0] + d[1] + d[2];
        Check(sum >= prev - 1e-4f);
        prev = sum;
    }
    // Lifting off lifts (negative), pulling curls (positive); scale 0 = animation only.
    Check(kTriggerOff[0] + kTriggerOff[1] + kTriggerOff[2] < 0.0f);
    Check(kTriggerPull[0] + kTriggerPull[1] + kTriggerPull[2] > 0.0f);
    TriggerIndexDelta(1.0f, d, 0.0f);
    for (int j = 0; j < kJoints; ++j) Check(Near(d[j], 0.0f));
}

static void TestActivities()
{
    Check(FamilyOf(1) == GunFamily::Pistol && FamilyOf(32) == GunFamily::Magnum);
    Check(FamilyOf(2) == GunFamily::Smg && FamilyOf(7) == GunFamily::Smg && FamilyOf(33) == GunFamily::Smg);
    Check(FamilyOf(5) == GunFamily::Rifle && FamilyOf(26) == GunFamily::Rifle && FamilyOf(9) == GunFamily::Rifle &&
          FamilyOf(34) == GunFamily::Rifle);
    Check(FamilyOf(35) == GunFamily::BoltSniper && FamilyOf(36) == GunFamily::BoltSniper);
    Check(FamilyOf(6) == GunFamily::Sniper && FamilyOf(10) == GunFamily::Sniper);
    Check(FamilyOf(3) == GunFamily::PumpShotgun && FamilyOf(8) == GunFamily::PumpShotgun);
    Check(FamilyOf(4) == GunFamily::AutoShotgun && FamilyOf(11) == GunFamily::AutoShotgun);
    Check(FamilyOf(37) == GunFamily::M60 && FamilyOf(21) == GunFamily::GrenadeLauncher);
    for (int id : { 0, 12, 13, 14, 15, 19, 20, 23, 24, 25 })  // items, melee, chainsaw
        Check(!IsGun(id));

    PoseId pose{};
    Check(HeldPose(HandActivity::Magazine, 1, pose) && pose == PoseId::PistolMagHold);
    Check(HeldPose(HandActivity::Magazine, 26, pose) && pose == PoseId::MagazineHold);
    Check(HeldPose(HandActivity::Shell, 3, pose) && pose == PoseId::ShellHold);
    Check(HeldPose(HandActivity::GunGripPosed, 1, pose) && pose == PoseId::PistolGrip);
    Check(!HeldPose(HandActivity::Free, 1, pose));
    Check(!HeldPose(HandActivity::GunGrip, 1, pose));
    Check(!HeldPose(HandActivity::Support, 5, pose));
    Check(BoltActivity(1) == HandActivity::Slide && BoltActivity(32) == HandActivity::Slide);
    Check(BoltActivity(5) == HandActivity::ChargingHandle && BoltActivity(26) == HandActivity::ChargingHandle);
    Check(BoltActivity(3) == HandActivity::Pump && BoltActivity(8) == HandActivity::Pump);
    Check(BoltActivity(35) == HandActivity::BoltAction && BoltActivity(36) == HandActivity::BoltAction);
    Check(BoltActivity(4) == HandActivity::ChargingHandle);
}

static void TestBlender()
{
    HandBlender b;
    HandInput in{};
    in.activity = HandActivity::Magazine;
    in.weaponId = 26;
    // First frame snaps to the held pose.
    HandOutput out = b.Step(in, 0.011f, 0.06f);
    Check(out.absolute && !out.triggerOverlay && SamePose(out.pose, Get(PoseId::MagazineHold)));
    // Switch to a shell: blends, reaching it within a few time constants.
    in.activity = HandActivity::Shell;
    out = b.Step(in, 0.011f, 0.06f);
    Check(!SamePose(out.pose, Get(PoseId::ShellHold), 0.5f));
    for (int i = 0; i < 60; ++i)
        out = b.Step(in, 0.011f, 0.06f);
    Check(SamePose(out.pose, Get(PoseId::ShellHold), 0.05f));
    // Blend time 0 snaps.
    in.activity = HandActivity::Magazine;
    out = b.Step(in, 0.011f, 0.0f);
    Check(SamePose(out.pose, Get(PoseId::MagazineHold)));
    // Gun grip: the animation stays, only the trigger finger overlay.
    in.activity = HandActivity::GunGrip;
    in.curlsValid = true;
    in.curls[1] = 1.0f;
    out = b.Step(in, 0.0f, 0.06f);
    Check(!out.absolute && out.triggerOverlay);
    for (int j = 0; j < kJoints; ++j) Check(Near(out.indexDelta[j], kTriggerPull[j]));
    // No curls: the finger rests on the trigger (no change to the animation).
    in.curlsValid = false;
    out = b.Step(in, 0.0f, 0.06f);
    for (int j = 0; j < kJoints; ++j) Check(Near(out.indexDelta[j], 0.0f));
    // Animated items and the support grip: nothing applied.
    in.activity = HandActivity::Animated;
    out = b.Step(in, 0.011f, 0.06f);
    Check(!out.absolute && !out.triggerOverlay);
    in.activity = HandActivity::Support;
    out = b.Step(in, 0.011f, 0.06f);
    Check(!out.absolute && !out.triggerOverlay);
    // Back to a free hand after an animated state: snaps (no blend from a stale pose).
    in.activity = HandActivity::Free;
    in.curlsValid = true;
    for (float& c : in.curls) c = 1.0f;
    out = b.Step(in, 0.011f, 0.06f);
    Check(out.absolute && SamePose(out.pose, Get(PoseId::Fist)));
    // A long gap snaps too; NaN dt is a gap.
    for (float& c : in.curls) c = 0.0f;
    out = b.Step(in, 2.0f, 0.06f);
    Check(SamePose(out.pose, Get(PoseId::Relaxed)));
    for (float& c : in.curls) c = 1.0f;
    out = b.Step(in, NAN, 0.06f);
    Check(SamePose(out.pose, Get(PoseId::Fist)));
    // Posed gun grip (a hand the game does not animate): pistol grip plus the trigger finger.
    in.activity = HandActivity::GunGripPosed;
    in.weaponId = 1;
    in.curls[1] = 0.0f;
    out = b.Step(in, 0.0f, 0.06f);
    Check(out.absolute);
    for (int j = 0; j < kJoints; ++j)
        Check(Near(out.pose.flex[1][j], Get(PoseId::PistolGrip).flex[1][j] + kTriggerOff[j]));
}

static void TestMapping()
{
    // Bind flex from a rotation about Z by -38.8 degrees (a survivor middle joint).
    const float a = -38.8f * 3.14159265f / 180.0f;
    Check(Near(BindFlexDegrees(std::cos(a), std::sin(a)), 38.8f, 0.01f));
    Check(Near(JointDelta(90.0f, 38.8f), 51.2f));
    // Splay mirrors between the hands and is opposite for the index and the pinky.
    for (int f = 1; f < kFingers; ++f)
        Check(SplayAxisSign(f, -1) == -SplayAxisSign(f, 1));
    Check(SplayAxisSign(1, -1) == -SplayAxisSign(4, -1));
}

int main()
{
    TestData();
    TestCurls();
    TestTrigger();
    TestActivities();
    TestBlender();
    TestMapping();
    std::printf("vr_hand_pose_library_tests: all passed\n");
    return 0;
}

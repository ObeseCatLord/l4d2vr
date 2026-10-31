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
                Check(pose.flex[f][j] >= -45.0f && pose.flex[f][j] <= 130.0f);
            }
            Check(std::isfinite(pose.splay[f]) && std::fabs(pose.splay[f]) <= 25.0f);
        }
        for (float a : pose.thumb)
            Check(std::isfinite(a) && std::fabs(a) <= 120.0f);
    }
    // Pavlov's empty-hand samples: Open < Rest < Close at the middle joints of the long fingers.
    const Pose& open = Get(PoseId::EmptyOpen);
    const Pose& rest = Get(PoseId::EmptyRest);
    const Pose& closed = Get(PoseId::EmptyClosed);
    for (int f = 1; f < kFingers; ++f)
    {
        Check(open.flex[f][1] < rest.flex[f][1]);
        Check(closed.flex[f][1] > rest.flex[f][1] + 25.0f);
        Check(closed.flex[f][0] + closed.flex[f][1] + closed.flex[f][2] > rest.flex[f][0] + rest.flex[f][1] + rest.flex[f][2] + 60.0f);
    }
    // Trigger discipline: the index's middle and tip joints straighten (at most slightly bent).
    const Pose& d = Get(PoseId::TriggerDiscipline);
    Check(d.flex[1][1] < 5.0f && d.flex[1][2] < 5.0f);
    // Holding something: index..ring close more than the resting hand.
    for (PoseId id : { PoseId::MagM16, PoseId::MagPistol, PoseId::MagAk, PoseId::PumpGrip, PoseId::RackLargePalm,
             PoseId::PistolGrip, PoseId::RifleGrip, PoseId::ChargingHandleAR, PoseId::SliderPinch, PoseId::BoltBall })
    {
        const Pose& p = Get(id);
        float held = 0.0f, resting = 0.0f;
        for (int f = 1; f <= 3; ++f)
            for (int j = 0; j < 2; ++j) { held += p.flex[f][j]; resting += rest.flex[f][j]; }
        Check(held > resting);
    }
    // Out-of-range ids clamp.
    Check(&Get(static_cast<PoseId>(-3)) == &Get(PoseId::EmptyOpen));
    Check(&Get(static_cast<PoseId>(999)) == &Get(static_cast<PoseId>(kPoseCount - 1)));
    // The blend curve: 0 -> 0, 1 -> 1, monotonic, ease-out (ahead of linear).
    Check(Near(HandBlendCurve(0.0f), 0.0f) && Near(HandBlendCurve(1.0f), 1.0f) && Near(HandBlendCurve(2.0f), 1.0f));
    float prev = -1.0f;
    for (int i = 0; i <= 20; ++i)
    {
        const float v = HandBlendCurve(static_cast<float>(i) / 20.0f);
        Check(v >= prev - 1e-5f);
        prev = v;
    }
    Check(HandBlendCurve(0.5f) > 0.55f);
}

static void TestCurls()
{
    // Nothing pressed: every finger at Pavlov's idle value 0.25 (between Open and Rest).
    const float none[5] = { 0, 0, 0, 0, 0 };
    float v[5];
    FingerValuesFromCurls(none, v);
    for (float x : v) Check(Near(x, kFingerValueIdle));
    // Everything closed: the Close sample (a fist).
    const float all[5] = { 1, 1, 1, 1, 1 };
    Check(SamePose(FromCurls(all), Get(PoseId::EmptyClosed)));
    // The value at the Rest sample gives the Rest pose; the Open sample the Open pose.
    const float restValues[5] = { kFingerValueRest, kFingerValueRest, kFingerValueRest, kFingerValueRest, kFingerValueRest };
    Check(SamePose(EmptyHand(restValues), Get(PoseId::EmptyRest)));
    const float openValues[5] = { -1, -1, -1, -1, -1 };  // clamps to -0.25
    Check(SamePose(EmptyHand(openValues), Get(PoseId::EmptyOpen)));
    // Grip held with the index off the trigger: the index points (value 0), the others close.
    const float grip[5] = { 1, 0, 1, 1, 1 };
    FingerValuesFromCurls(grip, v);
    Check(Near(v[1], 0.0f) && Near(v[2], 1.0f));
    const Pose p = FromCurls(grip);
    Check(p.flex[1][1] < Get(PoseId::EmptyRest).flex[1][1]);
    for (int f = 2; f < kFingers; ++f)
        for (int j = 0; j < kJoints; ++j)
            Check(Near(p.flex[f][j], Get(PoseId::EmptyClosed).flex[f][j]));
    // Trigger only: index closes, the rest follow less.
    const float trig[5] = { 1, 1, 0.5f, 0.25f, 0.17f };
    FingerValuesFromCurls(trig, v);
    Check(Near(v[1], 1.0f) && v[2] > v[3] && v[3] > v[4]);
    // NaN counts as open (idle).
    const float bad[5] = { NAN, NAN, NAN, NAN, NAN };
    FingerValuesFromCurls(bad, v);
    for (float x : v) Check(Near(x, kFingerValueIdle));
}

static void TestTrigger()
{
    TriggerFinger t;
    // Resting on the trigger: no pull, no discipline.
    for (int i = 0; i < 100; ++i) t.Step(kTriggerTouchCurl, 0.011f);
    Check(Near(t.pull, 0.0f) && Near(t.DisciplineWeight(0.5f), 0.0f));
    // A full pull reaches 1 within 0.05 s (20/s) and falls within 0.034 s (30/s).
    t.Step(1.0f, 0.025f);
    Check(Near(t.pull, 0.5f, 0.01f));
    t.Step(1.0f, 0.03f);
    Check(Near(t.pull, 1.0f));
    t.Step(kTriggerTouchCurl, 0.02f);
    Check(Near(t.pull, 0.4f, 0.01f));
    t.Step(kTriggerTouchCurl, 0.02f);
    Check(Near(t.pull, 0.0f));
    // Off the trigger: nothing for 2 s, then the discipline layer at the gun's weight.
    for (int i = 0; i < 180; ++i) t.Step(0.0f, 0.011f);  // 1.98 s
    Check(Near(t.DisciplineWeight(0.48f), 0.0f));
    for (int i = 0; i < 40; ++i) t.Step(0.0f, 0.011f);
    Check(Near(t.DisciplineWeight(0.48f), 0.48f));
    // Touching the trigger again drops it quickly (FInterpTo at 20/s).
    for (int i = 0; i < 12; ++i) t.Step(kTriggerTouchCurl, 0.011f);
    Check(Near(t.DisciplineWeight(0.48f), 0.0f));
    // ApplyTriggerFinger: weight 0 keeps the grip, 1 takes the discipline index; pull goes on the middle joint.
    const float grip[3] = { 30.0f, 40.0f, 20.0f };
    float out[3];
    ApplyTriggerFinger(grip, 0.0f, 12.0f, out);
    Check(Near(out[0], 30.0f) && Near(out[1], 52.0f) && Near(out[2], 20.0f));
    ApplyTriggerFinger(grip, 1.0f, 0.0f, out);
    for (int j = 0; j < kJoints; ++j) Check(Near(out[j], Get(PoseId::TriggerDiscipline).flex[1][j]));
    // Bad inputs.
    TriggerFinger u;
    u.Step(NAN, NAN);
    Check(std::isfinite(u.pull) && std::isfinite(u.disciplineValue));
    Check(Near(u.DisciplineWeight(NAN), 0.0f));
}

static void TestActivities()
{
    // Every L4D2 gun has Pavlov poses; items, melee and the chainsaw do not.
    for (int id : { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 21, 26, 32, 33, 34, 35, 36, 37 })
    {
        const GunPoses* g = GunPosesFor(id);
        Check(g != nullptr && g->weaponId == id);
        Check(g->triggerDiscipline > 0.0f && g->triggerDiscipline <= 1.0f);
        Check(g->triggerAngle >= 8.0f && g->triggerAngle <= 25.0f);
    }
    for (int id : { 0, 12, 13, 14, 15, 19, 20, 23, 24, 25, 22, 99 })
        Check(!IsGun(id));
    Check(GunPosesFor(1)->pistolGrip && GunPosesFor(32)->pistolGrip && !GunPosesFor(5)->pistolGrip);

    PoseId pose{};
    Check(HeldPose(HandActivity::Magazine, 1, pose) && pose == PoseId::MagPistol);
    Check(HeldPose(HandActivity::Magazine, 32, pose) && pose == PoseId::MagMagnum);
    Check(HeldPose(HandActivity::Magazine, 26, pose) && pose == PoseId::MagAk);
    Check(HeldPose(HandActivity::Magazine, 37, pose) && pose == PoseId::MagLmg);
    Check(HeldPose(HandActivity::Magazine, 0, pose) && pose == PoseId::MagM16);  // unknown gun: a rifle magazine
    Check(HeldPose(HandActivity::Shell, 3, pose) && pose == PoseId::Shell);
    Check(HeldPose(HandActivity::Shell, 21, pose) && pose == PoseId::GrenadeRound);
    // Pavlov's handle poses: pistols rack the slide, the AK pinches its charging handle from below,
    // ARs take the charging handle, bolt actions the bolt knob, pump guns the pump.
    Check(HeldPose(HandActivity::Handle, 1, pose) && pose == PoseId::RackSmall);
    Check(HeldPose(HandActivity::Handle, 32, pose) && pose == PoseId::RackLargePalm);
    Check(HeldPose(HandActivity::Handle, 26, pose) && pose == PoseId::SliderPinchUp);
    Check(HeldPose(HandActivity::Handle, 5, pose) && pose == PoseId::ChargingHandleAR);
    Check(HeldPose(HandActivity::Handle, 35, pose) && pose == PoseId::BoltBall);
    Check(HeldPose(HandActivity::Handle, 3, pose) && pose == PoseId::PumpGrip);
    Check(HeldPose(HandActivity::GunGripPosed, 1, pose) && pose == PoseId::PistolGrip);
    Check(HeldPose(HandActivity::GunGripPosed, 5, pose) && pose == PoseId::RifleGrip);
    Check(!HeldPose(HandActivity::Free, 1, pose));
    Check(!HeldPose(HandActivity::GunGrip, 1, pose));
    Check(!HeldPose(HandActivity::Support, 5, pose));
    Check(!HeldPose(HandActivity::Animated, 15, pose));
    Check(Near(TriggerDisciplineFor(35), 0.14f) && Near(TriggerDisciplineFor(33), 0.62f));
    Check(Near(TriggerDisciplineFor(0), kTriggerDisciplineDefault));
    Check(Near(TriggerAngleFor(3), 12.0f) && Near(TriggerAngleFor(5), 15.0f) && Near(TriggerAngleFor(0), kTriggerAngleDefault));
}

static void TestBlender()
{
    HandBlender b;
    HandInput in{};
    in.activity = HandActivity::Magazine;
    in.weaponId = 26;
    // First frame snaps to the held pose.
    HandOutput out = b.Step(in, 0.011f);
    Check(out.absolute && !out.triggerOverlay && SamePose(out.pose, Get(PoseId::MagAk)));
    // On the charging handle: Pavlov's handle blend (0.12 s); tips arrive first.
    in.activity = HandActivity::Handle;
    out = b.Step(in, 0.011f);
    Check(!SamePose(out.pose, Get(PoseId::SliderPinchUp), 0.5f));
    for (int i = 0; i < 4; ++i)
        out = b.Step(in, 0.011f);  // 0.055 s: tips (3x) done, knuckles not yet
    {
        const Pose& target = Get(PoseId::SliderPinchUp);
        const Pose& start = Get(PoseId::MagAk);
        bool tipDone = true, knuckleMoving = false;
        for (int f = 1; f < kFingers; ++f)
        {
            tipDone = tipDone && Near(out.pose.flex[f][2], target.flex[f][2], 0.05f);
            if (std::fabs(target.flex[f][0] - start.flex[f][0]) > 5.0f)
                knuckleMoving = knuckleMoving || !Near(out.pose.flex[f][0], target.flex[f][0], 0.5f);
        }
        Check(tipDone && knuckleMoving);
    }
    for (int i = 0; i < 10; ++i)
        out = b.Step(in, 0.011f);  // past 0.12 s
    Check(SamePose(out.pose, Get(PoseId::SliderPinchUp), 0.01f));
    // A magazine again: 0.2 s; blend time 0 snaps.
    in.activity = HandActivity::Magazine;
    out = b.Step(in, 0.011f, 0.0f);
    Check(SamePose(out.pose, Get(PoseId::MagAk)));
    // Gun grip: the animation stays, the trigger finger overlay rides on it.
    in.activity = HandActivity::GunGrip;
    in.curlsValid = true;
    in.curls[1] = 1.0f;
    for (int i = 0; i < 10; ++i)
        out = b.Step(in, 0.011f);
    Check(!out.absolute && out.triggerOverlay);
    Check(Near(out.pullDegrees, 20.0f) && Near(out.disciplineWeight, 0.0f));  // AK-47 TriggerAngle 20
    // Off the trigger for 2 s: the AK-47's discipline weight 0.48.
    in.curls[1] = 0.0f;
    for (int i = 0; i < 220; ++i)
        out = b.Step(in, 0.011f);
    Check(Near(out.pullDegrees, 0.0f) && Near(out.disciplineWeight, 0.48f));
    // Trigger finger scale 0: the animation only.
    out = b.Step(in, 0.011f, kGrabBlendSeconds, 0.0f);
    Check(Near(out.pullDegrees, 0.0f) && Near(out.disciplineWeight, 0.0f));
    // Animated items and the support grip: nothing applied.
    in.activity = HandActivity::Animated;
    out = b.Step(in, 0.011f);
    Check(!out.absolute && !out.triggerOverlay);
    in.activity = HandActivity::Support;
    out = b.Step(in, 0.011f);
    Check(!out.absolute && !out.triggerOverlay);
    // Back to a free hand after an animated state: snaps (no blend from a stale pose).
    in.activity = HandActivity::Free;
    for (float& c : in.curls) c = 1.0f;
    out = b.Step(in, 0.011f);
    Check(out.absolute && SamePose(out.pose, Get(PoseId::EmptyClosed)));
    // The free hand follows the curls directly (no grab blend while it stays free).
    for (float& c : in.curls) c = 0.0f;
    out = b.Step(in, 0.011f);
    float idle[5] = { kFingerValueIdle, kFingerValueIdle, kFingerValueIdle, kFingerValueIdle, kFingerValueIdle };
    Check(SamePose(out.pose, EmptyHand(idle)));
    // A long gap or a NaN dt snaps.
    in.activity = HandActivity::Shell;
    in.weaponId = 3;
    out = b.Step(in, NAN);
    Check(SamePose(out.pose, Get(PoseId::Shell)));
    // Posed gun grip (a hand the game does not animate): pistol grip plus the trigger finger.
    HandBlender g;
    in.activity = HandActivity::GunGripPosed;
    in.weaponId = 1;
    in.curls[1] = 1.0f;
    for (int i = 0; i < 10; ++i)
        out = g.Step(in, 0.011f);
    Check(out.absolute && !out.triggerOverlay);
    Check(Near(out.pose.flex[1][1], Get(PoseId::PistolGrip).flex[1][1] + 20.0f, 0.05f));
    Check(Near(out.pose.flex[1][0], Get(PoseId::PistolGrip).flex[1][0], 0.05f));
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

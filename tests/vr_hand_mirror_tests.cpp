#include "../L4D2VR/vr_hand_mirror.h"
#include <cmath>
#include <cstdio>

using namespace l4d2vr_hand_mirror;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

static bool Near(float a, float b, float eps = 1.0e-4f) { return std::fabs(a - b) <= eps; }
static bool NearMat(const Mat34& a, const Mat34& b, float eps = 1.0e-4f)
{
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 4; ++c)
            if (!Near(a.m[r][c], b.m[r][c], eps))
                return false;
    return true;
}

// Source frame: columns forward, left, up; rotated by yaw/pitch/roll (degrees), at origin o.
static Mat34 Frame(float yawDeg, float pitchDeg, float rollDeg, float ox, float oy, float oz)
{
    const float d = 3.14159265f / 180.0f;
    const float cy = std::cos(yawDeg * d), sy = std::sin(yawDeg * d);
    const float cp = std::cos(pitchDeg * d), sp = std::sin(pitchDeg * d);
    const float cr = std::cos(rollDeg * d), sr = std::sin(rollDeg * d);
    // R = Rz(yaw) * Ry(pitch) * Rx(roll)
    const float rz[3][3] = { { cy, -sy, 0 }, { sy, cy, 0 }, { 0, 0, 1 } };
    const float ry[3][3] = { { cp, 0, sp }, { 0, 1, 0 }, { -sp, 0, cp } };
    const float rx[3][3] = { { 1, 0, 0 }, { 0, cr, -sr }, { 0, sr, cr } };
    float t[3][3]{};
    float r[3][3]{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            for (int k = 0; k < 3; ++k)
                t[i][j] += ry[i][k] * rx[k][j];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            for (int k = 0; k < 3; ++k)
                r[i][j] += rz[i][k] * t[k][j];
    Mat34 out{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            out.m[i][j] = r[i][j];
    out.m[0][3] = ox; out.m[1][3] = oy; out.m[2][3] = oz;
    return out;
}

static Mat34 PlaneOf(const Mat34& frame)
{
    // The frame's own centre plane: through its origin, normal = its left axis (column 1).
    const float p[3] = { frame.m[0][3], frame.m[1][3], frame.m[2][3] };
    const float n[3] = { frame.m[0][1], frame.m[1][1], frame.m[2][1] };
    Mat34 out{};
    Reflection(p, n, out);
    return out;
}

static void TestReflectionBasics()
{
    const float p[3] = { 1.0f, 2.0f, 3.0f };
    const float n[3] = { 0.3f, -2.0f, 0.5f };
    Mat34 r{};
    CHECK(Reflection(p, n, r));
    CHECK(Near(Determinant(r), -1.0f));
    CHECK(NearMat(Mul(r, r), Identity()));             // an involution
    float q[3]{};
    TransformPoint(r, p, q);                           // points on the plane stay
    CHECK(Near(q[0], p[0]) && Near(q[1], p[1]) && Near(q[2], p[2]));
    const float off[3] = { p[0] + n[0], p[1] + n[1], p[2] + n[2] };
    TransformPoint(r, off, q);                         // the normal flips
    CHECK(Near(q[0], p[0] - n[0]) && Near(q[1], p[1] - n[1]) && Near(q[2], p[2] - n[2]));

    const float zero[3] = { 0.0f, 0.0f, 0.0f };
    CHECK(!Reflection(p, zero, r));
    const float bad[3] = { NAN, 0.0f, 0.0f };
    CHECK(!Reflection(bad, n, r));
}

// The gun in the physical LEFT hand: the IK solves a proper right-handed pose
// on the RIGHT side of the mirror world, and the final hand is the original
// grip reflected across the gun's own plane (a left hand on the same gun).
static void TestGunHand()
{
    // Eyes at the origin, body facing +x, Source y = left. Body mirror: the x-z plane.
    const float eye[3] = { 0.0f, 0.0f, 0.0f };
    const float bodyLeft[3] = { 0.0f, 1.0f, 0.0f };
    Mat34 body{};
    CHECK(Reflection(eye, bodyLeft, body));

    // The gun frame at the left controller, aimed a little right, pitched down, rolled.
    const Mat34 gun = Frame(-12.0f, 8.0f, 15.0f, 0.35f, 0.22f, -0.30f);
    const Mat34 gunPlane = PlaneOf(gun);
    // The game's right-hand grip on that gun (hand bone a little off the gun plane).
    const Mat34 gripLocal = Frame(5.0f, -20.0f, 70.0f, -0.04f, -0.03f, -0.05f);
    const Mat34 handTarget = Mul(gun, gripLocal);
    CHECK(Near(Determinant(handTarget), 1.0f));

    const Mat34 solveTarget = MirroredWorldTarget(body, gunPlane, handTarget);
    CHECK(Near(Determinant(solveTarget), 1.0f));       // proper: the solver can use it
    CHECK(solveTarget.m[1][3] < 0.0f);                 // on the right: a normal right arm, no crossing
    CHECK(handTarget.m[1][3] > 0.0f);

    // After the IK, every bone is reflected by the body mirror.
    const Mat34 finalHand = Mul(body, solveTarget);
    CHECK(Near(Determinant(finalHand), -1.0f));        // a mirrored (left) hand
    CHECK(NearMat(finalHand, Mul(gunPlane, handTarget)));
    // The gun model is drawn reflected by the gun plane: the hand keeps its grip on it.
    const Mat34 finalGun = Mul(gunPlane, gun);
    for (int i = 0; i < 3; ++i)
        CHECK(Near(finalGun.m[i][3], gun.m[i][3]));    // the gun does not move
    for (int i = 0; i < 3; ++i)                        // its forward and up axes stay, left flips
    {
        CHECK(Near(finalGun.m[i][0], gun.m[i][0]));
        CHECK(Near(finalGun.m[i][2], gun.m[i][2]));
        CHECK(Near(finalGun.m[i][1], -gun.m[i][1]));
    }
    // Grip relative to the gun is unchanged by the mirror.
    const float g[3] = { gripLocal.m[0][3], gripLocal.m[1][3], gripLocal.m[2][3] };
    float a[3]{}, b[3]{};
    const float zero[3] = { 0.0f, 0.0f, 0.0f };
    TransformPoint(finalHand, zero, a);
    const float mirroredLocal[3] = { g[0], -g[1], g[2] };    // the grip point, mirrored in the gun frame
    TransformPoint(gun, mirroredLocal, b);
    CHECK(Near(a[0], b[0]) && Near(a[1], b[1]) && Near(a[2], b[2]));
    // The body mirror cancels: any sagittal plane gives the same final hand.
    const float eye2[3] = { 0.1f, -0.05f, 0.2f };
    const float tilted[3] = { 0.1f, 1.0f, 0.0f };
    Mat34 body2{};
    CHECK(Reflection(eye2, tilted, body2));
    CHECK(NearMat(Mul(body2, MirroredWorldTarget(body2, gunPlane, handTarget)), finalHand));
}

// The free hand on the physical RIGHT controller: the model's left hand is
// placed there with a left-hand offset; reflecting it across the controller's
// own plane gives the mirror-image (right-hand) offset on the same controller.
static void TestFreeHand()
{
    const Mat34 rightController = Frame(10.0f, -25.0f, -30.0f, 0.30f, -0.25f, -0.35f);
    const Mat34 controllerPlane = PlaneOf(rightController);
    // A left hand's wrist relative to its controller: behind, to the left (outside), below.
    const Mat34 leftHandOffset = Frame(0.0f, 30.0f, 40.0f, -0.08f, 0.03f, -0.02f);
    const Mat34 placed = Mul(rightController, leftHandOffset);
    const Mat34 finalHand = Mul(controllerPlane, placed);
    CHECK(Near(Determinant(finalHand), -1.0f));
    const float zero[3] = { 0.0f, 0.0f, 0.0f };
    float a[3]{}, b[3]{};
    TransformPoint(finalHand, zero, a);
    const float rightOffset[3] = { -0.08f, -0.03f, -0.02f };   // a right hand's wrist: outside is now to the right
    TransformPoint(rightController, rightOffset, b);
    CHECK(Near(a[0], b[0]) && Near(a[1], b[1]) && Near(a[2], b[2]));
    // Undoing the free hand's own mirror recovers the original proper frame
    // (what the magazine logic reads as the free wrist).
    CHECK(NearMat(Mul(controllerPlane, finalHand), placed));
}

static void TestDecision()
{
    MirrorInput in{};
    in.vrActive = true;
    in.itemInPhysicalLeft = true;
    in.fullArmIk = true;
    CHECK(ShouldMirror(in));
    MirrorInput off = in; off.enabled = false; CHECK(!ShouldMirror(off));
    off = in; off.itemInPhysicalLeft = false; CHECK(!ShouldMirror(off));   // right hand: the model is already right
    off = in; off.vrActive = false; CHECK(!ShouldMirror(off));             // desktop / mouse mode unchanged
    off = in; off.dualPistols = true; CHECK(!ShouldMirror(off));
    off = in; off.fullArmIk = false; CHECK(!ShouldMirror(off));
}

// A grenade's hold correction (a roll in the hand) mirrors with the controller
// plane: a right-hand roll of +20 deg becomes -20 deg relative to the left controller.
static void TestControllerPlaneMirrorsHoldTurn()
{
    CHECK(PlaneForItem(true, false, false) == Plane::Weapon);
    CHECK(PlaneForItem(false, true, false) == Plane::Weapon);
    CHECK(PlaneForItem(false, false, false) == Plane::Controller);   // grenades, pills, medkits
    CHECK(PlaneForItem(true, false, true) == Plane::Controller);     // empty hands

    const Mat34 controller = Frame(30.0f, -10.0f, 5.0f, 0.3f, 0.2f, -0.3f);
    const Mat34 plane = PlaneOf(controller);
    const Mat34 held = Mul(controller, Frame(0.0f, 0.0f, 20.0f, 0.0f, 0.0f, 0.0f));
    const Mat34 mirrored = Mul(plane, held);
    // In the controller's frame, mirroring across its x-z plane is S*R*S (S flips y)
    // times S: the roll is negated, the geometry flipped.
    const Mat34 expected = Mul(controller, Mul(Frame(0.0f, 0.0f, -20.0f, 0.0f, 0.0f, 0.0f),
        Mat34{ { { 1, 0, 0, 0 }, { 0, -1, 0, 0 }, { 0, 0, 1, 0 } } }));
    CHECK(NearMat(mirrored, expected));
}

int main()
{
    TestControllerPlaneMirrorsHoldTurn();
    TestReflectionBasics();
    TestGunHand();
    TestFreeHand();
    TestDecision();
    if (g_failures)
    {
        std::printf("vr_hand_mirror: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("vr_hand_mirror tests passed\n");
    return 0;
}

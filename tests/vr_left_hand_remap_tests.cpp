#include "../L4D2VR/vr_left_hand_remap.h"
#include <cmath>
#include <cstdio>

using namespace l4d2vr_left_hand;
using l4d2vr_hand_mirror::Mul;
using l4d2vr_hand_mirror::Determinant;
using l4d2vr_hand_mirror::Reflection;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

static bool Near(float a, float b, float eps = 1.0e-3f) { return std::fabs(a - b) <= eps; }
static bool NearMat(const Mat34& a, const Mat34& b, float eps = 1.0e-3f)
{
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 4; ++c)
            if (!Near(a.m[r][c], b.m[r][c], eps))
                return false;
    return true;
}

static Mat34 Frame(float yawDeg, float pitchDeg, float rollDeg, float ox, float oy, float oz)
{
    const float d = 3.14159265f / 180.0f;
    const float cy = std::cos(yawDeg * d), sy = std::sin(yawDeg * d);
    const float cp = std::cos(pitchDeg * d), sp = std::sin(pitchDeg * d);
    const float cr = std::cos(rollDeg * d), sr = std::sin(rollDeg * d);
    const float rz[3][3] = { { cy, -sy, 0 }, { sy, cy, 0 }, { 0, 0, 1 } };
    const float ry[3][3] = { { cp, 0, sp }, { 0, 1, 0 }, { -sp, 0, cp } };
    const float rx[3][3] = { { 1, 0, 0 }, { 0, cr, -sr }, { 0, sr, cr } };
    float t[3][3]{}, r[3][3]{};
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) for (int k = 0; k < 3; ++k) t[i][j] += ry[i][k] * rx[k][j];
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) for (int k = 0; k < 3; ++k) r[i][j] += rz[i][k] * t[k][j];
    Mat34 out{};
    for (int i = 0; i < 3; ++i) { for (int j = 0; j < 3; ++j) out.m[i][j] = r[i][j]; }
    out.m[0][3] = ox; out.m[1][3] = oy; out.m[2][3] = oz;
    return out;
}

static Mat34 LocalFlipY()
{
    Mat34 s = l4d2vr_hand_mirror::Identity();
    s.m[1][1] = -1.0f;
    return s;
}

// A symmetric arms skeleton (model plane y = 0, right side at -y as in Source).
struct Rig
{
    std::vector<std::string> names;
    std::vector<Mat34> rest;
};

static Rig MakeRig(float planeY = 0.0f)
{
    Rig rig;
    Mat34 mrest{};
    const float p[3] = { 0.0f, planeY, 0.0f }, n[3] = { 0.0f, 1.0f, 0.0f };
    Reflection(p, n, mrest);
    auto add = [&](const char* name, const Mat34& m) { rig.names.push_back(name); rig.rest.push_back(m); };
    add("ValveBiped.ValveBiped", Frame(0, 0, 0, 0, planeY, 0));
    add("ValveBiped.Bip01_Spine4", Frame(0, -10, 0, 0, planeY, -20));
    const char* right[] = { "ValveBiped.Bip01_R_UpperArm", "ValveBiped.Bip01_R_Forearm", "ValveBiped.Bip01_R_Hand",
        "ValveBiped.Bip01_R_Finger1", "ValveBiped.R_Ulna" };
    const char* left[] = { "ValveBiped.Bip01_L_UpperArm", "ValveBiped.Bip01_L_Forearm", "ValveBiped.Bip01_L_Hand",
        "ValveBiped.Bip01_L_Finger1", "ValveBiped.L_Ulna" };
    const Mat34 rframes[] = { Frame(20, 30, 10, 5, planeY - 12, -22), Frame(35, 10, -20, 18, planeY - 15, -30),
        Frame(50, -5, 40, 30, planeY - 16, -28), Frame(60, -15, 45, 33, planeY - 17, -27), Frame(30, 12, 0, 22, planeY - 15, -29) };
    for (int i = 0; i < 5; ++i) add(right[i], rframes[i]);
    for (int i = 0; i < 5; ++i) add(left[i], Mul(Mul(mrest, rframes[i]), LocalFlipY()));
    add("ValveBiped.weapon_bone", Frame(0, 0, 0, 30, planeY - 16, -25)); // one-sided by position, centre by name
    return rig;
}

static void TestNames()
{
    CHECK(SideOfName("ValveBiped.Bip01_L_Hand") == -1);
    CHECK(SideOfName("ValveBiped.Bip01_R_Finger12") == 1);
    CHECK(SideOfName("ValveBiped.L_Ulna") == -1);
    CHECK(SideOfName("ValveBiped.Bip01_Spine4") == 0);
    CHECK(SideOfName("ValveBiped.weapon_bone") == 0);
    CHECK(SideOfName("Arm_Right") == 1);
    CHECK(CounterpartName("ValveBiped.Bip01_L_Hand") == "valvebiped.bip01_r_hand");
    CHECK(CounterpartName("ValveBiped.R_Ulna") == "valvebiped.l_ulna");
    CHECK(CounterpartName("Arm_Left") == "arm_right");
    CHECK(CounterpartName("ValveBiped.Bip01_Spine4").empty());
}

static void TestBindPoseLandsOnCounterpart()
{
    for (float planeY : { 0.0f, 3.0f })
    {
        Rig rig = MakeRig(planeY);
        Remap remap;
        CHECK(BuildRemap(rig.names, rig.rest, remap));
        CHECK(remap.valid && remap.pairs == 5);
        CHECK(remap.counterpart[2] == 7 && remap.counterpart[7] == 2);
        CHECK(remap.counterpart[0] == 0 && remap.counterpart[12] == 12 && remap.counterpart[11] == 6);
        // Model placed in the world by W; the world mirror is the model's own plane.
        const Mat34 w = Frame(70, 5, 0, 100, 200, 50);
        const Mat34 m = Mul(Mul(w, remap.restMirror), InverseOrthonormal(w));
        const size_t n = rig.names.size();
        std::vector<Mat34> mirrored(n), out(n);
        for (size_t b = 0; b < n; ++b)
            mirrored[b] = Mul(m, Mul(w, rig.rest[b]));
        Apply(remap, mirrored.data(), out.data(), static_cast<int>(n));
        for (size_t b = 0; b < n; ++b)
        {
            CHECK(NearMat(out[b], Mul(w, rig.rest[b])));
            CHECK(Near(Determinant(out[b]), 1.0f));
        }
    }
}

static void TestPosedRightArmBecomesLeftArm()
{
    Rig rig = MakeRig();
    Remap remap;
    CHECK(BuildRemap(rig.names, rig.rest, remap));
    // The solver's right hand somewhere in the world; mirror across an arbitrary plane.
    const Mat34 hand = Frame(-30, 20, 70, 40, -10, 60);
    Mat34 m{};
    const float p[3] = { 10, 5, 0 }, nrm[3] = { 0.3f, 1.0f, 0.1f };
    CHECK(Reflection(p, nrm, m));
    const size_t n = rig.names.size();
    std::vector<Mat34> mirrored(n, Mul(m, Frame(0, 0, 0, 0, 0, 0))), out(n);
    mirrored[4] = Mul(m, hand);   // R_Hand
    Apply(remap, mirrored.data(), out.data(), static_cast<int>(n));
    // The left hand sits on the mirror image of the right hand, as a proper frame
    // equal to the mirrored right hand with the model's local left/right flip.
    const Mat34 expected = Mul(Mul(m, hand), LocalFlipY());
    CHECK(NearMat(out[9], expected));
    CHECK(Near(Determinant(out[9]), 1.0f));
    float o[3]{};
    const float h[3] = { hand.m[0][3], hand.m[1][3], hand.m[2][3] };
    l4d2vr_hand_mirror::TransformPoint(m, h, o);
    CHECK(Near(out[9].m[0][3], o[0]) && Near(out[9].m[1][3], o[1]) && Near(out[9].m[2][3], o[2]));
}

static void TestRejectsAsymmetricOrUnpairedRigs()
{
    Rig rig = MakeRig();
    rig.rest[7].m[0][3] += 5.0f;   // left hand 5 units off its mirror position
    Remap remap;
    CHECK(!BuildRemap(rig.names, rig.rest, remap));
    CHECK(!remap.valid);
    Rig few;
    few.names = { "root", "Bip01_R_Hand", "Bip01_L_Hand" };
    few.rest = { Frame(0, 0, 0, 0, 0, 0), Frame(0, 0, 0, 1, -5, 0), Frame(0, 0, 0, 1, 5, 0) };
    CHECK(!BuildRemap(few.names, few.rest, remap));  // fewer than 3 pairs
    CHECK(BuildRemap(few.names, few.rest, remap, 1));
}

static void TestComposeModelSpace()
{
    const std::vector<Mat34> local = { Frame(90, 0, 0, 1, 0, 0), Frame(0, 0, 0, 2, 0, 0) };
    const std::vector<Mat34> model = ComposeModelSpace(local, { -1, 0 });
    CHECK(Near(model[1].m[0][3], 1.0f) && Near(model[1].m[1][3], 2.0f));
}

int main()
{
    TestNames();
    TestBindPoseLandsOnCounterpart();
    TestPosedRightArmBecomesLeftArm();
    TestRejectsAsymmetricOrUnpairedRigs();
    TestComposeModelSpace();
    if (g_failures == 0)
        std::printf("vr_left_hand_remap: all tests passed\n");
    return g_failures == 0 ? 0 : 1;
}

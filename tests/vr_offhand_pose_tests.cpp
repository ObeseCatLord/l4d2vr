// The free off hand's orientation in the controller frame (vr_offhand_pose.h).
// Player report: "the off hand is bent inwards even with the controller held
// straight forward; with LeftHanded the right (new off) hand bends the same way".
#include "../L4D2VR/vr_offhand_pose.h"
#include "../L4D2VR/vr_hand_mirror.h"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

namespace op = l4d2vr_offhand_pose;
namespace hm = l4d2vr_hand_mirror;

static int g_failures = 0;
static int g_checks = 0;
#define CHECK(cond) do { ++g_checks; if (!(cond)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

struct V { float x, y, z; };
static float Dot(const V& a, const V& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static float AngleDeg(const V& a, const V& b)
{
    const float c = Dot(a, b) / std::sqrt(Dot(a, a) * Dot(b, b));
    return std::acos(std::fmax(-1.0f, std::fmin(1.0f, c))) * 180.0f / 3.14159265f;
}

// The controller anchor the arm IK uses (hooks_misc.inl
// HooksNativeViewmodelHandsOnlyBuildControllerWorldFromAxes): columns
// (right, up, -forward) of the calibrated controller, here at the origin.
static hm::Mat34 ControllerAnchor(const V& forward, const V& right, const V& up)
{
    hm::Mat34 m{};
    const V cols[3] = { right, up, { -forward.x, -forward.y, -forward.z } };
    for (int c = 0; c < 3; ++c)
    {
        m.m[0][c] = cols[c].x;
        m.m[1][c] = cols[c].y;
        m.m[2][c] = cols[c].z;
    }
    return m;
}

static hm::Mat34 WithLocalRotation(const hm::Mat34& anchor, const float deg[3])
{
    float r[3][3];
    op::LocalRotation(deg, r);
    hm::Mat34 local = hm::Identity();
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            local.m[i][j] = r[i][j];
    return hm::Mul(anchor, local);
}

static V Column(const hm::Mat34& m, int c) { return V{ m.m[0][c], m.m[1][c], m.m[2][c] }; }
static V Neg(const V& v) { return V{ -v.x, -v.y, -v.z }; }

// Bip01_L_Hand's axes (handdump on the survivor arms): +x along the fingers,
// +z toward the index finger's side, -y toward the thumb and palm.
struct HandLook { V fingers, indexSide, palm; };
static HandLook Look(const hm::Mat34& bone) { return HandLook{ Column(bone, 0), Column(bone, 2), Neg(Column(bone, 1)) }; }

static std::string ReadFile(const char* path)
{
    std::ifstream in(path, std::ios::binary);
    std::ostringstream s;
    s << in.rdbuf();
    std::string text = s.str();
    std::string out;
    for (char c : text)
        if (c != '\r')
            out.push_back(c);  // CRLF checkouts
    return out;
}

int main()
{
    // ---- LocalRotation = Rz * Ry * Rx -------------------------------------------
    {
        const float deg[3] = { 30.0f, -20.0f, 50.0f };
        float r[3][3];
        op::LocalRotation(deg, r);
        auto rad = [](float d) { return d * 3.14159265f / 180.0f; };
        const float ax = rad(30.0f), ay = rad(-20.0f), az = rad(50.0f);
        const float Rx[3][3] = { { 1, 0, 0 }, { 0, std::cos(ax), -std::sin(ax) }, { 0, std::sin(ax), std::cos(ax) } };
        const float Ry[3][3] = { { std::cos(ay), 0, std::sin(ay) }, { 0, 1, 0 }, { -std::sin(ay), 0, std::cos(ay) } };
        const float Rz[3][3] = { { std::cos(az), -std::sin(az), 0 }, { std::sin(az), std::cos(az), 0 }, { 0, 0, 1 } };
        float zy[3][3] = {}, e[3][3] = {};
        for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) for (int k = 0; k < 3; ++k) zy[i][j] += Rz[i][k] * Ry[k][j];
        for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) for (int k = 0; k < 3; ++k) e[i][j] += zy[i][k] * Rx[k][j];
        bool same = true;
        for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) same = same && std::fabs(r[i][j] - e[i][j]) < 1e-5f;
        CHECK(same);
    }

    // A controller held straight forward and level, in Source axes
    // (x forward, y left, z up): right = -y.
    const V forward{ 1, 0, 0 }, right{ 0, -1, 0 }, up{ 0, 0, 1 };
    const hm::Mat34 anchor = ControllerAnchor(forward, right, up);
    const float zero[3] = { 0.0f, 0.0f, 0.0f };

    // ---- The bug: the old 0,0,0 default -------------------------------------------
    {
        const HandLook left = Look(WithLocalRotation(anchor, zero));
        // The fingers lie on the controller's right axis: the free LEFT hand is
        // bent 90 degrees in toward the body although the controller points ahead.
        CHECK(AngleDeg(left.fingers, forward) > 89.0f && AngleDeg(left.fingers, forward) < 91.0f);
        CHECK(AngleDeg(left.fingers, right) < 0.5f);  // right of a left hand = inward
        std::printf("old default 0,0,0: left fingers %.1f deg from the controller's forward (toward its right)\n",
            AngleDeg(left.fingers, forward));

        // LeftHanded / ItemHandMirror: the free RIGHT hand is the left pose
        // reflected across its controller's centre plane (vr_hand_mirror.h, Ps).
        hm::Mat34 ps{};
        const float p[3] = { 0, 0, 0 };
        const float n[3] = { right.x, right.y, right.z };
        CHECK(hm::Reflection(p, n, ps));
        const HandLook mirrored = Look(hm::Mul(ps, WithLocalRotation(anchor, zero)));
        CHECK(AngleDeg(mirrored.fingers, Neg(right)) < 0.5f);  // left of a right hand = inward: "bends the same way"
    }

    // ---- The fix: the calibrated default -90,90,0 ------------------------------------
    {
        const hm::Mat34 bone = WithLocalRotation(anchor, op::kDefaultRotationDeg);
        const HandLook left = Look(bone);
        CHECK(AngleDeg(left.fingers, forward) < 0.5f);    // fingers along the controller
        CHECK(AngleDeg(left.indexSide, up) < 0.5f);        // index knuckle on top (thumb up)
        CHECK(AngleDeg(left.palm, right) < 0.5f);          // a left palm faces the other hand
        CHECK(std::fabs(hm::Determinant(bone) - 1.0f) < 1e-4f);
        std::printf("default -90,90,0: left fingers %.1f deg from forward, palm %.1f deg from the controller's right\n",
            AngleDeg(left.fingers, forward), AngleDeg(left.palm, right));

        hm::Mat34 ps{};
        const float p[3] = { 0, 0, 0 };
        const float n[3] = { right.x, right.y, right.z };
        CHECK(hm::Reflection(p, n, ps));
        const HandLook mirrored = Look(hm::Mul(ps, bone));
        CHECK(AngleDeg(mirrored.fingers, forward) < 0.5f);      // mirrored right hand points ahead too
        CHECK(AngleDeg(mirrored.indexSide, up) < 0.5f);
        CHECK(AngleDeg(mirrored.palm, Neg(right)) < 0.5f);      // a right palm faces the other (left) hand

        // The hand-align run's [VR][HandDump] on the tuned install put the
        // controller at (4.26, 0.00, 3.70) units in the hand bone's frame; the
        // defaults reproduce that (43.2 units per metre, before reprojection).
        const float units = 43.2f;
        const V t{ op::kDefaultOffsetMeters[0] * units, op::kDefaultOffsetMeters[1] * units, op::kDefaultOffsetMeters[2] * units };
        float r[3][3];
        op::LocalRotation(op::kDefaultRotationDeg, r);
        // controller origin in the hand frame = -R^T t
        const V local{ -(r[0][0] * t.x + r[1][0] * t.y + r[2][0] * t.z),
            -(r[0][1] * t.x + r[1][1] * t.y + r[2][1] * t.z),
            -(r[0][2] * t.x + r[1][2] * t.y + r[2][2] * t.z) };
        CHECK(std::fabs(local.x - 4.26f) < 0.2f && std::fabs(local.y) < 0.05f && std::fabs(local.z - 3.70f) < 0.3f);
    }

    // ---- Migration rule ----------------------------------------------------------------
    {
        float v[3];
        CHECK(op::ParseVector3("-90,90,0", v) && v[0] == -90.0f && v[1] == 90.0f && v[2] == 0.0f);
        CHECK(op::ParseVector3(" 0.0 , -0.08, 0.1 ", v) && std::fabs(v[1] + 0.08f) < 1e-6f);
        CHECK(!op::ParseVector3("1,2", v) && !op::ParseVector3("1,2,3,4", v) && !op::ParseVector3("a,b,c", v) && !op::ParseVector3("", v));
        CHECK(op::ShouldMigrateLegacyZero(false, ""));           // key missing: take the default
        CHECK(op::ShouldMigrateLegacyZero(true, "0,0,0"));
        CHECK(op::ShouldMigrateLegacyZero(true, "0.0, 0.0, -0.0"));
        CHECK(!op::ShouldMigrateLegacyZero(true, "-90,90,0"));    // tuned: kept
        CHECK(!op::ShouldMigrateLegacyZero(true, "0,0,5"));
        CHECK(!op::ShouldMigrateLegacyZero(true, "junk"));
        CHECK(op::ParseVector3(op::kDefaultRotationText, v) && v[0] == op::kDefaultRotationDeg[0] &&
            v[1] == op::kDefaultRotationDeg[1] && v[2] == op::kDefaultRotationDeg[2]);
        CHECK(op::ParseVector3(op::kDefaultOffsetText, v) && v[0] == op::kDefaultOffsetMeters[0] &&
            v[1] == op::kDefaultOffsetMeters[1] && v[2] == op::kDefaultOffsetMeters[2]);
    }

    // ---- Shipped defaults agree ----------------------------------------------------------
    {
        const std::string rot = std::string("NativeViewmodelLeftHandPoseRotationOffsetDeg=") + op::kDefaultRotationText + "\n";
        const std::string off = std::string("NativeViewmodelLeftHandPoseOffsetMeters=") + op::kDefaultOffsetText + "\n";
        const std::string config = ReadFile(L4D2VR_SOURCE_DIR "/config.txt");
        const std::string sample = ReadFile(L4D2VR_SOURCE_DIR "/vr_config_overlay_embedded.inl");
        const std::string header = ReadFile(L4D2VR_SOURCE_DIR "/vr.h");
        CHECK(!config.empty() && config.find(rot) != std::string::npos && config.find(off) != std::string::npos);
        CHECK(!sample.empty() && sample.find(rot) != std::string::npos && sample.find(off) != std::string::npos);
        CHECK(header.find("m_NativeViewmodelLeftHandPoseRotationOffsetDeg = { -90.0f, 90.0f, 0.0f };") != std::string::npos);
        CHECK(header.find("m_NativeViewmodelLeftHandPoseOffsetMeters = { 0.0f, -0.08f, 0.1f };") != std::string::npos);
    }
    // The right hand mirrors the left (2.0.6): same offset, x = 0 so the mirror keeps it.
    {
        const std::string off = std::string("NativeViewmodelRightHandPoseOffsetMeters=") + op::kDefaultRightOffsetText + "\n";
        const std::string config = ReadFile(L4D2VR_SOURCE_DIR "/config.txt");
        const std::string sample = ReadFile(L4D2VR_SOURCE_DIR "/vr_config_overlay_embedded.inl");
        const std::string header = ReadFile(L4D2VR_SOURCE_DIR "/vr.h");
        CHECK(config.find(off) != std::string::npos && sample.find(off) != std::string::npos);
        CHECK(header.find("m_NativeViewmodelRightHandPoseOffsetMeters = { 0.0f, -0.08f, 0.1f };") != std::string::npos);
        CHECK(op::kDefaultRightOffsetMeters[0] == -op::kDefaultOffsetMeters[0] &&
            op::kDefaultRightOffsetMeters[1] == op::kDefaultOffsetMeters[1] &&
            op::kDefaultRightOffsetMeters[2] == op::kDefaultOffsetMeters[2]);
    }

    if (g_failures)
        return 1;
    std::printf("offhand pose: %d checks passed\n", g_checks);
    return 0;
}

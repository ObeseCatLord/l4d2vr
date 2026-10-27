#include "../L4D2VR/vr_overlay_pointer.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>

using namespace l4d2vr_overlay_pointer;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

static bool Near(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) <= eps; }
static bool NearV(const Vec3& a, const Vec3& b, float eps = 1e-3f)
{
    return Near(a.x, b.x, eps) && Near(a.y, b.y, eps) && Near(a.z, b.z, eps);
}

static Mat34 Translation(float x, float y, float z)
{
    Mat34 m;
    m.m[0][0] = m.m[1][1] = m.m[2][2] = 1.0f;
    m.m[0][3] = x;
    m.m[1][3] = y;
    m.m[2][3] = z;
    return m;
}

// Yaw about +Y, same layout RepositionOverlays builds (facing transform).
static Mat34 Yaw(float radians, const Vec3& position)
{
    Mat34 m = Translation(position.x, position.y, position.z);
    m.m[0][0] = std::cos(radians);
    m.m[0][2] = std::sin(radians);
    m.m[2][0] = -std::sin(radians);
    m.m[2][2] = std::cos(radians);
    return m;
}

static Ray RayAt(const Vec3& origin, const Vec3& target)
{
    Ray r;
    r.origin = origin;
    Normalize(Sub(target, origin), r.direction);
    return r;
}

static void TestFlatPanelCenterAndCorners()
{
    // 1.5 m wide 16:9 panel 3 m in front of the origin (-Z), facing +Z.
    Panel panel;
    CHECK(BuildPanel(Translation(0.0f, 1.0f, -3.0f), 1.5f, 9.0f / 16.0f, 0.0f, RuntimeModel::OpenXRLayer, panel));
    CHECK(Near(panel.width, 1.5f) && Near(panel.height, 1.5f * 9.0f / 16.0f));

    const Vec3 eye{ 0.0f, 1.0f, 0.0f };
    Hit hit;
    CHECK(Intersect(panel, RayAt(eye, Vec3{ 0.0f, 1.0f, -3.0f }), 100.0f, hit));
    CHECK(Near(hit.u, 0.5f) && Near(hit.v, 0.5f) && Near(hit.distance, 3.0f));
    CHECK(NearV(hit.normal, Vec3{ 0.0f, 0.0f, 1.0f }));

    // Top-left corner region -> small u, small v (pixel origin is top-left).
    const float halfW = panel.width * 0.5f, halfH = panel.height * 0.5f;
    CHECK(Intersect(panel, RayAt(eye, Vec3{ -halfW * 0.9f, 1.0f + halfH * 0.9f, -3.0f }), 100.0f, hit));
    CHECK(Near(hit.u, 0.05f) && Near(hit.v, 0.05f));
    // Bottom-right.
    CHECK(Intersect(panel, RayAt(eye, Vec3{ halfW * 0.5f, 1.0f - halfH * 0.5f, -3.0f }), 100.0f, hit));
    CHECK(Near(hit.u, 0.75f) && Near(hit.v, 0.75f));

    // Misses: outside the quad, pointing away, beyond max distance, parallel.
    CHECK(!Intersect(panel, RayAt(eye, Vec3{ halfW * 1.1f, 1.0f, -3.0f }), 100.0f, hit));
    CHECK(!Intersect(panel, RayAt(eye, Vec3{ 0.0f, 1.0f, 3.0f }), 100.0f, hit));
    CHECK(!Intersect(panel, RayAt(eye, Vec3{ 0.0f, 1.0f, -3.0f }), 2.0f, hit));
    CHECK(!Intersect(panel, RayAt(eye, Vec3{ 1.0f, 1.0f, 0.0f }), 100.0f, hit));

    // From behind the panel the hit normal faces the ray origin.
    CHECK(Intersect(panel, RayAt(Vec3{ 0.0f, 1.0f, -5.0f }, Vec3{ 0.0f, 1.0f, -3.0f }), 100.0f, hit));
    CHECK(NearV(hit.normal, Vec3{ 0.0f, 0.0f, -1.0f }));
}

static void TestYawedPanel()
{
    // Panel 2 m to the +X side, rotated so it faces back towards the origin.
    const float yaw = kPi * 0.5f; // +Z normal turns to +X... so place it at -X
    Panel panel;
    CHECK(BuildPanel(Yaw(yaw, Vec3{ -2.0f, 0.0f, 0.0f }), 1.0f, 0.5f, 0.0f, RuntimeModel::OpenXRLayer, panel));
    CHECK(NearV(panel.normal, Vec3{ 1.0f, 0.0f, 0.0f }));
    Hit hit;
    CHECK(Intersect(panel, RayAt(Vec3{ 0.0f, 0.0f, 0.0f }, Vec3{ -2.0f, 0.0f, 0.0f }), 100.0f, hit));
    CHECK(Near(hit.u, 0.5f) && Near(hit.v, 0.5f) && Near(hit.distance, 2.0f));
    // Panel right axis is -Z here (cos 90, 0, -sin 90).
    CHECK(NearV(panel.right, Vec3{ 0.0f, 0.0f, -1.0f }));
    CHECK(Intersect(panel, RayAt(Vec3{ 0.0f, 0.0f, 0.0f }, Vec3{ -2.0f, 0.1f, -0.25f }), 100.0f, hit));
    CHECK(Near(hit.u, 0.75f) && Near(hit.v, 0.3f));
}

static void TestScaledTransformModels()
{
    // The main menu transform carries axis scale (window / backbuffer ratio).
    Mat34 m = Translation(0.0f, 0.0f, -3.0f);
    m.m[0][0] = 0.5f; // x scale
    m.m[1][1] = 2.0f; // y scale
    Panel steam, xr;
    CHECK(BuildPanel(m, 1.0f, 0.5f, 0.0f, RuntimeModel::OpenVRCompositor, steam));
    CHECK(BuildPanel(m, 1.0f, 0.5f, 0.0f, RuntimeModel::OpenXRLayer, xr));
    CHECK(Near(steam.width, 0.5f) && Near(steam.height, 1.0f));
    CHECK(Near(xr.width, 1.0f) && Near(xr.height, 0.5f));
    CHECK(NearV(steam.right, Vec3{ 1.0f, 0.0f, 0.0f }) && NearV(steam.up, Vec3{ 0.0f, 1.0f, 0.0f }));

    // Degenerate / invalid inputs.
    Panel bad;
    CHECK(!BuildPanel(m, 0.0f, 0.5f, 0.0f, RuntimeModel::OpenXRLayer, bad));
    CHECK(!BuildPanel(m, 1.0f, -1.0f, 0.0f, RuntimeModel::OpenXRLayer, bad));
    Mat34 nan = m;
    nan.m[1][3] = std::numeric_limits<float>::quiet_NaN();
    CHECK(!BuildPanel(nan, 1.0f, 0.5f, 0.0f, RuntimeModel::OpenXRLayer, bad));
    Mat34 zero;
    CHECK(!BuildPanel(zero, 1.0f, 0.5f, 0.0f, RuntimeModel::OpenXRLayer, bad));
}

static void TestCurvedPanel()
{
    // HUD-like: 1.3 m arc, curvature 0.2, 16:9 texture, 1 m in front.
    const float aspect = 9.0f / 16.0f;
    Panel steam, xr;
    CHECK(BuildPanel(Translation(0.0f, 0.0f, -1.0f), 1.3f, aspect, 0.2f, RuntimeModel::OpenVRCompositor, steam));
    CHECK(BuildPanel(Translation(0.0f, 0.0f, -1.0f), 1.3f, aspect, 0.2f, RuntimeModel::OpenXRLayer, xr));
    CHECK(Near(steam.height, 1.3f * aspect));
    CHECK(Near(xr.height, 1.3f / aspect)); // XR cylinder aspectRatio quirk
    CHECK(Near(xr.curvature, 0.2f));

    const Vec3 eye{ 0.0f, 0.0f, 0.0f };
    Hit hit;
    CHECK(Intersect(steam, RayAt(eye, Vec3{ 0.0f, 0.0f, -1.0f }), 100.0f, hit));
    CHECK(Near(hit.u, 0.5f) && Near(hit.v, 0.5f) && Near(hit.distance, 1.0f));
    CHECK(NearV(hit.normal, Vec3{ 0.0f, 0.0f, 1.0f }));

    // A point on the arc at a quarter of the width: angle = -0.25 * central angle.
    const float angle = 2.0f * kPi * 0.2f;
    const float radius = 1.3f / angle;
    const Vec3 axis{ 0.0f, 0.0f, -1.0f + radius };
    const float a = -0.25f * angle;
    const Vec3 onArc{ axis.x + radius * std::sin(a), 0.1f, axis.z - radius * std::cos(a) };
    CHECK(Intersect(steam, RayAt(eye, onArc), 100.0f, hit));
    CHECK(Near(hit.u, 0.25f) && Near(hit.v, 0.5f - 0.1f / steam.height));
    CHECK(NearV(hit.point, onArc));
    // Inward normal points back at the axis.
    Vec3 expectNormal;
    Normalize(Sub(Vec3{ axis.x, 0.1f, axis.z }, onArc), expectNormal);
    CHECK(NearV(hit.normal, Vec3{ expectNormal.x, 0.0f, expectNormal.z }));

    // Past the end of the arc misses even though the full cylinder continues.
    const float beyond = 0.6f * angle;
    const Vec3 offArc{ axis.x + radius * std::sin(beyond), 0.0f, axis.z - radius * std::cos(beyond) };
    CHECK(!Intersect(steam, RayAt(eye, offArc), 100.0f, hit));

    // A viewer far outside the cylinder still hits the front of the arc.
    CHECK(Intersect(steam, RayAt(Vec3{ 0.0f, 0.0f, 5.0f }, Vec3{ 0.0f, 0.0f, -1.0f }), 100.0f, hit));
    CHECK(Near(hit.u, 0.5f) && Near(hit.distance, 6.0f));

    // Ray along the cylinder axis cannot hit.
    Ray vertical;
    vertical.origin = eye;
    vertical.direction = Vec3{ 0.0f, 1.0f, 0.0f };
    CHECK(!Intersect(steam, vertical, 100.0f, hit));

    // Curvature below the threshold is treated as flat.
    Panel almostFlat;
    CHECK(BuildPanel(Translation(0.0f, 0.0f, -1.0f), 1.3f, aspect, 0.0005f, RuntimeModel::OpenXRLayer, almostFlat));
    CHECK(almostFlat.curvature == 0.0f && Near(almostFlat.height, 1.3f * aspect));
}

static void TestControllerRay()
{
    // Controller 1 m up, pitched down 90 degrees: -Z of the pose points to -Y.
    Mat34 pose = Translation(0.0f, 1.0f, 0.0f);
    pose.m[1][1] = 0.0f;
    pose.m[1][2] = 1.0f;
    pose.m[2][1] = -1.0f;
    pose.m[2][2] = 0.0f;
    Ray ray;
    CHECK(RayFromPose(pose, ray));
    CHECK(NearV(ray.origin, Vec3{ 0.0f, 1.0f, 0.0f }) && NearV(ray.direction, Vec3{ 0.0f, -1.0f, 0.0f }));

    // Tip offset composes as pose * tip.
    const Mat34 tip = Translation(0.0f, 0.0f, -0.1f);
    const Mat34 world = Multiply(pose, tip);
    CHECK(RayFromPose(world, ray));
    CHECK(NearV(ray.origin, Vec3{ 0.0f, 0.9f, 0.0f }));

    Mat34 broken = pose;
    broken.m[0][0] = std::numeric_limits<float>::infinity();
    CHECK(!RayFromPose(broken, ray));
}

static void TestUvToPixel()
{
    Pixel p = UvToPixel(0.5f, 0.25f, 1920, 1080);
    CHECK(p.x == 960 && p.y == 270);
    p = UvToPixel(1.0f, 1.0f, 1920, 1080);
    CHECK(p.x == 1919 && p.y == 1079);
    p = UvToPixel(-0.2f, 2.0f, 800, 600);
    CHECK(p.x == 0 && p.y == 599);
    p = UvToPixel(0.5f, 0.5f, 0, 600);
    CHECK(p.x == 0 && p.y == 0);
}

static void TestMarkerAndBeam()
{
    Panel panel;
    CHECK(BuildPanel(Translation(0.0f, 1.0f, -3.0f), 1.5f, 0.5625f, 0.0f, RuntimeModel::OpenXRLayer, panel));
    Hit hit;
    CHECK(Intersect(panel, RayAt(Vec3{ 0.0f, 1.0f, 0.0f }, Vec3{ 0.2f, 1.1f, -3.0f }), 100.0f, hit));
    Mat34 marker;
    CHECK(MarkerTransform(panel, hit, 0.01f, marker));
    CHECK(NearV(Column(marker, 3), Vec3{ 0.2f, 1.1f, -2.99f }));
    CHECK(NearV(Column(marker, 0), panel.right) && NearV(Column(marker, 1), panel.up) &&
        NearV(Column(marker, 2), panel.normal));

    Mat34 beam;
    float length = 0.0f;
    CHECK(BeamTransform(Vec3{ 0.0f, 1.0f, 0.0f }, Vec3{ 0.0f, 1.0f, -2.0f }, Vec3{ 0.0f, 1.5f, 0.2f }, beam, length));
    CHECK(Near(length, 2.0f));
    CHECK(NearV(Column(beam, 3), Vec3{ 0.0f, 1.0f, -1.0f }));
    CHECK(NearV(Column(beam, 0), Vec3{ 0.0f, 0.0f, -1.0f }));
    // Face turns towards the viewer (above the beam).
    CHECK(Dot(Column(beam, 2), Vec3{ 0.0f, 1.0f, 0.0f }) > 0.99f);
    // Orthonormal basis.
    CHECK(Near(Dot(Column(beam, 0), Column(beam, 1)), 0.0f) && Near(Length(Column(beam, 1)), 1.0f));
    // Viewer on the beam line still yields a valid basis.
    CHECK(BeamTransform(Vec3{ 0.0f, 0.0f, 0.0f }, Vec3{ 0.0f, 0.0f, -1.0f }, Vec3{ 0.0f, 0.0f, 1.0f }, beam, length));
    CHECK(Near(Length(Column(beam, 2)), 1.0f) && Near(Dot(Column(beam, 2), Column(beam, 0)), 0.0f));
    CHECK(!BeamTransform(Vec3{ 1.0f, 1.0f, 1.0f }, Vec3{ 1.0f, 1.0f, 1.0f }, Vec3{}, beam, length));
}

static PointerDecision Step(PointerTracker& t, bool hitL, bool hitR, bool trigL, bool trigR)
{
    PointerFrame f;
    f.hits[kLeft] = hitL;
    f.hits[kRight] = hitR;
    f.trigger[kLeft] = trigL;
    f.trigger[kRight] = trigR;
    return StepTracker(t, f);
}

static void TestTrackerClickAndDrag()
{
    PointerTracker t;
    const bool none[2] = { false, false };
    ResetTracker(t, none);

    // Right hand hovering: cursor follows right, no click.
    PointerDecision d = Step(t, false, true, false, false);
    CHECK(d.hand == kRight && d.action == ClickAction::None);
    // Trigger down: press once.
    d = Step(t, false, true, false, true);
    CHECK(d.hand == kRight && d.action == ClickAction::Press);
    // Held while dragging off the panel: stays on the right hand, no repeat.
    d = Step(t, false, false, false, true);
    CHECK(d.hand == kRight && d.action == ClickAction::None);
    // Release even though the ray left the panel.
    d = Step(t, false, false, false, false);
    CHECK(d.hand == kRight && d.action == ClickAction::Release);
    d = Step(t, false, false, false, false);
    CHECK(d.hand == -1 && d.action == ClickAction::None);
}

static void TestTrackerHandSelection()
{
    PointerTracker t;
    const bool none[2] = { false, false };
    ResetTracker(t, none);

    // Both rays on the panel: right is the default.
    CHECK(Step(t, true, true, false, false).hand == kRight);
    // Left trigger press switches the pointer to the left hand.
    PointerDecision d = Step(t, true, true, true, false);
    CHECK(d.hand == kLeft && d.action == ClickAction::Press);
    // Other trigger while the button is down does nothing.
    d = Step(t, true, true, true, true);
    CHECK(d.hand == kLeft && d.action == ClickAction::None);
    d = Step(t, true, true, false, true);
    CHECK(d.hand == kLeft && d.action == ClickAction::Release);
    // Left stays preferred afterwards.
    CHECK(Step(t, true, true, false, false).hand == kLeft);
    // Preferred hand off the panel: fall back to the other hand.
    CHECK(Step(t, false, true, false, false).hand == kRight);

    // A trigger pressed on a hand that is not on the panel does not click.
    d = Step(t, false, true, true, false);
    CHECK(d.hand == kRight && d.action == ClickAction::None);
    d = Step(t, false, true, false, false);
    CHECK(d.action == ClickAction::None);
}

static void TestTrackerHeldTriggerOnReset()
{
    PointerTracker t;
    const bool held[2] = { false, true };
    ResetTracker(t, held);
    // Trigger was already held when the menu opened: no click until re-pressed.
    PointerDecision d = Step(t, false, true, false, true);
    CHECK(d.action == ClickAction::None && d.hand == kRight);
    d = Step(t, false, true, false, false);
    CHECK(d.action == ClickAction::None);
    d = Step(t, false, true, false, true);
    CHECK(d.action == ClickAction::Press);
    // Preferred hand survives a reset; button state does not.
    ResetTracker(t, held);
    CHECK(!t.buttonDown && t.preferredHand == kRight);
}

int main()
{
    TestFlatPanelCenterAndCorners();
    TestYawedPanel();
    TestScaledTransformModels();
    TestCurvedPanel();
    TestControllerRay();
    TestUvToPixel();
    TestMarkerAndBeam();
    TestTrackerClickAndDrag();
    TestTrackerHandSelection();
    TestTrackerHeldTriggerOnReset();
    if (g_failures)
    {
        std::printf("%d failure(s)\n", g_failures);
        return EXIT_FAILURE;
    }
    std::printf("vr_overlay_pointer tests passed\n");
    return EXIT_SUCCESS;
}

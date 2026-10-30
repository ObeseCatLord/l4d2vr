// OpenXR overlays: geometry, layer planning, the synthetic overlay mouse and the
// in-mod keyboard (L4D2VR/vr_backend/openxr_overlay_logic.h).
#include "../L4D2VR/vr_backend/openxr_overlay_logic.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

static int checks = 0;
static void Check(bool passed, const char* contract)
{
    ++checks;
    if (!passed)
    {
        std::fprintf(stderr, "FAIL: %s\n", contract);
        std::exit(1);
    }
}

static bool Near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }

namespace ov = l4d2vr_openxr_overlay;
namespace op = l4d2vr_overlay_pointer;
namespace xrm = l4d2vr_openxr;

static op::Mat34 Translation(float x, float y, float z)
{
    op::Mat34 m;
    m.m[0][0] = m.m[1][1] = m.m[2][2] = 1.0f;
    m.m[0][3] = x;
    m.m[1][3] = y;
    m.m[2][3] = z;
    return m;
}

static op::Mat34 YawTranslation(float yaw, float x, float y, float z)
{
    op::Mat34 m = Translation(x, y, z);
    m.m[0][0] = std::cos(yaw); m.m[0][2] = std::sin(yaw);
    m.m[2][0] = -std::sin(yaw); m.m[2][2] = std::cos(yaw);
    return m;
}

static op::Ray RayAt(const op::Vec3& from, const op::Vec3& to)
{
    op::Ray r;
    r.origin = from;
    op::Normalize(op::Sub(to, from), r.direction);
    return r;
}

static void GeometryTests()
{
    // Flat quad, identity rotation: the pose is the transform, height from the texture aspect.
    {
        ov::Shape s;
        s.transform = Translation(0.1f, 1.4f, -2.0f);
        s.widthMeters = 1.6f;
        s.texWidth = 1600;
        s.texHeight = 900;
        ov::Geometry g;
        Check(ov::ComputeGeometry(s, true, g), "flat geometry valid");
        Check(!g.cylinder, "curvature 0 is a quad");
        Check(Near(g.width, 1.6f) && Near(g.height, 0.9f), "quad size = width x width*texH/texW");
        Check(Near(g.pose.position.x, 0.1f) && Near(g.pose.position.y, 1.4f) && Near(g.pose.position.z, -2.0f), "quad centre");
        Check(Near(g.pose.orientation.w, 1.0f), "identity orientation");
        Check(g.region.x == 0 && g.region.y == 0 && g.region.width == 1600 && g.region.height == 900, "full region");
    }
    // Texture bounds select a sub-rect; the size follows the visible aspect.
    {
        ov::Shape s;
        s.transform = Translation(0, 0, 0);
        s.widthMeters = 1.0f;
        s.texWidth = 1000;
        s.texHeight = 1000;
        s.bounds = { 0.0f, 0.5f, 0.5f, 1.0f };  // bottom-left quarter
        ov::Geometry g;
        Check(ov::ComputeGeometry(s, true, g), "bounded geometry valid");
        Check(g.region.x == 0 && g.region.y == 500 && g.region.width == 500 && g.region.height == 500, "region from bounds (v down)");
        Check(Near(g.height, 1.0f), "square region gives a square quad");
        s.bounds = { 1.0f, 0.0f, 0.0f, 1.0f };
        Check(ov::ComputeGeometry(s, true, g) && g.region.flipU && !g.region.flipV && g.region.width == 1000, "u flip detected, full width");
    }
    // Texel aspect 2 (texels twice as wide as tall) halves the height.
    {
        ov::Shape s;
        s.transform = Translation(0, 0, 0);
        s.widthMeters = 1.0f;
        s.texelAspect = 2.0f;
        s.texWidth = 100;
        s.texHeight = 100;
        ov::Geometry g;
        Check(ov::ComputeGeometry(s, true, g) && Near(g.height, 0.5f), "texel aspect divides the height");
    }
    // SteamVR applies the transform's axis scale (the main menu transform scales x and y).
    {
        ov::Shape s;
        s.transform = YawTranslation(0.6f, 0.0f, 1.0f, -3.0f);
        for (int r = 0; r < 3; ++r)
        {
            s.transform.m[r][0] *= 1.5f;
            s.transform.m[r][1] *= 0.5f;
        }
        s.widthMeters = 2.0f;
        s.texWidth = 200;
        s.texHeight = 100;
        ov::Geometry g;
        Check(ov::ComputeGeometry(s, true, g), "scaled geometry valid");
        Check(Near(g.width, 3.0f) && Near(g.height, 0.5f), "axis scale stretches width and height");
        const vr::HmdMatrix34_t m = xrm::ToMatrix(g.pose);
        Check(Near(m.m[0][0], std::cos(0.6f)) && Near(m.m[0][2], std::sin(0.6f)) && Near(m.m[1][1], 1.0f), "rotation without the scale");
    }
    // Curved: arc length = width, axis radius meters in front (+Z), aspect width/height.
    {
        ov::Shape s;
        s.transform = Translation(0.0f, 1.5f, -1.3f);
        s.widthMeters = 1.3f;
        s.curvature = 0.2f;
        s.texWidth = 1920;
        s.texHeight = 1080;
        ov::Geometry g;
        Check(ov::ComputeGeometry(s, true, g) && g.cylinder, "curvature 0.2 is a cylinder");
        const float angle = 2.0f * ov::kPi * 0.2f;
        Check(Near(g.centralAngle, angle) && Near(g.radius, 1.3f / angle), "radius = width / (2 pi curvature)");
        Check(Near(g.pose.position.z, -1.3f + 1.3f / angle) && Near(g.pose.position.y, 1.5f), "axis lies radius meters along +Z");
        Check(Near(g.aspectRatio, 1.3f / (1.3f * 1080.0f / 1920.0f)), "aspect = arc width / height");
        // The OpenXR cylinder height (radius * angle / aspect) equals the OpenVR height.
        Check(Near(g.radius * g.centralAngle / g.aspectRatio, g.height), "cylinder height matches");
        // Without the extension the panel stays flat.
        Check(ov::ComputeGeometry(s, false, g) && !g.cylinder && Near(g.panel.curvature, 0.0f), "no cylinder extension -> quad");
    }
    // The hit-test panel matches the layer: centre ray hits uv (0.5, 0.5), edges map to 0/1.
    {
        ov::Shape s;
        s.transform = YawTranslation(0.3f, 0.2f, 1.2f, -1.0f);
        s.widthMeters = 0.8f;
        s.texWidth = 800;
        s.texHeight = 400;
        ov::Geometry g;
        ov::ComputeGeometry(s, true, g);
        const op::Vec3 c = g.panel.center;
        const op::Vec3 eye = op::Add(c, op::Scale(g.panel.normal, 1.0f));
        op::Hit hit;
        Check(op::Intersect(g.panel, RayAt(eye, c), 5.0f, hit) && Near(hit.u, 0.5f) && Near(hit.v, 0.5f), "centre hit");
        const op::Vec3 topLeft = op::Add(op::Add(c, op::Scale(g.panel.right, -0.39f)), op::Scale(g.panel.up, 0.19f));
        Check(op::Intersect(g.panel, RayAt(eye, topLeft), 5.0f, hit) && hit.u < 0.02f && hit.v < 0.03f, "top-left hit");
    }
    // Invalid inputs are rejected.
    {
        ov::Shape s;
        s.transform = Translation(0, 0, 0);
        s.widthMeters = 1.0f;
        ov::Geometry g;
        Check(!ov::ComputeGeometry(s, true, g), "no texture size -> invalid");
        s.texWidth = s.texHeight = 4;
        s.transform.m[0][3] = NAN;
        Check(!ov::ComputeGeometry(s, true, g), "non-finite transform -> invalid");
    }
}

static void PlanTests()
{
    auto cand = [](uint64_t id, uint32_t sort, uint64_t serial, ov::Priority p) {
        ov::Candidate c;
        c.id = id; c.sortOrder = sort; c.serial = serial; c.priority = p;
        return c;
    };
    // Back to front by sort order, creation order on ties.
    {
        std::vector<ov::Candidate> c = { cand(1, 5, 1, ov::Priority::High), cand(2, 1, 2, ov::Priority::High),
            cand(3, 5, 0, ov::Priority::Normal) };
        const ov::Plan p = ov::PlanLayers(c, 128, true, ov::Workaround::Off);
        Check(p.order.size() == 3 && c[p.order[0]].id == 2 && c[p.order[1]].id == 3 && c[p.order[2]].id == 1, "sorted by sort order then serial");
        Check(!p.dummyFirst && p.layerCount == 4, "no dummy on native runtimes");
    }
    // Wine: a dummy quad first when a projection is followed by 2+ layers.
    {
        std::vector<ov::Candidate> one = { cand(1, 0, 0, ov::Priority::High) };
        std::vector<ov::Candidate> two = { cand(1, 0, 0, ov::Priority::High), cand(2, 0, 1, ov::Priority::High) };
        Check(!ov::PlanLayers(one, 128, true, ov::Workaround::Auto).dummyFirst, "proj + 1 quad needs no dummy");
        Check(ov::PlanLayers(one, 128, true, ov::Workaround::Always).dummyFirst, "Always mode adds it for 1");
        const ov::Plan p = ov::PlanLayers(two, 128, true, ov::Workaround::Auto);
        Check(p.dummyFirst && p.layerCount == 4, "proj + 2 quads -> dummy first, 4 layers");
        Check(!ov::PlanLayers(two, 128, false, ov::Workaround::Auto).dummyFirst, "quads only (menu) need no dummy");
    }
    // Budget: the projection and the dummy count; low priority goes first.
    {
        std::vector<ov::Candidate> c;
        c.push_back(cand(100, 0x7FFFFF10u, 0, ov::Priority::High));  // config panel
        c.push_back(cand(101, 0, 1, ov::Priority::High));            // HUD
        c.push_back(cand(102, 0, 2, ov::Priority::Normal));          // wrist
        for (uint64_t i = 0; i < 16; ++i)
            c.push_back(cand(200 + i, 0, 10 + i, ov::Priority::Low));
        const ov::Plan p = ov::PlanLayers(c, 16, true, ov::Workaround::Auto);
        Check(p.layerCount == 16, "never more than maxLayerCount");
        Check(p.dummyFirst && p.order.size() == 14 && p.dropped.size() == 5, "16 = dummy + projection + 14 overlays");
        bool highKept = true;
        for (size_t i : p.dropped)
            if (c[i].priority != ov::Priority::Low)
                highKept = false;
        Check(highKept, "only kill indicators are dropped");
        Check(c[p.order.back()].id == 100, "config panel stays on top");
        // Earlier-created indicators survive (stable).
        bool firstKept = false;
        for (size_t i : p.order)
            if (c[i].id == 200) firstKept = true;
        Check(firstKept, "oldest indicator kept");
    }
    // Tiny budgets: never a dummy that does not fit.
    {
        std::vector<ov::Candidate> two = { cand(1, 0, 0, ov::Priority::High), cand(2, 1, 1, ov::Priority::High) };
        const ov::Plan p3 = ov::PlanLayers(two, 3, true, ov::Workaround::Auto);
        Check(p3.layerCount <= 3 && p3.order.size() == 1 && !p3.dummyFirst && two[p3.order[0]].id == 2, "limit 3: projection + top overlay");
        const ov::Plan p1 = ov::PlanLayers(two, 1, true, ov::Workaround::Auto);
        Check(p1.layerCount == 1 && p1.order.empty(), "limit 1: projection only");
        const ov::Plan p0 = ov::PlanLayers(two, 0, false, ov::Workaround::Auto);
        Check(p0.order.size() == 2, "unknown limit = 16");
    }
    Check(ov::PriorityForKey("KillIndicatorOverlayKey_7") == ov::Priority::Low, "kill indicator priority");
    Check(ov::PriorityForKey("LeftWristHudOverlayKey") == ov::Priority::Normal, "wrist priority");
    Check(ov::PriorityForKey("MenuOverlayKey") == ov::Priority::High, "menu priority");
}

static void AlphaFlipTests()
{
    uint8_t px[8] = { 10, 20, 30, 200, 1, 2, 3, 255 };
    ov::ApplyAlpha(px, 2, 0.5f, false);
    Check(px[3] == 100 && px[7] == 127 && px[0] == 10, "alpha scales the texture alpha only");
    ov::ApplyAlpha(px, 2, 0.25f, true);
    Check(px[3] == 63 && px[7] == 63, "ignored texture alpha becomes the overlay alpha");
    uint8_t img[2 * 2 * 4] = { 1, 0, 0, 0, 2, 0, 0, 0, 3, 0, 0, 0, 4, 0, 0, 0 };
    ov::FlipPixels(img, 2, 2, true, false);
    Check(img[0] == 2 && img[4] == 1 && img[8] == 4 && img[12] == 3, "horizontal flip");
    ov::FlipPixels(img, 2, 2, false, true);
    Check(img[0] == 4 && img[4] == 3 && img[8] == 2 && img[12] == 1, "vertical flip");
}

static void AimTests()
{
    // A controller at (0.2, 1.3, -0.5) pointing -Z, tip 5 cm forward; aim at a point up-left.
    const op::Mat34 raw = Translation(0.2f, 1.3f, -0.5f);
    const op::Mat34 tip = Translation(0.0f, 0.0f, -0.05f);
    const op::Vec3 target{ -0.4f, 1.8f, -2.0f };
    const op::Mat34 aimed = ov::AimPoseAt(raw, tip, target);
    const op::Mat34 tipWorld = op::Multiply(aimed, tip);
    op::Ray ray;
    Check(op::RayFromPose(tipWorld, ray), "aimed ray");
    op::Vec3 want;
    op::Normalize(op::Sub(target, ray.origin), want);
    Check(op::Dot(want, ray.direction) > 0.99999f, "tip ray points at the target");
    Check(Near(ray.origin.x, 0.2f) && Near(ray.origin.y, 1.3f) && Near(ray.origin.z, -0.55f), "tip stays where it was");
    // Already aimed: unchanged; straight behind: still aims.
    const op::Mat34 same = ov::AimPoseAt(raw, tip, op::Vec3{ 0.2f, 1.3f, -3.0f });
    Check(Near(same.m[0][0], 1.0f) && Near(same.m[2][3], -0.5f), "already aimed is identity");
    const op::Mat34 back = ov::AimPoseAt(raw, tip, op::Vec3{ 0.2f, 1.3f, 3.0f });
    op::Ray rb;
    op::RayFromPose(op::Multiply(back, tip), rb);
    Check(rb.direction.z > 0.9999f, "half turn aims backwards");
}

static ov::Target MakeTarget(uint64_t id, float z, float width, float sx, float sy, uint32_t sort = 0, bool scroll = false)
{
    ov::Shape s;
    s.transform = Translation(0.0f, 1.5f, z);
    s.widthMeters = width;
    s.texWidth = 1000;
    s.texHeight = 500;
    ov::Geometry g;
    ov::ComputeGeometry(s, true, g);
    ov::Target t;
    t.id = id;
    t.panel = g.panel;
    t.sortOrder = sort;
    t.mouseScaleX = sx;
    t.mouseScaleY = sy;
    t.scrollEvents = scroll;
    return t;
}

static ov::HandInput Aim(const op::Vec3& target, float trigger, float stickY = 0.0f)
{
    ov::HandInput h;
    h.valid = true;
    h.ray = RayAt(op::Vec3{ 0.2f, 1.3f, 0.0f }, target);
    h.trigger = trigger;
    h.stickY = stickY;
    return h;
}

static void PointerTests()
{
    std::vector<ov::Target> targets = { MakeTarget(7, -2.0f, 2.0f, 1280.0f, 720.0f, 0, true) };
    ov::Pointer pointer;
    std::vector<ov::Event> ev;
    ov::HandInput hands[2];

    // Hover: one move in mouse coordinates (y up from the bottom edge).
    hands[1] = Aim(op::Vec3{ 0.5f, 1.75f, -2.0f }, 0.0f);   // u 0.75, v 0.25
    ov::PointerOutput out = pointer.Step(targets, hands, 0, ev);
    Check(out.active == 1 && out.hands[1].hit && ev.size() == 1 && ev[0].type == ov::EventType::Move, "hover moves");
    Check(Near(ev[0].x, 960.0f, 0.5f) && Near(ev[0].y, 540.0f, 0.5f), "mouse x = u*scale, y = (1-v)*scale");
    Check(out.engaged[1] && !out.engaged[0], "pointing hand's trigger belongs to the overlay");
    ev.clear();
    pointer.Step(targets, hands, 10, ev);
    Check(ev.empty(), "no repeat move without motion");

    // Light trigger below the click threshold does nothing; a full pull clicks.
    hands[1].trigger = 0.5f;
    pointer.Step(targets, hands, 20, ev);
    Check(ev.empty(), "half trigger no click");
    hands[1].trigger = 1.0f;
    out = pointer.Step(targets, hands, 30, ev);
    Check(ev.size() == 1 && ev[0].type == ov::EventType::Down && ev[0].overlay == 7 && out.pressed, "full pull -> button down");
    ev.clear();
    // Drag: moves go to the captured overlay; release gives up.
    hands[1] = Aim(op::Vec3{ 0.0f, 1.5f, -2.0f }, 1.0f);
    pointer.Step(targets, hands, 40, ev);
    Check(ev.size() == 1 && ev[0].type == ov::EventType::Move && Near(ev[0].x, 640.0f, 0.5f), "drag move");
    ev.clear();
    hands[1].trigger = 0.5f;  // hysteresis: still held
    pointer.Step(targets, hands, 50, ev);
    Check(ev.empty(), "trigger hysteresis holds the button");
    hands[1].trigger = 0.2f;
    out = pointer.Step(targets, hands, 60, ev);
    Check(ev.size() == 1 && ev[0].type == ov::EventType::Up && !out.pressed, "release -> button up");
    ev.clear();

    // Dragging off the panel and releasing still ends on the captured overlay.
    hands[1] = Aim(op::Vec3{ 0.0f, 1.5f, -2.0f }, 1.0f);
    pointer.Step(targets, hands, 70, ev);
    ev.clear();
    hands[1] = Aim(op::Vec3{ 5.0f, 1.5f, -2.0f }, 1.0f);
    out = pointer.Step(targets, hands, 80, ev);
    Check(ev.empty() && out.pressed && out.engaged[1], "off-panel drag keeps the capture, no move");
    hands[1].trigger = 0.0f;
    pointer.Step(targets, hands, 90, ev);
    Check(ev.size() == 1 && ev[0].type == ov::EventType::Up && ev[0].overlay == 7, "release off the panel goes to the captured overlay");
    ev.clear();

    // A drag keeps going to the captured overlay under a co-planar copy in front
    // (the config panel's front/back copies swap while it is used).
    {
        std::vector<ov::Target> two = { MakeTarget(7, -2.0f, 2.0f, 1280.0f, 720.0f, 0), MakeTarget(8, -2.0f, 2.0f, 1280.0f, 720.0f, 9) };
        ov::Pointer p2;
        std::vector<ov::Event> e2;
        ov::HandInput h2[2];
        h2[1] = Aim(op::Vec3{ 0.0f, 1.5f, -2.0f }, 1.0f);
        p2.Step(two, h2, 0, e2);  // held at appearance: no click
        h2[1].trigger = 0.0f;
        p2.Step(two, h2, 10, e2);
        e2.clear();
        h2[1].trigger = 1.0f;
        p2.Step(two, h2, 20, e2);
        Check(!e2.empty() && e2.back().type == ov::EventType::Down && e2.back().overlay == 8, "tie goes to the higher sort order");
        e2.clear();
        std::swap(two[0].sortOrder, two[1].sortOrder);  // the copies swap
        h2[1] = Aim(op::Vec3{ 0.5f, 1.5f, -2.0f }, 1.0f);
        p2.Step(two, h2, 30, e2);
        Check(e2.size() == 1 && e2[0].type == ov::EventType::Move && e2[0].overlay == 8 && Near(e2[0].x, 960.0f, 0.5f),
            "drag moves stay on the captured copy");
        e2.clear();
        h2[1].trigger = 0.0f;
        p2.Step(two, h2, 40, e2);
        Check(e2.size() == 1 && e2[0].type == ov::EventType::Up && e2[0].overlay == 8 && Near(e2[0].x, 960.0f, 0.5f), "up at the drag end");
    }

    // A trigger already held when the pointer appears must be pressed again.
    hands[1] = Aim(op::Vec3{ 5.0f, 1.5f, -2.0f }, 1.0f);
    pointer.Step(targets, hands, 100, ev);
    hands[1] = Aim(op::Vec3{ 0.0f, 1.5f, -2.0f }, 1.0f);
    ev.clear();
    pointer.Step(targets, hands, 110, ev);
    bool down = false;
    for (const ov::Event& e : ev) down |= e.type == ov::EventType::Down;
    Check(!down, "held trigger sweeping onto the panel does not click");
    hands[1].trigger = 0.0f;
    pointer.Step(targets, hands, 120, ev);
    ev.clear();

    // Two hands: the left trigger clicks with the left ray and becomes the pointer hand.
    hands[0] = Aim(op::Vec3{ -0.5f, 1.5f, -2.0f }, 1.0f);
    hands[1] = Aim(op::Vec3{ 0.5f, 1.5f, -2.0f }, 0.0f);
    out = pointer.Step(targets, hands, 130, ev);
    Check(out.active == 0, "left trigger takes the pointer");
    bool leftDown = false;
    for (const ov::Event& e : ev) leftDown |= e.type == ov::EventType::Down && e.hand == 0 && Near(e.x, 320.0f, 0.5f);
    Check(leftDown, "left click at the left ray");
    hands[0].trigger = 0.0f;
    pointer.Step(targets, hands, 140, ev);
    ev.clear();
    out = pointer.Step(targets, hands, 150, ev);
    Check(out.active == 0, "left stays preferred while it points");
    hands[0].valid = false;
    ev.clear();

    // Nearest overlay wins (a keyboard in front of the panel).
    targets.push_back(MakeTarget(9, -1.0f, 0.6f, 1200.0f, 520.0f, 5));
    hands[1] = Aim(op::Vec3{ 0.0f, 1.5f, -1.0f }, 0.0f);
    out = pointer.Step(targets, hands, 160, ev);
    Check(out.hands[1].hit && targets[out.hands[1].target].id == 9, "nearest overlay is the target");
    ev.clear();
    targets.pop_back();

    // Discrete scroll from the stick: once on the push, repeating every 200 ms.
    hands[1] = Aim(op::Vec3{ 0.0f, 1.5f, -2.0f }, 0.0f, 0.9f);
    pointer.Step(targets, hands, 1000, ev);
    int scrolls = 0;
    for (const ov::Event& e : ev) if (e.type == ov::EventType::Scroll) { ++scrolls; Check(e.scrollY > 0.0f, "stick up scrolls up"); }
    Check(scrolls == 1, "first scroll immediately");
    ev.clear();
    pointer.Step(targets, hands, 1100, ev);
    Check(ev.empty(), "no repeat before 200 ms");
    pointer.Step(targets, hands, 1210, ev);
    Check(ev.size() == 1 && ev[0].type == ov::EventType::Scroll, "repeat after 200 ms");
    ev.clear();
    hands[1].stickY = -0.2f;
    pointer.Step(targets, hands, 1300, ev);
    hands[1].stickY = -0.8f;
    pointer.Step(targets, hands, 1310, ev);
    Check(ev.size() == 1 && ev[0].scrollY < 0.0f, "stick down scrolls down");
    ev.clear();

    // The captured overlay disappearing releases the button.
    hands[1] = Aim(op::Vec3{ 0.0f, 1.5f, -2.0f }, 1.0f);
    pointer.Step(targets, hands, 2000, ev);
    ev.clear();
    std::vector<ov::Target> none;
    out = pointer.Step(none, hands, 2010, ev);
    Check(ev.size() == 1 && ev[0].type == ov::EventType::Up && !out.pressed, "hidden overlay gets the button up");
    Check(out.engaged[1], "the clicking trigger stays engaged until released");
    hands[1].trigger = 0.0f;
    out = pointer.Step(none, hands, 2020, ev);
    Check(!out.engaged[1], "released trigger is free again");
}

static void KeyboardTests()
{
    const std::vector<ov::Key>& keys = ov::KeyboardLayout();
    for (const ov::Key& k : keys)
        Check(k.x >= 0 && k.y >= ov::kKeyboardKeysTop && k.x + k.w <= ov::kKeyboardWidth && k.y + k.h <= ov::kKeyboardHeight && k.w > 40,
            "keys inside the canvas");
    for (size_t i = 0; i < keys.size(); ++i)
        for (size_t j = i + 1; j < keys.size(); ++j)
        {
            const ov::Key& a = keys[i];
            const ov::Key& b = keys[j];
            const bool overlap = a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
            Check(!overlap, "keys do not overlap");
        }

    ov::Keyboard kb;
    kb.Open("Test [Key]", "ab", 6, false, 42);
    Check(kb.Active() && kb.Text() == "ab" && kb.UserValue() == 42, "open with the existing text");
    auto press = [&](char ch) {
        bool shift = false;
        float u = 0, v = 0;
        Check(ov::KeyCentreUv(ch, ov::KeyKind::Char, shift, u, v), "key for char");
        char typed = 0;
        if (shift)
        {
            bool s2 = false; float su = 0, sv = 0;
            ov::KeyCentreUv(0, ov::KeyKind::Shift, s2, su, sv);
            kb.Press(ov::KeyAt(static_cast<int>(su * ov::kKeyboardWidth), static_cast<int>(sv * ov::kKeyboardHeight)), typed);
        }
        const int index = ov::KeyAt(static_cast<int>(u * ov::kKeyboardWidth), static_cast<int>(v * ov::kKeyboardHeight));
        return kb.Press(index, typed);
    };
    Check(press('1') == ov::Keyboard::Result::Typed && kb.Text() == "ab1", "digit");
    Check(press('.') == ov::Keyboard::Result::Typed && kb.Text() == "ab1.", "period");
    Check(press('X') == ov::Keyboard::Result::Typed && kb.Text() == "ab1.X" && !kb.Shift(), "shift is one-shot");
    Check(press(' ') == ov::Keyboard::Result::Typed && kb.Text() == "ab1.X ", "space");
    Check(press('z') == ov::Keyboard::Result::None && kb.Text() == "ab1.X ", "charMax stops typing");
    auto special = [&](ov::KeyKind kind) {
        bool s = false; float u = 0, v = 0;
        Check(ov::KeyCentreUv(0, kind, s, u, v), "special key exists");
        char typed = 0;
        const ov::Keyboard::Result r = kb.Press(ov::KeyAt(static_cast<int>(u * ov::kKeyboardWidth), static_cast<int>(v * ov::kKeyboardHeight)), typed);
        return std::make_pair(r, typed);
    };
    auto bs = special(ov::KeyKind::Backspace);
    Check(bs.first == ov::Keyboard::Result::Typed && bs.second == '\b' && kb.Text() == "ab1.X", "backspace");
    special(ov::KeyKind::Clear);
    Check(kb.Text().empty(), "clear");
    press('2');
    Check(special(ov::KeyKind::Done).first == ov::Keyboard::Result::Done && !kb.Active() && kb.Text() == "2", "done keeps the text");
    kb.Open("", "x", 0, false, 1);
    Check(special(ov::KeyKind::Cancel).first == ov::Keyboard::Result::Cancelled && !kb.Active(), "cancel");
    char typed = 0;
    Check(kb.Press(0, typed) == ov::Keyboard::Result::None, "closed keyboard ignores keys");
    Check(ov::KeyAt(5, 5) == -1, "text area is not a key");
}

int main()
{
    GeometryTests();
    PlanTests();
    AlphaFlipTests();
    AimTests();
    PointerTests();
    KeyboardTests();
    std::printf("vr_openxr_overlay: %d checks passed\n", checks);
    return 0;
}

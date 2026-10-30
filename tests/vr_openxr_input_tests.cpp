// OpenXR input tables and app-side input logic (OpenXR port M3):
// L4D2VR/vr_backend/openxr_input_profiles.h and openxr_input_logic.h.
#include "../L4D2VR/vr_backend/openxr_input_logic.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
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
static void CheckS(bool passed, const std::string& contract) { Check(passed, contract.c_str()); }

static bool Near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }

namespace xi = l4d2vr_openxr_input;
namespace xrm = l4d2vr_openxr;
using xi::Action;
using xi::Hand;

static std::vector<const xi::Binding*> BindingsOf(const xi::Profile& p, Action a, Hand h)
{
    std::vector<const xi::Binding*> out;
    for (const xi::Binding& b : p.bindings)
        if (b.action == a && b.hand == h)
            out.push_back(&b);
    return out;
}

static bool HasBinding(const xi::Profile& p, Action a, Hand h, const char* input)
{
    for (const xi::Binding* b : BindingsOf(p, a, h))
        if (std::strcmp(b->input, input) == 0)
            return true;
    return false;
}

static bool Legal(const xi::Profile& p, Hand h, const std::string& path)
{
    for (const std::string& legal : p.legal[static_cast<int>(h)])
        if (legal == path)
            return true;
    return false;
}

static const xi::Profile& P(xi::ProfileId id)
{
    const xi::Profile* p = xi::FindProfile(id);
    Check(p != nullptr, "profile exists");
    return *p;
}

// Matrix of a pose, for checks independent of the quaternion helpers.
static void PoseMatrix(const xrm::Pose& p, float m[3][4])
{
    const vr::HmdMatrix34_t h = xrm::ToMatrix(p);
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 4; ++c)
            m[r][c] = h.m[r][c];
}

int main()
{
    // ---- Action table ---------------------------------------------------------
    {
        std::set<std::string> xrNames;
        for (size_t i = 0; i < xi::kActionCount; ++i)
        {
            const xi::ActionDef& d = xi::Actions()[i];
            Check(static_cast<size_t>(d.id) == i, "action table order matches the enum");
            Check(d.openvrPath && d.openvrPath[0] == '/', "OpenVR path present");
            if (d.kind == xi::Kind::Button || d.kind == xi::Kind::Vector2)
            {
                const std::string name = d.xrName;
                CheckS(!name.empty() && name.size() < 64, "XR action name length: " + name);
                for (char c : name)
                    CheckS((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.',
                        "XR action name characters: " + name);
                CheckS(xrNames.insert(name).second, "XR action names unique: " + name);
                CheckS(std::strlen(d.label) > 0 && std::strlen(d.label) < 128, "localized name: " + name);
            }
        }
        // Every name vr_lifecycle_init.inl and vr_hands look up.
        const char* modNames[] = {
            "/actions/main/in/ActivateVR", "/actions/main/in/Jump", "/actions/main/in/PrimaryAttack",
            "/actions/main/in/Reload", "/actions/main/in/Use", "/actions/main/in/Teleport", "/actions/main/in/Walk",
            "/actions/main/in/Turn", "/actions/main/in/boolean_turnleft", "/actions/main/in/boolean_turnright",
            "/actions/main/in/SecondaryAttack", "/actions/main/in/NextItem", "/actions/main/in/PrevItem",
            "/actions/main/in/ResetPosition", "/actions/main/in/Crouch", "/actions/main/in/WeaponGrip",
            "/actions/main/in/OffHandGrip", "/actions/main/in/MagazineRelease", "/actions/main/in/Flashlight",
            "/actions/main/in/InventoryGripLeft", "/actions/main/in/InventoryGripRight",
            "/actions/main/in/InventoryQuickSwitch", "/actions/main/in/SpecialInfectedAutoAimToggle",
            "/actions/main/in/SpecialInfectedDodgeToggle", "/actions/main/in/LedgeGuardToggle",
            "/actions/main/in/EffectiveAttackRangeAutoFireToggle", "/actions/main/in/SpeechToText",
            "/actions/main/in/MenuSelect", "/actions/main/in/MenuBack", "/actions/main/in/MenuUp",
            "/actions/main/in/MenuDown", "/actions/main/in/MenuLeft", "/actions/main/in/MenuRight",
            "/actions/main/in/Spray", "/actions/main/in/Scoreboard", "/actions/main/in/ShowHUD",
            "/actions/main/in/Pause", "/actions/main/in/NonVRServerMovementAngleToggle",
            "/actions/main/in/ScopeToggle", "/actions/main/in/FriendlyFireBlockToggle",
            "/actions/main/in/CustomAction1", "/actions/main/in/CustomAction2", "/actions/main/in/CustomAction3",
            "/actions/main/in/CustomAction4", "/actions/main/in/CustomAction5",
            "/actions/base/in/skeleton_lefthand", "/actions/base/in/skeleton_righthand",
        };
        for (const char* name : modNames)
            CheckS(xi::FindAction(name) != Action::Count, std::string("mod action in the table: ") + name);
        Check(xi::FindAction("/ACTIONS/MAIN/IN/primaryattack") == Action::PrimaryAttack, "names are case-insensitive");
        Check(xi::FindAction("/actions/main/in/NoSuchThing") == Action::Count, "unknown names stay unknown");
        Check(xi::Def(Action::Walk).kind == xi::Kind::Vector2 && xi::Def(Action::Turn).kind == xi::Kind::Vector2, "sticks are vector2");
        Check(xi::Def(Action::SkeletonLeft).kind == xi::Kind::Skeleton, "skeleton actions are facade-only");
    }

    // ---- Profiles: paths, legality, coverage ----------------------------------
    {
        std::set<std::string> paths;
        Check(xi::Profiles().size() == xi::kProfileCount, "one table entry per profile id");
        for (size_t i = 0; i < xi::Profiles().size(); ++i)
        {
            const xi::Profile& p = xi::Profiles()[i];
            Check(static_cast<size_t>(p.id) == i, "profile order matches the enum");
            CheckS(paths.insert(p.path).second, std::string("profile paths unique: ") + p.path);
            CheckS(std::strncmp(p.path, "/interaction_profiles/", 22) == 0, std::string("profile path prefix: ") + p.path);
            Check(xi::FindProfileByPath(p.path) == &p && xi::FindProfileByKey(p.key) == &p, "profile lookups");
            for (int h = 0; h < 2; ++h)
            {
                const Hand hand = static_cast<Hand>(h);
                CheckS(Legal(p, hand, "/input/grip/pose") && Legal(p, hand, "/input/aim/pose") && Legal(p, hand, "/output/haptic"),
                    std::string("poses and haptics on ") + p.key);
                if (p.mainAxis)
                    CheckS(Legal(p, hand, p.mainAxis), std::string("main axis legal on ") + p.key);
                if (p.mainAxisTouch)
                    CheckS(Legal(p, hand, p.mainAxisTouch), std::string("main axis touch legal on ") + p.key);
                if (p.triggerCurl)
                    CheckS(Legal(p, hand, p.triggerCurl), std::string("trigger curl legal on ") + p.key);
                if (p.squeezeCurl)
                    CheckS(Legal(p, hand, p.squeezeCurl), std::string("squeeze curl legal on ") + p.key);
                if (p.squeezeForceCurl)
                    CheckS(Legal(p, hand, p.squeezeForceCurl), std::string("squeeze force curl legal on ") + p.key);
                if (p.indexProximityCurl)
                    CheckS(Legal(p, hand, p.indexProximityCurl), std::string("index proximity curl legal on ") + p.key);
                for (const std::string& t : p.thumbTouch[h])
                    CheckS(Legal(p, hand, t), std::string("thumb touch legal on ") + p.key + ": " + t);
            }
            for (const xi::Binding& b : p.bindings)
            {
                const std::string where = std::string(p.key) + " " + xi::Def(b.action).openvrPath + " " +
                    xi::HandPath(b.hand) + b.input;
                CheckS(Legal(p, b.hand, b.input), "binding path legal for the profile and hand: " + where);
                const xi::Kind kind = xi::Def(b.action).kind;
                if (b.mode == xi::Mode::Stick)
                    CheckS(kind == xi::Kind::Vector2, "stick bindings target vector2 actions: " + where);
                else
                    CheckS(kind == xi::Kind::Button, "button/dpad bindings target button actions: " + where);
                if (b.mode == xi::Mode::Dpad)
                    CheckS(b.dir != xi::DpadDir::None && p.mainAxis && std::strcmp(b.input, p.mainAxis) == 0,
                        "dpad derives from the profile's main axis: " + where);
                if (b.mode == xi::Mode::Stick)
                    CheckS(std::strcmp(b.input, "/input/thumbstick") == 0 || std::strcmp(b.input, "/input/trackpad") == 0,
                        "stick bindings use a 2D input: " + where);
            }
            // Every default action is bound on each hand SteamVR binds it on,
            // unless the profile's hardware cannot host it.
            for (Action a : xi::DefaultBoundActions())
            {
                const bool exempt = std::find(p.unboundByDesign.begin(), p.unboundByDesign.end(), a) != p.unboundByDesign.end();
                const bool bound = !BindingsOf(p, a, Hand::Left).empty() || !BindingsOf(p, a, Hand::Right).empty();
                CheckS(bound != exempt, std::string("default action bound (or exempt with no binding) on ") + p.key + ": " +
                    xi::Def(a).openvrPath);
            }
            // All bindings of one action on one hand share a threshold (FLOAT
            // actions aggregate their bindings as the largest value).
            for (size_t a = 0; a < xi::kActionCount; ++a)
            {
                for (int h = 0; h < 2; ++h)
                {
                    const auto list = BindingsOf(p, static_cast<Action>(a), static_cast<Hand>(h));
                    float press0 = -1.0f, release0 = -1.0f;
                    for (const xi::Binding* b : list)
                    {
                        if (b->mode != xi::Mode::Button)
                            continue;
                        float press = 0.0f, release = 0.0f;
                        xi::Thresholds(*b, press, release);
                        CheckS(press >= release && press > 0.0f, std::string("thresholds ordered on ") + p.key);
                        if (press0 < 0.0f) { press0 = press; release0 = release; }
                        CheckS(Near(press, press0) && Near(release, release0),
                            std::string("one threshold per action and hand on ") + p.key + ": " + xi::Def(static_cast<Action>(a)).openvrPath);
                    }
                }
            }
            // Sticks drive the menu dpad on the left hand only (bindings files).
            if (p.mainAxis)
                CheckS(BindingsOf(p, Action::MenuUp, Hand::Left).size() == 1 && BindingsOf(p, Action::MenuUp, Hand::Right).empty(),
                    std::string("menu dpad on the left stick: ") + p.key);
        }
        Check(xi::FindProfile(xi::ProfileId::Touch)->extension == nullptr && xi::FindProfile(xi::ProfileId::Index)->extension == nullptr &&
            xi::FindProfile(xi::ProfileId::ViveWand)->extension == nullptr && xi::FindProfile(xi::ProfileId::Wmr)->extension == nullptr &&
            xi::FindProfile(xi::ProfileId::Simple)->extension == nullptr, "core profiles need no extension");
        Check(std::strcmp(P(xi::ProfileId::Cosmos).extension, "XR_HTC_vive_cosmos_controller_interaction") == 0, "cosmos extension");
        Check(std::strcmp(P(xi::ProfileId::Pico4).extension, "XR_BD_controller_interaction") == 0, "pico extension");
        Check(std::strcmp(P(xi::ProfileId::TouchPlus).extension, "XR_META_touch_controller_plus") == 0, "touch plus extension");
        Check(std::strcmp(P(xi::ProfileId::TouchPro).extension, "XR_FB_touch_controller_pro") == 0, "touch pro extension");
    }

    // ---- Parity with SteamVR's default binding files --------------------------
    {
        // bindings_knuckles.json
        const xi::Profile& idx = P(xi::ProfileId::Index);
        Check(HasBinding(idx, Action::Walk, Hand::Left, "/input/thumbstick"), "index walk = left stick");
        Check(HasBinding(idx, Action::ResetPosition, Hand::Left, "/input/thumbstick/click"), "index reset = left stick click");
        Check(HasBinding(idx, Action::Turn, Hand::Right, "/input/thumbstick"), "index turn = right stick");
        Check(HasBinding(idx, Action::Flashlight, Hand::Right, "/input/thumbstick/click"), "index flashlight = right stick click");
        Check(HasBinding(idx, Action::Flashlight, Hand::Left, "/input/a/click"), "index flashlight = left A");
        Check(HasBinding(idx, Action::Pause, Hand::Left, "/input/b/click"), "index pause = left B");
        Check(HasBinding(idx, Action::PrimaryAttack, Hand::Right, "/input/trigger/click"), "index primary = right trigger");
        Check(HasBinding(idx, Action::SecondaryAttack, Hand::Left, "/input/trigger/click"), "index secondary = left trigger");
        Check(HasBinding(idx, Action::Use, Hand::Right, "/input/b/click") && HasBinding(idx, Action::MenuBack, Hand::Right, "/input/b/click"),
            "index use/back = right B");
        Check(HasBinding(idx, Action::ActivateVR, Hand::Right, "/input/a/click") && HasBinding(idx, Action::MagazineRelease, Hand::Right, "/input/a/click") &&
            HasBinding(idx, Action::MenuSelect, Hand::Right, "/input/a/click"), "index select/release/activate = right A");
        Check(HasBinding(idx, Action::Scoreboard, Hand::Left, "/input/trackpad/force"), "index scoreboard = left trackpad press");
        Check(BindingsOf(idx, Action::WeaponGrip, Hand::Left).empty() && BindingsOf(idx, Action::OffHandGrip, Hand::Right).empty(),
            "grips are one hand each (leftright manifest)");
        float press = 0.0f, release = 0.0f;
        xi::Thresholds(*BindingsOf(idx, Action::OffHandGrip, Hand::Left)[0], press, release);
        Check(Near(press, 0.80f) && Near(release, 0.45f), "index off-hand grip 0.80/0.45 (force_input value)");
        xi::Thresholds(*BindingsOf(idx, Action::WeaponGrip, Hand::Right)[0], press, release);
        Check(Near(press, 0.85f) && Near(release, 0.50f), "index weapon grip 0.85/0.50 (force_input value)");
        Check(std::strcmp(BindingsOf(idx, Action::WeaponGrip, Hand::Right)[0]->input, "/input/squeeze/value") == 0, "index grip uses the value");

        // bindings_oculus_touch.json
        const xi::Profile& touch = P(xi::ProfileId::Touch);
        Check(HasBinding(touch, Action::Flashlight, Hand::Left, "/input/x/click") && HasBinding(touch, Action::Pause, Hand::Left, "/input/y/click"),
            "touch X flashlight, Y pause");
        Check(HasBinding(touch, Action::OffHandGrip, Hand::Left, "/input/squeeze/value") && HasBinding(touch, Action::WeaponGrip, Hand::Right, "/input/squeeze/value"),
            "touch grips on squeeze value");
        xi::Thresholds(*BindingsOf(touch, Action::WeaponGrip, Hand::Right)[0], press, release);
        Check(Near(press, 0.25f) && Near(release, 0.20f), "touch grip uses SteamVR's default click threshold");
        xi::Thresholds(*BindingsOf(touch, Action::PrimaryAttack, Hand::Right)[0], press, release);
        Check(Near(press, 0.25f) && Near(release, 0.20f), "touch trigger uses SteamVR's default click threshold");
        for (xi::ProfileId id : { xi::ProfileId::TouchPro, xi::ProfileId::TouchPlus })
            Check(P(id).bindings.size() == touch.bindings.size(), "Quest Pro/3 controllers share the Touch layout");

        // bindings_vive_cosmos_controller.json
        const xi::Profile& cosmos = P(xi::ProfileId::Cosmos);
        Check(HasBinding(cosmos, Action::Scoreboard, Hand::Left, "/input/shoulder/click"), "cosmos scoreboard = left bumper");
        Check(HasBinding(cosmos, Action::WeaponGrip, Hand::Right, "/input/squeeze/click"), "cosmos grip click");

        // New layouts.
        const xi::Profile& wmr = P(xi::ProfileId::Wmr);
        Check(HasBinding(wmr, Action::WeaponGrip, Hand::Right, "/input/squeeze/click") && HasBinding(wmr, Action::PrimaryAttack, Hand::Right, "/input/trigger/value") &&
            HasBinding(wmr, Action::Use, Hand::Right, "/input/menu/click") && HasBinding(wmr, Action::MenuSelect, Hand::Right, "/input/trackpad/click") &&
            HasBinding(wmr, Action::Pause, Hand::Left, "/input/menu/click"), "wmr layout");
        const xi::Profile& vive = P(xi::ProfileId::ViveWand);
        Check(HasBinding(vive, Action::Walk, Hand::Left, "/input/trackpad") && HasBinding(vive, Action::Turn, Hand::Right, "/input/trackpad"),
            "vive trackpads are sticks");
        const xi::Profile& simple = P(xi::ProfileId::Simple);
        Check(HasBinding(simple, Action::PrimaryAttack, Hand::Right, "/input/select/click") && HasBinding(simple, Action::WeaponGrip, Hand::Right, "/input/menu/click"),
            "simple: select fires, menu grips");
    }

    // ---- GetActionBindingInfo strings -------------------------------------------
    {
        const xi::Profile& idx = P(xi::ProfileId::Index);
        const xi::BindingInfoText trig = xi::DescribeBinding(*BindingsOf(idx, Action::PrimaryAttack, Hand::Right)[0]);
        Check(trig.device == "/user/hand/right" && trig.input == "/user/hand/right/input/trigger" && trig.mode == "button" &&
            trig.slot == "click" && trig.sourceType == "trigger", "trigger binding info");
        const xi::BindingInfoText walk = xi::DescribeBinding(*BindingsOf(idx, Action::Walk, Hand::Left)[0]);
        Check(walk.input == "/user/hand/left/input/thumbstick" && walk.mode == "joystick" && walk.slot == "position" &&
            walk.sourceType == "joystick", "stick binding info");
        const xi::BindingInfoText up = xi::DescribeBinding(*BindingsOf(idx, Action::MenuUp, Hand::Left)[0]);
        Check(up.mode == "dpad" && up.slot == "north" && up.input == "/user/hand/left/input/thumbstick", "dpad binding info");
        const xi::BindingInfoText x = xi::DescribeBinding(*BindingsOf(P(xi::ProfileId::Touch), Action::Flashlight, Hand::Left)[0]);
        Check(x.input == "/user/hand/left/input/x" && x.sourceType == "button", "face button binding info (mirror canonicalizes x/a)");
        // Mirror pairs: the left-handed swap pairs actions by the same input on
        // the other hand, so each primary right action has a left counterpart.
        const xi::BindingInfoText sec = xi::DescribeBinding(*BindingsOf(idx, Action::SecondaryAttack, Hand::Left)[0]);
        Check(sec.mode == trig.mode && sec.slot == trig.slot && sec.sourceType == trig.sourceType, "trigger pair mirrors");
    }

    // ---- Thresholds with hysteresis ---------------------------------------------
    {
        bool down = false;
        down = xi::NextClick(down, 0.84f, 0.85f, 0.50f); Check(!down, "0.84 does not press a 0.85 grip");
        down = xi::NextClick(down, 0.86f, 0.85f, 0.50f); Check(down, "0.86 presses");
        down = xi::NextClick(down, 0.55f, 0.85f, 0.50f); Check(down, "0.55 holds (release 0.50)");
        down = xi::NextClick(down, 0.50f, 0.85f, 0.50f); Check(down, "exactly the release value holds");
        down = xi::NextClick(down, 0.49f, 0.85f, 0.50f); Check(!down, "0.49 releases");
        down = xi::NextClick(down, 0.70f, 0.85f, 0.50f); Check(!down, "re-press needs the press value again");
        Check(!xi::NextClick(true, std::nanf(""), 0.5f, 0.4f), "NaN releases");
        Check(xi::NextClick(false, 1.0f, xi::kClickPress, xi::kClickRelease) && !xi::NextClick(true, 0.0f, xi::kClickPress, xi::kClickRelease),
            "click inputs follow 0/1");
    }

    // ---- Stick deadzone -------------------------------------------------------------
    {
        xi::Stick2 s = xi::RadialDeadzone({ 0.05f, 0.05f }, 0.10f);
        Check(s.x == 0.0f && s.y == 0.0f, "inside the deadzone is zero");
        s = xi::RadialDeadzone({ 0.0f, 0.55f }, 0.10f);
        Check(Near(s.x, 0.0f) && Near(s.y, 0.5f), "range rescaled from the deadzone edge");
        s = xi::RadialDeadzone({ 0.6f, 0.8f }, 0.10f);
        Check(Near(std::sqrt(s.x * s.x + s.y * s.y), 1.0f) && Near(s.x / s.y, 0.75f), "full deflection stays 1, direction kept");
        s = xi::RadialDeadzone({ 1.2f, 0.0f }, 0.10f);
        Check(Near(s.x, 1.0f), "clamped to the unit circle");
        s = xi::RadialDeadzone({ std::nanf(""), 0.0f }, 0.10f);
        Check(s.x == 0.0f && s.y == 0.0f, "NaN is zero");
        s = xi::RadialDeadzone({ 0.0f, 0.1001f }, 0.10f);
        Check(s.y > 0.0f && s.y < 0.001f, "continuous at the deadzone edge");
    }

    // ---- Menu dpad ----------------------------------------------------------------
    {
        using xi::DpadDir;
        Check(xi::NextDpad(false, DpadDir::North, { 0.0f, 0.8f }, true), "north pressed beyond 70%");
        Check(!xi::NextDpad(false, DpadDir::North, { 0.0f, 0.8f }, false), "touch sub-mode: untouched stick does nothing");
        Check(!xi::NextDpad(false, DpadDir::North, { 0.0f, 0.65f }, true), "65% does not press");
        Check(xi::NextDpad(true, DpadDir::North, { 0.0f, 0.65f }, true), "65% holds a pressed direction");
        Check(!xi::NextDpad(true, DpadDir::North, { 0.0f, 0.55f }, true), "below 60% releases");
        Check(!xi::NextDpad(false, DpadDir::East, { 0.0f, 0.9f }, true), "other directions stay up");
        Check(xi::NextDpad(false, DpadDir::East, { 0.9f, 0.3f }, true) && xi::NextDpad(false, DpadDir::West, { -0.9f, -0.3f }, true) &&
            xi::NextDpad(false, DpadDir::South, { 0.2f, -0.9f }, true), "east/west/south wedges");
        Check(!xi::NextDpad(true, DpadDir::North, { 0.9f, 0.3f }, true), "leaving the wedge releases");
        int pressed = 0;
        for (DpadDir d : { DpadDir::North, DpadDir::East, DpadDir::South, DpadDir::West })
            pressed += xi::NextDpad(false, d, { 0.6f, 0.6f }, true) ? 1 : 0;
        Check(pressed == 1, "diagonal presses exactly one direction (no overlap)");
    }

    // ---- Grip -> raw --------------------------------------------------------------
    {
        // Identity offset: raw is the grip pose.
        xrm::Pose grip;
        grip.orientation = xi::AxisAngle(0.0f, 1.0f, 0.0f, 0.7f);
        grip.position = { 0.2f, 1.3f, -0.4f };
        xi::GripInRaw none;
        const xrm::Pose same = xi::RawFromGrip(grip, none);
        Check(Near(same.position.x, 0.2f) && Near(same.position.y, 1.3f) && Near(same.position.z, -0.4f) &&
            Near(std::fabs(same.orientation.w), std::fabs(grip.orientation.w)), "identity offset passes the grip through");

        // Touch left: grip = raw * T(0.007, -0.0018, 0.1019) * Rx(20.6 deg).
        const xi::Profile& touch = P(xi::ProfileId::Touch);
        const xi::GripInRaw& tl = touch.grip[static_cast<int>(xi::RuntimeFamily::Other)][0];
        xrm::Pose gripAtOrigin;  // grip pose = identity in the base space
        const xrm::Pose raw = xi::RawFromGrip(gripAtOrigin, tl);
        // Independent expectation: raw = (T R)^-1 => position = -Rx(-a) t.
        const float a = 20.6f * 3.14159265f / 180.0f;
        const float tx = 0.007f, ty = -0.00182941f, tz = 0.1019482f;
        const float ex = -tx;
        const float ey = -(std::cos(a) * ty + std::sin(a) * tz);
        const float ez = -(-std::sin(a) * ty + std::cos(a) * tz);
        Check(Near(raw.position.x, ex) && Near(raw.position.y, ey) && Near(raw.position.z, ez), "touch raw position = -(R^-1 t)");
        float m[3][4];
        PoseMatrix(raw, m);
        // Raw -Z (forward) seen from the grip: rotated by -20.6 deg about X.
        Check(Near(-m[1][2], -std::sin(a), 1e-3f) && Near(-m[2][2], -std::cos(a), 1e-3f), "touch raw forward tilted by the grip angle");
        // Round trip: raw * gripInRaw = grip.
        const xrm::Pose back = xi::Compose(xi::RawFromGrip(grip, tl), xi::GripInRawPose(tl));
        Check(Near(back.position.x, grip.position.x) && Near(back.position.y, grip.position.y) && Near(back.position.z, grip.position.z) &&
            Near(std::fabs(back.orientation.x * grip.orientation.x + back.orientation.y * grip.orientation.y +
                back.orientation.z * grip.orientation.z + back.orientation.w * grip.orientation.w), 1.0f), "raw * offset = grip");
        // Right hand mirrors x.
        const xi::GripInRaw& tr = touch.grip[static_cast<int>(xi::RuntimeFamily::Other)][1];
        Check(Near(tr.position[0], -tl.position[0]) && Near(tr.position[2], tl.position[2]), "touch right mirrors x");

        // Index: SteamVR's handgrip and Monado's grip differ; the right hand
        // mirrors x position and the y/z rotations.
        const xi::Profile& idx = P(xi::ProfileId::Index);
        const xi::GripInRaw& sv = idx.grip[static_cast<int>(xi::RuntimeFamily::SteamVR)][0];
        const xi::GripInRaw& mo = idx.grip[static_cast<int>(xi::RuntimeFamily::Other)][0];
        Check(Near(sv.position[2], 0.075948f) && Near(mo.position[2], 0.13f) && Near(mo.position[1], -0.015f), "index per-runtime offsets");
        Check(Near(idx.grip[1][1].eulerDeg[1], 2.071f) && Near(idx.grip[1][1].eulerDeg[2], -0.303f), "index right rotation mirrored");
        Check(mo.order == xi::EulerOrder::ZYX, "monado composes Rz*Ry*Rx");
        // XRizer composes the same numbers as Rx*Ry*Rz: about half a degree apart.
        xi::GripInRaw xyz = mo;
        xyz.order = xi::EulerOrder::XYZ;
        const xrm::Quat q1 = xi::GripInRawPose(mo).orientation, q2 = xi::GripInRawPose(xyz).orientation;
        const float dot = std::fabs(q1.x * q2.x + q1.y * q2.y + q1.z * q2.z + q1.w * q2.w);
        const float deg = 2.0f * std::acos((std::min)(1.0f, dot)) * 180.0f / 3.14159265f;
        Check(deg > 0.05f && deg < 1.0f, "euler order difference to XRizer under 1 degree");
        // The Vive wand only has an offset on SteamVR.
        const xi::Profile& vive = P(xi::ProfileId::ViveWand);
        Check(!vive.grip[0][0].identity() && vive.grip[1][0].identity(), "vive: SteamVR handgrip, Monado identity");
        Check(P(xi::ProfileId::Wmr).grip[0][0].identity() && P(xi::ProfileId::Simple).grip[1][1].identity(), "wmr/simple identity");
        Check(xi::FamilyFromRuntimeName("SteamVR/OpenXR : steamvr") == xi::RuntimeFamily::SteamVR &&
            xi::FamilyFromRuntimeName("Monado(XRT) by Collabora et al") == xi::RuntimeFamily::Other, "runtime family");

        // Velocity: spinning about +Y at 1 rad/s with the raw origin 0.1 m in
        // -Z of the grip moves the raw origin along -X.
        xrm::Pose g0, r0;
        r0.position = { 0.0f, 0.0f, -0.1f };
        const xrm::Vec3 v = xi::RawLinearVelocity({ 0.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, g0, r0);
        Check(Near(v.x, -0.1f) && Near(v.y, 0.0f) && Near(v.z, 0.0f), "raw velocity includes w x r");
    }

    // Finger curls from controller sensors: tests/vr_openxr_hand_tests.cpp.
    {
        // Curl sources per profile (M5): Index grip force, Quest Pro / 3 index proximity.
        for (const xi::Profile& p : xi::Profiles())
        {
            const bool index = p.id == xi::ProfileId::Index;
            CheckS((p.squeezeForceCurl != nullptr) == index, std::string("grip force curl only on Index: ") + p.key);
            const bool proximity = p.id == xi::ProfileId::TouchPro || p.id == xi::ProfileId::TouchPlus;
            CheckS((p.indexProximityCurl != nullptr) == proximity, std::string("index proximity only on Quest Pro/3: ") + p.key);
            CheckS(p.triggerCurl != nullptr && p.squeezeCurl != nullptr, std::string("every profile has index and grip curl sources: ") + p.key);
        }
    }

    // ---- Fake input spec ---------------------------------------------------------
    {
        xi::FakeInput f;
        std::string err;
        Check(f.Apply("profile=index", err) && f.enabled && f.profile == xi::FindProfile(xi::ProfileId::Index), "fake profile");
        Check(f.Apply("/user/hand/right/input/squeeze/value=0.9", err) && Near(f.Get("/user/hand/right/input/squeeze/value").x, 0.9f), "fake scalar");
        Check(f.Apply("/user/hand/left/input/thumbstick=0.25:-0.75", err) && Near(f.Get("/user/hand/left/input/thumbstick").y, -0.75f), "fake vector");
        Check(!f.Apply("/user/head/input/x=1", err) && !err.empty(), "fake paths are per hand");
        Check(!f.Apply("profile=nope", err), "unknown profile rejected");
        Check(!f.Apply("/user/hand/left/input/a/click=abc", err), "bad number rejected");
        Check(f.Get("/user/hand/left/input/missing").x == 0.0f, "unset path is zero");
        Check(f.Apply("clear", err) && !f.enabled && f.values.empty() && f.profile == nullptr, "clear");
    }

    std::printf("openxr input: %d checks passed\n", checks);
    return 0;
}

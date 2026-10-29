#pragma once

// OpenXR input tables (OpenXR port M3). Std-only, so the tables are unit-tested
// without a runtime (tests/vr_openxr_input_tests.cpp).
//
// * Actions: every action name the mod looks up (vr_lifecycle_init.inl
//   SetActionManifest, vr_hands), with its OpenXR type. Buttons are FLOAT
//   actions so the mod applies its own click thresholds with hysteresis
//   (OpenXR suggested bindings carry no parameters; SteamVR's knuckles grip
//   thresholds 0.80/0.45 and 0.85/0.50 live in this table instead).
// * Profiles: one per interaction profile, with the suggested bindings that
//   reproduce SteamVR's default binding files (SteamVRActionManifest/*.json)
//   and new layouts for controllers that had none (Vive wand, WMR, simple).
// * Grip -> raw offsets: the OpenVR "raw" device pose every weapon, holster and
//   hand calibration was tuned on, expressed from the OpenXR grip pose. Each
//   runtime defines its grip pose relative to the raw pose with its own data:
//     - SteamVR: the render model component it builds the grip from
//       (openxr_grip, else handgrip; SteamVR/resources/rendermodels and
//       drivers/*/resources/rendermodels, measured values copied here).
//     - Monado (and Monado-based runtimes): xrt/auxiliary/vive/vive_poses.c
//       for Index; other Monado drivers report the device pose as grip.
//       XRizer (the OpenVR layer players use on Linux today) inverts the same
//       numbers, so this keeps those players' calibration.
//   Profiles without measured data use the closest controller and say so.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace l4d2vr_openxr_input
{
    enum class Hand : uint8_t { Left = 0, Right = 1 };
    inline const char* HandPath(Hand hand) { return hand == Hand::Left ? "/user/hand/left" : "/user/hand/right"; }

    enum class Kind : uint8_t
    {
        Button,    // XR_ACTION_TYPE_FLOAT_INPUT + app threshold -> InputDigitalActionData_t
        Vector2,   // XR_ACTION_TYPE_VECTOR2F_INPUT -> InputAnalogActionData_t
        Skeleton,  // no XrAction; per-hand skeletal facade (curl fallback)
        Unused,    // known OpenVR action the mod never reads (pose/vibration in the base set)
    };

    // Every action the mod acquires. Order is the ActionId.
#define L4D2VR_XR_ACTIONS(X) \
    X(ActivateVR, "/actions/main/in/ActivateVR", "activatevr", "Activate VR", Button) \
    X(Jump, "/actions/main/in/Jump", "jump", "Jump", Button) \
    X(PrimaryAttack, "/actions/main/in/PrimaryAttack", "primaryattack", "Primary Attack", Button) \
    X(Reload, "/actions/main/in/Reload", "reload", "Reload", Button) \
    X(Use, "/actions/main/in/Use", "use", "Use", Button) \
    X(Teleport, "/actions/main/in/Teleport", "teleport", "Teleport", Button) \
    X(Walk, "/actions/main/in/Walk", "walk", "Walk", Vector2) \
    X(Turn, "/actions/main/in/Turn", "turn", "Turn", Vector2) \
    X(BooleanTurnLeft, "/actions/main/in/boolean_turnleft", "boolean_turnleft", "Turn Left", Button) \
    X(BooleanTurnRight, "/actions/main/in/boolean_turnright", "boolean_turnright", "Turn Right", Button) \
    X(SecondaryAttack, "/actions/main/in/SecondaryAttack", "secondaryattack", "Secondary Attack", Button) \
    X(NextItem, "/actions/main/in/NextItem", "nextitem", "Next Item", Button) \
    X(PrevItem, "/actions/main/in/PrevItem", "previtem", "Previous Item", Button) \
    X(ResetPosition, "/actions/main/in/ResetPosition", "resetposition", "Reset Position", Button) \
    X(Crouch, "/actions/main/in/Crouch", "crouch", "Crouch", Button) \
    X(WeaponGrip, "/actions/main/in/WeaponGrip", "weapongrip", "Weapon Grip (release to drop)", Button) \
    X(OffHandGrip, "/actions/main/in/OffHandGrip", "offhandgrip", "Off-hand Grip", Button) \
    X(MagazineRelease, "/actions/main/in/MagazineRelease", "magazinerelease", "Magazine Release", Button) \
    X(Flashlight, "/actions/main/in/Flashlight", "flashlight", "Flashlight", Button) \
    X(InventoryGripLeft, "/actions/main/in/InventoryGripLeft", "inventorygripleft", "Inventory Grip Left", Button) \
    X(InventoryGripRight, "/actions/main/in/InventoryGripRight", "inventorygripright", "Inventory Grip Right", Button) \
    X(InventoryQuickSwitch, "/actions/main/in/InventoryQuickSwitch", "inventoryquickswitch", "Inventory Quick Switch", Button) \
    X(SpecialInfectedAutoAimToggle, "/actions/main/in/SpecialInfectedAutoAimToggle", "specialinfectedautoaimtoggle", "Special Infected Auto Aim Toggle", Button) \
    X(SpecialInfectedDodgeToggle, "/actions/main/in/SpecialInfectedDodgeToggle", "specialinfecteddodgetoggle", "Special Infected Dodge Toggle", Button) \
    X(LedgeGuardToggle, "/actions/main/in/LedgeGuardToggle", "ledgeguardtoggle", "Ledge Guard Toggle", Button) \
    X(EffectiveAttackRangeAutoFireToggle, "/actions/main/in/EffectiveAttackRangeAutoFireToggle", "effectiveattackrangeautofiretoggle", "Effective Range Auto Fire Toggle", Button) \
    X(SpeechToText, "/actions/main/in/SpeechToText", "speechtotext", "Speech To Text", Button) \
    X(MenuSelect, "/actions/main/in/MenuSelect", "menuselect", "Menu Select", Button) \
    X(MenuBack, "/actions/main/in/MenuBack", "menuback", "Menu Back", Button) \
    X(MenuUp, "/actions/main/in/MenuUp", "menuup", "Menu Up", Button) \
    X(MenuDown, "/actions/main/in/MenuDown", "menudown", "Menu Down", Button) \
    X(MenuLeft, "/actions/main/in/MenuLeft", "menuleft", "Menu Left", Button) \
    X(MenuRight, "/actions/main/in/MenuRight", "menuright", "Menu Right", Button) \
    X(Spray, "/actions/main/in/Spray", "spray", "Spray", Button) \
    X(Scoreboard, "/actions/main/in/Scoreboard", "scoreboard", "Show Scoreboard", Button) \
    X(ShowHUD, "/actions/main/in/ShowHUD", "showhud", "Show HUD", Button) \
    X(Pause, "/actions/main/in/Pause", "pause", "Pause", Button) \
    X(NonVRServerMovementAngleToggle, "/actions/main/in/NonVRServerMovementAngleToggle", "nonvrservermovementangletoggle", "Non-VR Server Movement Angle Toggle", Button) \
    X(ScopeToggle, "/actions/main/in/ScopeToggle", "scopetoggle", "Toggle Scope", Button) \
    X(ScopeMagnificationToggle, "/actions/main/in/ScopeMagnificationToggle", "scopemagnificationtoggle", "Toggle Scope Magnification", Button) \
    X(FriendlyFireBlockToggle, "/actions/main/in/FriendlyFireBlockToggle", "friendlyfireblocktoggle", "Friendly Fire Block Toggle", Button) \
    X(CustomAction1, "/actions/main/in/CustomAction1", "customaction1", "Custom Action 1", Button) \
    X(CustomAction2, "/actions/main/in/CustomAction2", "customaction2", "Custom Action 2", Button) \
    X(CustomAction3, "/actions/main/in/CustomAction3", "customaction3", "Custom Action 3", Button) \
    X(CustomAction4, "/actions/main/in/CustomAction4", "customaction4", "Custom Action 4", Button) \
    X(CustomAction5, "/actions/main/in/CustomAction5", "customaction5", "Custom Action 5", Button) \
    X(SkeletonLeft, "/actions/base/in/skeleton_lefthand", "", "", Skeleton) \
    X(SkeletonRight, "/actions/base/in/skeleton_righthand", "", "", Skeleton) \
    X(PoseLeft, "/actions/base/in/pose_lefthand", "", "", Unused) \
    X(PoseRight, "/actions/base/in/pose_righthand", "", "", Unused) \
    X(VibrationLeft, "/actions/base/out/vibration_left", "", "", Unused) \
    X(VibrationRight, "/actions/base/out/vibration_right", "", "", Unused)

    enum class Action : uint16_t
    {
#define L4D2VR_XR_ACTION_ENUM(id, path, xr, label, kind) id,
        L4D2VR_XR_ACTIONS(L4D2VR_XR_ACTION_ENUM)
#undef L4D2VR_XR_ACTION_ENUM
        Count
    };
    constexpr size_t kActionCount = static_cast<size_t>(Action::Count);

    struct ActionDef
    {
        Action id;
        const char* openvrPath;  // as the mod passes it to GetActionHandle
        const char* xrName;      // OpenXR action name ("" = no XrAction)
        const char* label;       // localized name
        Kind kind;
    };

    inline const ActionDef* Actions()
    {
        static const ActionDef table[kActionCount] = {
#define L4D2VR_XR_ACTION_DEF(id, path, xr, label, kind) { Action::id, path, xr, label, Kind::kind },
            L4D2VR_XR_ACTIONS(L4D2VR_XR_ACTION_DEF)
#undef L4D2VR_XR_ACTION_DEF
        };
        return table;
    }
    inline const ActionDef& Def(Action a) { return Actions()[static_cast<size_t>(a)]; }

    inline char LowerAscii(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }
    inline bool EqualsNoCase(const char* a, const char* b)
    {
        if (!a || !b) return false;
        for (; *a && *b; ++a, ++b)
            if (LowerAscii(*a) != LowerAscii(*b))
                return false;
        return *a == *b;
    }

    // OpenVR action names are case-insensitive. Count = unknown.
    inline Action FindAction(const char* openvrPath)
    {
        for (size_t i = 0; i < kActionCount; ++i)
            if (EqualsNoCase(Actions()[i].openvrPath, openvrPath))
                return static_cast<Action>(i);
        return Action::Count;
    }

    // ---- Bindings -------------------------------------------------------------
    enum class Mode : uint8_t
    {
        Button,   // FLOAT action on a click/value/force input, app threshold
        Stick,    // VECTOR2F action on a thumbstick/trackpad, app radial deadzone
        Dpad,     // derived in-app from the hand's main axis (not suggested to the runtime)
    };
    enum class DpadDir : uint8_t { None, North, East, South, West };

    // SteamVR's defaults: "button" mode on an analog input clicks at 0.25 and
    // releases at 0.20 unless the binding says otherwise (XRizer uses the same
    // defaults for the bindings players run on Linux today).
    constexpr float kValueClickPress = 0.25f;
    constexpr float kValueClickRelease = 0.20f;
    // Binary inputs (click/touch) report exactly 0 or 1.
    constexpr float kClickPress = 0.5f;
    constexpr float kClickRelease = 0.5f;
    // SteamVR default bindings: joystick deadzone_pct 10, menu dpad deadzone_pct 70 (sub_mode touch).
    constexpr float kStickDeadzone = 0.10f;
    constexpr float kDpadPress = 0.70f;
    constexpr float kDpadRelease = 0.60f;

    struct Binding
    {
        Action action = Action::Count;
        Hand hand = Hand::Left;
        const char* input = "";  // component path after /user/hand/<h>, e.g. "/input/trigger/value"
        Mode mode = Mode::Button;
        float press = 0.0f;      // Button: 0 = default for the input (click or value)
        float release = 0.0f;
        DpadDir dir = DpadDir::None;
    };

    inline bool EndsWith(const char* s, const char* suffix)
    {
        const size_t n = std::strlen(s), m = std::strlen(suffix);
        return n >= m && std::strcmp(s + n - m, suffix) == 0;
    }

    // Click thresholds a Button binding uses (explicit, else by input kind).
    inline void Thresholds(const Binding& b, float& press, float& release)
    {
        if (b.mode == Mode::Dpad)
        {
            press = kDpadPress;
            release = kDpadRelease;
            return;
        }
        if (b.press > 0.0f)
        {
            press = b.press;
            release = b.release;
            return;
        }
        const bool binary = EndsWith(b.input, "/click") || EndsWith(b.input, "/touch");
        press = binary ? kClickPress : kValueClickPress;
        release = binary ? kClickRelease : kValueClickRelease;
    }

    // ---- Profiles -------------------------------------------------------------
    enum class ProfileId : uint8_t
    {
        Index, Touch, TouchPro, TouchPlus, ViveWand, Cosmos, Focus3, Wmr, HpReverb, Pico4, Simple, Count
    };
    constexpr size_t kProfileCount = static_cast<size_t>(ProfileId::Count);

    enum class EulerOrder : uint8_t
    {
        XYZ,  // R = Rx * Ry * Rz (SteamVR render model rotate_xyz; glam EulerRot::XYZ in XRizer)
        ZYX,  // R = Rz * Ry * Rx (Monado math_quat_from_euler_angles)
    };

    // The grip pose expressed in the raw device frame (OpenVR render-model
    // "component_local"): grip = raw * T(position) * R(euler).
    struct GripInRaw
    {
        float position[3] = { 0.0f, 0.0f, 0.0f };
        float eulerDeg[3] = { 0.0f, 0.0f, 0.0f };
        EulerOrder order = EulerOrder::XYZ;
        bool identity() const
        {
            return position[0] == 0.0f && position[1] == 0.0f && position[2] == 0.0f &&
                eulerDeg[0] == 0.0f && eulerDeg[1] == 0.0f && eulerDeg[2] == 0.0f;
        }
    };

    enum class RuntimeFamily : uint8_t { SteamVR, Other };
    inline RuntimeFamily FamilyFromRuntimeName(const char* name)
    {
        if (!name) return RuntimeFamily::Other;
        std::string lower;
        for (const char* p = name; *p; ++p) lower.push_back(LowerAscii(*p));
        return lower.find("steamvr") != std::string::npos ? RuntimeFamily::SteamVR : RuntimeFamily::Other;
    }

    struct Profile
    {
        ProfileId id = ProfileId::Count;
        const char* key = "";             // short name (logs, fake input "profile=")
        const char* path = "";            // /interaction_profiles/...
        const char* extension = nullptr;  // required instance extension (nullptr = core)
        const char* controllerType = "";  // OpenVR Prop_ControllerType_String
        const char* renderModel[2] = { "", "" };  // Prop_RenderModelName_String (left, right)
        const char* mainAxis = "";        // stick used for the menu dpad and curls
        const char* mainAxisTouch = nullptr;  // touch gate for the dpad (sub_mode touch); nullptr = none
        const char* triggerCurl = nullptr;    // index finger curl source
        const char* squeezeCurl = nullptr;    // middle/ring/pinky curl source
        std::vector<std::string> thumbTouch[2];  // thumb-down sources per hand
        GripInRaw grip[2][2];             // [RuntimeFamily][Hand]
        const char* gripNote = "";        // where the offset comes from
        std::vector<Binding> bindings;
        std::vector<std::string> legal[2];  // every input/output path the profile defines, per hand
        std::vector<Action> unboundByDesign;  // default actions this hardware cannot host, with the reason in the table
    };

    namespace detail
    {
        inline GripInRaw Grip(float x, float y, float z, float rx, float ry, float rz, EulerOrder order)
        {
            GripInRaw g;
            g.position[0] = x; g.position[1] = y; g.position[2] = z;
            g.eulerDeg[0] = rx; g.eulerDeg[1] = ry; g.eulerDeg[2] = rz;
            g.order = order;
            return g;
        }
        inline void SameGripAllRuntimes(Profile& p, const GripInRaw& left, const GripInRaw& right)
        {
            for (int f = 0; f < 2; ++f)
            {
                p.grip[f][0] = left;
                p.grip[f][1] = right;
            }
        }
        inline void Both(std::vector<std::string>* legal, std::initializer_list<const char*> paths)
        {
            for (const char* path : paths)
            {
                legal[0].push_back(path);
                legal[1].push_back(path);
            }
        }
        inline void One(std::vector<std::string>* legal, Hand hand, std::initializer_list<const char*> paths)
        {
            for (const char* path : paths)
                legal[static_cast<int>(hand)].push_back(path);
        }
        inline void Bind(Profile& p, Action a, Hand h, const char* input, float press = 0.0f, float release = 0.0f)
        {
            Binding b;
            b.action = a; b.hand = h; b.input = input; b.mode = Mode::Button; b.press = press; b.release = release;
            p.bindings.push_back(b);
        }
        inline void Stick(Profile& p, Action a, Hand h, const char* input)
        {
            Binding b;
            b.action = a; b.hand = h; b.input = input; b.mode = Mode::Stick;
            p.bindings.push_back(b);
        }
        inline void MenuDpad(Profile& p, Hand h, const char* input)
        {
            const struct { Action a; DpadDir d; } dirs[] = {
                { Action::MenuUp, DpadDir::North }, { Action::MenuRight, DpadDir::East },
                { Action::MenuDown, DpadDir::South }, { Action::MenuLeft, DpadDir::West } };
            for (const auto& d : dirs)
            {
                Binding b;
                b.action = d.a; b.hand = h; b.input = input; b.mode = Mode::Dpad; b.dir = d.d;
                p.bindings.push_back(b);
            }
        }

        // The layout every Touch-style controller (two face buttons per hand,
        // thumbsticks, analog or click grips) gets: SteamVR's
        // bindings_oculus_touch.json.
        inline void TouchStyleBindings(Profile& p, const char* squeezeInput, const char* triggerInput,
            const char* leftPrimary, const char* leftSecondary)
        {
            Stick(p, Action::Walk, Hand::Left, "/input/thumbstick");
            Bind(p, Action::ResetPosition, Hand::Left, "/input/thumbstick/click");
            Stick(p, Action::Turn, Hand::Right, "/input/thumbstick");
            Bind(p, Action::Flashlight, Hand::Right, "/input/thumbstick/click");
            Bind(p, Action::OffHandGrip, Hand::Left, squeezeInput);
            Bind(p, Action::WeaponGrip, Hand::Right, squeezeInput);
            Bind(p, Action::PrimaryAttack, Hand::Right, triggerInput);
            Bind(p, Action::SecondaryAttack, Hand::Left, triggerInput);
            Bind(p, Action::Use, Hand::Right, "/input/b/click");
            Bind(p, Action::MenuBack, Hand::Right, "/input/b/click");
            Bind(p, Action::ActivateVR, Hand::Right, "/input/a/click");
            Bind(p, Action::MagazineRelease, Hand::Right, "/input/a/click");
            Bind(p, Action::MenuSelect, Hand::Right, "/input/a/click");
            MenuDpad(p, Hand::Left, "/input/thumbstick");
            Bind(p, Action::Flashlight, Hand::Left, leftPrimary);
            Bind(p, Action::Pause, Hand::Left, leftSecondary);
        }

        // SteamVR openxr_grip of every Touch-family render model (Rift, Rift S,
        // Quest 1/2/3/Pro) and of the Pico 4 and Focus 3 models.
        inline void TouchGrip(Profile& p)
        {
            SameGripAllRuntimes(p,
                Grip(0.007f, -0.00182941f, 0.1019482f, 20.6f, 0.0f, 0.0f, EulerOrder::XYZ),
                Grip(-0.007f, -0.00182941f, 0.1019482f, 20.6f, 0.0f, 0.0f, EulerOrder::XYZ));
        }

        inline void TouchLegal(Profile& p)
        {
            Both(p.legal, { "/input/squeeze/value", "/input/trigger/value", "/input/trigger/touch",
                "/input/thumbstick", "/input/thumbstick/x", "/input/thumbstick/y", "/input/thumbstick/click",
                "/input/thumbstick/touch", "/input/thumbrest/touch", "/input/grip/pose", "/input/aim/pose", "/output/haptic" });
            One(p.legal, Hand::Left, { "/input/x/click", "/input/x/touch", "/input/y/click", "/input/y/touch", "/input/menu/click" });
            One(p.legal, Hand::Right, { "/input/a/click", "/input/a/touch", "/input/b/click", "/input/b/touch", "/input/system/click" });
        }

        inline void TouchThumbs(Profile& p)
        {
            p.thumbTouch[0] = { "/input/thumbstick/touch", "/input/x/touch", "/input/y/touch", "/input/thumbrest/touch" };
            p.thumbTouch[1] = { "/input/thumbstick/touch", "/input/a/touch", "/input/b/touch", "/input/thumbrest/touch" };
        }

        inline std::vector<Profile> BuildProfiles()
        {
            std::vector<Profile> out;

            // ---- Valve Index (bindings_knuckles.json) ----
            {
                Profile p;
                p.id = ProfileId::Index; p.key = "index"; p.path = "/interaction_profiles/valve/index_controller";
                p.controllerType = "knuckles";
                p.renderModel[0] = "{indexcontroller}valve_controller_knu_1_0_left";
                p.renderModel[1] = "{indexcontroller}valve_controller_knu_1_0_right";
                p.mainAxis = "/input/thumbstick"; p.mainAxisTouch = "/input/thumbstick/touch";
                p.triggerCurl = "/input/trigger/value"; p.squeezeCurl = "/input/squeeze/value";
                p.thumbTouch[0] = p.thumbTouch[1] = { "/input/thumbstick/touch", "/input/a/touch", "/input/b/touch", "/input/trackpad/touch" };
                // SteamVR: handgrip of valve_controller_knu_1_0_{left,right}.json.
                p.grip[0][0] = Grip(-0.003851f, 0.003715f, 0.075948f, 15.392f, -2.071f, 0.303f, EulerOrder::XYZ);
                p.grip[0][1] = Grip(0.003851f, 0.003715f, 0.075948f, 15.392f, 2.071f, -0.303f, EulerOrder::XYZ);
                // Monado: vive_poses.c XRT_INPUT_INDEX_GRIP_POSE (right hand mirrors x, ry, rz).
                p.grip[1][0] = Grip(0.0f, -0.015f, 0.13f, 15.392f, -2.071f, 0.303f, EulerOrder::ZYX);
                p.grip[1][1] = Grip(0.0f, -0.015f, 0.13f, 15.392f, 2.071f, -0.303f, EulerOrder::ZYX);
                p.gripNote = "SteamVR knu_1_0 handgrip / Monado vive_poses.c";
                Stick(p, Action::Walk, Hand::Left, "/input/thumbstick");
                Bind(p, Action::ResetPosition, Hand::Left, "/input/thumbstick/click");
                Stick(p, Action::Turn, Hand::Right, "/input/thumbstick");
                Bind(p, Action::Flashlight, Hand::Right, "/input/thumbstick/click");
                // force_input=value with the binding file's click thresholds.
                Bind(p, Action::OffHandGrip, Hand::Left, "/input/squeeze/value", 0.80f, 0.45f);
                Bind(p, Action::WeaponGrip, Hand::Right, "/input/squeeze/value", 0.85f, 0.50f);
                Bind(p, Action::PrimaryAttack, Hand::Right, "/input/trigger/click");
                Bind(p, Action::SecondaryAttack, Hand::Left, "/input/trigger/click");
                Bind(p, Action::Use, Hand::Right, "/input/b/click");
                Bind(p, Action::MenuBack, Hand::Right, "/input/b/click");
                Bind(p, Action::ActivateVR, Hand::Right, "/input/a/click");
                Bind(p, Action::MagazineRelease, Hand::Right, "/input/a/click");
                Bind(p, Action::MenuSelect, Hand::Right, "/input/a/click");
                MenuDpad(p, Hand::Left, "/input/thumbstick");
                Bind(p, Action::Flashlight, Hand::Left, "/input/a/click");
                Bind(p, Action::Pause, Hand::Left, "/input/b/click");
                // OpenXR has no trackpad click on Index: SteamVR's click is a force threshold.
                Bind(p, Action::Scoreboard, Hand::Left, "/input/trackpad/force");
                Both(p.legal, { "/input/system/click", "/input/system/touch", "/input/a/click", "/input/a/touch",
                    "/input/b/click", "/input/b/touch", "/input/squeeze/value", "/input/squeeze/force",
                    "/input/trigger/click", "/input/trigger/value", "/input/trigger/touch", "/input/thumbstick",
                    "/input/thumbstick/x", "/input/thumbstick/y", "/input/thumbstick/click", "/input/thumbstick/touch",
                    "/input/trackpad", "/input/trackpad/x", "/input/trackpad/y", "/input/trackpad/force", "/input/trackpad/touch",
                    "/input/grip/pose", "/input/aim/pose", "/output/haptic" });
                out.push_back(p);
            }

            // ---- Oculus Touch (bindings_oculus_touch.json), Quest via Link/VD/WiVRn ----
            {
                Profile p;
                p.id = ProfileId::Touch; p.key = "touch"; p.path = "/interaction_profiles/oculus/touch_controller";
                p.controllerType = "oculus_touch";
                p.renderModel[0] = "oculus_quest2_controller_left"; p.renderModel[1] = "oculus_quest2_controller_right";
                p.mainAxis = "/input/thumbstick"; p.mainAxisTouch = "/input/thumbstick/touch";
                p.triggerCurl = "/input/trigger/value"; p.squeezeCurl = "/input/squeeze/value";
                TouchThumbs(p);
                TouchGrip(p);
                p.gripNote = "SteamVR openxr_grip (Touch render models)";
                TouchStyleBindings(p, "/input/squeeze/value", "/input/trigger/value", "/input/x/click", "/input/y/click");
                TouchLegal(p);
                p.unboundByDesign = { Action::Scoreboard };  // no spare input (as bindings_oculus_touch.json)
                out.push_back(p);
            }

            // ---- Meta Quest Pro controllers ----
            {
                Profile p = out.back();
                p.id = ProfileId::TouchPro; p.key = "touchpro"; p.path = "/interaction_profiles/facebook/touch_controller_pro";
                p.extension = "XR_FB_touch_controller_pro";
                p.renderModel[0] = "oculus_quest_pro_controller_left"; p.renderModel[1] = "oculus_quest_pro_controller_right";
                p.gripNote = "SteamVR openxr_grip (oculus_quest_pro_controller)";
                Both(p.legal, { "/input/thumbrest/force", "/input/stylus_fb/force", "/input/trigger/curl_fb",
                    "/input/trigger/slide_fb", "/input/trigger/proximity_fb", "/input/thumb_fb/proximity_fb",
                    "/output/haptic_trigger_fb", "/output/haptic_thumb_fb" });
                out.push_back(p);
            }

            // ---- Meta Quest 3 (Touch Plus) controllers ----
            {
                Profile p = out[1];
                p.id = ProfileId::TouchPlus; p.key = "touchplus"; p.path = "/interaction_profiles/meta/touch_controller_plus";
                p.extension = "XR_META_touch_controller_plus";
                p.renderModel[0] = "oculus_quest_plus_controller_left"; p.renderModel[1] = "oculus_quest_plus_controller_right";
                p.gripNote = "SteamVR openxr_grip (oculus_quest_plus_controller)";
                Both(p.legal, { "/input/thumb_meta/proximity_meta", "/input/trigger/proximity_meta",
                    "/input/trigger/curl_meta", "/input/trigger/slide_meta", "/input/trigger/force" });
                out.push_back(p);
            }

            // ---- HTC Vive wand (no binding file before; SteamVR converted others) ----
            {
                Profile p;
                p.id = ProfileId::ViveWand; p.key = "vive"; p.path = "/interaction_profiles/htc/vive_controller";
                p.controllerType = "vive_controller";
                p.renderModel[0] = p.renderModel[1] = "vr_controller_vive_1_5";
                p.mainAxis = "/input/trackpad"; p.mainAxisTouch = "/input/trackpad/touch";
                p.triggerCurl = "/input/trigger/value"; p.squeezeCurl = "/input/squeeze/click";
                p.thumbTouch[0] = p.thumbTouch[1] = { "/input/trackpad/touch" };
                // SteamVR: handgrip of vr_controller_vive_1_5.json. Monado's
                // wand grip is the device pose (XRizer: identity).
                p.grip[0][0] = p.grip[0][1] = Grip(0.0f, 0.003f, 0.097f, 5.037f, 0.0f, 0.0f, EulerOrder::XYZ);
                p.gripNote = "SteamVR vr_controller_vive_1_5 handgrip / Monado identity";
                // Trackpads act as sticks (SteamVR joystick mode on a trackpad).
                Stick(p, Action::Walk, Hand::Left, "/input/trackpad");
                Stick(p, Action::Turn, Hand::Right, "/input/trackpad");
                MenuDpad(p, Hand::Left, "/input/trackpad");
                Bind(p, Action::OffHandGrip, Hand::Left, "/input/squeeze/click");
                Bind(p, Action::WeaponGrip, Hand::Right, "/input/squeeze/click");
                Bind(p, Action::PrimaryAttack, Hand::Right, "/input/trigger/click");
                Bind(p, Action::SecondaryAttack, Hand::Left, "/input/trigger/click");
                // Right trackpad press = A (select, magazine release); right menu = B.
                Bind(p, Action::ActivateVR, Hand::Right, "/input/trackpad/click");
                Bind(p, Action::MagazineRelease, Hand::Right, "/input/trackpad/click");
                Bind(p, Action::MenuSelect, Hand::Right, "/input/trackpad/click");
                Bind(p, Action::Use, Hand::Right, "/input/menu/click");
                Bind(p, Action::MenuBack, Hand::Right, "/input/menu/click");
                // Left trackpad press = X (flashlight); left menu = Y (pause).
                Bind(p, Action::Flashlight, Hand::Left, "/input/trackpad/click");
                Bind(p, Action::Pause, Hand::Left, "/input/menu/click");
                // Stick clicks have no separate input on a wand (the pads are
                // already A and X), and there is no spare button.
                p.unboundByDesign = { Action::ResetPosition, Action::Scoreboard };
                Both(p.legal, { "/input/system/click", "/input/squeeze/click", "/input/menu/click", "/input/trigger/click",
                    "/input/trigger/value", "/input/trackpad", "/input/trackpad/x", "/input/trackpad/y",
                    "/input/trackpad/click", "/input/trackpad/touch", "/input/grip/pose", "/input/aim/pose", "/output/haptic" });
                out.push_back(p);
            }

            // ---- HTC Vive Cosmos (bindings_vive_cosmos_controller.json) ----
            {
                Profile p;
                p.id = ProfileId::Cosmos; p.key = "cosmos"; p.path = "/interaction_profiles/htc/vive_cosmos_controller";
                p.extension = "XR_HTC_vive_cosmos_controller_interaction";
                p.controllerType = "vive_cosmos_controller";
                p.renderModel[0] = "vive_cosmos_controller_left"; p.renderModel[1] = "vive_cosmos_controller_right";
                p.mainAxis = "/input/thumbstick"; p.mainAxisTouch = "/input/thumbstick/touch";
                p.triggerCurl = "/input/trigger/value"; p.squeezeCurl = "/input/squeeze/click";
                p.thumbTouch[0] = p.thumbTouch[1] = { "/input/thumbstick/touch" };
                // No measured data: the Cosmos is a Touch-style controller; HTC's
                // later Focus 3 controller uses exactly the Touch openxr_grip.
                TouchGrip(p);
                p.gripNote = "ESTIMATE: Touch-style openxr_grip (no Cosmos data)";
                TouchStyleBindings(p, "/input/squeeze/click", "/input/trigger/click", "/input/x/click", "/input/y/click");
                Bind(p, Action::Scoreboard, Hand::Left, "/input/shoulder/click");
                Both(p.legal, { "/input/shoulder/click", "/input/squeeze/click", "/input/trigger/click", "/input/trigger/value",
                    "/input/thumbstick", "/input/thumbstick/x", "/input/thumbstick/y", "/input/thumbstick/click",
                    "/input/thumbstick/touch", "/input/grip/pose", "/input/aim/pose", "/output/haptic" });
                One(p.legal, Hand::Left, { "/input/x/click", "/input/y/click", "/input/menu/click" });
                One(p.legal, Hand::Right, { "/input/a/click", "/input/b/click", "/input/system/click" });
                out.push_back(p);
            }

            // ---- HTC Vive Focus 3 ----
            {
                Profile p;
                p.id = ProfileId::Focus3; p.key = "focus3"; p.path = "/interaction_profiles/htc/vive_focus3_controller";
                p.extension = "XR_HTC_vive_focus3_controller_interaction";
                p.controllerType = "vive_focus3_controller";
                p.renderModel[0] = "vive_focus3_controller_left"; p.renderModel[1] = "vive_focus3_controller_right";
                p.mainAxis = "/input/thumbstick"; p.mainAxisTouch = "/input/thumbstick/touch";
                p.triggerCurl = "/input/trigger/value"; p.squeezeCurl = "/input/squeeze/click";
                p.thumbTouch[0] = p.thumbTouch[1] = { "/input/thumbstick/touch", "/input/thumbrest/touch" };
                TouchGrip(p);
                p.gripNote = "SteamVR openxr_grip (vive_focus3_controller)";
                TouchStyleBindings(p, "/input/squeeze/click", "/input/trigger/click", "/input/x/click", "/input/y/click");
                p.unboundByDesign = { Action::Scoreboard };
                Both(p.legal, { "/input/squeeze/click", "/input/squeeze/touch", "/input/trigger/click", "/input/trigger/touch",
                    "/input/trigger/value", "/input/thumbstick", "/input/thumbstick/x", "/input/thumbstick/y",
                    "/input/thumbstick/click", "/input/thumbstick/touch", "/input/thumbrest/touch", "/input/grip/pose",
                    "/input/aim/pose", "/output/haptic" });
                One(p.legal, Hand::Left, { "/input/x/click", "/input/y/click", "/input/menu/click" });
                One(p.legal, Hand::Right, { "/input/a/click", "/input/b/click", "/input/system/click" });
                out.push_back(p);
            }

            // ---- Windows Mixed Reality (also the Monado simulator) ----
            {
                Profile p;
                p.id = ProfileId::Wmr; p.key = "wmr"; p.path = "/interaction_profiles/microsoft/motion_controller";
                p.controllerType = "holographic_controller";
                p.renderModel[0] = "holographic_controller_left"; p.renderModel[1] = "holographic_controller_right";
                p.mainAxis = "/input/thumbstick"; p.mainAxisTouch = nullptr;
                p.triggerCurl = "/input/trigger/value"; p.squeezeCurl = "/input/squeeze/click";
                p.thumbTouch[0] = p.thumbTouch[1] = { "/input/trackpad/touch" };
                // ESTIMATE: no measured data (XRizer has no WMR profile; SteamVR WMR models are not installed here).
                p.gripNote = "ESTIMATE: identity (no measured data)";
                Stick(p, Action::Walk, Hand::Left, "/input/thumbstick");
                Bind(p, Action::ResetPosition, Hand::Left, "/input/thumbstick/click");
                Stick(p, Action::Turn, Hand::Right, "/input/thumbstick");
                Bind(p, Action::Flashlight, Hand::Right, "/input/thumbstick/click");
                MenuDpad(p, Hand::Left, "/input/thumbstick");
                Bind(p, Action::OffHandGrip, Hand::Left, "/input/squeeze/click");
                Bind(p, Action::WeaponGrip, Hand::Right, "/input/squeeze/click");
                Bind(p, Action::PrimaryAttack, Hand::Right, "/input/trigger/value");
                Bind(p, Action::SecondaryAttack, Hand::Left, "/input/trigger/value");
                // Right trackpad press = A, right menu = B, left menu = Y (pause),
                // left trackpad press = scoreboard (Index uses its left trackpad too).
                Bind(p, Action::ActivateVR, Hand::Right, "/input/trackpad/click");
                Bind(p, Action::MagazineRelease, Hand::Right, "/input/trackpad/click");
                Bind(p, Action::MenuSelect, Hand::Right, "/input/trackpad/click");
                Bind(p, Action::Use, Hand::Right, "/input/menu/click");
                Bind(p, Action::MenuBack, Hand::Right, "/input/menu/click");
                Bind(p, Action::Pause, Hand::Left, "/input/menu/click");
                Bind(p, Action::Scoreboard, Hand::Left, "/input/trackpad/click");
                Both(p.legal, { "/input/menu/click", "/input/squeeze/click", "/input/trigger/value", "/input/thumbstick",
                    "/input/thumbstick/x", "/input/thumbstick/y", "/input/thumbstick/click", "/input/trackpad",
                    "/input/trackpad/x", "/input/trackpad/y", "/input/trackpad/click", "/input/trackpad/touch",
                    "/input/grip/pose", "/input/aim/pose", "/output/haptic" });
                out.push_back(p);
            }

            // ---- HP Reverb G2 ----
            {
                Profile p;
                p.id = ProfileId::HpReverb; p.key = "hpreverb"; p.path = "/interaction_profiles/hp/mixed_reality_controller";
                p.extension = "XR_EXT_hp_mixed_reality_controller";
                p.controllerType = "hpmotioncontroller";
                p.renderModel[0] = "hpmotioncontroller_left"; p.renderModel[1] = "hpmotioncontroller_right";
                p.mainAxis = "/input/thumbstick"; p.mainAxisTouch = nullptr;
                p.triggerCurl = "/input/trigger/value"; p.squeezeCurl = "/input/squeeze/value";
                p.gripNote = "ESTIMATE: identity (WMR family)";
                TouchStyleBindings(p, "/input/squeeze/value", "/input/trigger/value", "/input/x/click", "/input/y/click");
                p.unboundByDesign = { Action::Scoreboard };
                Both(p.legal, { "/input/menu/click", "/input/squeeze/value", "/input/trigger/value", "/input/thumbstick",
                    "/input/thumbstick/x", "/input/thumbstick/y", "/input/thumbstick/click", "/input/grip/pose",
                    "/input/aim/pose", "/output/haptic" });
                One(p.legal, Hand::Left, { "/input/x/click", "/input/y/click" });
                One(p.legal, Hand::Right, { "/input/a/click", "/input/b/click" });
                out.push_back(p);
            }

            // ---- Pico 4 ----
            {
                Profile p;
                p.id = ProfileId::Pico4; p.key = "pico4"; p.path = "/interaction_profiles/bytedance/pico4_controller";
                p.extension = "XR_BD_controller_interaction";
                p.controllerType = "pico_controller";
                p.renderModel[0] = "pico_4_controller_left"; p.renderModel[1] = "pico_4_controller_right";
                p.mainAxis = "/input/thumbstick"; p.mainAxisTouch = "/input/thumbstick/touch";
                p.triggerCurl = "/input/trigger/value"; p.squeezeCurl = "/input/squeeze/value";
                // XR_BD_controller_interaction defines no thumbrest on the Pico 4.
                p.thumbTouch[0] = { "/input/thumbstick/touch", "/input/x/touch", "/input/y/touch" };
                p.thumbTouch[1] = { "/input/thumbstick/touch", "/input/a/touch", "/input/b/touch" };
                TouchGrip(p);
                p.gripNote = "SteamVR openxr_grip (pico_4_controller)";
                TouchStyleBindings(p, "/input/squeeze/value", "/input/trigger/click", "/input/x/click", "/input/y/click");
                p.unboundByDesign = { Action::Scoreboard };
                Both(p.legal, { "/input/trigger/click", "/input/trigger/value", "/input/trigger/touch", "/input/thumbstick",
                    "/input/thumbstick/x", "/input/thumbstick/y", "/input/thumbstick/click", "/input/thumbstick/touch",
                    "/input/squeeze/click", "/input/squeeze/value", "/input/grip/pose",
                    "/input/aim/pose", "/output/haptic" });
                One(p.legal, Hand::Left, { "/input/x/click", "/input/x/touch", "/input/y/click", "/input/y/touch", "/input/menu/click" });
                One(p.legal, Hand::Right, { "/input/a/click", "/input/a/touch", "/input/b/click", "/input/b/touch", "/input/system/click" });
                out.push_back(p);
            }

            // ---- Khronos simple controller (fallback) ----
            {
                Profile p;
                p.id = ProfileId::Simple; p.key = "simple"; p.path = "/interaction_profiles/khr/simple_controller";
                p.controllerType = "generic";
                p.renderModel[0] = p.renderModel[1] = "generic_controller";
                p.mainAxis = nullptr; p.mainAxisTouch = nullptr;
                p.triggerCurl = "/input/select/click"; p.squeezeCurl = "/input/menu/click";
                p.gripNote = "identity (XRizer simple_controller)";
                // Two buttons: select fires, menu grips (as XRizer maps them).
                Bind(p, Action::PrimaryAttack, Hand::Right, "/input/select/click");
                Bind(p, Action::SecondaryAttack, Hand::Left, "/input/select/click");
                Bind(p, Action::WeaponGrip, Hand::Right, "/input/menu/click");
                Bind(p, Action::OffHandGrip, Hand::Left, "/input/menu/click");
                p.unboundByDesign = { Action::Walk, Action::Turn, Action::ResetPosition, Action::Flashlight, Action::Use,
                    Action::MenuBack, Action::ActivateVR, Action::MagazineRelease, Action::MenuSelect, Action::MenuUp,
                    Action::MenuDown, Action::MenuLeft, Action::MenuRight, Action::Pause, Action::Scoreboard };
                Both(p.legal, { "/input/select/click", "/input/menu/click", "/input/grip/pose", "/input/aim/pose", "/output/haptic" });
                out.push_back(p);
            }

            // Profiles built from a copy keep their index in ProfileId order.
            return out;
        }
    }

    inline const std::vector<Profile>& Profiles()
    {
        static const std::vector<Profile> profiles = detail::BuildProfiles();
        return profiles;
    }
    inline const Profile* FindProfile(ProfileId id)
    {
        for (const Profile& p : Profiles())
            if (p.id == id)
                return &p;
        return nullptr;
    }
    inline const Profile* FindProfileByPath(const char* path)
    {
        for (const Profile& p : Profiles())
            if (path && std::strcmp(p.path, path) == 0)
                return &p;
        return nullptr;
    }
    inline const Profile* FindProfileByKey(const char* key)
    {
        for (const Profile& p : Profiles())
            if (EqualsNoCase(p.key, key))
                return &p;
        return nullptr;
    }

    // The actions SteamVR's default binding files bind on every controller; a
    // profile must bind each of them unless its hardware lacks the input.
    inline const std::vector<Action>& DefaultBoundActions()
    {
        static const std::vector<Action> actions = {
            Action::Walk, Action::Turn, Action::ResetPosition, Action::Flashlight, Action::OffHandGrip,
            Action::WeaponGrip, Action::PrimaryAttack, Action::SecondaryAttack, Action::Use, Action::MenuBack,
            Action::ActivateVR, Action::MagazineRelease, Action::MenuSelect, Action::MenuUp, Action::MenuDown,
            Action::MenuLeft, Action::MenuRight, Action::Pause, Action::Scoreboard };
        return actions;
    }

    // ---- GetActionBindingInfo strings (SteamVR shapes; vr_lifecycle_pose_hud.inl
    // builds the left-handed mirror from them) -----------------------------------
    struct BindingInfoText
    {
        std::string device;      // /user/hand/left
        std::string input;       // /user/hand/left/input/trigger
        std::string mode;        // button | joystick | dpad
        std::string slot;        // click | position | north ...
        std::string sourceType;  // button | trigger | joystick | trackpad
    };

    inline BindingInfoText DescribeBinding(const Binding& b)
    {
        BindingInfoText t;
        t.device = HandPath(b.hand);
        std::string component = b.input;
        const char* leaves[] = { "/click", "/value", "/force", "/touch", "/x", "/y" };
        for (const char* leaf : leaves)
        {
            if (EndsWith(component.c_str(), leaf))
            {
                component.resize(component.size() - std::strlen(leaf));
                break;
            }
        }
        t.input = t.device + component;
        const bool trigger = component == "/input/trigger" || component == "/input/squeeze";
        const bool stick = component == "/input/thumbstick";
        const bool pad = component == "/input/trackpad";
        t.sourceType = trigger ? "trigger" : stick ? "joystick" : pad ? "trackpad" : "button";
        switch (b.mode)
        {
        case Mode::Stick: t.mode = "joystick"; t.slot = "position"; break;
        case Mode::Dpad:
            t.mode = "dpad";
            t.slot = b.dir == DpadDir::North ? "north" : b.dir == DpadDir::East ? "east" : b.dir == DpadDir::South ? "south" : "west";
            break;
        default: t.mode = "button"; t.slot = "click"; break;
        }
        return t;
    }
}

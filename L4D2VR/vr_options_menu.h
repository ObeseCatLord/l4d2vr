#pragma once

// VR Options menu model (2.0.5): the sections and rows of the in-VR options
// page, value rules (ranges, steps, choices), editing of the config.txt text
// (one key at a time, everything else kept byte for byte), per-section reset,
// "reset all VR options" (vr/config.txt only, backed up first) and the page
// layout with its hit tests. std-only, so it is unit tested
// (tests/vr_options_menu_tests.cpp); vr_config_overlay_embedded.inl renders it.
//
// Every keyed row is a vr/config.txt option the game already reads on
// hot-reload; the menu writes config.txt on each change, so changes apply live
// (rows with `restart` say a game restart is needed). Nothing here touches the
// game's own settings (config.cfg, video.txt).

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

namespace l4d2vr_options_menu
{
    enum class RowKind { Toggle, Slider, Choice, Action, Info };
    enum class RowAction { None, Recenter, OpenAdvanced };

    struct Row
    {
        const char* key = nullptr;        // config.txt key (nullptr for Action / Info rows)
        RowKind kind = RowKind::Info;
        const char* label = "";
        const char* help = "";
        float minValue = 0.0f;
        float maxValue = 1.0f;
        float step = 0.1f;
        const char* defaultValue = "";
        const char* choices = nullptr;    // "value:Label|value:Label" (Choice rows)
        const char* dependsOn = nullptr;  // "Key" (on), "!Key" (off) or "Key=value"
        bool restart = false;             // takes effect after a game restart
        const char* unit = "";            // shown after slider values
        RowAction action = RowAction::None;
    };

    struct Section
    {
        const char* id;
        const char* title;
        const char* titleZh;
        const Row* rows;
        int rowCount;
        // Bool options of the config panel's table whose key starts with one of
        // these (comma separated) and that no section lists join this section
        // (settings added by other features show up without editing this file).
        const char* autoPrefixes;
        bool advanced;                    // the "Advanced" page: every config row
    };

    // ---- Rows ---------------------------------------------------------------
    inline const Row kComfortRows[] = {
        { "SnapTurning", RowKind::Toggle, "Snap turning", "Turn in fixed steps with the right stick instead of smoothly.", 0, 0, 0, "false" },
        { "SnapTurnAngle", RowKind::Slider, "Snap turn angle", "How far one snap turns.", 15.0f, 120.0f, 15.0f, "45.0", nullptr, "SnapTurning", false, " deg" },
        { "TurnSpeed", RowKind::Slider, "Smooth turn speed", "How fast the right stick turns you when snap turning is off.", 0.1f, 1.0f, 0.05f, "0.3", nullptr, "!SnapTurning" },
        { "MoveDirectionFromController", RowKind::Choice, "Walk direction", "Which way the left stick walks.", 0, 0, 0, "false", "false:Where you look|true:Where the hand points" },
        { "LeftHanded", RowKind::Choice, "Dominant hand", "Your gun hand. Left swaps the hands and their buttons.", 0, 0, 0, "false", "false:Right|true:Left" },
        { "Roomscale1To1Movement", RowKind::Toggle, "Room-scale 1:1 movement", "Walking in your room moves the survivor one to one.", 0, 0, 0, "false" },
        { "Roomscale1To1PhysicalCrouch", RowKind::Toggle, "Physical crouching", "Ducking for real crouches the survivor.", 0, 0, 0, "true" },
        { "VRScale", RowKind::Slider, "World scale", "Units per metre: lower makes the world look bigger.", 30.0f, 55.0f, 0.4f, "43.2" },
        { "ControllerSmoothing", RowKind::Slider, "Hand smoothing", "Smooths shaky controller tracking (0 = off).", 0.0f, 0.8f, 0.05f, "0.0" },
        { nullptr, RowKind::Action, "Recenter and set height", "Stand or sit the way you play and press this: your head height becomes the survivor's eye height (seated play: press it seated). Same as the recenter button.", 0, 0, 0, "", nullptr, nullptr, false, "", RowAction::Recenter },
    };

    inline const Row kBodyRows[] = {
        { "FirstPersonBodyEnabled", RowKind::Toggle, "Full body", "Show your survivor's body below you.", 0, 0, 0, "false" },
        { "FirstPersonBodyArmIK", RowKind::Toggle, "Body arms reach your hands", "The body's arms follow your VR hands.", 0, 0, 0, "true", nullptr, "FirstPersonBodyEnabled" },
        { "NativeViewmodelHandsOnly", RowKind::Choice, "Hand mode", "How the survivor's arms are shown in first person.", 0, 0, 0, "true", "true:Arms follow your hands|false:Game's animated arms" },
        { "NativeViewmodelArmCroppingEnabled", RowKind::Toggle, "Hands only (cropped arms)", "Show only the hands, not the arms.", 0, 0, 0, "false" },
        { "AmbidextrousItemHands", RowKind::Toggle, "Either hand takes items", "The hand that draws an item holds and uses it.", 0, 0, 0, "true" },
        { "ChestSlingEnabled", RowKind::Toggle, "Chest sling", "Let go of a long gun near your body to hang it on your chest.", 0, 0, 0, "true" },
        { "HolsterPavlovLayoutEnabled", RowKind::Toggle, "Pavlov holster layout", "Holsters on your hips, chest and back like Pavlov VR.", 0, 0, 0, "true" },
        { "HolsterItemsVisible", RowKind::Toggle, "Show holstered items", "Draw the items that sit in your holsters.", 0, 0, 0, "true" },
        { "HandPosesEnabled", RowKind::Toggle, "Finger poses", "Fingers wrap around what you hold.", 0, 0, 0, "true" },
        { "HideArms", RowKind::Toggle, "Hide arms", "Hide the first-person arms completely.", 0, 0, 0, "false" },
    };

    inline const Row kWeaponRows[] = {
        { "RecoilEnabled", RowKind::Toggle, "Recoil", "Guns kick in your hand on each shot (Pavlov VR values).", 0, 0, 0, "true" },
        { "RecoilScale", RowKind::Slider, "Recoil strength", "1 is Pavlov's kick, 0 none.", 0.0f, 2.0f, 0.1f, "1.0", nullptr, "RecoilEnabled", false, "x" },
        { "RecoilStanceEnabled", RowKind::Toggle, "Stance recoil", "Crouching, moving and jumping change the kick.", 0, 0, 0, "true", nullptr, "RecoilEnabled" },
        { "CrosshairEnabled", RowKind::Toggle, "Crosshair", "Show a crosshair where the gun points (scopes keep their reticles).", 0, 0, 0, "false" },
        { "IronSightAimEnabled", RowKind::Toggle, "Iron-sight aiming", "Bullets go where the sights point.", 0, 0, 0, "true" },
        { "ScopeEnabled", RowKind::Toggle, "Scopes", "Magnified scopes on sniper rifles and the SG552.", 0, 0, 0, "true" },
        { "ScopePictureInPictureLens", RowKind::Toggle, "Picture-in-picture lens", "The zoomed view shows in the scope's lens.", 0, 0, 0, "true", nullptr, "ScopeEnabled" },
        { "ManualPumpEveryShot", RowKind::Choice, "Pump shotguns", "When the pump and Chrome shotguns need a pump.", 0, 0, 0, "false", "false:Only when empty|true:Every shot", "ManualPumpEnabled" },
        { "MagazineSlapEnabled", RowKind::Toggle, "Magazine slap", "Strike the old magazine out with a fresh one (AK-47, SG552, military sniper).", 0, 0, 0, "true" },
        { "MagazineInteractionEnabled", RowKind::Toggle, "Physical reloads", "Reload by hand: magazines, shells, slides and bolts.", 0, 0, 0, "true" },
        { "SpreadDisabled", RowKind::Toggle, "No bullet spread", "Bullets fly exactly where the gun points.", 0, 0, 0, "true" },
        { "GunRigidInHand", RowKind::Toggle, "Gun locked to the hand", "The game's fire animation does not move the gun in your hand.", 0, 0, 0, "true" },
    };

    inline const Row kGrenadeRows[] = {
        { "GrenadeThrowMode", RowKind::Choice, "Throw mode", "Arc: aim with the shown arc and let go of the trigger. Physics: throw with your hand's own motion.", 0, 0, 0, "arc", "arc:Arc|physics:Physics" },
        { "GrenadeThrowAssist", RowKind::Toggle, "Physics throw assistance", "Gently steers a hand throw onto the spot your hand points at, when the throw is already heading there.", 0, 0, 0, "true", nullptr, "GrenadeThrowMode=physics" },
        { "GrenadeThrowAssistStrength", RowKind::Slider, "Assistance strength", "0 = your throw as it is, 1 = lands on the spot you point at.", 0.0f, 1.0f, 0.05f, "0.5", nullptr, "GrenadeThrowAssist" },
        { "ManualThrowHandVelocityScale", RowKind::Slider, "Throw boost", "Physics throws leave at your hand's speed times this.", 1.0f, 4.0f, 0.1f, "2.5", nullptr, "GrenadeThrowMode=physics", false, "x" },
        { "ManualThrowPinRequired", RowKind::Toggle, "Pull the pin first", "Without the pin, letting go of the grip drops the grenade.", 0, 0, 0, "true", nullptr, "GrenadeThrowMode=physics" },
    };

    inline const Row kHudRows[] = {
        { "LeftWristHudEnabled", RowKind::Toggle, "Health on your wrist", "Team health on the off-hand wrist.", 0, 0, 0, "true" },
        { "RightAmmoHudEnabled", RowKind::Toggle, "Ammo on your wrist", "Ammo on the gun hand's wrist.", 0, 0, 0, "true" },
        { "WristHudStyle", RowKind::Choice, "Wrist HUD style", "The game's own HUD pieces or simple panels.", 0, 0, 0, "native", "native:Game HUD|custom:Simple panels" },
        { "HudAlwaysVisible", RowKind::Toggle, "Floating HUD always on", "Keep the floating game HUD in view all the time.", 0, 0, 0, "false" },
        { "HudAlwaysHidden", RowKind::Toggle, "No floating HUD", "Never show the floating game HUD (wrist HUDs only).", 0, 0, 0, "false" },
        { "HudDistance", RowKind::Slider, "Floating HUD distance", "Metres from your head.", 0.5f, 3.0f, 0.1f, "1.3", nullptr, "!HudAlwaysHidden", false, " m" },
        { "HudSize", RowKind::Slider, "Floating HUD size", "Width of the floating HUD.", 0.8f, 3.0f, 0.1f, "1.3", nullptr, "!HudAlwaysHidden", false, " m" },
        { "MenuLaserPointerEnabled", RowKind::Toggle, "Menu laser pointer", "Point and pull the trigger to use menus.", 0, 0, 0, "true" },
        { "HitIndicatorEnabled", RowKind::Toggle, "Hit markers", "Show a marker when your shots hit.", 0, 0, 0, "false" },
        { "KillIndicatorEnabled", RowKind::Toggle, "Kill markers", "Show a marker on kills.", 0, 0, 0, "false" },
    };

    inline const Row kAudioRows[] = {
        { "BackgroundAudio", RowKind::Toggle, "Audio when unfocused", "Keep game audio playing when the desktop window is not focused.", 0, 0, 0, "true", nullptr, nullptr, true },
        { "HitSoundEnabled", RowKind::Toggle, "Hit sounds", "A sound when your shots hit.", 0, 0, 0, "false" },
        { "HitSoundVolume", RowKind::Slider, "Hit sound volume", "", 0.5f, 2.0f, 0.1f, "1.2", nullptr, "HitSoundEnabled", false, "x" },
        { "KillSoundEnabled", RowKind::Toggle, "Kill sounds", "A sound on kills.", 0, 0, 0, "false" },
        { "KillSoundVolume", RowKind::Slider, "Kill sound volume", "", 0.0f, 2.0f, 0.1f, "1.8", nullptr, "KillSoundEnabled", false, "x" },
        { "HeadshotSoundVolume", RowKind::Slider, "Headshot sound volume", "", 0.0f, 2.0f, 0.1f, "1.3", nullptr, "KillSoundEnabled", false, "x" },
    };

    inline const Row kControlRows[] = {
        { nullptr, RowKind::Info, "Open this menu", "Hold the pause button (left B on Index, Y on Quest/Touch, menu on Vive/WMR), point at VR Options next to the game's main or pause menu, press F8, or type vroptions in the console." },
        { "VrOptionsMenuHoldSeconds", RowKind::Slider, "Hold pause to open", "How long to hold the pause button to open this menu (0 = off).", 0.0f, 2.0f, 0.1f, "0.8", nullptr, nullptr, false, " s" },
        { nullptr, RowKind::Info, "Buttons", "Trigger: fire, pull the pin, menus. Grip: take, hold and holster items. Left stick: walk (click: recenter). Right stick: turn, jump and crouch. Bindings are built in per controller (docs/openxr.md)." },
        { "GripToggle", RowKind::Toggle, "Toggle grips", "One squeeze takes an item, the next lets go. Off: hold the grip.", 0, 0, 0, "true" },
        { "StickJumpCrouch", RowKind::Toggle, "Jump and crouch on the stick", "Right stick up jumps, down crouches.", 0, 0, 0, "true" },
        { "StickCrouchToggle", RowKind::Toggle, "Stick crouch toggles", "Stick down toggles crouching instead of holding it.", 0, 0, 0, "true", nullptr, "StickJumpCrouch" },
        { "HolsterReleaseDrop", RowKind::Toggle, "Drop on grip release", "Letting go of the grip away from a holster drops the item.", 0, 0, 0, "true" },
        { "MotionOnlyMeleeAndShove", RowKind::Toggle, "Swing to melee and shove", "Melee and shoves only come from real swings.", 0, 0, 0, "true" },
    };

    inline const Row kAdvancedRows[] = {
        { nullptr, RowKind::Action, "Open every setting", "The full L4D2VR config panel: every option, calibration, materials.", 0, 0, 0, "", nullptr, nullptr, false, "", RowAction::OpenAdvanced },
    };

    template <std::size_t N>
    constexpr int CountOf(const Row (&)[N]) { return static_cast<int>(N); }

    inline const Section kSections[] = {
        { "comfort", "Comfort", "\xE8\x88\x92\xE9\x80\x82", kComfortRows, CountOf(kComfortRows), "", false },
        { "body", "Hands & Body", "\xE6\x89\x8B\xE9\x83\xA8\xE4\xB8\x8E\xE8\xBA\xAB\xE4\xBD\x93", kBodyRows, CountOf(kBodyRows), "Holster,ItemHand,GunHand,FirstPersonBody", false },
        { "weapons", "Weapons", "\xE6\xAD\xA6\xE5\x99\xA8", kWeaponRows, CountOf(kWeaponRows), "Recoil,Scope", false },
        { "grenades", "Grenades", "\xE6\x8A\x95\xE6\x8E\xB7\xE7\x89\xA9", kGrenadeRows, CountOf(kGrenadeRows), "GrenadeThrow,ManualThrow", false },
        { "hud", "HUD", "HUD", kHudRows, CountOf(kHudRows), "WristHud", false },
        { "audio", "Audio", "\xE9\x9F\xB3\xE9\xA2\x91", kAudioRows, CountOf(kAudioRows), "Voice,Audio", false },
        { "controls", "Controls", "\xE6\x8E\xA7\xE5\x88\xB6", kControlRows, CountOf(kControlRows), "", false },
        { "advanced", "Advanced", "\xE9\xAB\x98\xE7\xBA\xA7", kAdvancedRows, CountOf(kAdvancedRows), "", true },
    };
    constexpr int kSectionCount = static_cast<int>(sizeof(kSections) / sizeof(kSections[0]));

    // Settings that only take effect after the game restarts.
    inline const char* const kRestartKeys[] = {
        "AntiAliasing", "AutoMatQueueMode", "BackgroundAudio", "ReShadeVRCompat", "VrRuntime",
        "VrRecommendedVideoSettingsEnabled", "DesktopModeEnabled", "DesktopModeAuto", "ScopeRTTSize",
    };

    inline bool IsRestartKey(const std::string& key)
    {
        for (const char* k : kRestartKeys)
            if (key == k)
                return true;
        return false;
    }

    inline const Row* FindRow(const std::string& key, int* sectionOut = nullptr)
    {
        for (int s = 0; s < kSectionCount; ++s)
            for (int i = 0; i < kSections[s].rowCount; ++i)
            {
                const Row& r = kSections[s].rows[i];
                if (r.key && key == r.key)
                {
                    if (sectionOut)
                        *sectionOut = s;
                    return &r;
                }
            }
        return nullptr;
    }

    inline int FindSection(const std::string& id)
    {
        for (int s = 0; s < kSectionCount; ++s)
            if (id == kSections[s].id)
                return s;
        return -1;
    }

    // ---- Choices --------------------------------------------------------------
    struct ChoiceOption { std::string value; std::string label; };

    inline std::vector<ChoiceOption> Choices(const Row& row)
    {
        std::vector<ChoiceOption> out;
        if (!row.choices)
            return out;
        std::string all = row.choices;
        size_t start = 0;
        while (start <= all.size())
        {
            const size_t bar = all.find('|', start);
            const std::string item = all.substr(start, bar == std::string::npos ? std::string::npos : bar - start);
            const size_t colon = item.find(':');
            if (colon != std::string::npos)
                out.push_back({ item.substr(0, colon), item.substr(colon + 1) });
            if (bar == std::string::npos)
                break;
            start = bar + 1;
        }
        return out;
    }

    // ---- Values ---------------------------------------------------------------
    using Values = std::map<std::string, std::string>;

    inline std::string Lower(std::string s)
    {
        for (char& c : s)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    }

    inline std::string Trim(const std::string& s)
    {
        size_t a = 0, b = s.size();
        while (a < b && std::isspace(static_cast<unsigned char>(s[a])))
            ++a;
        while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1])))
            --b;
        return s.substr(a, b - a);
    }

    inline bool ParseBool(const std::string& text, bool fallback)
    {
        const std::string v = Lower(Trim(text));
        if (v == "true" || v == "1" || v == "yes" || v == "on")
            return true;
        if (v == "false" || v == "0" || v == "no" || v == "off")
            return false;
        return fallback;
    }

    inline bool ParseFloat(const std::string& text, float& out)
    {
        const std::string t = Trim(text);
        if (t.empty())
            return false;
        char* end = nullptr;
        const float v = std::strtof(t.c_str(), &end);
        if (end == t.c_str() || !std::isfinite(v))
            return false;
        out = v;
        return true;
    }

    // Digits after the point for a step (0.05 -> 2, 0.4 -> 1, 15 -> 0).
    inline int StepDecimals(float step)
    {
        for (int d = 0; d < 4; ++d)
        {
            const float scaled = step * std::pow(10.0f, static_cast<float>(d));
            if (std::fabs(scaled - std::round(scaled)) < 1e-3f)
                return d == 0 ? 1 : d;   // keep "45.0" style for whole steps
        }
        return 4;
    }

    inline std::string FormatFloat(float v, float step)
    {
        char buf[48];
        std::snprintf(buf, sizeof(buf), "%.*f", StepDecimals(step), static_cast<double>(v));
        std::string s = buf;
        if (s == "-0.0" || s == "-0.00")
            s.erase(0, 1);
        return s;
    }

    inline float SnapToStep(const Row& row, float v)
    {
        v = std::clamp(v, row.minValue, row.maxValue);
        if (row.step > 0.0f)
        {
            v = row.minValue + std::round((v - row.minValue) / row.step) * row.step;
            v = std::clamp(v, row.minValue, row.maxValue);
        }
        return v;
    }

    // The row's value from `values` (or its default), normalized: bools as
    // true/false, sliders clamped to the menu range, choices one of the listed.
    inline std::string Normalize(const Row& row, const std::string& text)
    {
        switch (row.kind)
        {
        case RowKind::Toggle:
            return ParseBool(text, ParseBool(row.defaultValue ? row.defaultValue : "", false)) ? "true" : "false";
        case RowKind::Slider:
        {
            float v = 0.0f;
            if (!ParseFloat(text, v) && !ParseFloat(row.defaultValue ? row.defaultValue : "", v))
                v = row.minValue;
            return FormatFloat(std::clamp(v, row.minValue, row.maxValue), row.step);
        }
        case RowKind::Choice:
        {
            const std::vector<ChoiceOption> options = Choices(row);
            const std::string want = Lower(Trim(text));
            for (const ChoiceOption& o : options)
                if (Lower(o.value) == want)
                    return o.value;
            // Bool-valued choices accept any bool spelling.
            if (options.size() == 2 && (options[0].value == "true" || options[0].value == "false"))
                return ParseBool(text, ParseBool(row.defaultValue ? row.defaultValue : "", false)) ? "true" : "false";
            return row.defaultValue ? row.defaultValue : "";
        }
        default:
            return std::string();
        }
    }

    inline std::string ValueOf(const Row& row, const Values& values)
    {
        if (!row.key)
            return std::string();
        const auto it = values.find(row.key);
        return Normalize(row, it != values.end() ? it->second : std::string(row.defaultValue));
    }

    inline float SliderFraction(const Row& row, const std::string& value)
    {
        float v = 0.0f;
        if (!ParseFloat(value, v))
            ParseFloat(row.defaultValue, v);
        const float range = row.maxValue - row.minValue;
        return range > 0.0f ? std::clamp((v - row.minValue) / range, 0.0f, 1.0f) : 0.0f;
    }

    // The value a slider click at fraction t (0..1 of the bar) selects.
    inline std::string SliderValueAt(const Row& row, float t)
    {
        t = std::isfinite(t) ? std::clamp(t, 0.0f, 1.0f) : 0.0f;
        return FormatFloat(SnapToStep(row, row.minValue + t * (row.maxValue - row.minValue)), row.step);
    }

    // One step (dir -1 / +1): sliders by their step, toggles flip, choices cycle.
    inline std::string StepValue(const Row& row, const std::string& current, int dir)
    {
        switch (row.kind)
        {
        case RowKind::Toggle:
            return ParseBool(current, false) ? "false" : "true";
        case RowKind::Slider:
        {
            float v = 0.0f;
            if (!ParseFloat(current, v) && !ParseFloat(row.defaultValue, v))
                v = row.minValue;
            return FormatFloat(SnapToStep(row, v + static_cast<float>(dir) * row.step), row.step);
        }
        case RowKind::Choice:
        {
            const std::vector<ChoiceOption> options = Choices(row);
            if (options.empty())
                return current;
            const std::string now = Normalize(row, current);
            int index = 0;
            for (int i = 0; i < static_cast<int>(options.size()); ++i)
                if (options[static_cast<size_t>(i)].value == now)
                    index = i;
            const int n = static_cast<int>(options.size());
            index = ((index + (dir < 0 ? -1 : 1)) % n + n) % n;
            return options[static_cast<size_t>(index)].value;
        }
        default:
            return current;
        }
    }

    inline bool DependencyMet(const Row& row, const Values& values)
    {
        if (!row.dependsOn || !*row.dependsOn)
            return true;
        std::string dep = row.dependsOn;
        bool negate = false;
        if (dep[0] == '!')
        {
            negate = true;
            dep.erase(0, 1);
        }
        std::string want;
        const size_t eq = dep.find('=');
        if (eq != std::string::npos)
        {
            want = Lower(dep.substr(eq + 1));
            dep = dep.substr(0, eq);
        }
        std::string value;
        const Row* parent = FindRow(dep);
        if (parent)
            value = ValueOf(*parent, values);
        else
        {
            const auto it = values.find(dep);
            value = it != values.end() ? it->second : std::string("true");
        }
        const bool met = want.empty() ? ParseBool(value, false) : Lower(Trim(value)) == want;
        return negate ? !met : met;
    }

    // ---- config.txt text -----------------------------------------------------
    // The key of an active "Key=value" line ("" for comments and blank lines).
    inline std::string LineKey(const std::string& rawLine)
    {
        std::string line = rawLine;
        if (line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF &&
            static_cast<unsigned char>(line[1]) == 0xBB && static_cast<unsigned char>(line[2]) == 0xBF)
            line.erase(0, 3);
        const std::string t = Trim(line);
        if (t.empty() || t[0] == '#' || t[0] == ';' || t.rfind("//", 0) == 0)
            return std::string();
        const size_t eq = t.find('=');
        if (eq == std::string::npos)
            return std::string();
        const std::string key = Trim(t.substr(0, eq));
        if (key.empty() || !std::isalpha(static_cast<unsigned char>(key[0])))
            return std::string();
        for (char c : key)
            if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_')
                return std::string();
        return key;
    }

    // Same comment rules as VR::ParseConfigFile: "//" always cuts, '#' and ';'
    // cut except in the *Overrides values that use them.
    inline std::string LineValue(const std::string& rawLine)
    {
        const size_t eq = rawLine.find('=');
        if (eq == std::string::npos)
            return std::string();
        const std::string key = Lower(Trim(rawLine.substr(0, eq)));
        std::string value = rawLine.substr(eq + 1);
        const bool allowSemicolon = key.size() >= 9 && key.compare(key.size() - 9, 9, "overrides") == 0;
        const bool allowHash = key == "manualreloadmagazineboneoverrides" || key == "magazineinteractionboltboneoverrides";
        size_t cut = value.find("//");
        if (!allowHash)
            cut = std::min(cut, value.find('#'));
        if (!allowSemicolon)
            cut = std::min(cut, value.find(';'));
        if (cut != std::string::npos)
            value.erase(cut);
        return Trim(value);
    }

    inline std::vector<std::string> SplitLines(const std::string& text, bool& crlf)
    {
        std::vector<std::string> lines;
        crlf = text.find("\r\n") != std::string::npos;
        size_t start = 0;
        while (start < text.size())
        {
            size_t nl = text.find('\n', start);
            std::string line = text.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            lines.push_back(line);
            if (nl == std::string::npos)
                break;
            start = nl + 1;
        }
        return lines;
    }

    inline std::string JoinLines(const std::vector<std::string>& lines, bool crlf)
    {
        std::string out;
        for (const std::string& l : lines)
        {
            out += l;
            out += crlf ? "\r\n" : "\n";
        }
        return out;
    }

    // Every key's value as the game reads it (the last line of a key wins).
    inline Values ParseConfigText(const std::string& text)
    {
        Values values;
        bool crlf = false;
        for (const std::string& line : SplitLines(text, crlf))
        {
            const std::string key = LineKey(line);
            if (!key.empty())
                values[key] = LineValue(line);
        }
        return values;
    }

    // `text` with key set to value: the key's last active line is rewritten,
    // a missing key is appended; every other byte stays as it was.
    inline std::string SetConfigValue(const std::string& text, const std::string& key, const std::string& value)
    {
        bool crlf = false;
        std::vector<std::string> lines = SplitLines(text, crlf);
        for (int i = static_cast<int>(lines.size()) - 1; i >= 0; --i)
        {
            if (LineKey(lines[static_cast<size_t>(i)]) == key)
            {
                if (LineValue(lines[static_cast<size_t>(i)]) == value)
                    return text;
                lines[static_cast<size_t>(i)] = key + "=" + value;
                return JoinLines(lines, crlf);
            }
        }
        lines.push_back(key + "=" + value);
        return JoinLines(lines, crlf);
    }

    // ---- Sections -----------------------------------------------------------
    // A row shown on a page: a section row or an option from the panel table.
    // `row` holds the kind and ranges; key and texts are owned strings (use
    // these, not row.key/label/defaultValue, which may be null for table rows).
    struct PageRow
    {
        Row row{};
        std::string key;
        std::string label;
        std::string help;
        std::string defaultValue;
        bool fromTable = false;
    };

    struct TableOption
    {
        std::string key;
        std::string label;
        std::string help;
        std::string defaultValue;
        bool isBool = false;
    };

    inline bool KeyListedInAnySection(const std::string& key) { return FindRow(key) != nullptr; }

    inline bool HasPrefix(const std::string& key, const char* prefixes)
    {
        if (!prefixes || !*prefixes)
            return false;
        std::string all = prefixes;
        size_t start = 0;
        while (start < all.size())
        {
            const size_t comma = all.find(',', start);
            const std::string p = Trim(all.substr(start, comma == std::string::npos ? std::string::npos : comma - start));
            if (!p.empty() && key.compare(0, p.size(), p) == 0)
                return true;
            if (comma == std::string::npos)
                break;
            start = comma + 1;
        }
        return false;
    }

    // The section a table option joins (-1: none): the first whose prefix
    // matches, for bool options no section lists (debug/log switches excluded).
    inline int AutoSectionFor(const TableOption& t)
    {
        if (!t.isBool || t.key.empty() || KeyListedInAnySection(t.key) ||
            t.key.find("Debug") != std::string::npos || t.key.find("Log") != std::string::npos)
            return -1;
        for (int k = 0; k < kSectionCount; ++k)
            if (HasPrefix(t.key, kSections[k].autoPrefixes))
                return k;
        return -1;
    }

    // The rows of a section page: its own rows, then the table's bool options
    // that join it (AutoSectionFor).
    inline std::vector<PageRow> PageRows(int section, const std::vector<TableOption>& table)
    {
        std::vector<PageRow> out;
        if (section < 0 || section >= kSectionCount)
            return out;
        const Section& s = kSections[section];
        for (int i = 0; i < s.rowCount; ++i)
        {
            PageRow pr{};
            pr.row = s.rows[i];
            pr.key = pr.row.key ? pr.row.key : "";
            pr.label = pr.row.label;
            pr.help = pr.row.help ? pr.row.help : "";
            pr.defaultValue = pr.row.defaultValue ? pr.row.defaultValue : "";
            out.push_back(pr);
        }
        std::set<std::string> seen;
        for (const TableOption& t : table)
        {
            if (AutoSectionFor(t) != section || seen.count(t.key))
                continue;
            seen.insert(t.key);
            PageRow pr{};
            pr.fromTable = true;
            pr.key = t.key;
            pr.label = t.label.empty() ? t.key : t.label;
            pr.help = t.help;
            pr.defaultValue = Lower(Trim(t.defaultValue)) == "true" ? "true" : "false";
            pr.row.kind = RowKind::Toggle;
            pr.row.key = nullptr;
            pr.row.label = "";
            pr.row.help = "";
            pr.row.defaultValue = "";
            out.push_back(pr);
        }
        return out;
    }

    inline std::string PageValue(const PageRow& pr, const Values& values)
    {
        if (pr.key.empty())
            return std::string();
        Row r = pr.row;
        r.defaultValue = pr.defaultValue.c_str();
        const auto it = values.find(pr.key);
        return Normalize(r, it != values.end() ? it->second : pr.defaultValue);
    }

    inline std::string PageStep(const PageRow& pr, const Values& values, int dir)
    {
        Row r = pr.row;
        r.defaultValue = pr.defaultValue.c_str();
        return Normalize(r, StepValue(r, PageValue(pr, values), dir));
    }

    // ---- Resets ---------------------------------------------------------------
    // `text` with every keyed row of the section at its default (table rows use
    // their table defaults). Other keys and all comments are kept.
    inline std::string ResetSectionText(const std::string& text, int section, const std::vector<TableOption>& table = {})
    {
        if (section < 0 || section >= kSectionCount)
            return text;
        std::string out = text;
        const Section& s = kSections[section];
        for (int i = 0; i < s.rowCount; ++i)
            if (s.rows[i].key)
                out = SetConfigValue(out, s.rows[i].key, Normalize(s.rows[i], s.rows[i].defaultValue));
        for (const TableOption& t : table)
            if (AutoSectionFor(t) == section)
                out = SetConfigValue(out, t.key, Lower(Trim(t.defaultValue)) == "true" ? "true" : "false");
        return out;
    }

    // The shipped defaults: vr/config.sample is config.txt with every key line
    // commented out ("#Key=value", tools/package_client_release.py); a new
    // install uncomments them (dllmain BuildConfigTextFromSampleForNewInstall).
    inline std::string DefaultsTextFromSample(const std::string& sample)
    {
        bool crlf = false;
        std::vector<std::string> lines = SplitLines(sample, crlf);
        for (std::string& line : lines)
        {
            size_t i = 0;
            while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i])))
                ++i;
            if (i < line.size() && line[i] == '#')
            {
                size_t j = i + 1;
                while (j < line.size() && std::isspace(static_cast<unsigned char>(line[j])))
                    ++j;
                const std::string rest = line.substr(j);
                if (!LineKey(rest).empty())
                    line = line.substr(0, i) + rest;
            }
        }
        return JoinLines(lines, crlf);
    }

    // Keys whose value differs (missing counts as different).
    inline std::vector<std::string> ChangedKeys(const Values& before, const Values& after)
    {
        std::vector<std::string> out;
        std::set<std::string> keys;
        for (const auto& kv : before) keys.insert(kv.first);
        for (const auto& kv : after) keys.insert(kv.first);
        for (const std::string& k : keys)
        {
            const auto a = before.find(k), b = after.find(k);
            const bool same = a != before.end() && b != after.end() && Lower(a->second) == Lower(b->second);
            if (!same)
                out.push_back(k);
        }
        return out;
    }

    inline std::vector<std::string> RestartNeeded(const std::vector<std::string>& changed)
    {
        std::vector<std::string> out;
        for (const std::string& k : changed)
            if (IsRestartKey(k))
                out.push_back(k);
        return out;
    }

    inline std::string BackupFileName(std::time_t now)
    {
        std::tm tmv{};
#ifdef _MSC_VER
        localtime_s(&tmv, &now);
#else
        localtime_r(&now, &tmv);
#endif
        char buf[64];
        std::strftime(buf, sizeof(buf), "config.txt.bak-%Y%m%d-%H%M%S", &tmv);
        return buf;
    }

    inline bool ReadFile(const std::filesystem::path& path, std::string& out)
    {
        std::ifstream in(path, std::ios::binary);
        if (!in.good())
            return false;
        std::ostringstream ss;
        ss << in.rdbuf();
        out = ss.str();
        return !in.bad();
    }

    // Writes `text` to `path` through a temporary file in the same folder and a
    // rename, so the game's config watcher never reads a half-written file.
    inline bool WriteFileAtomic(const std::filesystem::path& path, const std::string& text)
    {
        const std::filesystem::path tmp = path.parent_path() / (path.filename().string() + ".tmp");
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            if (!out.good())
                return false;
            out.write(text.data(), static_cast<std::streamsize>(text.size()));
            out.flush();
            if (!out.good())
                return false;
        }
        std::error_code ec;
        std::filesystem::rename(tmp, path, ec);
        if (ec)
        {
            std::filesystem::remove(tmp, ec);
            return false;
        }
        return true;
    }

    struct ResetAllResult
    {
        bool ok = false;
        std::string error;
        std::filesystem::path backup;          // the old config.txt
        std::vector<std::string> changed;      // keys that changed
        std::vector<std::string> restart;      // of those, the ones needing a restart
    };

    // Resets every VR option: vr/config.txt becomes the shipped defaults
    // (DefaultsTextFromSample of vr/config.sample). The current config.txt is
    // copied to vr/config.txt.bak-<time> first. Only those two files in `vrDir`
    // are written; nothing is touched when the sample is missing or empty or the
    // backup fails. The game's own settings (left4dead2/cfg) are never touched.
    inline ResetAllResult ResetAllVrOptions(const std::filesystem::path& vrDir, std::time_t now)
    {
        ResetAllResult r{};
        const std::filesystem::path configPath = vrDir / "config.txt";
        const std::filesystem::path samplePath = vrDir / "config.sample";
        std::string sample;
        if (!ReadFile(samplePath, sample) || ParseConfigText(DefaultsTextFromSample(sample)).empty())
        {
            r.error = "cannot read the shipped defaults (" + samplePath.string() + ")";
            return r;
        }
        const std::string defaults = DefaultsTextFromSample(sample);
        std::string current;
        const bool hadConfig = ReadFile(configPath, current);
        if (hadConfig)
        {
            std::filesystem::path backup = vrDir / BackupFileName(now);
            for (int n = 2; std::filesystem::exists(backup) && n < 100; ++n)
                backup = vrDir / (BackupFileName(now) + "-" + std::to_string(n));
            std::ofstream out(backup, std::ios::binary | std::ios::trunc);
            out.write(current.data(), static_cast<std::streamsize>(current.size()));
            out.flush();
            if (!out.good())
            {
                r.error = "cannot write the backup " + backup.string();
                return r;
            }
            r.backup = backup;
        }
        if (!WriteFileAtomic(configPath, defaults))
        {
            r.error = "cannot write " + configPath.string();
            return r;
        }
        r.changed = ChangedKeys(ParseConfigText(current), ParseConfigText(defaults));
        r.restart = RestartNeeded(r.changed);
        r.ok = true;
        return r;
    }

    // ---- Controller hold (open the menu by holding the pause button) ---------
    class HoldToOpen
    {
    public:
        // True once per hold, when the button has been held `seconds`
        // (<= 0: never). A short press never fires.
        bool Update(bool held, double nowSeconds, float seconds)
        {
            if (!held || !(seconds > 0.0f))
            {
                m_Down = false;
                m_Fired = false;
                return false;
            }
            if (!m_Down)
            {
                m_Down = true;
                m_Since = nowSeconds;
                return false;
            }
            if (!m_Fired && nowSeconds - m_Since >= seconds)
            {
                m_Fired = true;
                return true;
            }
            return false;
        }

    private:
        bool m_Down = false;
        bool m_Fired = false;
        double m_Since = 0.0;
    };

    // ---- Layout (1280 x 900 panel texture) ------------------------------------
    struct Rect
    {
        int x = 0, y = 0, w = 0, h = 0;
        bool Contains(int px, int py) const { return px >= x && px < x + w && py >= y && py < y + h; }
    };

    namespace layout
    {
        constexpr int kW = 1280, kH = 900;
        constexpr int kHeaderH = 96;
        constexpr int kTabX = 16, kTabW = 288, kTabY = 112, kTabH = 72, kTabGap = 6;
        constexpr int kRowsX = 322, kRowsY = 112, kRowsW = 900, kRowH = 92, kRowsVisible = 7;
        constexpr int kScrollX = 1232, kScrollW = 40;
        constexpr int kFooterY = 846;
        constexpr int kControlX = 852;                    // controls start here inside a row
        constexpr int kControlW = kRowsX + kRowsW - 12 - kControlX;
    }

    inline Rect TabRect(int i) { using namespace layout; return { kTabX, kTabY + i * (kTabH + kTabGap), kTabW, kTabH }; }
    inline Rect ResetAllRect() { using namespace layout; return { kTabX, kFooterY - 78, kTabW, 64 }; }
    inline Rect CloseRect() { return { 1104, 18, 160, 60 }; }
    inline Rect ResetSectionRect() { return { 846, 18, 246, 60 }; }
    inline Rect LanguageRect() { return { 736, 18, 98, 60 }; }
    inline Rect RowRect(int visibleIndex) { using namespace layout; return { kRowsX, kRowsY + visibleIndex * kRowH, kRowsW, kRowH - 6 }; }
    inline Rect ScrollUpRect() { using namespace layout; return { kScrollX, kRowsY, kScrollW, 120 }; }
    inline Rect ScrollDownRect() { using namespace layout; return { kScrollX, kRowsY + kRowsVisible * kRowH - 126, kScrollW, 120 }; }
    inline Rect ToggleRect(const Rect& row) { return { layout::kControlX + layout::kControlW - 150, row.y + (row.h - 56) / 2, 150, 56 }; }
    inline Rect MinusRect(const Rect& row) { return { layout::kControlX, row.y + (row.h - 56) / 2, 60, 56 }; }
    inline Rect PlusRect(const Rect& row) { return { layout::kControlX + layout::kControlW - 60, row.y + (row.h - 56) / 2, 60, 56 }; }
    inline Rect BarRect(const Rect& row)
    {
        const Rect m = MinusRect(row), p = PlusRect(row);
        return { m.x + m.w + 10, row.y + row.h / 2 + 4, p.x - 10 - (m.x + m.w + 10), 22 };
    }
    inline Rect ValueTextRect(const Rect& row)
    {
        const Rect m = MinusRect(row), p = PlusRect(row);
        return { m.x + m.w + 10, row.y + 4, p.x - 10 - (m.x + m.w + 10), row.h / 2 };
    }
    inline Rect ChoiceRect(const Rect& row, int index, int count)
    {
        count = std::max(1, count);
        const int gap = 8;
        const int w = (layout::kControlW - gap * (count - 1)) / count;
        return { layout::kControlX + index * (w + gap), row.y + (row.h - 56) / 2, w, 56 };
    }
    inline Rect ActionRect(const Rect& row) { return { layout::kControlX + layout::kControlW - 300, row.y + (row.h - 60) / 2, 300, 60 }; }
    // Confirmation dialog.
    inline Rect DialogRect() { return { 250, 230, 780, 420 }; }
    inline Rect DialogYesRect() { return { 330, 560, 280, 70 }; }
    inline Rect DialogNoRect() { return { 670, 560, 280, 70 }; }
    inline Rect DialogOkRect() { return { 500, 560, 280, 70 }; }

    inline int MaxScroll(int rowCount) { return std::max(0, rowCount - layout::kRowsVisible); }

    enum class HitKind { None, Tab, ResetAll, ResetSection, Close, Language, ScrollUp, ScrollDown, Row, Toggle, Minus, Plus, Bar, Choice, Action, DialogYes, DialogNo, DialogOk };

    struct Hit
    {
        HitKind kind = HitKind::None;
        int index = -1;        // tab or row index (in the page's row list)
        int choice = -1;
        float barT = 0.0f;
    };

    enum class Dialog { None, Confirm, Message };

    // What a click at (x, y) on the page hits. `rows` are the page's rows,
    // `scroll` the first visible one.
    inline Hit HitTest(int x, int y, const std::vector<PageRow>& rows, int scroll, Dialog dialog)
    {
        Hit h{};
        if (dialog == Dialog::Confirm)
        {
            if (DialogYesRect().Contains(x, y)) h.kind = HitKind::DialogYes;
            else if (DialogNoRect().Contains(x, y)) h.kind = HitKind::DialogNo;
            return h;
        }
        if (dialog == Dialog::Message)
        {
            if (DialogOkRect().Contains(x, y)) h.kind = HitKind::DialogOk;
            return h;
        }
        for (int i = 0; i < kSectionCount; ++i)
            if (TabRect(i).Contains(x, y)) { h.kind = HitKind::Tab; h.index = i; return h; }
        if (ResetAllRect().Contains(x, y)) { h.kind = HitKind::ResetAll; return h; }
        if (CloseRect().Contains(x, y)) { h.kind = HitKind::Close; return h; }
        if (ResetSectionRect().Contains(x, y)) { h.kind = HitKind::ResetSection; return h; }
        if (LanguageRect().Contains(x, y)) { h.kind = HitKind::Language; return h; }
        const int total = static_cast<int>(rows.size());
        if (total > layout::kRowsVisible)
        {
            if (ScrollUpRect().Contains(x, y)) { h.kind = HitKind::ScrollUp; return h; }
            if (ScrollDownRect().Contains(x, y)) { h.kind = HitKind::ScrollDown; return h; }
        }
        scroll = std::clamp(scroll, 0, MaxScroll(total));
        for (int v = 0; v < layout::kRowsVisible; ++v)
        {
            const int index = scroll + v;
            if (index >= total)
                break;
            const Rect rr = RowRect(v);
            if (!rr.Contains(x, y))
                continue;
            h.index = index;
            h.kind = HitKind::Row;
            const Row& row = rows[static_cast<size_t>(index)].row;
            switch (row.kind)
            {
            case RowKind::Toggle:
                if (ToggleRect(rr).Contains(x, y)) h.kind = HitKind::Toggle;
                break;
            case RowKind::Slider:
            {
                const Rect bar = BarRect(rr);
                if (MinusRect(rr).Contains(x, y)) h.kind = HitKind::Minus;
                else if (PlusRect(rr).Contains(x, y)) h.kind = HitKind::Plus;
                else if (x >= bar.x && x < bar.x + bar.w && y >= rr.y + rr.h / 2 - 8 && y < rr.y + rr.h)
                {
                    h.kind = HitKind::Bar;
                    h.barT = std::clamp(static_cast<float>(x - bar.x) / static_cast<float>(std::max(1, bar.w - 1)), 0.0f, 1.0f);
                }
                break;
            }
            case RowKind::Choice:
            {
                const int n = static_cast<int>(Choices(row).size());
                for (int c = 0; c < n; ++c)
                    if (ChoiceRect(rr, c, n).Contains(x, y)) { h.kind = HitKind::Choice; h.choice = c; }
                break;
            }
            case RowKind::Action:
                if (ActionRect(rr).Contains(x, y)) h.kind = HitKind::Action;
                break;
            default:
                break;
            }
            return h;
        }
        return h;
    }
}

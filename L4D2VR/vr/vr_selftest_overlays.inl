// Self-test steps for overlays, the overlay laser and the VR keyboard (OpenXR
// port M4). Included by vr.cpp after vr_selftest.inl.
//
//   overlays                     log every mod overlay: visible, width, curvature, transform (any backend)
//   xrdump=<name>                the above, plus (OpenXR) every submitted layer's image and pose:
//                                l4d2vr_selftest_<name>_ov_<key>.bmp, _ov_layers.txt, and the eye (_xr.bmp)
//   xrdumpevery=<ms>:<n>:<name>  OpenXR: n layer dumps <ms> apart from the frame loop (loading screens)
//   xraim=<hand>:<target>:<u>:<v>  OpenXR: turn that controller about its tip so its laser points at
//                                (u, v) of an overlay (menu, hud, config, configbutton, keyboard or a key);
//                                xraim=<hand>:off releases it. The pose reaches the mod like a real one.
//   xrclick=<hand>[:down|:up]    pull (and release) that hand's trigger through the backend's fake input
//   xrtype=<text>                type on the VR keyboard with the right laser and trigger, then Done
//   xrcfgrow=<option>[:<u>]      scroll the config panel to that option and aim the right laser at its row
//   xrmaxlayers=<n>              OpenXR: layer budget override (0 = the runtime's)
//   xrstate                      log the backend's laser/keyboard/layer state
//   uicheck=<what>               paused=0|1 cursor=0|1 ingame=0|1 menu=visible|hidden hud=visible|hidden
//                                config=open|closed cfgvalue=<option>=<value> keyboard=open|closed
//                                laser=<target|off> layers>=<n>
//   uiwait=<seconds>             wait (also in menus)
//   cfg=<option>=<value>         in-memory config override (never written to config.txt), reparsed now
//   killind=<n>                  spawn n kill indicators in front of the head (needs KillIndicatorEnabled)
// Steps from overlays to uiwait (and cmd=, xrinput=) also run in the main menu and while a map loads.

bool L4D2VRConfigOverlay_TestFocusRow(const char* key, float& rowV);
std::string L4D2VRConfigOverlay_TestState(const char* key);
std::string L4D2VRConfigOverlay_CanonicalKey(const char* key);

// cfg=<option>=<value>: applied by ParseConfigFile after config.txt/config2.txt
// (self-test only; nothing is written to disk).
static std::map<std::string, std::string>& SelfTestConfigOverrides()
{
    static std::map<std::string, std::string> overrides;
    return overrides;
}

namespace l4d2vr_selftest_overlay
{
    // The config panel's scroll position set by the last xrcfgrow= (uicheck=cfgscrolled).
    inline int& LastFocusScroll() { static int value = -1; return value; }
    // The calibration preview angles at uicheck=cfgpreviewmark (uicheck=cfgpreviewmoved>=<deg>).
    inline float* PreviewMark() { static float value[3] = { 0.0f, 0.0f, 0.0f }; return value; }

    inline std::vector<std::string> Split(const std::string& text, char sep)
    {
        std::vector<std::string> out;
        size_t start = 0;
        for (;;)
        {
            const size_t at = text.find(sep, start);
            out.push_back(text.substr(start, at == std::string::npos ? std::string::npos : at - start));
            if (at == std::string::npos)
                return out;
            start = at + 1;
        }
    }

    inline std::string Field(const std::string& text, const char* name)
    {
        const std::string key = std::string(name) + "=";
        const size_t at = text.find(key);
        if (at == std::string::npos)
            return std::string();
        size_t start = at + key.size();
        if (start < text.size() && text[start] == '\'')
        {
            const size_t end = text.find('\'', start + 1);
            return text.substr(start + 1, end == std::string::npos ? std::string::npos : end - start - 1);
        }
        const size_t end = text.find(' ', start);
        return text.substr(start, end == std::string::npos ? std::string::npos : end - start);
    }

    inline std::string Lower(std::string s)
    {
        for (char& c : s)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    }
}

bool VR::SelfTestRunsWithoutPlayer(const std::string& step)
{
    static const char* const prefixes[] = { "overlays", "xrdump=", "xrdumpevery=", "xraim=", "xrclick=", "xrtype=",
        "xrcfgrow=", "xrmaxlayers=", "xrstate", "uicheck=", "uiwait=", "uimode=", "cfg=", "cmd=", "xrinput=" };
    for (const char* p : prefixes)
        if (step.rfind(p, 0) == 0)
            return true;
    return false;
}

void VR::SelfTestLogOverlays(const char* label)
{
    l4d2vr_backend::IVrOverlay* ov = l4d2vr_backend::Overlay();
    if (!ov)
        return;
    struct Named { std::string name; vr::VROverlayHandle_t handle; };
    std::vector<Named> list = {
        { "MenuOverlay", m_MainMenuHandle }, { "HUDOverlayTop", m_HUDTopHandle }, { "ScopeOverlay", m_ScopeHandle },
        { "RearMirrorOverlay", m_RearMirrorHandle }, { "LeftWristHUD", m_LeftWristHudHandle }, { "RightAmmoHUD", m_RightAmmoHudHandle },
        { "SpecialInfectedIntentSenseHUD", m_SpecialInfectedIntentSenseHudHandle },
        { "MenuPointerDot", m_MenuPointerDotHandle }, { "MenuPointerBeam", m_MenuPointerBeamHandle } };
    for (size_t i = 0; i < m_HUDBottomHandles.size(); ++i)
        list.push_back({ "HUDOverlayBottom" + std::to_string(i + 1), m_HUDBottomHandles[i] });
    for (size_t i = 0; i < m_KillIndicatorOverlaySlots.size(); ++i)
        list.push_back({ "KillIndicatorOverlay_" + std::to_string(i), m_KillIndicatorOverlaySlots[i].overlayHandle });
    for (const char* key : { "l4d2vr.config.menu_button", "l4d2vr.config.overlay.front", "l4d2vr.config.overlay.back" })
    {
        vr::VROverlayHandle_t h = vr::k_ulOverlayHandleInvalid;
        if (ov->FindOverlay(key, &h) == vr::VROverlayError_None)
            list.push_back({ key, h });
    }
    int visible = 0;
    for (const Named& n : list)
    {
        if (n.handle == vr::k_ulOverlayHandleInvalid)
            continue;
        const bool shown = ov->IsOverlayVisible(n.handle);
        if (!shown && n.name.rfind("KillIndicator", 0) == 0)
            continue;
        visible += shown ? 1 : 0;
        float width = 0.0f, curvature = 0.0f;
        ov->GetOverlayWidthInMeters(n.handle, &width);
        ov->GetOverlayCurvature(n.handle, &curvature);
        vr::ETrackingUniverseOrigin origin = vr::TrackingUniverseStanding;
        vr::HmdMatrix34_t m{};
        const vr::EVROverlayError e = ov->GetOverlayTransformAbsolute(n.handle, &origin, &m);
        Game::logMsg("[VR][SelfTest][Overlay] %s %s visible=%d width=%.3f curvature=%.2f transform%s=[%.3f %.3f %.3f %.3f; %.3f %.3f %.3f %.3f; %.3f %.3f %.3f %.3f]",
            label, n.name.c_str(), shown ? 1 : 0, width, curvature, e == vr::VROverlayError_None ? "" : "(unavailable)",
            m.m[0][0], m.m[0][1], m.m[0][2], m.m[0][3], m.m[1][0], m.m[1][1], m.m[1][2], m.m[1][3], m.m[2][0], m.m[2][1], m.m[2][2], m.m[2][3]);
    }
    Game::logMsg("[VR][SelfTest][Overlay] %s scope state: enabled=%d physical=%d firearm=%d alwaysVisible=%d render=%d texture=%d lensReady=%u lens=%d",
        label, m_ScopeEnabled ? 1 : 0, IsPhysicalScopeMode() ? 1 : 0, m_ScopeWeaponIsFirearm ? 1 : 0, m_ScopeOverlayAlwaysVisible ? 1 : 0,
        ShouldRenderScope() ? 1 : 0, m_ScopeTexture ? 1 : 0, m_ScopeLensOverlayReady.load(std::memory_order_acquire),
        m_VKScopeLens.m_VRTexture.handle ? 1 : 0);
    const vr::HmdMatrix34_t& hmd = m_Poses[vr::k_unTrackedDeviceIndex_Hmd].mDeviceToAbsoluteTracking;
    Game::logMsg("[VR][SelfTest][Overlay] %s %d visible; backend %s; hmd pos (%.3f %.3f %.3f) fwd (%.3f %.3f %.3f)", label, visible,
        m_Backend ? m_Backend->Name() : "?", hmd.m[0][3], hmd.m[1][3], hmd.m[2][3], -hmd.m[0][2], -hmd.m[1][2], -hmd.m[2][2]);
}

bool VR::SelfTestCheckUi(const std::string& predicate, char* detail, size_t detailSize)
{
    namespace so = l4d2vr_selftest_overlay;
    const size_t ge = predicate.find(">=");
    const size_t eq = predicate.find('=');
    const std::string what = predicate.substr(0, ge != std::string::npos ? ge : eq);
    const std::string want = eq == std::string::npos ? std::string() : predicate.substr(ge != std::string::npos ? ge + 2 : eq + 1);
    auto backendState = [&]() {
        std::string state;
        if (m_Backend)
            m_Backend->OverlayTestCommand("state", state);
        return state;
    };
    l4d2vr_backend::IVrOverlay* ov = l4d2vr_backend::Overlay();
    if (what == "paused" || what == "cursor" || what == "ingame")
    {
        int value = -1;
        if (m_Game && m_Game->m_EngineClient && what == "paused") value = m_Game->m_EngineClient->IsPaused() ? 1 : 0;
        if (m_Game && m_Game->m_VguiSurface && what == "cursor") value = m_Game->m_VguiSurface->IsCursorVisible() ? 1 : 0;
        if (m_Game && m_Game->m_EngineClient && what == "ingame") value = m_Game->m_EngineClient->IsInGame() ? 1 : 0;
        std::snprintf(detail, detailSize, "%s=%d (want %s)", what.c_str(), value, want.c_str());
        return value >= 0 && std::to_string(value) == want;
    }
    if (what == "menu" || what == "hud")
    {
        const bool shown = ov && ov->IsOverlayVisible(what == "menu" ? m_MainMenuHandle : m_HUDTopHandle);
        std::snprintf(detail, detailSize, "%s overlay %s (want %s)", what.c_str(), shown ? "visible" : "hidden", want.c_str());
        return (want == "visible") == shown;
    }
    if (what == "config" || what == "cfgvalue" || what == "cfgvalue!" || what == "cfgscroll" || what == "cfgscrolled" ||
        what == "cfgmode" || what == "cfgpreviewangle" || what == "cfgpreviewmark" || what == "cfgpreviewmoved")
    {
        std::string key;
        std::string value = want;
        if (what == "cfgvalue" || what == "cfgvalue!")
        {
            const size_t e2 = want.find('=');
            key = want.substr(0, e2);
            value = e2 == std::string::npos ? std::string() : want.substr(e2 + 1);
        }
        const std::string state = L4D2VRConfigOverlay_TestState(key.c_str());
        std::snprintf(detail, detailSize, "%s", state.c_str());
        if (what == "config")
            return (so::Field(state, "visible") == "1") == (want == "open");
        if (what == "cfgscroll")
            return std::atoi(so::Field(state, "scroll").c_str()) >= std::atoi(want.c_str());
        if (what == "cfgmode")
            return so::Field(state, "mode") == want;
        if (what == "cfgpreviewmark" || what == "cfgpreviewmoved")
        {
            float a[3] = { 0.0f, 0.0f, 0.0f };
            std::sscanf(so::Field(state, "preview").c_str(), "%f,%f,%f", &a[0], &a[1], &a[2]);
            float* mark = so::PreviewMark();
            if (what == "cfgpreviewmark")
            {
                std::copy(a, a + 3, mark);
                return true;
            }
            const float moved = std::fabs(a[0] - mark[0]) + std::fabs(a[1] - mark[1]) + std::fabs(a[2] - mark[2]);
            std::snprintf(detail, detailSize, "preview angles moved %.1f deg (%.1f %.1f %.1f -> %.1f %.1f %.1f); %s", moved,
                mark[0], mark[1], mark[2], a[0], a[1], a[2], state.c_str());
            return moved >= static_cast<float>(std::atof(want.c_str()));
        }
        if (what == "cfgpreviewangle")
        {
            float p = 0.0f, y = 0.0f, r = 0.0f;
            std::sscanf(so::Field(state, "preview").c_str(), "%f,%f,%f", &p, &y, &r);
            return std::fabs(p) + std::fabs(y) + std::fabs(r) >= static_cast<float>(std::atof(want.c_str()));
        }
        if (what == "cfgscrolled")
            return so::LastFocusScroll() >= 0 && std::atoi(so::Field(state, "scroll").c_str()) != so::LastFocusScroll();
        if (what == "cfgvalue!")
            return so::Lower(so::Field(state, "value")) != so::Lower(value);
        return so::Lower(so::Field(state, "value")) == so::Lower(value);
    }
    if (what == "keyboard" || what == "laser" || what == "layers")
    {
        const std::string state = backendState();
        std::snprintf(detail, detailSize, "%s", state.c_str());
        if (what == "keyboard")
            return (so::Field(state, "keyboard") == "1") == (want == "open");
        if (what == "laser")
        {
            const std::string on = so::Lower(so::Field(state, "laser"));
            if (want == "off")
                return on == "-" || on.empty();
            const std::string alias = want == "menu" ? "menuoverlaykey" : want == "hud" ? "hudoverlaytopkey"
                : want == "config" ? "l4d2vr.config.overlay" : want == "configbutton" ? "l4d2vr.config.menu_button"
                : want == "keyboard" ? "__l4d2vr.keyboard" : want;
            return on.rfind(alias, 0) == 0;
        }
        return std::atoi(so::Field(state, "layers").c_str()) >= std::atoi(want.c_str());
    }
    std::snprintf(detail, detailSize, "unknown check %s", predicate.c_str());
    return false;
}

bool VR::RunOverlaySelfTestStep(const std::string& step, float t, bool& handled)
{
    namespace so = l4d2vr_selftest_overlay;
    handled = true;
    char detail[400];
    std::string result;
    auto command = [&](const std::string& spec) {
        result.clear();
        const bool ok = m_Backend && m_Backend->OverlayTestCommand(spec.c_str(), result);
        if (!ok)
            Game::logMsg("[VR][SelfTest] overlay command %s: %s", spec.c_str(), result.c_str());
        return ok;
    };
    auto fakeTrigger = [&](const std::string& hand, float value) {
        std::string error;
        const std::string spec = "/user/hand/" + hand + "/input/trigger/value=" + std::to_string(value);
        if (!m_Backend || !m_Backend->SetFakeInput(spec.c_str(), error))
            Game::logMsg("[VR][SelfTest] fake trigger refused: %s", error.c_str());
    };

    if (step == "overlays")
    {
        SelfTestLogOverlays("table");
        return true;
    }
    if (step == "xrstate")
    {
        command("state");
        Game::logMsg("[VR][SelfTest] overlay state: %s", result.c_str());
        return true;
    }
    if (step.rfind("uiwait=", 0) == 0)
        return t >= static_cast<float>(std::atof(step.c_str() + 7));
    if (step.rfind("uimode=", 0) == 0)
    {
        m_SelfTestUiMode = step.size() > 7 && step[7] == '1';
        Game::logMsg("[VR][SelfTest] ui mode %d (a visible cursor %s menu input)", m_SelfTestUiMode ? 1 : 0,
            m_SelfTestUiMode ? "means" : "does not mean");
        return true;
    }
    if (step.rfind("xrdump=", 0) == 0)
    {
        const std::string name = step.substr(7);
        if (m_SelfTestStage == 0)
        {
            m_SelfTestStage = 1;
            SelfTestLogOverlays(name.c_str());
            m_SelfTestValueA = 0;
            if (m_Backend && m_Backend->Kind() == l4d2vr_backend::RuntimeKind::OpenXR)
            {
                m_Backend->RequestSubmittedEyeDump(("l4d2vr_selftest_" + name + "_xr.bmp").c_str());
                m_SelfTestValueA = command("dump=l4d2vr_selftest_" + name + "_ov") ? 1 : 2;
            }
        }
        if (t < 0.8f)
            return false;
        std::snprintf(detail, sizeof(detail), "%s: %s", name.c_str(),
            m_SelfTestValueA == 0 ? "transforms logged (no layer dump on this runtime)"
            : m_SelfTestValueA == 1 ? "layers dumped (l4d2vr_selftest_*_ov_*.bmp, _ov_layers.txt)" : result.c_str());
        SelfTestReport("overlay dump", m_SelfTestValueA != 2, detail);
        return true;
    }
    if (step.rfind("xrdumpevery=", 0) == 0)
    {
        const std::vector<std::string> p = so::Split(step.substr(12), ':');
        const bool ok = p.size() == 3 && command("dumpevery=" + p[0] + ":" + p[1] + ":l4d2vr_selftest_" + p[2] + "_ov");
        std::snprintf(detail, sizeof(detail), "%s", result.c_str());
        SelfTestReport("periodic overlay dump", ok, detail);
        return true;
    }
    if (step.rfind("xrmaxlayers=", 0) == 0)
    {
        const bool ok = command("maxlayers=" + step.substr(12));
        SelfTestReport("layer budget", ok, result.c_str());
        return true;
    }
    if (step.rfind("xraim=", 0) == 0)
    {
        if (m_SelfTestStage == 0)
        {
            m_SelfTestStage = 1;
            if (!command("aim=" + step.substr(6)))
            {
                SelfTestReport("aim", false, result.c_str());
                return true;
            }
        }
        return t >= 0.4f;
    }
    if (step.rfind("xrclick=", 0) == 0)
    {
        const std::vector<std::string> p = so::Split(step.substr(8), ':');
        const std::string hand = p[0] == "left" ? "left" : "right";
        const std::string mode = p.size() > 1 ? p[1] : "";
        if (m_SelfTestStage == 0)
        {
            m_SelfTestStage = 1;
            l4d2vr_selftest::ForgetAll();
            fakeTrigger(hand, mode == "up" ? 0.0f : 1.0f);
            if (!mode.empty())
                return false;
        }
        if (!mode.empty())
            return t >= 0.35f;
        if (m_SelfTestStage == 1 && t >= 0.3f)
        {
            m_SelfTestStage = 2;
            fakeTrigger(hand, 0.0f);
        }
        return t >= 0.65f;
    }
    if (step.rfind("xrcfgrow=", 0) == 0)
    {
        const std::vector<std::string> p = so::Split(step.substr(9), ':');
        if (m_SelfTestStage == 0)
        {
            m_SelfTestStage = 1;
            float v = 0.0f;
            if (!L4D2VRConfigOverlay_TestFocusRow(p[0].c_str(), v))
            {
                SelfTestReport("config row", false, ("option not shown in the panel: " + p[0]).c_str());
                return true;
            }
            so::LastFocusScroll() = std::atoi(so::Field(L4D2VRConfigOverlay_TestState(""), "scroll").c_str());
            const std::string u = p.size() > 1 ? p[1] : "0.55";
            char spec[128];
            std::snprintf(spec, sizeof(spec), "aim=right:config:%s:%.4f", u.c_str(), v);
            command(spec);
            Game::logMsg("[VR][SelfTest] config row %s at v=%.3f; %s", p[0].c_str(), v, result.c_str());
        }
        return t >= 0.8f;
    }
    if (step.rfind("xrtype=", 0) == 0)
    {
        // Phase per key: aim (0.35 s), press (0.25 s), release (0.3 s).
        const uint64_t nowMs = GetTickCount64();
        if (m_SelfTestStage == 0)
        {
            m_SelfTestStage = 1;
            m_SelfTestKeyQueue.clear();
            l4d2vr_selftest::ForgetAll();
            auto add = [&](const std::string& key) {
                if (!command("keyuv=" + key))
                    return false;
                float u = 0.0f, v = 0.0f;
                int shift = 0;
                if (std::sscanf(result.c_str(), "%f %f %d", &u, &v, &shift) < 2)
                    return false;
                if (shift)
                {
                    std::string sr;
                    m_Backend->OverlayTestCommand("keyuv=Shift", sr);
                    float su = 0.0f, sv = 0.0f;
                    if (std::sscanf(sr.c_str(), "%f %f", &su, &sv) == 2)
                        m_SelfTestKeyQueue.push_back({ su, sv });
                }
                m_SelfTestKeyQueue.push_back({ u, v });
                return true;
            };
            const std::string text = step.substr(7);
            bool ok = true;
            for (char c : text)
                ok = add(std::string(1, c)) && ok;
            ok = add("Done") && ok;
            if (!ok)
            {
                SelfTestReport("keyboard typing", false, "a key is missing from the keyboard layout");
                return true;
            }
            m_SelfTestPhase = 0;
            m_SelfTestCycle = 0;
            m_SelfTestPhaseStartMs = nowMs;
        }
        if (m_SelfTestCycle >= static_cast<int>(m_SelfTestKeyQueue.size()))
        {
            command("aim=right:off");
            std::snprintf(detail, sizeof(detail), "%zu key presses for '%s'", m_SelfTestKeyQueue.size(), step.c_str() + 7);
            SelfTestReport("keyboard typing", true, detail);
            return true;
        }
        const float phaseT = (nowMs - m_SelfTestPhaseStartMs) / 1000.0f;
        const auto& key = m_SelfTestKeyQueue[static_cast<size_t>(m_SelfTestCycle)];
        if (m_SelfTestPhase == 0 && phaseT >= 0.0f)
        {
            char spec[96];
            std::snprintf(spec, sizeof(spec), "aim=right:keyboard:%.4f:%.4f", key.first, key.second);
            command(spec);
            m_SelfTestPhase = 1;
            m_SelfTestPhaseStartMs = nowMs;
        }
        else if (m_SelfTestPhase == 1 && phaseT >= 0.35f)
        {
            fakeTrigger("right", 1.0f);
            m_SelfTestPhase = 2;
            m_SelfTestPhaseStartMs = nowMs;
        }
        else if (m_SelfTestPhase == 2 && phaseT >= 0.25f)
        {
            fakeTrigger("right", 0.0f);
            m_SelfTestPhase = 3;
            m_SelfTestPhaseStartMs = nowMs;
        }
        else if (m_SelfTestPhase == 3 && phaseT >= 0.3f)
        {
            ++m_SelfTestCycle;
            m_SelfTestPhase = 0;
            m_SelfTestPhaseStartMs = nowMs;
        }
        return false;
    }
    if (step.rfind("uicheck=", 0) == 0)
    {
        if (t < 0.2f)
            return false;
        const bool ok = SelfTestCheckUi(step.substr(8), detail, sizeof(detail));
        SelfTestReport(step.c_str(), ok, detail);
        return true;
    }
    if (step.rfind("cfg=", 0) == 0)
    {
        if (m_SelfTestStage == 0)
        {
            m_SelfTestStage = 1;
            const std::string spec = step.substr(4);
            const size_t eq = spec.find('=');
            if (eq == std::string::npos)
            {
                SelfTestReport("config override", false, "expected cfg=<option>=<value>");
                return true;
            }
            // Steps arrive lower-cased: use the option's real spelling when the panel knows it.
            std::string key = spec.substr(0, eq);
            const std::string canonical = L4D2VRConfigOverlay_CanonicalKey(key.c_str());
            if (!canonical.empty())
                key = canonical;
            SelfTestConfigOverrides()[key] = spec.substr(eq + 1);
            ParseConfigFile();
            Game::logMsg("[VR][SelfTest] config override %s (in memory only)", spec.c_str());
        }
        return t >= 0.5f;
    }
    if (step.rfind("killind=", 0) == 0)
    {
        if (m_SelfTestStage == 0)
        {
            m_SelfTestStage = 1;
            const int n = std::clamp(std::atoi(step.c_str() + 8), 1, 16);
            Vector fwd = m_HmdForward;
            fwd.z = 0.0f;
            if (VectorNormalize(fwd) == 0.0f)
                fwd = { 1.0f, 0.0f, 0.0f };
            const Vector up{ 0.0f, 0.0f, 1.0f };
            Vector right = CrossProduct(fwd, up);
            VectorNormalize(right);
            for (int i = 0; i < n; ++i)
            {
                const float col = static_cast<float>(i % 4) - 1.5f;
                const float row = static_cast<float>(i / 4) - 1.5f;
                const Vector pos = m_HmdPosAbs + fwd * 160.0f + right * (col * 28.0f) + up * (-row * 22.0f);
                SpawnKillIndicator((i % 2) == 1, pos);
            }
            std::snprintf(detail, sizeof(detail), "%d spawned (enabled=%d, active=%zu)", n, m_KillIndicatorEnabled ? 1 : 0,
                m_ActiveKillIndicators.size());
            SelfTestReport("kill indicators", m_KillIndicatorEnabled && !m_ActiveKillIndicators.empty(), detail);
        }
        return t >= 0.15f;
    }
    handled = false;
    return false;
}

// Developer self-test: "-l4d2vrselftest features,holster,throwpipe,..." on the
// game's command line. With a simulated headset (Monado "simulated" driver)
// it scripts head and hand poses plus button presses, so each online mechanic
// runs against a real server and reports PASS/FAIL lines in the log. Without
// the launch option nothing here runs.

namespace
{
    int SelfTestActiveSlot(C_BasePlayer* player)
    {
        if (!player)
            return -1;
        C_BaseCombatWeapon* const active = player->GetActiveWeapon();
        if (!active)
            return -1;
        for (int slot = 0; slot < 5; ++slot)
            if (player->Weapon_GetSlot(slot) == active)
                return slot;
        return -1;
    }

    int SelfTestActiveWeaponId(C_BasePlayer* player)
    {
        C_WeaponCSBase* const weapon = player ? static_cast<C_WeaponCSBase*>(player->GetActiveWeapon()) : nullptr;
        return weapon ? static_cast<int>(weapon->GetWeaponID()) : 0;
    }

    l4d2vr_selftest::Vec3 SelfTestVec(float x, float y, float z)
    {
        l4d2vr_selftest::Vec3 v;
        v.x = x; v.y = y; v.z = z;
        return v;
    }
}

void VR::SelfTestReport(const char* name, bool ok, const char* detail)
{
    ok ? ++m_SelfTestPass : ++m_SelfTestFail;
    Game::logMsg("[VR][SelfTest] %s %s%s%s", ok ? "PASS" : "FAIL", name, detail && *detail ? ": " : "", detail ? detail : "");
}

void VR::SelfTestStartPath(const std::vector<l4d2vr_selftest::Key>& position, const std::vector<l4d2vr_selftest::Key>& angles)
{
    m_SelfTestPath = position;
    m_SelfTestAngPath = angles;
    m_SelfTestPathStartMs = GetTickCount64();
    m_SelfTestPathActive = true;
    m_SelfTestRightWorldValid = false;
}

void VR::SelfTestHoldRight(const l4d2vr_selftest::Vec3& local, const QAngle& angles)
{
    m_SelfTestPathActive = false;
    m_SelfTestRightWorldValid = false;
    m_SelfTestRightLocal = local;
    m_SelfTestRightAngLocal = angles;
}

// Reaches the right hand into a holster zone (world target, refreshed every frame).
void VR::SelfTestReachHolster(int zone)
{
    l4d2vr_holster::BodyFrame frame{};
    if (!GetHolsterBodyFrame(frame))
        return;
    m_SelfTestPathActive = false;
    m_SelfTestRightWorld = l4d2vr_holster::ZoneCenterWorld(frame, m_HolsterLayout, static_cast<l4d2vr_holster::Zone>(zone));
    m_SelfTestRightWorldValid = true;
}

void VR::ApplySelfTestPoses()
{
    if (!m_SelfTestEnabled || !m_SelfTestPoseActive)
        return;
    using namespace l4d2vr_selftest;
    const uint64_t nowMs = GetTickCount64();
    const float dt = m_SelfTestPrevPoseMs ? std::clamp((nowMs - m_SelfTestPrevPoseMs) / 1000.0f, 0.001f, 0.1f) : 0.011f;
    m_SelfTestPrevPoseMs = nowMs;

    Vector head = m_HmdPose.TrackedDevicePos;
    const float yaw = m_HmdPose.TrackedDeviceAng.y;
    head.z -= m_SelfTestHeadDrop;
    m_HmdPose.TrackedDevicePos = head;
    const Vec3 h = SelfTestVec(head.x, head.y, head.z);

    Vec3 rightLocal = m_SelfTestRightLocal;
    Vec3 rightAng = SelfTestVec(m_SelfTestRightAngLocal.x, m_SelfTestRightAngLocal.y, m_SelfTestRightAngLocal.z);
    if (m_SelfTestPathActive)
    {
        const float t = (nowMs - m_SelfTestPathStartMs) / 1000.0f;
        rightLocal = Evaluate(m_SelfTestPath, t);
        if (!m_SelfTestAngPath.empty())
            rightAng = Evaluate(m_SelfTestAngPath, t);
    }
    Vec3 right{};
    if (m_SelfTestRightWorldValid)
    {
        const Vector offset = m_SelfTestRightWorld - m_HmdPosAbs;
        right = WorldOffsetToTracking(h, SelfTestVec(offset.x, offset.y, offset.z), m_RotationOffset, m_VRScale);
    }
    else
    {
        right = HeadLocalToTracking(h, yaw, rightLocal);
    }
    const Vec3 left = HeadLocalToTracking(h, yaw, m_SelfTestLeftLocal);

    const Vector rightPos(right.x, right.y, right.z), leftPos(left.x, left.y, left.z);
    if (m_SelfTestPrevValid)
    {
        m_RightControllerPose.TrackedDeviceVel = (rightPos - m_SelfTestPrevRight) * (1.0f / dt);
        m_LeftControllerPose.TrackedDeviceVel = (leftPos - m_SelfTestPrevLeft) * (1.0f / dt);
        m_HmdPose.TrackedDeviceVel = (head - m_SelfTestPrevHmd) * (1.0f / dt);
    }
    m_SelfTestPrevRight = rightPos;
    m_SelfTestPrevLeft = leftPos;
    m_SelfTestPrevHmd = head;
    m_SelfTestPrevValid = true;

    m_RightControllerPose.TrackedDevicePos = rightPos;
    m_RightControllerPose.TrackedDeviceAng = QAngle(rightAng.x, yaw + rightAng.y, rightAng.z);
    m_RightControllerPose.TrackedDeviceAngVel = QAngle(0.0f, 0.0f, 0.0f);
    m_LeftControllerPose.TrackedDevicePos = leftPos;
    m_LeftControllerPose.TrackedDeviceAng = QAngle(m_SelfTestLeftAngLocal.x, yaw + m_SelfTestLeftAngLocal.y, m_SelfTestLeftAngLocal.z);
    m_LeftControllerPose.TrackedDeviceAngVel = QAngle(0.0f, 0.0f, 0.0f);
}

void VR::UpdateSelfTest(C_BasePlayer* localPlayer)
{
    if (!m_SelfTestParsed)
    {
        m_SelfTestParsed = true;
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (argv)
        {
            for (int i = 0; i + 1 < argc; ++i)
            {
                if (_wcsicmp(argv[i], L"-l4d2vrselftest") != 0)
                    continue;
                std::string steps;
                for (const wchar_t* p = argv[i + 1]; *p; ++p)
                    steps.push_back(static_cast<char>(*p & 0x7F));
                m_SelfTestSteps = l4d2vr_selftest::ParseSteps(steps);
                m_SelfTestEnabled = !m_SelfTestSteps.empty();
            }
            LocalFree(argv);
        }
        if (m_SelfTestEnabled)
        {
            l4d2vr_selftest::Get().active.store(true, std::memory_order_release);
            Game::logMsg("[VR][SelfTest] enabled with %zu step(s)", m_SelfTestSteps.size());
        }
    }
    if (!m_SelfTestEnabled)
        return;
    l4d2vr_selftest::BeginFrame();

    const uint64_t nowMs = GetTickCount64();
    unsigned char lifeState = 1;
    const bool alive = localPlayer && m_FirstPersonControlReady.load(std::memory_order_acquire) &&
        VR_TryReadU8(reinterpret_cast<const unsigned char*>(localPlayer), kLifeStateOffset, lifeState) && lifeState == 0;
    if (!alive)
    {
        static uint64_t s_LastControllerDiagMs = 0;
        if (localPlayer && m_System && nowMs - s_LastControllerDiagMs >= 5000u)
        {
            s_LastControllerDiagMs = nowMs;
            for (int role = 0; role < 2; ++role)
            {
                const vr::TrackedDeviceIndex_t index = m_System->GetTrackedDeviceIndexForControllerRole(
                    role == 0 ? vr::TrackedControllerRole_LeftHand : vr::TrackedControllerRole_RightHand);
                if (index >= vr::k_unMaxTrackedDeviceCount)
                {
                    Game::logMsg("[VR][SelfTest] %s controller: no device", role == 0 ? "left" : "right");
                    continue;
                }
                const vr::TrackedDevicePose_t& pose = m_Poses[index];
                Game::logMsg("[VR][SelfTest] %s controller %u: connected=%d valid=%d result=%d pos=(%.2f %.2f %.2f)",
                    role == 0 ? "left" : "right", index, pose.bDeviceIsConnected ? 1 : 0, pose.bPoseIsValid ? 1 : 0,
                    static_cast<int>(pose.eTrackingResult), pose.mDeviceToAbsoluteTracking.m[0][3],
                    pose.mDeviceToAbsoluteTracking.m[1][3], pose.mDeviceToAbsoluteTracking.m[2][3]);
            }
        }
        m_SelfTestReadySinceMs = 0;
        m_SelfTestPoseActive = false;
        l4d2vr_selftest::ReleaseAll();
        return;
    }
    m_SelfTestPoseActive = true;
    if (!m_SelfTestReadySinceMs)
    {
        m_SelfTestReadySinceMs = nowMs;
        Game::logMsg("[VR][SelfTest] local player ready; starting in 3 s");
    }
    if (nowMs - m_SelfTestReadySinceMs < 3000u || m_SelfTestIndex >= m_SelfTestSteps.size())
        return;
    if (!m_SelfTestStepStartMs)
    {
        m_SelfTestStepStartMs = nowMs;
        m_SelfTestStage = 0;
        Game::logMsg("[VR][SelfTest] step %zu/%zu: %s", m_SelfTestIndex + 1, m_SelfTestSteps.size(),
            m_SelfTestSteps[m_SelfTestIndex].c_str());
    }
    {
        static uint64_t s_LastMenuDiagMs = 0;
        if (nowMs - s_LastMenuDiagMs >= 3000u && m_Game && m_Game->m_EngineClient && m_Game->m_VguiSurface)
        {
            s_LastMenuDiagMs = nowMs;
            Game::logMsg("[VR][SelfTest] input state: paused=%d cursorVisible=%d suppress=%d actionFrame=%d",
                m_Game->m_EngineClient->IsPaused() ? 1 : 0, m_Game->m_VguiSurface->IsCursorVisible() ? 1 : 0,
                m_SuppressPlayerInput ? 1 : 0, IsInputActionFrameAvailable() ? 1 : 0);
        }
    }
    const float t = (nowMs - m_SelfTestStepStartMs) / 1000.0f;
    if (RunSelfTestStep(m_SelfTestSteps[m_SelfTestIndex], t, localPlayer) || t > 40.0f)
    {
        if (t > 40.0f)
            SelfTestReport(m_SelfTestSteps[m_SelfTestIndex].c_str(), false, "step timed out");
        l4d2vr_selftest::ReleaseAll();
        SelfTestHoldRight(SelfTestVec(0.35f, 0.2f, -0.3f), QAngle(0.0f, 0.0f, 0.0f));
        m_SelfTestHeadDrop = 0.0f;
        m_SelfTestStepStartMs = 0;
        if (++m_SelfTestIndex >= m_SelfTestSteps.size())
            Game::logMsg("[VR][SelfTest] complete: %d passed, %d failed", m_SelfTestPass, m_SelfTestFail);
    }
}

bool VR::RunSelfTestStep(const std::string& step, float t, C_BasePlayer* player)
{
    using l4d2vr_selftest::Key;
    auto cmd = [&](const char* text) { if (m_Game) m_Game->ClientCmd_Unrestricted(text); };
    auto at = [&](int stage, float when) -> bool
    {
        if (m_SelfTestStage != stage || t < when)
            return false;
        m_SelfTestStage = stage + 1;
        return true;
    };
    auto button = [&](vr::VRActionHandle_t action, bool down) { l4d2vr_selftest::SetButton(action, down); };
    char detail[192];

    if (step == "features")
    {
        const uint32_t mask = m_ServerFeatureMask.load(std::memory_order_acquire);
        std::snprintf(detail, sizeof(detail), "serverAck=%d interaction=%u features=0x%03X magProto=%d shellProto=%d",
            Hooks::s_ServerUnderstandsVR ? 1 : 0, m_ServerPhysicalInteractionVersion.load(std::memory_order_acquire),
            mask, RemoteMagazineProtocolSupported() ? 1 : 0, RemoteShellProtocolSupported() ? 1 : 0);
        // Dedicated plugin: every bit. Modded listen host: its own set.
        SelfTestReport("server capabilities", Hooks::s_ServerUnderstandsVR &&
            (mask == l4d2vr_features::kKnownMask || mask == l4d2vr_features::kListenHostMask), detail);
        return true;
    }

    if (step == "holster")
    {
        static uint32_t s_HoverPulseBaseline = 0u;
        if (at(0, 0.0f)) { cmd("give rifle"); }
        if (at(1, 1.5f)) { cmd("use weapon_rifle"); }
        if (at(2, 3.0f))
        {
            m_SelfTestValueA = SelfTestActiveSlot(player);
            // Holster draw counters at the start of the window in which the
            // pistol (and later the rifle) sits in its holster.
            m_SelfTestValueB = static_cast<int>(m_HolsterDrawSubmittedCount.load(std::memory_order_relaxed));
            m_SelfTestValueC = static_cast<int>(m_HolsterDrawBonesPlacedCount.load(std::memory_order_relaxed));
            s_HoverPulseBaseline = m_HolsterHoverPulseCount;
        }
        if (m_SelfTestStage >= 3 && m_SelfTestStage <= 4) SelfTestReachHolster(1);
        if (at(3, 3.8f))
        {
            std::snprintf(detail, sizeof(detail), "hover pulses %u -> %u", s_HoverPulseBaseline, m_HolsterHoverPulseCount);
            SelfTestReport("holster hover pulse", m_HolsterHoverPulseCount > s_HoverPulseBaseline, detail);
            button(m_ActionWeaponGrip, true);
        }
        if (at(4, 4.3f))
        {
            const int slot = SelfTestActiveSlot(player);
            std::snprintf(detail, sizeof(detail), "slot before=%d after=%d", m_SelfTestValueA + 1, slot + 1);
            SelfTestReport("holster draw from hip", slot == 1, detail);
            SelfTestHoldRight(SelfTestVec(0.4f, 0.2f, -0.3f), QAngle(0.0f, 0.0f, 0.0f));
        }
        if (at(5, 5.0f)) { button(m_ActionWeaponGrip, false); }
        if (m_SelfTestStage >= 6 && m_SelfTestStage <= 7) SelfTestReachHolster(0);
        if (at(6, 5.4f)) {}
        if (at(7, 6.4f)) { button(m_ActionWeaponGrip, true); }
        if (at(8, 6.9f))
        {
            const int slot = SelfTestActiveSlot(player);
            std::snprintf(detail, sizeof(detail), "slot after=%d", slot + 1);
            SelfTestReport("holster draw from back", slot == 0, detail);
            SelfTestHoldRight(SelfTestVec(0.4f, 0.2f, -0.3f), QAngle(0.0f, 0.0f, 0.0f));
        }
        if (at(9, 7.6f))
        {
            button(m_ActionWeaponGrip, false);
            // Holstered items reached the screen: DrawModel accepted them and the
            // weapon SetupBones hook moved their bones to the holster.
            const unsigned submitted =
                m_HolsterDrawSubmittedCount.load(std::memory_order_relaxed) - static_cast<unsigned>(m_SelfTestValueB);
            const unsigned placed =
                m_HolsterDrawBonesPlacedCount.load(std::memory_order_relaxed) - static_cast<unsigned>(m_SelfTestValueC);
            std::snprintf(detail, sizeof(detail),
                "draws=%u bonesPlaced=%u controlReady=%d (see [VR][HolsterDraw] lines)", submitted, placed,
                m_FirstPersonControlReady.load(std::memory_order_acquire) ? 1 : 0);
            SelfTestReport("holster items drawn", submitted > 0u && placed > 0u, detail);
            return true;
        }
        return false;
    }

    if (step == "throwpipe" || step == "throwbile")
    {
        const bool pipe = step == "throwpipe";
        const int expectedId = pipe ? C_WeaponCSBase::PIPE_BOMB : C_WeaponCSBase::VOMITJAR;
        if (at(0, 0.0f)) { cmd(pipe ? "give pipe_bomb" : "give vomitjar"); }
        if (m_SelfTestStage >= 1 && m_SelfTestStage <= 2) SelfTestReachHolster(2);
        if (at(1, 1.0f)) {}
        if (at(2, 2.0f)) { button(m_ActionWeaponGrip, true); }
        if (at(3, 2.4f)) { SelfTestHoldRight(SelfTestVec(0.4f, 0.2f, -0.3f), QAngle(0.0f, 0.0f, 0.0f)); }
        if (at(4, 2.9f))
        {
            button(m_ActionWeaponGrip, false);
            const int id = SelfTestActiveWeaponId(player);
            std::snprintf(detail, sizeof(detail), "active weapon id=%d", id);
            SelfTestReport("holster draw grenade from chest", id == expectedId, detail);
            if (id != expectedId)
                cmd(pipe ? "use weapon_pipe_bomb" : "use weapon_vomitjar");
        }
        if (at(5, 4.0f)) { SelfTestHoldRight(SelfTestVec(-0.12f, 0.25f, 0.08f), QAngle(-20.0f, 0.0f, 0.0f)); }
        if (at(6, 5.0f)) { button(m_ActionPrimaryAttack, true); }
        if (at(7, 5.8f))
        {
            // Overhand throw: about 3.5 m/s forward and up, released at the end.
            SelfTestStartPath(
                { Key{ 0.0f, SelfTestVec(-0.12f, 0.25f, 0.08f) }, Key{ 0.2f, SelfTestVec(0.55f, 0.15f, 0.3f), false } },
                { Key{ 0.0f, SelfTestVec(-20.0f, 0.0f, 0.0f) }, Key{ 0.2f, SelfTestVec(-35.0f, 0.0f, 0.0f), false } });
        }
        if (at(8, 6.0f)) { button(m_ActionPrimaryAttack, false); }
        if (at(9, 8.0f))
        {
            void* const grenade = player->Weapon_GetSlot(2);
            std::snprintf(detail, sizeof(detail), "grenade slot %s (check the server log for the throw velocity)",
                grenade ? "still occupied" : "empty");
            SelfTestReport(pipe ? "hand throw pipe bomb" : "hand throw bile jar", grenade == nullptr, detail);
            return true;
        }
        return false;
    }

    if (step == "swing")
    {
        if (at(0, 0.0f))
        {
            // Campaigns allow different melee weapons; one of these exists everywhere.
            for (const char* melee : { "give machete", "give fireaxe", "give crowbar", "give baseball_bat", "give katana" })
                cmd(melee);
            m_SelfTestSerial = m_MeleeSwingStartSerial.load(std::memory_order_acquire);
        }
        if (at(1, 1.2f)) { cmd("use weapon_melee"); }
        for (int swing = 0; swing < 2; ++swing)
        {
            const float start = 3.0f + swing * 2.0f;
            if (at(2 + swing * 2, start))
                SelfTestHoldRight(SelfTestVec(0.2f, 0.35f, 0.2f), QAngle(-60.0f, 30.0f, 0.0f));
            if (at(3 + swing * 2, start + 0.8f))
            {
                // Diagonal downward slash at about 5 m/s.
                SelfTestStartPath(
                    { Key{ 0.0f, SelfTestVec(0.2f, 0.35f, 0.2f) }, Key{ 0.16f, SelfTestVec(0.55f, -0.15f, -0.35f), false } },
                    { Key{ 0.0f, SelfTestVec(-60.0f, 30.0f, 0.0f) }, Key{ 0.16f, SelfTestVec(50.0f, -30.0f, 0.0f), false } });
            }
        }
        if (at(6, 8.0f))
        {
            const uint32_t swings = m_MeleeSwingStartSerial.load(std::memory_order_acquire) - m_SelfTestSerial;
            std::snprintf(detail, sizeof(detail), "%u swing(s) detected for 2 slashes, weapon id=%d", swings,
                SelfTestActiveWeaponId(player));
            SelfTestReport("motion melee swing", swings >= 2u, detail);
            return true;
        }
        return false;
    }

    if (step == "shove")
    {
        if (at(0, 0.0f)) { cmd("give rifle"); }
        if (at(1, 1.0f)) { cmd("use weapon_rifle"); SelfTestHoldRight(SelfTestVec(0.25f, 0.2f, -0.2f), QAngle(0.0f, 0.0f, 0.0f)); }
        if (at(2, 3.0f))
        {
            m_SelfTestValueA = 0;
            SelfTestStartPath(
                { Key{ 0.0f, SelfTestVec(0.25f, 0.2f, -0.2f) }, Key{ 0.12f, SelfTestVec(0.75f, 0.2f, -0.15f), false } }, {});
        }
        if (m_SelfTestStage == 3 && std::chrono::steady_clock::now() < m_PhysicalShoveHoldUntil)
            m_SelfTestValueA = 1;
        if (at(3, 4.0f))
        {
            std::snprintf(detail, sizeof(detail), "server shove aim %s",
                ServerFeature(l4d2vr_features::ShoveAim) ? "on (impulse 238 sent)" : "off (view-angle fallback)");
            SelfTestReport("gun bash shove", m_SelfTestValueA == 1, detail);
            return true;
        }
        return false;
    }

    if (step == "reload" || step == "shells")
    {
        const bool shells = step == "shells";
        if (at(0, 0.0f)) { cmd(shells ? "give pumpshotgun" : "give rifle"); }
        if (at(1, 1.0f)) { cmd(shells ? "use weapon_pumpshotgun" : "use weapon_rifle"); SelfTestHoldRight(SelfTestVec(0.4f, 0.2f, -0.25f), QAngle(0.0f, 0.0f, 0.0f)); }
        if (at(2, 3.0f)) { button(m_ActionPrimaryAttack, true); }
        if (at(3, shells ? 3.15f : 3.6f)) { button(m_ActionPrimaryAttack, false); }
        if (at(4, 4.5f))
        {
            int clip = -1, reserve = -1;
            m_SelfTestValueA = SelfTestAmmoTransaction(player, shells ? 2 : 0, true, clip, reserve);
            m_SelfTestValueB = clip;
            m_SelfTestValueC = reserve;
            Game::logMsg("[VR][SelfTest] before: weapon=%d clip=%d reserve=%d", SelfTestActiveWeaponId(player), clip, reserve);
            if (m_SelfTestValueA < 0)
            {
                SelfTestReport(shells ? "shell insert transaction" : "magazine eject transaction", false, "request not started");
                return true;
            }
        }
        if (m_SelfTestStage == 5)
        {
            int clip = -1, reserve = -1;
            const int r = SelfTestAmmoTransaction(player, shells ? 2 : 0, false, clip, reserve);
            if (r != 0 || t > 9.0f)
            {
                std::snprintf(detail, sizeof(detail), "clip %d -> %d, reserve %d -> %d", m_SelfTestValueB, clip,
                    m_SelfTestValueC, reserve);
                if (shells)
                {
                    SelfTestReport("shell insert transaction", r == 1 && clip == m_SelfTestValueB + 1, detail);
                    return true;
                }
                SelfTestReport("magazine eject transaction", r == 1 && clip <= 1, detail);
                if (r != 1)
                    return true;
                m_SelfTestValueB = clip;
                m_SelfTestValueC = reserve;
                m_SelfTestStage = 6;
            }
        }
        if (at(6, 0.0f))
        {
            int clip = -1, reserve = -1;
            if (SelfTestAmmoTransaction(player, 1, true, clip, reserve) < 0)
            {
                SelfTestReport("magazine insert transaction", false, "request not started");
                return true;
            }
        }
        if (m_SelfTestStage == 7)
        {
            int clip = -1, reserve = -1;
            const int r = SelfTestAmmoTransaction(player, 1, false, clip, reserve);
            if (r != 0 || t > 15.0f)
            {
                std::snprintf(detail, sizeof(detail), "clip %d -> %d, reserve %d -> %d", m_SelfTestValueB, clip,
                    m_SelfTestValueC, reserve);
                SelfTestReport("magazine insert transaction", r == 1 && clip > m_SelfTestValueB && reserve < m_SelfTestValueC, detail);
                return true;
            }
        }
        return false;
    }

    if (step == "pills" || step == "adrenaline")
    {
        const bool pills = step == "pills";
        if (at(0, 0.0f))
        {
            cmd(pills ? "give pain_pills" : "give adrenaline");
            if (pills)
                cmd("hurtme 40"); // pills are only taken below full health
        }
        if (m_SelfTestStage >= 1 && m_SelfTestStage <= 2) SelfTestReachHolster(4);
        if (at(1, 1.0f)) {}
        if (at(2, 2.0f)) { button(m_ActionWeaponGrip, true); }
        if (at(3, 2.4f))
        {
            m_SelfTestValueA = 0;
            SelfTestHoldRight(SelfTestVec(0.45f, 0.05f, -0.25f), QAngle(0.0f, 0.0f, 0.0f));
        }
        if (at(4, 2.9f))
        {
            button(m_ActionWeaponGrip, false);
            const int id = SelfTestActiveWeaponId(player);
            std::snprintf(detail, sizeof(detail), "active weapon id=%d", id);
            SelfTestReport(pills ? "holster draw pills from chest" : "holster draw adrenaline from chest",
                id == (pills ? C_WeaponCSBase::PAIN_PILLS : C_WeaponCSBase::ADRENALINE), detail);
            if (pills)
                SelfTestStartPath({ Key{ 0.0f, SelfTestVec(0.45f, 0.05f, -0.25f) }, Key{ 0.6f, SelfTestVec(0.07f, 0.0f, -0.07f) } }, {});
        }
        if (at(5, 3.6f) && !pills)
        {
            // Needle pointing back at the chest; the tip (10 cm ahead of the
            // hand) lands on the chest point at about 2.3 m/s.
            SelfTestStartPath({ Key{ 0.0f, SelfTestVec(0.50f, 0.05f, -0.26f) }, Key{ 0.12f, SelfTestVec(0.22f, 0.0f, -0.30f), false } },
                { Key{ 0.0f, SelfTestVec(0.0f, 180.0f, 0.0f) }, Key{ 0.12f, SelfTestVec(0.0f, 180.0f, 0.0f) } });
        }
        if (m_SelfTestStage >= 5 && std::chrono::steady_clock::now() < m_ConsumableUseHoldUntil)
            m_SelfTestValueA = 1;
        if (at(6, 7.0f))
        {
            void* const item = player->Weapon_GetSlot(4);
            std::snprintf(detail, sizeof(detail), "gesture %s, item %s", m_SelfTestValueA ? "fired" : "not fired",
                item ? "still held" : "used");
            SelfTestReport(pills ? "pills to the mouth" : "adrenaline stab", m_SelfTestValueA == 1 && !item, detail);
            return true;
        }
        return false;
    }

    if (step == "crouch")
    {
        // Two physical crouches for a second client to watch.
        const float cycle = std::fmod(t, 6.0f);
        const float depth = cycle < 1.0f ? cycle : (cycle < 4.0f ? 1.0f : (cycle < 5.0f ? 5.0f - cycle : 0.0f));
        m_SelfTestHeadDrop = 0.45f * l4d2vr_selftest::SmoothStep(depth);
        if (t >= 12.0f)
        {
            SelfTestReport("physical crouch script", true, "watch the second client's log for [VR][VRIK] remote crouch");
            return true;
        }
        return false;
    }

    if (step == "wave")
    {
        const float a = t * 3.0f;
        m_SelfTestPathActive = false;
        m_SelfTestRightWorldValid = false;
        m_SelfTestRightLocal = SelfTestVec(0.35f + 0.1f * std::cos(a), 0.3f, 0.1f * std::sin(a));
        m_SelfTestLeftLocal = SelfTestVec(0.3f, -0.35f - 0.1f * std::sin(a), -0.1f + 0.15f * std::cos(a));
        if (t >= 8.0f)
        {
            m_SelfTestLeftLocal = SelfTestVec(0.35f, -0.2f, -0.35f);
            SelfTestReport("arm motion script", true, "watch the second client");
            return true;
        }
        return false;
    }

    if (step == "togglegrip")
    {
        // Toggle grips: squeeze once to take, again to let go.
        static bool s_savedToggle = true;
        auto squeeze = [&](bool down) { button(m_ActionWeaponGrip, down); };
        if (at(0, 0.0f)) { s_savedToggle = m_GripToggleEnabled; m_GripToggleEnabled = true; cmd("give rifle"); button(m_ActionWeaponGrip, false); }
        if (at(1, 1.0f)) cmd("use weapon_pistol");
        if (m_SelfTestStage >= 2 && m_SelfTestStage <= 4) SelfTestReachHolster(0);
        if (at(2, 1.6f)) {}
        if (at(3, 2.4f)) squeeze(true);
        if (at(4, 2.6f)) squeeze(false);
        if (at(5, 3.0f)) SelfTestHoldRight(SelfTestVec(0.30f, 0.25f, -0.15f), QAngle(0.0f, 0.0f, 0.0f));
        if (at(6, 4.5f))
        {
            const int slot = SelfTestActiveSlot(player);
            std::snprintf(detail, sizeof(detail), "active slot %d, toggle %s", slot + 1, m_GripToggle.Held() ? "held" : "released");
            SelfTestReport("toggle grip keeps the drawn item after letting go", slot == 0 && m_GripToggle.Held(), detail);
        }
        if (at(7, 4.8f)) squeeze(true);
        if (at(8, 5.0f)) squeeze(false);
        if (at(9, 6.5f))
        {
            void* const primary = player->Weapon_GetSlot(0);
            std::snprintf(detail, sizeof(detail), "primary slot %s after the second squeeze",
                primary ? "still occupied" : "empty");
            SelfTestReport("toggle grip second squeeze drops the item", primary == nullptr && !m_GripToggle.Held(), detail);
            cmd("give rifle");
        }
        if (at(10, 7.5f)) cmd("use weapon_pistol");
        if (m_SelfTestStage >= 11 && m_SelfTestStage <= 13) SelfTestReachHolster(0);
        if (at(11, 8.0f)) {}
        if (at(12, 8.8f)) squeeze(true);
        if (at(13, 9.0f)) squeeze(false);
        if (at(14, 9.5f)) SelfTestHoldRight(SelfTestVec(0.30f, 0.25f, -0.15f), QAngle(0.0f, 0.0f, 0.0f));
        if (m_SelfTestStage >= 15 && m_SelfTestStage <= 17) SelfTestReachHolster(0);
        if (at(15, 11.2f)) {}
        if (at(16, 12.0f)) squeeze(true);
        if (at(17, 12.2f)) squeeze(false);
        if (at(18, 13.5f))
        {
            void* const primary = player->Weapon_GetSlot(0);
            const int slot = SelfTestActiveSlot(player);
            std::snprintf(detail, sizeof(detail), "primary %s, active slot %d", primary ? "kept" : "dropped", slot + 1);
            SelfTestReport("toggle grip squeeze inside the holster puts it away", primary != nullptr && slot != 0, detail);
            m_GripToggleEnabled = s_savedToggle;
            return true;
        }
        return false;
    }

    if (step == "holsterdrop")
    {
        // Hold-to-grip mode.
        static bool s_savedToggle = true;
        // 1) Draw the rifle from the back with the grip, swing, let go: dropped.
        // Register the grip as released first: a real grip action is always active,
        // and a press that arrives together with activation is (rightly) ignored.
        if (at(0, 0.0f)) { s_savedToggle = m_GripToggleEnabled; m_GripToggleEnabled = false; cmd("give rifle"); button(m_ActionWeaponGrip, false); }
        if (at(1, 1.0f)) cmd("use weapon_pistol");
        if (m_SelfTestStage >= 2 && m_SelfTestStage <= 3) SelfTestReachHolster(0);
        if (at(2, 1.6f)) {}
        if (at(3, 2.4f)) button(m_ActionWeaponGrip, true);
        if (at(4, 2.9f)) SelfTestHoldRight(SelfTestVec(0.30f, 0.25f, -0.15f), QAngle(0.0f, 0.0f, 0.0f));
        if (at(5, 3.6f))
            SelfTestStartPath({ Key{ 0.0f, SelfTestVec(0.30f, 0.25f, -0.15f) }, Key{ 0.2f, SelfTestVec(0.70f, 0.15f, 0.05f), false } }, {});
        if (at(6, 3.8f)) button(m_ActionWeaponGrip, false);
        if (at(7, 5.5f))
        {
            void* const primary = player->Weapon_GetSlot(0);
            std::snprintf(detail, sizeof(detail), "primary slot %s after letting go away from the holster",
                primary ? "still occupied" : "empty");
            SelfTestReport("holster drop on grip release", primary == nullptr, detail);
            cmd("give rifle");
        }
        // 2) Draw it again and let go inside its own holster: kept, put away.
        if (at(8, 6.5f)) cmd("use weapon_pistol");
        if (m_SelfTestStage >= 9 && m_SelfTestStage <= 10) SelfTestReachHolster(0);
        if (at(9, 7.0f)) {}
        if (at(10, 7.8f)) button(m_ActionWeaponGrip, true);
        if (at(11, 8.3f)) SelfTestHoldRight(SelfTestVec(0.30f, 0.25f, -0.15f), QAngle(0.0f, 0.0f, 0.0f));
        if (m_SelfTestStage >= 12 && m_SelfTestStage <= 13) SelfTestReachHolster(0);
        if (at(12, 9.0f)) {}
        if (at(13, 9.8f)) button(m_ActionWeaponGrip, false);
        if (at(14, 11.0f))
        {
            void* const primary = player->Weapon_GetSlot(0);
            const int slot = SelfTestActiveSlot(player);
            std::snprintf(detail, sizeof(detail), "primary %s, active slot %d", primary ? "kept" : "dropped", slot + 1);
            SelfTestReport("letting go inside the holster keeps the item", primary != nullptr && slot != 0, detail);
            m_GripToggleEnabled = s_savedToggle;
            return true;
        }
        return false;
    }

    if (step == "give")
    {
        if (at(0, 0.0f)) cmd("give pain_pills");
        if (at(1, 1.0f)) cmd("use weapon_pain_pills");
        if (m_SelfTestStage == 2 && t >= 2.0f)
        {
            // Nearest living survivor teammate.
            float best = 1e9f;
            Vector chest{};
            const int highest = (std::min)(64, m_Game->m_ClientEntityList->GetHighestEntityIndex());
            for (int i = 1; i <= highest; ++i)
            {
                C_BaseEntity* const entity = m_Game->GetClientEntity(i);
                if (!entity || entity == reinterpret_cast<C_BaseEntity*>(player))
                    continue;
                const unsigned char* const base = reinterpret_cast<const unsigned char*>(entity);
                unsigned char lifeState = 1;
                int team = 0;
                Vector origin{};
                if (!VR_TryReadU8(base, kLifeStateOffset, lifeState) || !VR_TryReadI32(base, kTeamNumOffset, team) ||
                    lifeState != 0 || team != 2 || !VR_TryGetEntityAbsOrigin(entity, origin))
                    continue;
                const float d = (origin - m_HmdPosAbs).Length();
                if (d < best)
                {
                    best = d;
                    chest = origin + Vector(0.0f, 0.0f, 46.0f);
                }
            }
            if (best > 1e8f)
            {
                SelfTestReport("hand pills to a teammate", false, "no teammate found");
                return true;
            }
            m_SelfTestValueA = static_cast<int>(best / (std::max)(1.0f, m_VRScale) * 100.0f);
            m_SelfTestRightWorld = chest;
            m_SelfTestRightWorldValid = true;
            m_SelfTestPathActive = false;
            m_SelfTestStage = 3;
        }
        if (at(3, 4.5f)) SelfTestHoldRight(SelfTestVec(0.35f, 0.2f, -0.3f), QAngle(0.0f, 0.0f, 0.0f));
        if (at(4, 6.0f))
        {
            void* const item = player->Weapon_GetSlot(4);
            std::snprintf(detail, sizeof(detail), "teammate %.2f m away, pills %s", m_SelfTestValueA / 100.0f,
                item ? "still held" : "given");
            SelfTestReport("hand pills to a teammate", item == nullptr, detail);
            return true;
        }
        return false;
    }

    if (step == "wait")
        return t >= 10.0f;

    if (step.rfind("cmd=", 0) == 0)
    {
        Game::logMsg("[VR][SelfTest] console command: %s", step.c_str() + 4);
        cmd(step.c_str() + 4);
        return true;
    }

    if (step == "weapons")
    {
        if (t < 1.0f)
            return false;
        char line[160] = {};
        int length = 0;
        for (int slot = 0; slot < 5; ++slot)
        {
            C_WeaponCSBase* const weapon = static_cast<C_WeaponCSBase*>(player->Weapon_GetSlot(slot));
            length += std::snprintf(line + length, sizeof(line) - length, " slot%d=%d", slot + 1,
                weapon ? static_cast<int>(weapon->GetWeaponID()) : 0);
        }
        Game::logMsg("[VR][SelfTest] weapons:%s active=%d", line, SelfTestActiveWeaponId(player));
        return true;
    }

    if (step == "quit")
    {
        if (at(0, 0.0f)) cmd("disconnect");
        if (at(1, 2.0f)) { cmd("quit"); return true; }
        return false;
    }

    SelfTestReport(step.c_str(), false, "unknown step");
    return true;
}

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

void VR::SelfTestReachRight(const l4d2vr_selftest::Vec3& target, const l4d2vr_selftest::Vec3& angles, float averageSpeed)
{
    std::vector<l4d2vr_selftest::Key> position, rotation;
    l4d2vr_selftest::BuildReach(m_SelfTestRightNow, target, m_SelfTestRightAngNow, angles, averageSpeed,
        m_SelfTestJitterSeed++ * 7919u, position, rotation);
    SelfTestStartPath(position, rotation);
}

void VR::SelfTestReachLeft(const l4d2vr_selftest::Vec3& target, const l4d2vr_selftest::Vec3& angles, float averageSpeed)
{
    l4d2vr_selftest::BuildReach(m_SelfTestLeftNow, target, m_SelfTestLeftAngNow, angles, averageSpeed,
        m_SelfTestJitterSeed++ * 7919u, m_SelfTestLeftPath, m_SelfTestLeftAngPath);
    m_SelfTestLeftWorldValid = false;
    m_SelfTestLeftPathStartMs = GetTickCount64();
    m_SelfTestLeftPathActive = true;
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
    if (m_SelfTestLookActive)
    {
        m_HmdPose.TrackedDeviceAng.x = m_SelfTestLookPitch;
        m_HmdPose.TrackedDeviceAng.z = 0.0f;
    }
    // Hands are placed from the unswayed head, like a body that stays put while
    // the head looks and leans around.
    const Vec3 h = SelfTestVec(head.x, head.y, head.z);
    if (m_SelfTestSwayDeg > 0.0f)
    {
        // Head looks around and leans: yaw +-A, pitch +-A/3, a few cm of sway.
        const float st = (nowMs - m_SelfTestSwayStartMs) / 1000.0f;
        const float tau = 6.2831853f;
        m_HmdPose.TrackedDeviceAng.y += m_SelfTestSwayDeg * std::sin(tau * 0.35f * st);
        m_HmdPose.TrackedDeviceAng.x += (m_SelfTestSwayDeg / 3.0f) * std::sin(tau * 0.5f * st);
        head.x += 0.04f * std::sin(tau * 0.3f * st);
        head.y += 0.05f * std::sin(tau * 0.45f * st + 1.0f);
        head.z += 0.03f * std::sin(tau * 0.6f * st + 2.0f);
    }
    m_HmdPose.TrackedDevicePos = head;
    if (m_SelfTestTurnRate != 0.0f)
    {
        m_RotationOffset += m_SelfTestTurnRate * dt;
        m_RotationOffset -= 360.0f * std::floor(m_RotationOffset / 360.0f);
    }
    // Scene hands (lefthand=/righthand=) in the holster body frame. A step's own
    // world target or scripted path still wins while it runs.
    l4d2vr_holster::BodyFrame sceneFrame{};
    const bool sceneFrameValid = (m_SelfTestSceneRightValid || m_SelfTestSceneLeftValid) && GetHolsterBodyFrame(sceneFrame);
    auto sceneTracking = [&](const Vec3& local)
    {
        const Vector offset = l4d2vr_holster::ToWorld(sceneFrame, Vector(local.x, local.y, local.z)) - m_HmdPosAbs;
        return WorldOffsetToTracking(h, SelfTestVec(offset.x, offset.y, offset.z), m_RotationOffset, m_VRScale);
    };

    Vec3 rightLocal = m_SelfTestRightLocal;
    Vec3 rightAng = SelfTestVec(m_SelfTestRightAngLocal.x, m_SelfTestRightAngLocal.y, m_SelfTestRightAngLocal.z);
    if (m_SelfTestPathActive)
    {
        const float t = (nowMs - m_SelfTestPathStartMs) / 1000.0f;
        rightLocal = Evaluate(m_SelfTestPath, t);
        if (!m_SelfTestAngPath.empty())
            rightAng = Evaluate(m_SelfTestAngPath, t);
    }
    m_SelfTestRightNow = rightLocal;
    m_SelfTestRightAngNow = rightAng;
    Vec3 leftLocal = m_SelfTestLeftLocal;
    Vec3 leftAng = SelfTestVec(m_SelfTestLeftAngLocal.x, m_SelfTestLeftAngLocal.y, m_SelfTestLeftAngLocal.z);
    if (m_SelfTestLeftPathActive)
    {
        const float t = (nowMs - m_SelfTestLeftPathStartMs) / 1000.0f;
        leftLocal = Evaluate(m_SelfTestLeftPath, t);
        if (!m_SelfTestLeftAngPath.empty())
            leftAng = Evaluate(m_SelfTestLeftAngPath, t);
    }
    m_SelfTestLeftNow = leftLocal;
    m_SelfTestLeftAngNow = leftAng;
    Vec3 right{};
    if (m_SelfTestSceneRightValid && sceneFrameValid && !m_SelfTestRightWorldValid && !m_SelfTestPathActive)
    {
        right = sceneTracking(m_SelfTestSceneRight);
    }
    else if (m_SelfTestRightWorldValid)
    {
        const Vector offset = m_SelfTestRightWorld - m_HmdPosAbs;
        right = WorldOffsetToTracking(h, SelfTestVec(offset.x, offset.y, offset.z), m_RotationOffset, m_VRScale);
    }
    else
    {
        right = HeadLocalToTracking(h, yaw, rightLocal);
    }
    Vec3 left{};
    if (m_SelfTestSceneLeftValid && sceneFrameValid && !m_SelfTestLeftWorldValid && !m_SelfTestLeftPathActive)
    {
        left = sceneTracking(m_SelfTestSceneLeft);
    }
    else if (m_SelfTestLeftWorldValid)
    {
        const Vector offset = m_SelfTestLeftWorld - m_HmdPosAbs;
        left = WorldOffsetToTracking(h, SelfTestVec(offset.x, offset.y, offset.z), m_RotationOffset, m_VRScale);
    }
    else
    {
        left = HeadLocalToTracking(h, yaw, leftLocal);
    }

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

    if (m_SelfTestSceneRightAngValid && m_SelfTestSceneRightValid && !m_SelfTestRightWorldValid && !m_SelfTestPathActive)
        rightAng = SelfTestVec(m_SelfTestSceneRightAng.x, m_SelfTestSceneRightAng.y, m_SelfTestSceneRightAng.z);
    if (m_SelfTestSceneLeftAngValid && m_SelfTestSceneLeftValid && !m_SelfTestLeftWorldValid && !m_SelfTestLeftPathActive &&
        !m_SelfTestLeftAngWorldValid)
        leftAng = SelfTestVec(m_SelfTestSceneLeftAng.x, m_SelfTestSceneLeftAng.y, m_SelfTestSceneLeftAng.z);
    m_RightControllerPose.TrackedDevicePos = rightPos;
    m_RightControllerPose.TrackedDeviceAng = QAngle(rightAng.x, yaw + rightAng.y, rightAng.z);
    m_RightControllerPose.TrackedDeviceAngVel = QAngle(0.0f, 0.0f, 0.0f);
    m_LeftControllerPose.TrackedDevicePos = leftPos;
    if (m_SelfTestLeftAngWorldValid)
    {
        // World angles (stick turning included) back to tracking space.
        m_LeftControllerPose.TrackedDeviceAng = QAngle(m_SelfTestLeftAngWorld.x,
            m_SelfTestLeftAngWorld.y - m_RotationOffset, m_SelfTestLeftAngWorld.z);
    }
    else
    {
        m_LeftControllerPose.TrackedDeviceAng = QAngle(leftAng.x, yaw + leftAng.y, leftAng.z);
    }
    m_LeftControllerPose.TrackedDeviceAngVel = QAngle(0.0f, 0.0f, 0.0f);
    // Scripted hands are physical hands (righthand= is the configured gun
    // hand's controller); after an ambidextrous swap the gameplay right pose is
    // the other controller.
    if (m_ItemHandSwapped.load(std::memory_order_acquire))
        std::swap(m_RightControllerPose, m_LeftControllerPose);
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
        m_SelfTestPhase = 0;
        m_SelfTestPhaseStartMs = nowMs;
        m_SelfTestGlideMs = 0;
        m_SelfTestCycle = 0;
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
    // Hand-driven reload steps run several full reloads.
    const float stepLimit = m_SelfTestSteps[m_SelfTestIndex].find("e2e") != std::string::npos ? 100.0f : 40.0f;
    if (RunSelfTestStep(m_SelfTestSteps[m_SelfTestIndex], t, localPlayer) || t > stepLimit)
    {
        if (t > stepLimit)
            SelfTestReport(m_SelfTestSteps[m_SelfTestIndex].c_str(), false, "step timed out");
        l4d2vr_selftest::ReleaseAll();
        SelfTestHoldRight(SelfTestVec(0.35f, 0.2f, -0.3f), QAngle(0.0f, 0.0f, 0.0f));
        m_SelfTestLeftWorldValid = false;
        m_SelfTestLeftPathActive = false;
        m_SelfTestLeftLocal = SelfTestVec(0.35f, -0.2f, -0.35f);
        m_SelfTestLeftAngLocal = QAngle(0.0f, 0.0f, 0.0f);
        m_VmAnimTraceUntilMs.store(0u, std::memory_order_release);
        m_SelfTestLeftAngWorldValid = false;
        m_SelfTestGlideMs = 0;
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

    if (step == "ambie2e" || step == "ambimelee" || step == "ambimedkit" || step == "switchanim" || step == "switchtotal" || step.rfind("lfire=", 0) == 0)
        return RunItemHandSelfTestStep(step, t, player);

    if (step == "features")
    {
        const uint32_t mask = m_ServerFeatureMask.load(std::memory_order_acquire);
        std::snprintf(detail, sizeof(detail), "serverAck=%d interaction=%u features=0x%03X handThrow=%d magProto=%d shellProto=%d",
            Hooks::s_ServerUnderstandsVR ? 1 : 0, m_ServerPhysicalInteractionVersion.load(std::memory_order_acquire),
            mask, HandThrowServerReady() ? 1 : 0, RemoteMagazineProtocolSupported() ? 1 : 0, RemoteShellProtocolSupported() ? 1 : 0);
        // Dedicated plugin: every bit. Modded listen host: its own set.
        // Dedicated plugin: every bit (an older plugin: all but HandThrow).
        // Modded listen host: its own set, with HandThrow when its fields resolved.
        SelfTestReport("server capabilities", Hooks::s_ServerUnderstandsVR &&
            (mask == l4d2vr_features::kKnownMask || mask == l4d2vr_features::kPreHandThrowMask ||
                mask == l4d2vr_features::kListenHostMask ||
                mask == (l4d2vr_features::kListenHostMask | l4d2vr_features::HandThrow)), detail);
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
        if (at(6, 5.0f))
        {
            button(m_ActionPrimaryAttack, true);
            std::vector<std::pair<int, Vector>> now;
            SelfTestListProjectiles(now);
            m_SelfTestValueC = now.empty() ? 0 : now.back().first;   // newest projectile before the throw
            m_SelfTestValueD = 0;
        }
        if (m_SelfTestStage >= 9 && m_SelfTestStage <= 9 && m_SelfTestValueD == 0)
        {
            // Release -> first sight of the new projectile on this client.
            std::vector<std::pair<int, Vector>> now;
            SelfTestListProjectiles(now);
            for (const auto& p : now)
                if (p.first != m_SelfTestValueC)
                {
                    m_SelfTestValueD = static_cast<int>(static_cast<uint32_t>(GetTickCount64()) - m_SelfTestSerialB);
                    Game::logMsg("[VR][SelfTest] INFO %s: trigger release -> projectile seen %d ms, %.1f units from the hand, hand throws %s",
                        step.c_str(), m_SelfTestValueD, (p.second - GetRightControllerViewmodelAbsPos()).Length(),
                        HandThrowServerReady() ? "on" : "off (native throw)");
                    break;
                }
        }
        if (at(7, 5.8f))
        {
            // Overhand throw: about 3.5 m/s forward and up, released at the end.
            SelfTestStartPath(
                { Key{ 0.0f, SelfTestVec(-0.12f, 0.25f, 0.08f) }, Key{ 0.2f, SelfTestVec(0.55f, 0.15f, 0.3f), false } },
                { Key{ 0.0f, SelfTestVec(-20.0f, 0.0f, 0.0f) }, Key{ 0.2f, SelfTestVec(-35.0f, 0.0f, 0.0f), false } });
        }
        if (at(8, 6.0f)) { button(m_ActionPrimaryAttack, false); m_SelfTestSerialB = static_cast<uint32_t>(GetTickCount64()); }
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

    if (step == "shells")
    {
        if (at(0, 0.0f)) { cmd("give pumpshotgun"); }
        if (at(1, 1.0f)) { cmd("use weapon_pumpshotgun"); SelfTestHoldRight(SelfTestVec(0.4f, 0.2f, -0.25f), QAngle(0.0f, 0.0f, 0.0f)); }
        if (at(2, 3.0f)) { button(m_ActionPrimaryAttack, true); }
        if (at(3, 3.15f)) { button(m_ActionPrimaryAttack, false); }
        if (at(4, 4.5f))
        {
            int clip = -1, reserve = -1;
            m_SelfTestValueA = SelfTestAmmoTransaction(player, 2, true, clip, reserve);
            m_SelfTestValueB = clip;
            m_SelfTestValueC = reserve;
            Game::logMsg("[VR][SelfTest] before: weapon=%d clip=%d reserve=%d", SelfTestActiveWeaponId(player), clip, reserve);
            if (m_SelfTestValueA < 0)
            {
                SelfTestReport("shell insert transaction", false, "request not started");
                return true;
            }
        }
        if (m_SelfTestStage == 5)
        {
            int clip = -1, reserve = -1;
            const int r = SelfTestAmmoTransaction(player, 2, false, clip, reserve);
            if (r != 0 || t > 9.0f)
            {
                std::snprintf(detail, sizeof(detail), "clip %d -> %d, reserve %d -> %d", m_SelfTestValueB, clip,
                    m_SelfTestValueC, reserve);
                SelfTestReport("shell insert transaction", r == 1 && clip == m_SelfTestValueB + 1, detail);
                return true;
            }
        }
        return false;
    }

    if (step == "reload")
    {
        // Pavlov-style release through the real button path: the first press
        // ejects once (rounds back to the reserve, one stays chambered), the
        // second press while the magazine is out does nothing. A fresh
        // magazine then refills the clip from the reserve.
        auto ammo = [&](int& clip, int& reserve) { SelfTestAmmoTransaction(player, 3, false, clip, reserve); };
        if (at(0, 0.0f)) { cmd("give rifle"); button(m_ActionMagazineRelease, false); }
        if (at(1, 1.0f)) { cmd("use weapon_rifle"); SelfTestHoldRight(SelfTestVec(0.4f, 0.2f, -0.25f), QAngle(0.0f, 0.0f, 0.0f)); }
        if (at(2, 3.0f)) { button(m_ActionPrimaryAttack, true); }
        if (at(3, 3.6f)) { button(m_ActionPrimaryAttack, false); }
        if (at(4, 4.5f))
        {
            ammo(m_SelfTestValueB, m_SelfTestValueC);
            m_SelfTestSerial = m_MagazineEjectSerial.load(std::memory_order_acquire);
            m_SelfTestSerialB = m_DroppedMagazineSpawnSerial.load(std::memory_order_acquire);
            Game::logMsg("[VR][SelfTest] before: weapon=%d clip=%d reserve=%d magProto=%d", SelfTestActiveWeaponId(player),
                m_SelfTestValueB, m_SelfTestValueC, RemoteMagazineProtocolSupported() ? 1 : 0);
            button(m_ActionMagazineRelease, true);
        }
        if (at(5, 4.7f)) { button(m_ActionMagazineRelease, false); }
        if (at(6, 5.5f)) { button(m_ActionMagazineRelease, true); }
        if (at(7, 5.7f)) { button(m_ActionMagazineRelease, false); }
        if (at(8, 7.0f))
        {
            int clip = -1, reserve = -1;
            ammo(clip, reserve);
            const uint32_t ejects = m_MagazineEjectSerial.load(std::memory_order_acquire) - m_SelfTestSerial;
            const uint32_t drops = m_DroppedMagazineSpawnSerial.load(std::memory_order_acquire) - m_SelfTestSerialB;
            std::snprintf(detail, sizeof(detail), "%u ejection(s), %u dropped magazine(s) for 2 presses; clip %d -> %d",
                ejects, drops, m_SelfTestValueB, clip);
            SelfTestReport("magazine release ejects once", ejects == 1u && drops <= 1u &&
                (drops == 1u || !m_MagazineDropEnabled), detail);
            if (ejects == 0u)
            {
                // No magazine box or session: check the ledger directly instead.
                Game::logMsg("[VR][SelfTest] release button did not eject; running the eject transaction directly");
                if (SelfTestAmmoTransaction(player, 0, true, clip, reserve) < 0)
                {
                    SelfTestReport("magazine eject transaction", false, "request not started");
                    return true;
                }
                m_SelfTestStage = 9;
            }
            else
            {
                // Plain servers keep the stock reload, which never loses rounds.
                if (!RemoteMagazineProtocolSupported())
                    return true;
                const int expectedClip = std::min(1, m_SelfTestValueB);
                const int expectedReserve = m_SelfTestValueC + m_SelfTestValueB - expectedClip;
                std::snprintf(detail, sizeof(detail), "clip %d -> %d, reserve %d -> %d (expected %d, %d)",
                    m_SelfTestValueB, clip, m_SelfTestValueC, reserve, expectedClip, expectedReserve);
                SelfTestReport("magazine eject returns rounds to reserve",
                    clip == expectedClip && reserve == expectedReserve, detail);
                m_SelfTestValueD = m_SelfTestValueB + m_SelfTestValueC;
                m_SelfTestValueB = clip;
                m_SelfTestValueC = reserve;
                m_SelfTestStage = 10;
            }
        }
        if (m_SelfTestStage == 9)
        {
            int clip = -1, reserve = -1;
            const int r = SelfTestAmmoTransaction(player, 0, false, clip, reserve);
            if (r != 0 || t > 11.0f)
            {
                const int expectedClip = std::min(1, m_SelfTestValueB);
                std::snprintf(detail, sizeof(detail), "clip %d -> %d, reserve %d -> %d", m_SelfTestValueB, clip,
                    m_SelfTestValueC, reserve);
                SelfTestReport("magazine eject returns rounds to reserve", r == 1 && clip == expectedClip &&
                    reserve == m_SelfTestValueC + m_SelfTestValueB - expectedClip, detail);
                if (r != 1)
                    return true;
                m_SelfTestValueD = m_SelfTestValueB + m_SelfTestValueC;
                m_SelfTestValueB = clip;
                m_SelfTestValueC = reserve;
                m_SelfTestStage = 10;
            }
        }
        if (at(10, 0.0f))
        {
            int clip = -1, reserve = -1;
            if (SelfTestAmmoTransaction(player, 1, true, clip, reserve) < 0)
            {
                SelfTestReport("magazine insert transaction", false, "request not started");
                return true;
            }
        }
        if (m_SelfTestStage == 11)
        {
            int clip = -1, reserve = -1;
            const int r = SelfTestAmmoTransaction(player, 1, false, clip, reserve);
            if (r != 0 || t > 17.0f)
            {
                // The rifle holds 50; the eject and insert together conserve ammo.
                std::snprintf(detail, sizeof(detail), "clip %d -> %d, reserve %d -> %d, total %d -> %d",
                    m_SelfTestValueB, clip, m_SelfTestValueC, reserve, m_SelfTestValueD, clip + reserve);
                SelfTestReport("magazine insert transaction", r == 1 && clip > m_SelfTestValueB &&
                    reserve < m_SelfTestValueC && (clip == 50 || reserve == 0) &&
                    clip + reserve == m_SelfTestValueD, detail);
                return true;
            }
        }
        return false;
    }

    if (step == "slap")
    {
        // Pavlov magazine slap on the AK-47. First the detection against the live
        // magazine box (or the AK-47 profile when none is published) with scripted
        // strikes; then, when the box and the pouch exist, the real path: take the
        // spare with the old magazine seated, strike it out along its outward
        // axis, and expect one ejection with the spare still in the hand.
        using l4d2vr_mag_slap::Vec3;
        auto ammo = [&](int& clip, int& reserve) { SelfTestAmmoTransaction(player, 3, false, clip, reserve); };
        auto slapBox = [](const MagazineInteractionBoxSnapshot& box)
        {
            l4d2vr_mag_slap::Box out{};
            out.origin = { box.origin.x, box.origin.y, box.origin.z };
            out.axisX = { box.axisX.x, box.axisX.y, box.axisX.z };
            out.axisY = { box.axisY.x, box.axisY.y, box.axisY.z };
            out.axisZ = { box.axisZ.x, box.axisZ.y, box.axisZ.z };
            out.mins = { box.mins.x, box.mins.y, box.mins.z };
            out.maxs = { box.maxs.x, box.maxs.y, box.maxs.z };
            return out;
        };
        auto toVector = [](const Vec3& v) { return Vector(v.x, v.y, v.z); };
        // A direction across the magazine (its thin axis, square to the outward axis).
        auto sideOf = [](const l4d2vr_mag_slap::Box& box, const Vec3& outward)
        {
            Vec3 side{};
            if (!l4d2vr_mag_slap::Normalize(l4d2vr_mag_slap::Sub(box.axisX,
                    l4d2vr_mag_slap::Scale(outward, l4d2vr_mag_slap::Dot(box.axisX, outward))), side))
                side = { 1.0f, 0.0f, 0.0f };
            return side;
        };
        const vr::VRActionHandle_t slapHand = m_MagazineInteractionTriggerInteractions
            ? m_ActionSecondaryAttack : m_ActionOffHandGrip;
        const Vector insertionAxis = m_MagazineInteractionMagazineInsertionAxisLocal;
        const float scale = (std::max)(0.001f, m_VRScale);
        if (at(0, 0.0f)) { cmd("give rifle_ak47"); button(slapHand, false); }
        if (at(1, 1.0f)) { cmd("use weapon_rifle_ak47"); SelfTestHoldRight(SelfTestVec(0.4f, 0.2f, -0.25f), QAngle(0.0f, 0.0f, 0.0f)); }
        if (at(2, 3.5f))
        {
            const int weaponId = SelfTestActiveWeaponId(player);
            l4d2vr_reload_tuning::Slap tuning{};
            const bool slapGun = l4d2vr_reload_tuning::SlapForWeapon(weaponId, tuning);
            l4d2vr_mag_slap::Params params{};
            if (slapGun)
            {
                params.speedMetersPerSecond = tuning.speedMetersPerSecond;
                params.forwardDot = tuning.forwardDot;
                params.reinsertSeconds = tuning.reinsertSeconds;
            }
            MagazineInteractionBoxSnapshot box{};
            const bool liveBox = GetMagazineInteractionBox(box) &&
                std::chrono::duration<float>(std::chrono::steady_clock::now() - box.publishedAt).count() < 1.0f &&
                box.modelName.find("ak47") != std::string::npos;
            l4d2vr_mag_slap::Box seated{};
            if (liveBox)
            {
                seated = slapBox(box);
            }
            else
            {
                // AK-47 clip profile under a level gun: clip -Y down, +Z forward.
                seated.axisX = { 0.0f, 1.0f, 0.0f };
                seated.axisY = { 0.0f, 0.0f, 1.0f };
                seated.axisZ = { 1.0f, 0.0f, 0.0f };
                seated.mins = { -0.65f, -9.53f, -0.69f };
                seated.maxs = { 0.63f, 1.34f, 7.11f };
            }
            Vec3 outward{};
            const bool axisOk = l4d2vr_mag_slap::OutwardAxis(seated, { insertionAxis.x, insertionAxis.y, insertionAxis.z }, outward);
            if (liveBox)
            {
                // The self-test holds the gun level, so out of the well is down.
                std::snprintf(detail, sizeof(detail), "outward=(%.2f %.2f %.2f) model=%s", outward.x, outward.y, outward.z,
                    box.modelName.c_str());
                SelfTestReport("magazine slap axis points out of the well", axisOk && outward.z < -0.5f, detail);
            }
            const Vec3 side = sideOf(seated, outward);
            const float touchingGap = l4d2vr_mag_slap::Gap(seated,
                l4d2vr_mag_slap::Translated(seated, l4d2vr_mag_slap::Scale(outward, 0.02f * scale))) / scale;
            const float apartGap = l4d2vr_mag_slap::Gap(seated,
                l4d2vr_mag_slap::Translated(seated, l4d2vr_mag_slap::Scale(side, 0.20f * scale))) / scale;
            auto strike = [&](const Vec3& direction, float speed, float gap)
            {
                return l4d2vr_mag_slap::Evaluate(params, l4d2vr_mag_slap::Scale(direction, speed), outward, gap).hit;
            };
            const bool detection = slapGun && axisOk &&
                strike(outward, params.speedMetersPerSecond + 0.2f, touchingGap) &&
                !strike(outward, params.speedMetersPerSecond * 0.6f, touchingGap) &&
                !strike(side, 1.5f, touchingGap) &&
                !strike(l4d2vr_mag_slap::Scale(outward, -1.0f), 1.5f, touchingGap) &&
                !strike(outward, 1.5f, apartGap);
            std::snprintf(detail, sizeof(detail), "weapon=%d slap>=%.2f m/s dot>=%.2f gap touching=%.3f m apart=%.3f m (%s box)",
                weaponId, params.speedMetersPerSecond, params.forwardDot, touchingGap, apartGap,
                liveBox ? "live" : "AK-47 profile");
            SelfTestReport("magazine slap detection (scripted strikes)", detection, detail);

            int clip = -1, reserve = -1;
            ammo(clip, reserve);
            m_SelfTestValueB = clip;
            m_SelfTestValueC = reserve;
            Vector pouch{}, held{};
            bool heldValid = false;
            const char* skip = !slapGun ? "the AK-47 is not the active weapon" :
                !liveBox ? "no live magazine box" :
                !m_MagazineSlapEnabled ? "MagazineSlapEnabled=false" :
                m_MagazineInteractionState != MagazineInteractionManualState::Idle ? "a reload is already in progress" :
                reserve <= 0 ? "no reserve ammo" :
                !GetMagazineSlapSelfTestPoints(pouch, held, heldValid) ? "no pouch position" : nullptr;
            if (skip)
            {
                Game::logMsg("[VR][SelfTest] slap: real path skipped (%s)", skip);
                return true;
            }
            m_SelfTestSerial = m_MagazineEjectSerial.load(std::memory_order_acquire);
            m_SelfTestSerialB = m_DroppedMagazineSpawnSerial.load(std::memory_order_acquire);
            m_SelfTestValueA = static_cast<int>(m_MagazineSlapSerial.load(std::memory_order_acquire));
            Game::logMsg("[VR][SelfTest] before: weapon=%d clip=%d reserve=%d magProto=%d", weaponId, clip, reserve,
                RemoteMagazineProtocolSupported() ? 1 : 0);
            m_SelfTestLeftWorld = pouch;
            m_SelfTestLeftWorldValid = true;
        }
        if (at(3, 4.3f)) { button(slapHand, true); }
        if (at(4, 4.9f))
        {
            const bool holding = m_MagazineInteractionState == MagazineInteractionManualState::HoldingFreshMagazineForSlap;
            std::snprintf(detail, sizeof(detail), "state=%d (see [VR][MagazineSlap] lines)",
                static_cast<int>(m_MagazineInteractionState));
            SelfTestReport("slap: spare taken with the old magazine seated", holding, detail);
            Vector pouch{}, held{};
            bool heldValid = false;
            MagazineInteractionBoxSnapshot box{};
            if (!holding || !GetMagazineSlapSelfTestPoints(pouch, held, heldValid) || !heldValid || !GetMagazineInteractionBox(box))
            {
                button(slapHand, false);
                return true;
            }
            // Put the held magazine on the seated one, then strike 30 cm before
            // to 4 cm past it along the outward axis. Park it to the side first so
            // that moving into the start point is a sideways move (no slap).
            const l4d2vr_mag_slap::Box seated = slapBox(box);
            Vec3 outward{};
            l4d2vr_mag_slap::OutwardAxis(seated, { insertionAxis.x, insertionAxis.y, insertionAxis.z }, outward);
            const Vector contact = m_LeftControllerPosAbs + (toVector(l4d2vr_mag_slap::Center(seated)) - held);
            // 35 cm back: clear of the 25 cm magazine, so the strike makes the touch.
            m_SelfTestSlapStart = contact - toVector(outward) * (0.35f * scale);
            m_SelfTestSlapEnd = contact + toVector(outward) * (0.04f * scale);
            m_SelfTestLeftWorld = m_SelfTestSlapStart + toVector(sideOf(seated, outward)) * (0.30f * scale);
        }
        if (at(5, 5.6f))
        {
            m_SelfTestSlapPark = m_SelfTestLeftWorld - m_SelfTestSlapStart;
            m_SelfTestLeftWorld = m_SelfTestSlapStart;
        }
        if (at(6, 6.4f)) {}
        if (m_SelfTestStage == 7)
        {
            // 39 cm in 0.25 s: 1.6 m/s relative to the still gun hand.
            const float u = std::clamp((t - 6.4f) / 0.25f, 0.0f, 1.0f);
            m_SelfTestLeftWorld = m_SelfTestSlapStart + (m_SelfTestSlapEnd - m_SelfTestSlapStart) * u;
        }
        if (at(7, 6.67f))
        {
            // Sideways off the gun before SlapReinsertTime, so the spare stays in the hand.
            m_SelfTestLeftWorld = m_SelfTestSlapEnd + m_SelfTestSlapPark;
        }
        if (at(8, 7.8f))
        {
            const uint32_t ejects = m_MagazineEjectSerial.load(std::memory_order_acquire) - m_SelfTestSerial;
            const uint32_t drops = m_DroppedMagazineSpawnSerial.load(std::memory_order_acquire) - m_SelfTestSerialB;
            const uint32_t slaps = m_MagazineSlapSerial.load(std::memory_order_acquire) - static_cast<uint32_t>(m_SelfTestValueA);
            const bool keptSpare = m_MagazineInteractionState == MagazineInteractionManualState::HoldingFreshMagazine;
            int clip = -1, reserve = -1;
            ammo(clip, reserve);
            std::snprintf(detail, sizeof(detail), "%u slap(s), %u ejection(s), %u dropped magazine(s), spare %s; clip %d -> %d",
                slaps, ejects, drops, keptSpare ? "in hand" : "not in hand", m_SelfTestValueB, clip);
            SelfTestReport("magazine slap ejects once", slaps == 1u && ejects == 1u && drops <= 1u &&
                (drops == 1u || !m_MagazineDropEnabled) && keptSpare, detail);
            if (ejects == 1u && RemoteMagazineProtocolSupported())
            {
                const int expectedClip = std::min(1, m_SelfTestValueB);
                const int expectedReserve = m_SelfTestValueC + m_SelfTestValueB - expectedClip;
                std::snprintf(detail, sizeof(detail), "clip %d -> %d, reserve %d -> %d (expected %d, %d)",
                    m_SelfTestValueB, clip, m_SelfTestValueC, reserve, expectedClip, expectedReserve);
                SelfTestReport("magazine slap returns rounds to reserve",
                    clip == expectedClip && reserve == expectedReserve, detail);
            }
        }
        if (at(9, 8.4f)) { button(slapHand, false); }
        if (at(10, 9.2f))
        {
            std::snprintf(detail, sizeof(detail), "state=%d", static_cast<int>(m_MagazineInteractionState));
            SelfTestReport("letting go after a slap puts the spare back",
                m_MagazineInteractionState == MagazineInteractionManualState::WaitingForFreshMagazine, detail);
            return true;
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

    if (step == "dropreturn")
    {
        // Toggle grips: draw the pump shotgun from the back, hold it close to
        // the body and squeeze to let it go. It lands at the feet, where L4D2
        // may pick it straight back up. If it comes back it is in the hand
        // again and the grip must own it: the next squeeze lets it go.
        static bool s_savedToggle = true;
        static uint32_t s_reclaims = 0u;
        static float s_backAt = -1.0f;
        auto squeeze = [&](bool down) { button(m_ActionWeaponGrip, down); };
        if (at(0, 0.0f))
        {
            s_savedToggle = m_GripToggleEnabled; m_GripToggleEnabled = true; s_backAt = -1.0f;
            cmd("give pumpshotgun"); button(m_ActionWeaponGrip, false);
        }
        if (at(1, 1.0f)) cmd("use weapon_pistol");
        if (m_SelfTestStage >= 2 && m_SelfTestStage <= 4) SelfTestReachHolster(0);
        if (at(2, 1.6f)) {}
        if (at(3, 2.4f)) squeeze(true);
        if (at(4, 2.6f)) squeeze(false);
        if (at(5, 3.0f)) SelfTestHoldRight(SelfTestVec(0.22f, 0.12f, -0.38f), QAngle(0.0f, 0.0f, 0.0f));
        if (at(6, 4.5f))
        {
            const int slot = SelfTestActiveSlot(player);
            std::snprintf(detail, sizeof(detail), "active slot %d, toggle %s", slot + 1, m_GripToggle.Held() ? "held" : "released");
            SelfTestReport("drop return: the grip holds the drawn shotgun", slot == 0 && m_GripToggle.Held(), detail);
            s_reclaims = m_GripDropReclaims.load(std::memory_order_acquire);
        }
        if (at(7, 4.8f)) squeeze(true);
        if (at(8, 5.0f)) squeeze(false);
        if (at(9, 5.4f))
        {
            void* const primary = player->Weapon_GetSlot(0);
            std::snprintf(detail, sizeof(detail), "primary slot %s after the squeeze", primary ? "still occupied" : "empty");
            SelfTestReport("drop return: the squeeze drops the shotgun", primary == nullptr, detail);
        }
        if (m_SelfTestStage == 10 && s_backAt < 0.0f && player->Weapon_GetSlot(0) != nullptr)
            s_backAt = t;
        if (at(10, 9.5f))
        {
            const bool back = player->Weapon_GetSlot(0) != nullptr;
            const uint32_t reclaims = m_GripDropReclaims.load(std::memory_order_acquire) - s_reclaims;
            std::snprintf(detail, sizeof(detail), "%s; the grip took it back %u time(s), active slot %d",
                back ? "the host put the shotgun back in the hand" : "the shotgun stayed on the floor",
                reclaims, SelfTestActiveSlot(player) + 1);
            if (back)
                Game::logMsg("[VR][SelfTest] drop return: back %.1f s after the drop", s_backAt - 5.0f);
            SelfTestReport("drop return: a returned shotgun is owned by the grip", !back || (reclaims >= 1u && SelfTestActiveSlot(player) == 0), detail);
            if (back) SelfTestRequestShot("dropreturn_back", 1);
        }
        if (at(11, 10.0f)) squeeze(true);
        if (at(12, 10.2f)) squeeze(false);
        if (at(13, 10.6f))
        {
            void* const primary = player->Weapon_GetSlot(0);
            std::snprintf(detail, sizeof(detail), "primary slot %s after squeezing again", primary ? "still occupied" : "empty");
            if (s_backAt >= 0.0f)
                SelfTestReport("drop return: squeezing again lets the returned shotgun go", primary == nullptr, detail);
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

    if (step.rfind("handthrow=", 0) == 0)
        return RunHandThrowStep(step, t, player);
    if (step == "flashlight")
    {
        // The flashlight action (right stick click in every default binding)
        // toggles the light on and off again; the server's EF_DIMLIGHT shows it.
        auto lightOn = [&](bool& on) { return QueryFlashlightState(player, on); };
        if (at(0, 0.5f))
        {
            bool on = false;
            m_SelfTestValueA = lightOn(on) ? (on ? 1 : 0) : -1;
            button(m_ActionFlashlight, true);
        }
        if (at(1, 0.65f)) button(m_ActionFlashlight, false);
        if (at(2, 2.0f))
        {
            bool on = false;
            const bool known = lightOn(on);
            std::snprintf(detail, sizeof(detail), "before %d, after one press %d", m_SelfTestValueA, known ? (on ? 1 : 0) : -1);
            SelfTestReport("flashlight action toggles the light", m_SelfTestValueA >= 0 && known &&
                (on ? 1 : 0) != m_SelfTestValueA, detail);
            m_SelfTestValueB = known ? (on ? 1 : 0) : -1;
            if (m_SelfTestValueB == 1) SelfTestRequestShot("flashlight_on", 1);
            button(m_ActionFlashlight, true);
        }
        if (at(3, 2.15f)) button(m_ActionFlashlight, false);
        if (at(4, 3.5f))
        {
            bool on = false;
            const bool known = lightOn(on);
            std::snprintf(detail, sizeof(detail), "before %d, after the second press %d", m_SelfTestValueB, known ? (on ? 1 : 0) : -1);
            SelfTestReport("flashlight action toggles the light back", m_SelfTestValueB >= 0 && known &&
                (on ? 1 : 0) == m_SelfTestValueA, detail);
            return true;
        }
        return false;
    }

    if (step.find("e2e") != std::string::npos)
        return RunReloadE2EStep(step, t, player);

    if (step == "wait")
        return t >= 10.0f;

    // ---- Motion scenes (hold until changed) -----------------------------------
    // walk=<forward>[,<right>] stick deflection -1..1 or walk=off; hold=<s>
    // waits; turnrate=<deg/s> smooth-turns; turn=<deg> snap-turns; sway=<deg>
    // moves the head (look around + lean), sway=0 stops.
    if (step.rfind("walk=", 0) == 0)
    {
        if (at(0, 0.0f))
        {
            const std::string value = step.substr(5);
            m_SelfTestWalkActive = value != "off";
            m_SelfTestWalkY = 0.0f;
            m_SelfTestWalkX = 0.0f;
            if (m_SelfTestWalkActive)
            {
                m_SelfTestWalkY = std::clamp(static_cast<float>(std::atof(value.c_str())), -1.0f, 1.0f);
                const size_t comma = value.find(',');
                if (comma != std::string::npos)
                    m_SelfTestWalkX = std::clamp(static_cast<float>(std::atof(value.c_str() + comma + 1)), -1.0f, 1.0f);
            }
            Game::logMsg("[VR][SelfTest] walk %s", value.c_str());
        }
        return true;
    }
    if (step.rfind("hold=", 0) == 0)
        return t >= std::clamp(static_cast<float>(std::atof(step.c_str() + 5)), 0.0f, 30.0f);
    if (step.rfind("turnrate=", 0) == 0)
    {
        m_SelfTestTurnRate = std::clamp(static_cast<float>(std::atof(step.c_str() + 9)), -360.0f, 360.0f);
        Game::logMsg("[VR][SelfTest] turn rate %.0f deg/s", m_SelfTestTurnRate);
        return true;
    }
    // turnstick=<x> holds the right stick at x (-1..1, positive turns right)
    // through the game's own turn input (smooth or snap, TurnSpeed); "off"
    // releases it. snapturn=on|off switches snap turning.
    if (step.rfind("turnstick=", 0) == 0)
    {
        const std::string value = step.substr(10);
        m_SelfTestTurnStickActive = value != "off";
        m_SelfTestTurnStickX = m_SelfTestTurnStickActive ? std::clamp(static_cast<float>(std::atof(value.c_str())), -1.0f, 1.0f) : 0.0f;
        Game::logMsg("[VR][SelfTest] turn stick %s", value.c_str());
        return true;
    }
    if (step.rfind("snapturn=", 0) == 0)
    {
        m_SnapTurning = step.substr(9) == "on";
        Game::logMsg("[VR][SelfTest] snap turning %s", m_SnapTurning ? "on" : "off");
        return true;
    }
    if (step.rfind("turn=", 0) == 0)
    {
        m_RotationOffset += static_cast<float>(std::atof(step.c_str() + 5));
        m_RotationOffset -= 360.0f * std::floor(m_RotationOffset / 360.0f);
        return true;
    }
    if (step.rfind("sway=", 0) == 0)
    {
        m_SelfTestSwayDeg = std::clamp(static_cast<float>(std::atof(step.c_str() + 5)), 0.0f, 80.0f);
        m_SelfTestSwayStartMs = GetTickCount64();
        Game::logMsg("[VR][SelfTest] head sway %.0f deg", m_SelfTestSwayDeg);
        return true;
    }
    // track=start begins measuring holster item motion relative to the eye;
    // track=report:<cm> passes when every item stayed within <cm> (each eye).
    if (step == "track=start")
    {
        std::lock_guard<std::mutex> lock(m_SelfTestHolsterTrackMutex);
        m_SelfTestHolsterTrack = {};
        m_SelfTestHolsterTrack.active = true;
        return true;
    }
    if (step.rfind("track=report", 0) == 0)
    {
        const float limitCm = step.size() > 13 ? static_cast<float>(std::atof(step.c_str() + 13)) : 2.0f;
        SelfTestHolsterTrack track{};
        {
            std::lock_guard<std::mutex> lock(m_SelfTestHolsterTrackMutex);
            track = m_SelfTestHolsterTrack;
            m_SelfTestHolsterTrack.active = false;
        }
        float worst = 0.0f;
        uint32_t samples = 0u;
        int length = 0;
        char perSlot[220] = {};
        for (int slot = 0; slot < l4d2vr_holster::kZoneCount; ++slot)
        {
            float slotWorst = 0.0f;
            uint32_t slotSamples = 0u;
            for (int eye = 0; eye < 2; ++eye)
            {
                if (track.n[eye][slot] == 0u)
                    continue;
                slotSamples += track.n[eye][slot];
                const Vector spread = track.mx[eye][slot] - track.mn[eye][slot];
                slotWorst = (std::max)(slotWorst, (std::max)(spread.x, (std::max)(spread.y, spread.z)));
            }
            if (slotSamples == 0u)
                continue;
            samples += slotSamples;
            worst = (std::max)(worst, slotWorst);
            length += std::snprintf(perSlot + length, sizeof(perSlot) - length, " slot%d=%.1fcm(%u)",
                slot + 1, slotWorst * 100.0f, slotSamples);
        }
        std::snprintf(detail, sizeof(detail), "worst %.1f cm (limit %.1f), %u draws:%s", worst * 100.0f, limitCm,
            samples, perSlot);
        SelfTestReport("holster items stay on the body while moving", samples > 0u && worst * 100.0f <= limitCm, detail);
        return true;
    }

    // ---- Render verification scenes -----------------------------------------
    // look=<pitch>: scripted head pitch in degrees (positive looks down; "off"
    // returns to the headset's own). Holds for the following steps.
    if (step.rfind("look=", 0) == 0)
    {
        if (at(0, 0.0f))
        {
            const std::string value = step.substr(5);
            m_SelfTestLookActive = value != "off";
            m_SelfTestLookPitch = m_SelfTestLookActive ? std::clamp(static_cast<float>(std::atof(value.c_str())), -89.0f, 89.0f) : 0.0f;
            Game::logMsg("[VR][SelfTest] head pitch %s", m_SelfTestLookActive ? value.c_str() : "released");
        }
        return t >= 1.0f;
    }

    // lefthand=x,y,z / righthand=x,y,z: hold a hand at a point of the holster
    // body frame in metres (x forward, y toward the dominant hand, z up, from
    // the head), e.g. righthand=0.02,0.22,-0.7 is the hip holster. "off" releases.
    if (step.rfind("lefthand=", 0) == 0 || step.rfind("righthand=", 0) == 0)
    {
        const bool leftHand = step[0] == 'l';
        if (at(0, 0.0f))
        {
            const std::string value = step.substr(leftHand ? 9 : 10);
            l4d2vr_selftest::Vec3 local{};
            const bool valid = value != "off" && l4d2vr_selftest::ParseVec3(value, local);
            if (value != "off" && !valid)
            {
                SelfTestReport(step.c_str(), false, "expected x,y,z in metres or off");
                return true;
            }
            (leftHand ? m_SelfTestSceneLeftValid : m_SelfTestSceneRightValid) = valid;
            (leftHand ? m_SelfTestSceneLeft : m_SelfTestSceneRight) = local;
        }
        return t >= 1.0f;
    }

    // backpreview=<metres>: draw the back holster items that far in front of
    // their real place (also with the first-person body off), keeping their
    // orientation, so shot= shows them as someone behind the player sees
    // them. 0 turns it off. Grabbing is unaffected.
    if (step.rfind("backpreview=", 0) == 0)
    {
        m_SelfTestBackPreviewMeters = std::clamp(static_cast<float>(std::atof(step.c_str() + 12)), 0.0f, 2.0f);
        Game::logMsg("[VR][SelfTest] back holster preview %.2f m", m_SelfTestBackPreviewMeters);
        return true;
    }

    // allpreview=<metres>: the same for every holster item (hips and chest
    // too), to check how each one is turned from a normal viewing angle.
    if (step.rfind("allpreview=", 0) == 0)
    {
        m_SelfTestAllPreviewMeters = std::clamp(static_cast<float>(std::atof(step.c_str() + 11)), 0.0f, 2.0f);
        Game::logMsg("[VR][SelfTest] holster preview %.2f m", m_SelfTestAllPreviewMeters);
        return true;
    }

    // lefthanded=1|0: switch the left-handed layout (dominant hand = the left
    // controller) for the following steps. The self-test drives buttons by
    // action, so m_ActionWeaponGrip is then the left hand's grip.
    if (step.rfind("lefthanded=", 0) == 0)
    {
        m_LeftHanded = step.size() > 11 && step[11] == '1';
        Game::logMsg("[VR][SelfTest] left-handed %d", m_LeftHanded ? 1 : 0);
        return true;
    }

    // leftang=pitch,yaw,roll / rightang=pitch,yaw,roll: turn a scripted hand
    // (degrees; yaw relative to the head's). Holds like lefthand=/righthand=.
    if (step.rfind("leftang=", 0) == 0 || step.rfind("rightang=", 0) == 0)
    {
        const bool leftHand = step[0] == 'l';
        if (at(0, 0.0f))
        {
            const std::string value = step.substr(leftHand ? 8 : 9);
            l4d2vr_selftest::Vec3 angles{};
            const bool valid = value != "off" && l4d2vr_selftest::ParseVec3(value, angles);
            if (value != "off" && !valid)
            {
                SelfTestReport(step.c_str(), false, "expected pitch,yaw,roll in degrees or off");
                return true;
            }
            (leftHand ? m_SelfTestSceneLeftAng : m_SelfTestSceneRightAng) = QAngle(angles.x, angles.y, angles.z);
            (leftHand ? m_SelfTestSceneLeftAngValid : m_SelfTestSceneRightAngValid) = valid;
        }
        return t >= 1.0f;
    }

    // freezeanyframe=on|off: comparison runs; on pauses the viewmodel for a
    // reload or pump on whatever frame it shows, as before the resting-pose fix.
    if (step.rfind("freezeanyframe=", 0) == 0)
    {
        m_SelfTestFreezeAnyFrame = step.substr(15) == "on";
        Game::logMsg("[VR][SelfTest] viewmodel pause on any frame: %s", m_SelfTestFreezeAnyFrame ? "on" : "off");
        return true;
    }

    // handdump=<seconds>: log the drawn off hand's bones and its fist in the
    // hand bone's frame ([VR][HandDump]).
    if (step.rfind("handdump=", 0) == 0)
    {
        const float seconds = std::clamp(static_cast<float>(std::atof(step.c_str() + 9)), 0.5f, 60.0f);
        m_SelfTestHandDumpUntilMs.store(GetTickCount64() + static_cast<uint64_t>(seconds * 1000.0f), std::memory_order_release);
        return true; // runs on beside the following steps
    }

    // glowmat=<material>: use this material for every glow from now on ("off"
    // = the built-in one), to compare glow materials in screenshots.
    if (step.rfind("glowmat=", 0) == 0)
    {
        const std::string name = step.substr(8);
        IMaterial* material = nullptr;
        if (name != "off" && m_Game && m_Game->m_MaterialSystem)
            material = m_Game->m_MaterialSystem->FindMaterial(name.c_str(), "Model textures", false, nullptr);
        const bool usable = material && !material->IsErrorMaterial();
        if (usable)
            material->IncrementReferenceCount(); // see HooksHolsterHighlightMaterial
        m_SelfTestGlowMaterial.store(usable ? material : nullptr, std::memory_order_release);
        std::snprintf(detail, sizeof(detail), "%s shader=%s translucent=%d", name.c_str(),
            usable ? material->GetShaderName() : "-", usable && material->IsTranslucent() ? 1 : 0);
        SelfTestReport("glow material", name == "off" || usable, detail);
        return true;
    }

    // fire=<seconds>: hold the trigger (e.g. to empty a magazine).
    if (step.rfind("fire=", 0) == 0)
    {
        const float seconds = std::clamp(static_cast<float>(std::atof(step.c_str() + 5)), 0.1f, 30.0f);
        if (at(0, 0.0f)) { button(m_ActionPrimaryAttack, true); }
        if (at(1, seconds)) { button(m_ActionPrimaryAttack, false); }
        return t >= seconds + 1.0f;
    }

    // magrelease: one press of the magazine release (the old magazine drops and
    // the spare-magazine hint lights up).
    if (step == "magrelease")
    {
        if (at(0, 0.0f)) { button(m_ActionMagazineRelease, true); }
        if (at(1, 0.2f)) { button(m_ActionMagazineRelease, false); }
        return t >= 1.5f;
    }

    // shot=<name>[*N]: save the left eye as l4d2vr_selftest_<name>.png in the
    // game folder (N captures 0.3 s apart: <name>_1 .. <name>_N, to compare for
    // flicker). shotlr=<name> saves both eyes as <name>_l / <name>_r.
    if (step.rfind("shot=", 0) == 0 || step.rfind("shotlr=", 0) == 0)
    {
        const bool both = step.rfind("shotlr=", 0) == 0;
        std::string name = step.substr(both ? 7 : 5);
        int count = 1;
        const size_t star = name.find('*');
        if (star != std::string::npos)
        {
            count = std::clamp(std::atoi(name.c_str() + star + 1), 1, 20);
            name.resize(star);
        }
        for (char& ch : name)
            if (!std::isalnum(static_cast<unsigned char>(ch)) && ch != '-' && ch != '_')
                ch = '_';
        if (name.empty())
            name = "shot";
        const int index = m_SelfTestStage / 2;
        const float due = 0.5f + index * 0.3f;
        if (m_SelfTestStage % 2 == 0)
        {
            if (index >= count)
            {
                std::snprintf(detail, sizeof(detail), "%d of %d image(s) saved as l4d2vr_selftest_%s%s%s.png",
                    count - m_SelfTestValueD, count, name.c_str(), count > 1 ? "_N" : "", both ? "_l/_r" : "");
                SelfTestReport("screenshot", m_SelfTestValueD == 0, detail);
                return true;
            }
            if (t >= due)
            {
                if (index == 0)
                    m_SelfTestValueD = 0; // failed captures
                SelfTestRequestShot(count > 1 ? name + "_" + std::to_string(index + 1) : name, both ? 3 : 1);
                m_SelfTestSerial = m_SelfTestShotRequest.load(std::memory_order_acquire);
                ++m_SelfTestStage;
            }
        }
        else if (m_SelfTestShotDone.load(std::memory_order_acquire) == m_SelfTestSerial)
        {
            if (!m_SelfTestShotOk.load(std::memory_order_acquire))
                ++m_SelfTestValueD;
            ++m_SelfTestStage;
        }
        else if (t > due + 5.0f)
        {
            SelfTestReport("screenshot", false, "no eye frame was rendered within 5 s");
            return true;
        }
        return false;
    }

    if (step.rfind("wriststyle=", 0) == 0)
    {
        m_WristHudNativeStyle = step.substr(11) != "custom";
        Game::logMsg("[VR][SelfTest] wrist HUD style: %s", m_WristHudNativeStyle ? "native" : "custom");
        return true;
    }

    // Saves the HUD texture and the exact textures submitted to the wrist overlays
    // (l4d2vr_selftest_wrist_*.bmp in the game folder) for visual checks.
    if (step == "wristdump")
    {
        if (at(0, 0.0f))
        {
            m_SelfTestValueA = m_NativeWristHudDumpCount.load(std::memory_order_acquire);
            m_NativeWristHudDumpRequested.store(true, std::memory_order_release);
        }
        const bool dumped = m_NativeWristHudDumpCount.load(std::memory_order_acquire) != m_SelfTestValueA;
        if (!dumped && t < 8.0f)
            return false;
        const uint32_t usedL = m_NativeWristHudUsedSize[0].load(std::memory_order_acquire);
        const uint32_t usedR = m_NativeWristHudUsedSize[1].load(std::memory_order_acquire);
        std::snprintf(detail, sizeof(detail), "style=%s left=%ux%u ready=%d shown=%d right=%ux%u ready=%d shown=%d",
            m_WristHudNativeStyle ? "native" : "custom", usedL >> 16, usedL & 0xFFFFu,
            m_NativeWristHudTexReady[0].load() ? 1 : 0, m_NativeWristHudShown[0].load() ? 1 : 0,
            usedR >> 16, usedR & 0xFFFFu, m_NativeWristHudTexReady[1].load() ? 1 : 0, m_NativeWristHudShown[1].load() ? 1 : 0);
        SelfTestReport("native wrist HUD dump", dumped && m_NativeWristHudTexReady[0].load() && m_NativeWristHudTexReady[1].load(), detail);
        return true;
    }

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

    if (step == "reach" || step == "reachlh" || step == "emptysqueeze" || step == "pickup" || step == "bashanim" || step == "swinganim" ||
        step == "bashanimraw" || step == "swinganimraw" || step == "bashmotion")
        return RunSelfTestRealisticStep(step, t, player);

    SelfTestReport(step.c_str(), false, "unknown step");
    return true;
}

// Realistic-motion steps: both hands move along minimum-jerk paths between
// points of a standing body (l4d2vr_selftest::body), never toward the holster
// layout's own coordinates, at human speeds with tracking jitter. They check
// what a player feels: draws happen, hovers pulse, nothing is bashed on the
// way, and squeezing the grip without taking anything never drops a weapon.
bool VR::RunSelfTestRealisticStep(const std::string& step, float t, C_BasePlayer* player)
{
    using namespace l4d2vr_selftest;
    struct Action
    {
        float at;
        std::function<bool(C_BasePlayer*, float)> run; // true ends the step
    };
    static std::vector<Action> s_Script;
    static bool s_SavedToggle = true;
    static bool s_SavedContact = false;
    static bool s_SavedSnap = false;
    // bashmotion: degrees turned, snap turns and distance walked while measuring.
    static float s_TurnedDeg = 0.0f, s_LastYaw = 0.0f, s_Walked = 0.0f;
    static int s_Snaps = 0;
    static bool s_Measure = false;
    static bool s_OwnsLocomotion = false;
    static Vector s_LastOrigin{};
    static bool s_SavedLeftHanded = false;
    static uint32_t s_ShoveBaseline = 0u;
    static float s_WorldReachStart = -1.0f;
    // Script actions run in later frames: they capture only `this` and values.
    auto cmd = [this](const char* text) { if (m_Game) m_Game->ClientCmd_Unrestricted(text); };
    auto button = [](vr::VRActionHandle_t action, bool down) { l4d2vr_selftest::SetButton(action, down); };
    auto level = [](float pitch, float yaw) { return body::V(pitch, yaw, 0.0f); };
    auto add = [](float at, std::function<bool(C_BasePlayer*, float)> run) { s_Script.push_back(Action{ at, std::move(run) }); };
    // A squeeze of the given hand: press now, let go after `hold` seconds.
    auto squeeze = [&](float at, bool left, float hold)
    {
        const vr::VRActionHandle_t action = left ? m_ActionOffHandGrip : m_ActionWeaponGrip;
        add(at, [action](C_BasePlayer*, float) { l4d2vr_selftest::SetButton(action, true); return false; });
        add(at + hold, [action](C_BasePlayer*, float) { l4d2vr_selftest::SetButton(action, false); return false; });
    };
    auto restore = [this]()
    {
        m_GripToggleEnabled = s_SavedToggle;
        m_GripReleaseDropEnabled = s_SavedContact;
        if (s_OwnsLocomotion)
        {
            m_SnapTurning = s_SavedSnap;
            m_SelfTestTurnStickActive = false;
            m_SelfTestTurnStickX = 0.0f;
            m_SelfTestWalkActive = false;
        }
        s_OwnsLocomotion = false;
        s_Measure = false;
        m_LeftHanded = s_SavedLeftHanded;
        m_SelfTestHeadDrop = 0.0f;
        m_VmStrikeHoldDisabled = false;
        s_WorldReachStart = -1.0f;
    };

    if (m_SelfTestStage == 0)
    {
        s_Script.clear();
        s_SavedToggle = m_GripToggleEnabled;
        s_SavedContact = m_GripReleaseDropEnabled;
        s_SavedSnap = m_SnapTurning;
        s_Measure = false;
        s_OwnsLocomotion = false;
        s_SavedLeftHanded = m_LeftHanded;
        s_WorldReachStart = -1.0f;
        // A real grip action is always active: register both grips as released
        // before the first press (a press arriving with activation is ignored).
        button(m_ActionWeaponGrip, false);
        button(m_ActionOffHandGrip, false);

        if (step == "reach")
        {
            // Pavlov toggle grips, both hands, every holster reached the way a
            // person reaches for it.
            m_GripToggleEnabled = true;
            cmd("give rifle"); cmd("give pipe_bomb"); cmd("give first_aid_kit"); cmd("give pain_pills");
            add(1.2f, [this, cmd, level, restore](C_BasePlayer* player, float t) {
                cmd("use weapon_rifle");
                s_ShoveBaseline = m_PhysicalShoveSerial.load(std::memory_order_acquire);
                m_SelfTestValueB = static_cast<int>(m_HolsterHoverPulseCount);
                m_SelfTestValueC = static_cast<int>(m_HolsterLeftHoverPulseCount);
                return false; });
            add(1.8f, [this, cmd, level, restore](C_BasePlayer* player, float t) { SelfTestReachRight(body::RightHip(), level(60.0f, 0.0f), 0.8f); return false; });
            squeeze(3.0f, false, 0.15f);
            add(3.6f, [this, cmd, level, restore](C_BasePlayer* player, float t) {
                char detail[256];
                std::snprintf(detail, sizeof(detail), "active slot %d (pistol is slot 2), hover pulses %d -> %u",
                    SelfTestActiveSlot(player) + 1, m_SelfTestValueB, m_HolsterHoverPulseCount);
                SelfTestReport("reach: right hand draws from the right hip", SelfTestActiveSlot(player) == 1, detail);
                SelfTestReport("reach: hip hover pulse", static_cast<int>(m_HolsterHoverPulseCount) > m_SelfTestValueB, detail);
                m_SelfTestValueB = static_cast<int>(m_HolsterHoverPulseCount);
                SelfTestReachRight(body::RightReady(), level(0.0f, 0.0f), 1.0f);
                return false; });
            add(4.6f, [this, cmd, level, restore](C_BasePlayer* player, float t) { SelfTestReachRight(body::OverRightShoulder(), level(-60.0f, 0.0f), 1.2f); return false; });
            squeeze(5.6f, false, 0.15f);
            add(6.2f, [this, cmd, level, restore](C_BasePlayer* player, float t) {
                char detail[256];
                std::snprintf(detail, sizeof(detail), "active slot %d, hover pulses %d -> %u", SelfTestActiveSlot(player) + 1,
                    m_SelfTestValueB, m_HolsterHoverPulseCount);
                SelfTestReport("reach: right hand draws over the right shoulder", SelfTestActiveSlot(player) == 0, detail);
                SelfTestReport("reach: shoulder hover pulse", static_cast<int>(m_HolsterHoverPulseCount) > m_SelfTestValueB, detail);
                m_SelfTestValueB = static_cast<int>(m_HolsterHoverPulseCount);
                SelfTestReachRight(body::RightReady(), level(0.0f, 0.0f), 1.2f);
                return false; });
            add(7.2f, [this, cmd, level, restore](C_BasePlayer* player, float t) { SelfTestReachRight(body::RightChest(), level(30.0f, 60.0f), 0.6f); return false; });
            squeeze(8.2f, false, 0.15f);
            add(8.8f, [this, cmd, level, restore](C_BasePlayer* player, float t) {
                char detail[256];
                std::snprintf(detail, sizeof(detail), "active slot %d (pills are slot 5), hover pulses %d -> %u",
                    SelfTestActiveSlot(player) + 1, m_SelfTestValueB, m_HolsterHoverPulseCount);
                SelfTestReport("reach: right hand draws from the chest", SelfTestActiveSlot(player) == 4, detail);
                SelfTestReport("reach: chest hover pulse", static_cast<int>(m_HolsterHoverPulseCount) > m_SelfTestValueB, detail);
                SelfTestReachRight(body::RightReady(), level(0.0f, 0.0f), 0.8f);
                return false; });
            // The kit is on the back, over the off (left) shoulder: the right
            // hand crosses the chest and reaches over it.
            add(9.6f, [this, cmd, level, restore](C_BasePlayer* player, float t) {
                m_SelfTestValueB = static_cast<int>(m_HolsterHoverPulseCount);
                SelfTestReachRight(body::V(0.20f, -0.22f, -0.18f), level(-20.0f, -40.0f), 1.0f);
                return false; });
            add(10.3f, [this, cmd, level, restore](C_BasePlayer* player, float t) { SelfTestReachRight(body::OverLeftShoulder(), level(-60.0f, -30.0f), 0.8f); return false; });
            squeeze(11.2f, false, 0.15f);
            add(11.8f, [this, cmd, level, restore](C_BasePlayer* player, float t) {
                char detail[256];
                std::snprintf(detail, sizeof(detail), "active slot %d (the kit is slot 4), hover pulses %d -> %u",
                    SelfTestActiveSlot(player) + 1, m_SelfTestValueB, m_HolsterHoverPulseCount);
                SelfTestReport("reach: right hand draws the kit over the left shoulder", SelfTestActiveSlot(player) == 3, detail);
                SelfTestReport("reach: off-shoulder hover pulse (right hand)", static_cast<int>(m_HolsterHoverPulseCount) > m_SelfTestValueB, detail);
                SelfTestReachRight(body::RightReady(), level(0.0f, 0.0f), 1.0f);
                return false; });
            // The off hand swaps too: over the right shoulder to the rifle, over
            // the left shoulder to the kit, then the chest.
            add(12.8f, [this, cmd, level, restore](C_BasePlayer* player, float t) {
                m_SelfTestValueC = static_cast<int>(m_HolsterLeftHoverPulseCount);
                SelfTestReachLeft(body::V(0.20f, 0.22f, -0.18f), level(-20.0f, 40.0f), 1.0f);
                return false; });
            add(13.5f, [this, cmd, level, restore](C_BasePlayer* player, float t) { SelfTestReachLeft(body::OverRightShoulder(), level(-60.0f, 30.0f), 0.8f); return false; });
            squeeze(14.4f, true, 0.15f);
            add(15.0f, [this, cmd, level, restore](C_BasePlayer* player, float t) {
                char detail[256];
                std::snprintf(detail, sizeof(detail), "active slot %d (the rifle is slot 1), left hover pulses %d -> %u",
                    SelfTestActiveSlot(player) + 1, m_SelfTestValueC, m_HolsterLeftHoverPulseCount);
                SelfTestReport("reach: off hand swaps to the rifle over the right shoulder", SelfTestActiveSlot(player) == 0, detail);
                SelfTestReport("reach: off-hand right-shoulder hover pulse",
                    static_cast<int>(m_HolsterLeftHoverPulseCount) > m_SelfTestValueC, detail);
                m_SelfTestValueC = static_cast<int>(m_HolsterLeftHoverPulseCount);
                SelfTestReachLeft(body::LeftReady(), level(0.0f, 0.0f), 1.0f);
                return false; });
            add(16.0f, [this, cmd, level, restore](C_BasePlayer* player, float t) { SelfTestReachLeft(body::OverLeftShoulder(), level(-60.0f, 0.0f), 1.0f); return false; });
            squeeze(17.0f, true, 0.15f);
            add(17.6f, [this, cmd, level, restore](C_BasePlayer* player, float t) {
                char detail[256];
                std::snprintf(detail, sizeof(detail), "active slot %d (the kit is slot 4), left hover pulses %d -> %u",
                    SelfTestActiveSlot(player) + 1, m_SelfTestValueC, m_HolsterLeftHoverPulseCount);
                SelfTestReport("reach: off hand swaps to the kit over the left shoulder", SelfTestActiveSlot(player) == 3, detail);
                SelfTestReport("reach: off-hand left-shoulder hover pulse",
                    static_cast<int>(m_HolsterLeftHoverPulseCount) > m_SelfTestValueC, detail);
                m_SelfTestValueC = static_cast<int>(m_HolsterLeftHoverPulseCount);
                SelfTestReachLeft(body::LeftReady(), level(0.0f, 0.0f), 1.0f);
                return false; });
            add(18.4f, [this, cmd, level, restore](C_BasePlayer* player, float t) { SelfTestReachLeft(body::LeftChest(), level(30.0f, -40.0f), 0.5f); return false; });
            squeeze(19.3f, true, 0.15f);
            add(19.9f, [this, cmd, level, restore](C_BasePlayer* player, float t) {
                char detail[256];
                std::snprintf(detail, sizeof(detail), "active slot %d (pipe bomb is slot 3), left hover pulses %d -> %u",
                    SelfTestActiveSlot(player) + 1, m_SelfTestValueC, m_HolsterLeftHoverPulseCount);
                SelfTestReport("reach: off hand swaps at the chest", SelfTestActiveSlot(player) == 2, detail);
                SelfTestReachLeft(body::LeftReady(), level(0.0f, 0.0f), 0.8f);
                return false; });
            // The off hand at the magazine pouch (off-hand hip) never draws from
            // the holsters, even looking up, where the back sphere swings low.
            add(20.6f, [this, cmd, level, restore](C_BasePlayer* player, float t) {
                m_SelfTestLookActive = true;
                m_SelfTestLookPitch = -30.0f;
                SelfTestReachLeft(body::V(0.02f, -0.20f, -0.55f), level(30.0f, 0.0f), 0.6f);
                m_SelfTestValueA = SelfTestActiveSlot(player);
                return false; });
            squeeze(21.6f, true, 0.15f);
            add(22.2f, [this, cmd, level, restore](C_BasePlayer* player, float t) {
                char detail[256];
                std::snprintf(detail, sizeof(detail), "active slot %d -> %d", m_SelfTestValueA + 1, SelfTestActiveSlot(player) + 1);
                SelfTestReport("reach: off hand at the magazine pouch draws nothing", SelfTestActiveSlot(player) == m_SelfTestValueA, detail);
                m_SelfTestLookActive = false;
                m_SelfTestLookPitch = 0.0f;
                SelfTestReachLeft(body::LeftReady(), level(0.0f, 0.0f), 0.8f);
                return false; });
            add(23.0f, [this, cmd, level, restore](C_BasePlayer* player, float t) {
                char detail[256];
                const uint32_t shoves = m_PhysicalShoveSerial.load(std::memory_order_acquire) - s_ShoveBaseline;
                std::snprintf(detail, sizeof(detail), "%u gun bash(es) during 8 holster reaches (see [VR][Holster] grip pressed lines)", shoves);
                SelfTestReport("reach: no gun bash while reaching for holsters", shoves == 0u, detail);
                restore();
                return true; });
        }
        else if (step == "reachlh")
        {
            // Left-handed: the left controller is the dominant hand (weapon
            // grip), every holster mirrors. Dominant reaches: left hip, over the
            // left shoulder (rifle), left chest (pills), across to over the
            // right shoulder (kit). Off-hand (right) reaches: over the left
            // shoulder (rifle), over the right shoulder (kit), right chest (pipe).
            // The self-test poses gameplay hands (Right = dominant, now the
            // physical left hand); the targets are physical body points.
            m_LeftHanded = true;
            m_GripToggleEnabled = true;
            auto M = [](Vec3 v) { v.y = -v.y; return v; };
            cmd("give rifle"); cmd("give pipe_bomb"); cmd("give first_aid_kit"); cmd("give pain_pills");
            add(1.2f, [this, cmd](C_BasePlayer* player, float t) {
                cmd("use weapon_rifle");
                s_ShoveBaseline = m_PhysicalShoveSerial.load(std::memory_order_acquire);
                SelfTestReachRight(body::V(0.40f, -0.15f, -0.35f), body::V(0.0f, 0.0f, 0.0f), 0.8f);
                SelfTestReachLeft(body::V(0.32f, 0.18f, -0.42f), body::V(0.0f, 0.0f, 0.0f), 0.8f);
                return false; });
            struct Reach { bool dominant; std::vector<Vec3> path; int slot; const char* name; };
            const std::vector<Reach> reaches = {
                { true, { M(body::RightHip()) }, 1, "reachlh: left (dominant) hand draws from the left hip" },
                { true, { M(body::OverRightShoulder()) }, 0, "reachlh: left hand draws the rifle over the left shoulder" },
                { true, { M(body::RightChest()) }, 4, "reachlh: left hand draws the pills from the left chest" },
                { true, { body::V(0.20f, 0.22f, -0.18f), M(body::OverLeftShoulder()) }, 3, "reachlh: left hand draws the kit over the right shoulder" },
                { false, { body::V(0.20f, -0.22f, -0.18f), body::OverLeftShoulder() }, 0, "reachlh: right (off) hand swaps to the rifle over the left shoulder" },
                { false, { body::OverRightShoulder() }, 3, "reachlh: right hand swaps to the kit over the right shoulder" },
                { false, { body::RightChest() }, 2, "reachlh: right hand swaps to the pipe bomb at the right chest" },
            };
            float at = 2.2f;
            for (const Reach& reach : reaches)
            {
                const bool dominant = reach.dominant;
                add(at, [this, dominant](C_BasePlayer*, float) {
                    m_SelfTestValueB = static_cast<int>(dominant ? m_HolsterHoverPulseCount : m_HolsterLeftHoverPulseCount);
                    return false; });
                for (const Vec3& point : reach.path)
                {
                    add(at, [this, dominant, point](C_BasePlayer*, float) {
                        if (dominant) SelfTestReachRight(point, body::V(-40.0f, 0.0f, 0.0f), 0.9f);
                        else SelfTestReachLeft(point, body::V(-40.0f, 0.0f, 0.0f), 0.9f);
                        return false; });
                    at += 0.8f;
                }
                at += 0.2f;
                // The dominant grip is the weapon grip (the left controller now).
                squeeze(at, !dominant, 0.15f);
                at += 0.6f;
                const int slot = reach.slot;
                const char* const name = reach.name;
                add(at, [this, dominant, slot, name](C_BasePlayer* player, float) {
                    char detail[256];
                    const uint32_t pulses = dominant ? m_HolsterHoverPulseCount : m_HolsterLeftHoverPulseCount;
                    std::snprintf(detail, sizeof(detail), "active slot %d (expected %d), %s hover pulses %d -> %u, left-handed %d",
                        SelfTestActiveSlot(player) + 1, slot + 1, dominant ? "dominant" : "off-hand", m_SelfTestValueB, pulses,
                        m_LeftHanded ? 1 : 0);
                    SelfTestReport(name, SelfTestActiveSlot(player) == slot && static_cast<int>(pulses) > m_SelfTestValueB, detail);
                    if (dominant) SelfTestReachRight(body::V(0.40f, -0.15f, -0.35f), body::V(0.0f, 0.0f, 0.0f), 1.0f);
                    else SelfTestReachLeft(body::V(0.32f, 0.18f, -0.42f), body::V(0.0f, 0.0f, 0.0f), 1.0f);
                    return false; });
                at += 1.0f;
            }
            add(at, [this, restore](C_BasePlayer*, float) {
                char detail[256];
                const uint32_t shoves = m_PhysicalShoveSerial.load(std::memory_order_acquire) - s_ShoveBaseline;
                std::snprintf(detail, sizeof(detail), "%u gun bash(es) during 7 left-handed holster reaches", shoves);
                SelfTestReport("reachlh: no gun bash while reaching for holsters", shoves == 0u, detail);
                restore();
                return true; });
        }
        else if (step == "emptysqueeze")
        {
            // The headset bug: squeezing the grip in the air with a gun the grip
            // never took (switched to with a command) dropped it.
            m_GripToggleEnabled = true;
            cmd("give rifle");
            add(1.0f, [this, cmd, level, restore](C_BasePlayer* player, float t) { cmd("use weapon_rifle"); return false; });
            add(2.0f, [this, cmd, level, restore](C_BasePlayer* player, float t) { SelfTestReachRight(body::RightReady(), level(0.0f, 0.0f), 0.6f); return false; });
            for (int i = 0; i < 3; ++i)
            {
                // Small hand movements while squeezing, like a real hand.
                add(2.8f + i * 1.0f, [this, i, level](C_BasePlayer* player, float t) {
                    SelfTestReachRight(body::V(0.40f + 0.03f * i, 0.15f - 0.02f * i, -0.35f + 0.02f * i), level(0.0f, 0.0f), 0.3f);
                    return false; });
                squeeze(3.0f + i * 1.0f, false, 0.15f);
            }
            add(6.0f, [this, cmd, level, restore](C_BasePlayer* player, float t) {
                char detail[256];
                std::snprintf(detail, sizeof(detail), "rifle %s, active slot %d, toggle %s", (player->Weapon_GetSlot(0) != nullptr) ? "kept" : "DROPPED",
                    SelfTestActiveSlot(player) + 1, m_GripToggle.Held() ? "held" : "released");
                SelfTestReport("empty squeeze (toggle grips) never drops the weapon in hand", (player->Weapon_GetSlot(0) != nullptr) && SelfTestActiveSlot(player) == 0, detail);
                m_GripToggleEnabled = false;
                return false; });
            for (int i = 0; i < 3; ++i)
                squeeze(6.5f + i * 1.0f, false, 0.4f);
            add(9.8f, [this, cmd, level, restore](C_BasePlayer* player, float t) {
                char detail[256];
                std::snprintf(detail, sizeof(detail), "rifle %s, active slot %d", (player->Weapon_GetSlot(0) != nullptr) ? "kept" : "DROPPED", SelfTestActiveSlot(player) + 1);
                SelfTestReport("empty squeeze (hold grips) never drops the weapon in hand", (player->Weapon_GetSlot(0) != nullptr) && SelfTestActiveSlot(player) == 0, detail);
                m_GripToggleEnabled = true;
                cmd("use weapon_pistol");
                return false; });
            for (int i = 0; i < 2; ++i)
                squeeze(11.0f + i * 1.0f, false, 0.15f);
            add(13.5f, [this, cmd, level, restore](C_BasePlayer* player, float t) {
                char detail[256];
                std::snprintf(detail, sizeof(detail), "pistol %s, active slot %d", (player->Weapon_GetSlot(1) != nullptr) ? "kept" : "DROPPED", SelfTestActiveSlot(player) + 1);
                SelfTestReport("empty squeeze never drops the pistol", (player->Weapon_GetSlot(1) != nullptr) && SelfTestActiveSlot(player) == 1, detail);
                restore();
                return true; });
        }
        else if (step == "pickup")
        {
            // Grip contact pickups: a first aid kit taken from the body and
            // dropped by letting go (the grip took it), then picked up from the
            // floor by reaching and squeezing. The gun in hand is never dropped.
            m_GripToggleEnabled = true;
            m_GripReleaseDropEnabled = true;
            cmd("give rifle"); cmd("give first_aid_kit");
            add(1.0f, [this, cmd, level, restore](C_BasePlayer* player, float t) { cmd("use weapon_rifle"); return false; });
            add(1.4f, [this, cmd, level, restore](C_BasePlayer* player, float t) { SelfTestReachRight(body::V(0.20f, -0.22f, -0.18f), level(-20.0f, -40.0f), 1.0f); return false; });
            add(2.0f, [this, cmd, level, restore](C_BasePlayer* player, float t) { SelfTestReachRight(body::OverLeftShoulder(), level(-60.0f, -30.0f), 0.8f); return false; });
            squeeze(2.8f, false, 0.15f);
            add(3.4f, [this, cmd, level, restore](C_BasePlayer* player, float t) {
                char detail[256];
                std::snprintf(detail, sizeof(detail), "active slot %d (medkit is slot 4)", SelfTestActiveSlot(player) + 1);
                SelfTestReport("pickup: right hand draws the kit over the left shoulder", SelfTestActiveSlot(player) == 3, detail);
                // From over the shoulder down in front of the chest, then out to
                // where the kit is let go (still for half a second before that).
                SelfTestReachRight(body::CentreChest(), level(30.0f, 40.0f), 1.2f);
                return false; });
            add(4.0f, [this, cmd, level, restore](C_BasePlayer* player, float t) {
                SelfTestReachRight(body::V(0.45f, 0.10f, -0.55f), level(30.0f, 0.0f), 0.8f);
                return false; });
            add(4.9f, [this](C_BasePlayer* player, float) {
                m_SelfTestKitEntity = player->Weapon_GetSlot(3);
                return false; });
            squeeze(5.0f, false, 0.15f);
            add(6.2f, [this, cmd, level, restore](C_BasePlayer* player, float t) {
                char detail[256];
                std::snprintf(detail, sizeof(detail), "kit slot %s after letting go of the kit the grip took",
                    (player->Weapon_GetSlot(3) != nullptr) ? "still occupied" : "empty");
                SelfTestReport("pickup: an item the grip took drops when let go", !(player->Weapon_GetSlot(3) != nullptr), detail);
                // Find the dropped kit (the entity the grip let go of) and crouch toward it.
                float best = 1.0e9f;
                Vector kit{};
                const int highest = (std::min)(2048, m_Game->m_ClientEntityList->GetHighestEntityIndex());
                for (int i = 1; i <= highest && m_SelfTestKitEntity; ++i)
                {
                    C_BaseEntity* const entity = m_Game->GetClientEntity(i);
                    if (!entity || reinterpret_cast<void*>(entity) != m_SelfTestKitEntity || !VR_TryGetEntityAbsOrigin(entity, kit))
                        continue;
                    best = (kit - m_RightControllerPosAbs).Length();
                    Game::logMsg("[VR][SelfTest] pickup: dropped kit is entity %d, %.2f m from the hand", i,
                        best / (std::max)(1.0f, m_VRScale));
                }
                if (best > 3.0f * m_VRScale)
                {
                    Game::logMsg("[VR][SelfTest] pickup: scanned %d entities, hand at (%.0f %.0f %.0f)",
                        highest, m_RightControllerPosAbs.x, m_RightControllerPosAbs.y, m_RightControllerPosAbs.z);
                    SelfTestReport("pickup: grip contact picks up the kit", false, "dropped kit not found near the hand");
                    restore();
                    return true;
                }
                m_SelfTestHeadDrop = 0.55f;
                m_SelfTestSlapStart = m_RightControllerPosAbs;
                m_SelfTestSlapEnd = kit + Vector(0.0f, 0.0f, 0.04f * m_VRScale);
                s_WorldReachStart = t + 0.6f;
                m_SelfTestValueA = static_cast<int>(best / (std::max)(1.0f, m_VRScale) * 100.0f);
                return false; });
            squeeze(8.0f, false, 0.15f);
            add(10.1f, [this, cmd, level, restore](C_BasePlayer* player, float t) {
                char detail[256];
                std::snprintf(detail, sizeof(detail), "kit %s (was %.2f m from the hand), rifle %s, active slot %d",
                    (player->Weapon_GetSlot(3) != nullptr) ? "in slot 4" : "not picked up", m_SelfTestValueA / 100.0f, (player->Weapon_GetSlot(0) != nullptr) ? "kept" : "DROPPED",
                    SelfTestActiveSlot(player) + 1);
                SelfTestReport("pickup: grip contact picks up the kit", (player->Weapon_GetSlot(3) != nullptr), detail);
                SelfTestReport("pickup: the gun in hand is not dropped by the pickup squeeze", (player->Weapon_GetSlot(0) != nullptr), detail);
                restore();
                return true; });
        }
        else if (step == "bashanim" || step == "swinganim" || step == "bashanimraw" || step == "swinganimraw")
        {
            // Native strike animations never play on the controller-driven
            // viewmodel; the strike itself still happens. The *raw variants
            // leave the native animation on, for comparison screenshots.
            const bool melee = step == "swinganim" || step == "swinganimraw";
            const bool raw = step == "bashanimraw" || step == "swinganimraw";
            const std::string shotName = step;
            m_VmStrikeHoldDisabled = raw;
            // Left-eye pictures before and after the strike (at 3.5 s). A picture
            // stalls the game a moment, so none is taken during the push itself.
            add(2.8f, [this, shotName](C_BasePlayer*, float) { SelfTestRequestShot(shotName + "_0before", 1); return false; });
            for (int i = 1; i <= 4; ++i)
                add(3.7f + 0.2f * i, [this, shotName, i](C_BasePlayer*, float) {
                    SelfTestRequestShot(shotName + "_" + std::to_string(i), 1); return false; });
            if (melee)
            {
                for (const char* item : { "give machete", "give fireaxe", "give crowbar", "give baseball_bat", "give katana" })
                    cmd(item);
            }
            else
            {
                cmd("give rifle");
            }
            add(1.2f, [this, melee, cmd, level, restore](C_BasePlayer* player, float t) {
                cmd(melee ? "use weapon_melee" : "use weapon_rifle");
                SelfTestHoldRight(melee ? body::V(0.2f, 0.35f, 0.2f) : body::RightReady(),
                    melee ? QAngle(-60.0f, 30.0f, 0.0f) : QAngle(0.0f, 0.0f, 0.0f));
                return false; });
            add(3.5f, [this, melee, cmd, level, restore](C_BasePlayer* player, float t) {
                s_ShoveBaseline = melee ? m_MeleeSwingStartSerial.load(std::memory_order_acquire)
                    : m_PhysicalShoveSerial.load(std::memory_order_acquire);
                m_SelfTestValueA = static_cast<int>(m_VmStrikeFrames.load(std::memory_order_acquire));
                m_SelfTestValueB = static_cast<int>(m_VmStrikeFramesHeld.load(std::memory_order_acquire));
                m_SelfTestValueC = static_cast<int>(m_VmStrikeFramesShown.load(std::memory_order_acquire));
                m_VmStrikeMaxDeviation.store(0.0f, std::memory_order_release);
                m_VmAnimTraceUntilMs.store(GetTickCount64() + 2800u, std::memory_order_release);
                if (melee)
                {
                    // Diagonal downward slash at about 5 m/s.
                    SelfTestStartPath(
                        { Key{ 0.0f, body::V(0.2f, 0.35f, 0.2f) }, Key{ 0.16f, body::V(0.55f, -0.15f, -0.35f), false } },
                        { Key{ 0.0f, body::V(-60.0f, 30.0f, 0.0f) }, Key{ 0.16f, body::V(50.0f, -30.0f, 0.0f), false } });
                }
                else
                {
                    // A 0.30 m push along the barrel in 0.15 s.
                    SelfTestReachRight(body::Push(), level(0.0f, 0.0f), 2.0f);
                }
                return false; });
            add(4.3f, [this, melee, cmd, level, restore](C_BasePlayer* player, float t) {
                if (!melee)
                    SelfTestReachRight(body::RightReady(), level(0.0f, 0.0f), 0.6f);
                return false; });
            add(6.5f, [this, melee, cmd, level, restore](C_BasePlayer* player, float t) {
                char detail[256];
                const uint32_t strikes = (melee ? m_MeleeSwingStartSerial.load(std::memory_order_acquire)
                    : m_PhysicalShoveSerial.load(std::memory_order_acquire)) - s_ShoveBaseline;
                const int frames = static_cast<int>(m_VmStrikeFrames.load(std::memory_order_acquire)) - m_SelfTestValueA;
                const int held = static_cast<int>(m_VmStrikeFramesHeld.load(std::memory_order_acquire)) - m_SelfTestValueB;
                const int shown = static_cast<int>(m_VmStrikeFramesShown.load(std::memory_order_acquire)) - m_SelfTestValueC;
                std::snprintf(detail, sizeof(detail), "%u %s, server shove aim %s", strikes, melee ? "swing(s)" : "bash(es)",
                    ServerFeature(l4d2vr_features::ShoveAim) ? "on (impulse 238)" : "off (IN_ATTACK2)");
                SelfTestReport(melee ? "swinganim: motion swing registered" : "bashanim: gun bash registered", strikes >= 1u, detail);
                std::snprintf(detail, sizeof(detail),
                    "native strike pose on %d draw(s) (up to %.1f units off the idle pose): %d held at the controller pose, %d shown (see [VR][VmAnim])",
                    frames, m_VmStrikeMaxDeviation.load(std::memory_order_acquire), held, shown);
                if (m_VmStrikeHoldDisabled)
                {
                    // Control run: with the hold off the native animation must show,
                    // or the held runs prove nothing.
                    SelfTestReport(melee ? "swinganimraw: control, native swing animation plays when not held"
                        : "bashanimraw: control, native shove animation plays when not held", frames > 0 && held == 0, detail);
                }
                else
                {
                    SelfTestReport(melee ? "swinganim: native swing animation never shown" : "bashanim: native shove animation never shown",
                        shown == 0, detail);
                }
                restore();
                return true; });
        }
        else if (step == "bashmotion")
        {
            // Artificial locomotion through the real input paths (the right
            // stick at full deflection, smooth then snap turning, the walk
            // stick) with the gun held still must never bash; real pushes and
            // sideways sweeps during it must, once each.
            cmd("give rifle");
            auto stick = [this](float x) { m_SelfTestTurnStickActive = true; m_SelfTestTurnStickX = x; };
            auto walk = [this](float y) { m_SelfTestWalkActive = y != 0.0f; m_SelfTestWalkX = 0.0f; m_SelfTestWalkY = y; };
            auto measure = [this](C_BasePlayer* player, bool on) {
                s_Measure = on;
                s_OwnsLocomotion = true;
                s_LastYaw = m_RotationOffset;
                if (player) s_LastOrigin = player->GetAbsOrigin();
                if (on) { s_TurnedDeg = 0.0f; s_Walked = 0.0f; s_Snaps = 0; }
            };
            add(1.2f, [this, cmd](C_BasePlayer*, float) {
                cmd("use weapon_rifle");
                m_SnapTurning = false;
                SelfTestHoldRight(body::RightReady(), QAngle(0.0f, 0.0f, 0.0f));
                return false; });
            // Negatives: 2.5 s of full right stick, 2.5 s of full left stick,
            // ten snap turns, 3 s of walking at full speed while turning, aiming.
            add(2.5f, [this, measure, stick](C_BasePlayer* player, float) {
                s_ShoveBaseline = m_PhysicalShoveSerial.load(std::memory_order_acquire);
                measure(player, true);
                stick(1.0f);
                return false; });
            add(4.0f, [this](C_BasePlayer*, float) { SelfTestRequestShot("bashmotion_turning", 1); return false; });
            add(5.0f, [stick](C_BasePlayer*, float) { stick(-1.0f); return false; });
            add(7.5f, [this, stick](C_BasePlayer*, float) { stick(0.0f); m_SnapTurning = true; return false; });
            for (int i = 0; i < 10; ++i)
            {
                const float at = 7.8f + 0.3f * i;
                const float x = (i % 4 < 2) ? 1.0f : -1.0f;
                add(at, [stick, x](C_BasePlayer*, float) { stick(x); return false; });
                add(at + 0.15f, [stick](C_BasePlayer*, float) { stick(0.0f); return false; });
            }
            add(10.8f, [this, stick, walk](C_BasePlayer*, float) {
                m_SnapTurning = false;
                stick(0.7f);
                walk(1.0f);
                return false; });
            add(12.3f, [stick](C_BasePlayer*, float) { stick(-0.7f); return false; });
            add(12.8f, [this](C_BasePlayer*, float) { SelfTestRequestShot("bashmotion_walkturn", 1); return false; });
            add(13.8f, [this, stick, walk, level](C_BasePlayer*, float) {
                stick(0.0f);
                walk(0.0f);
                // Aim sweeps: slow tracking left, then a quicker re-aim right.
                SelfTestReachRight(body::V(0.42f, -0.10f, -0.33f), level(0.0f, 25.0f), 0.6f);
                return false; });
            add(15.0f, [this, level](C_BasePlayer*, float) {
                SelfTestReachRight(body::V(0.42f, 0.40f, -0.33f), level(0.0f, -30.0f), 1.0f); return false; });
            add(15.9f, [this, level](C_BasePlayer*, float) { SelfTestReachRight(body::RightReady(), level(0.0f, 0.0f), 0.8f); return false; });
            add(17.0f, [this, measure](C_BasePlayer* player, float) {
                char detail[256];
                const uint32_t shoves = m_PhysicalShoveSerial.load(std::memory_order_acquire) - s_ShoveBaseline;
                std::snprintf(detail, sizeof(detail), "%u shove(s) while turning %.0f deg with the stick (%d snap turns), walking %.1f m and aiming",
                    shoves, s_TurnedDeg, s_Snaps, s_Walked / 43.2f);
                SelfTestReport("bashmotion: no bash from stick turning, snap turning, walking or aiming",
                    shoves == 0u && s_TurnedDeg > 900.0f && s_Snaps >= 8 && s_Walked > 43.2f * 2.0f, detail);
                measure(player, true);
                s_ShoveBaseline = m_PhysicalShoveSerial.load(std::memory_order_acquire);
                return false; });
            // Positives: a push while smooth turning at full stick, a sweep while
            // turning, a push while walking and snap turning, a sweep while walking.
            add(17.2f, [stick](C_BasePlayer*, float) { stick(1.0f); return false; });
            add(18.2f, [this, level](C_BasePlayer*, float) {
                SelfTestReachRight(body::Push(), level(0.0f, 0.0f), 2.0f); return false; });
            add(18.7f, [this, level](C_BasePlayer*, float) {
                SelfTestReachRight(body::V(0.38f, 0.30f, -0.30f), level(0.0f, -35.0f), 0.6f); return false; });
            add(20.0f, [this, level](C_BasePlayer*, float) {
                // 0.45 m right to left in 0.25 s (peak about 3.4 m/s).
                SelfTestReachRight(body::V(0.42f, -0.15f, -0.32f), level(0.0f, 35.0f), 1.8f); return false; });
            add(20.6f, [this, stick, walk, level](C_BasePlayer*, float) {
                stick(0.0f);
                m_SnapTurning = true;
                walk(1.0f);
                SelfTestReachRight(body::RightReady(), level(0.0f, 0.0f), 0.6f);
                return false; });
            for (int i = 0; i < 4; ++i)
            {
                const float at = 21.5f + 0.5f * i;
                add(at, [stick](C_BasePlayer*, float) { stick(1.0f); return false; });
                add(at + 0.15f, [stick](C_BasePlayer*, float) { stick(0.0f); return false; });
            }
            add(22.2f, [this, level](C_BasePlayer*, float) {
                SelfTestReachRight(body::Push(), level(0.0f, 0.0f), 2.0f); return false; });
            add(22.7f, [this, level](C_BasePlayer*, float) {
                SelfTestReachRight(body::V(0.40f, -0.15f, -0.30f), level(0.0f, 35.0f), 0.6f); return false; });
            add(24.0f, [this, level](C_BasePlayer*, float) {
                // Left to right, 0.50 m in 0.25 s.
                SelfTestReachRight(body::V(0.38f, 0.35f, -0.35f), level(0.0f, -35.0f), 2.0f); return false; });
            add(25.0f, [this, restore, level](C_BasePlayer*, float) {
                char detail[256];
                const uint32_t shoves = m_PhysicalShoveSerial.load(std::memory_order_acquire) - s_ShoveBaseline;
                std::snprintf(detail, sizeof(detail), "%u bash(es) for 4 strikes (2 pushes, 2 sweeps) while turning %.0f deg (%d snap turns) and walking %.1f m",
                    shoves, s_TurnedDeg, s_Snaps, s_Walked / 43.2f);
                SelfTestReport("bashmotion: pushes and sweeps during locomotion bash once each", shoves == 4u, detail);
                SelfTestReachRight(body::RightReady(), level(0.0f, 0.0f), 0.6f);
                restore();
                return true; });
        }
        // Actions run in time order, whatever order they were added in.
        std::stable_sort(s_Script.begin(), s_Script.end(),
            [](const Action& a, const Action& b) { return a.at < b.at; });
        m_SelfTestStage = 1;
    }

    // bashmotion: measure the turning and walking that really happened.
    if (s_Measure && player)
    {
        float step = m_RotationOffset - s_LastYaw;
        step -= 360.0f * std::floor((step + 180.0f) / 360.0f);
        s_TurnedDeg += std::fabs(step);
        if (std::fabs(step) >= 20.0f)
            ++s_Snaps;
        s_LastYaw = m_RotationOffset;
        const Vector origin = player->GetAbsOrigin();
        const float moved = (origin - s_LastOrigin).Length();
        if (moved < 100.0f)
            s_Walked += moved;
        s_LastOrigin = origin;
    }

    // Crouched world reach toward the dropped kit (pickup step).
    if (s_WorldReachStart >= 0.0f && t >= s_WorldReachStart)
    {
        const float u = MinimumJerk((t - s_WorldReachStart) / 0.9f);
        m_SelfTestPathActive = false;
        m_SelfTestRightWorld = m_SelfTestSlapStart + (m_SelfTestSlapEnd - m_SelfTestSlapStart) * u;
        m_SelfTestRightWorldValid = true;
    }

    while (m_SelfTestStage - 1 < static_cast<int>(s_Script.size()) && t >= s_Script[m_SelfTestStage - 1].at)
    {
        const Action action = s_Script[m_SelfTestStage - 1];
        ++m_SelfTestStage;
        if (action.run(player, t))
        {
            s_Script.clear();
            return true;
        }
    }
    return false;
}

// Hand-driven reload tests. Every stage goes through the real off-hand path:
// the hand is moved onto the published boxes (bolt or slide, the waist pouch,
// the magazine well or loading port, all in world units) and the grip and
// trigger are pressed like a player would; nothing calls the server
// transactions directly. Steps:
//   mage2e=<give name>    detachable magazine: run dry, rack without a new
//                         magazine (no refill), release, rack with the
//                         magazine out (no refill), take a spare from the
//                         waist, seat it, rack the bolt or slide, fire.
//   shellse2e=<give name> shotgun: fire one, (pump), take a shell from the
//                         waist, load it; three times, the second time with
//                         the shell loaded before the pump is cycled.
//   slape2e               AK-47: take a spare with the old magazine seated,
//                         slap the old one out, seat the spare, fire.
//   sparereturne2e        AK-47: take a spare with the old magazine seated
//                         and let it go: nothing changes, the gun still
//                         fires. Follow it with mage2e=rifle (a gun switch
//                         after the spare went back must not grant a round).
bool VR::RunReloadE2EStep(const std::string& step, float t, C_BasePlayer* player)
{
    (void)t;
    (void)player;
    const uint64_t nowMs = GetTickCount64();
    auto cmd = [&](const char* text) { if (m_Game) m_Game->ClientCmd_Unrestricted(text); };
    auto button = [&](vr::VRActionHandle_t action, bool down) { l4d2vr_selftest::SetButton(action, down); };
    // Magazines, shells, bolts and pumps: the free hand's trigger with trigger
    // interactions (Pavlov), else its grip.
    const vr::VRActionHandle_t reloadHand = m_MagazineInteractionTriggerInteractions
        ? m_ActionSecondaryAttack : m_ActionOffHandGrip;
    const float scale = (std::max)(0.001f, m_VRScale);
    char detail[320];
    char name[96];

    // pumpredrawe2e=<pumpshotgun|shotgun_chrome>: fire one, switch to the
    // pistol and back with the pump still waiting; the drawn gun must stay in
    // the hand with its pump where it was, the pump must cycle and it must fire.
    const bool redraw = step.rfind("pumpredrawe2e=", 0) == 0;
    const bool shells = step.rfind("shellse2e=", 0) == 0 || redraw;
    // emptywatche2e=<gun>: run dry, then leave the gun alone and watch for a
    // refill nobody made (the host's stock auto reload).
    const bool watch = step.rfind("emptywatche2e=", 0) == 0;
    const bool magazine = watch || step.rfind("mage2e=", 0) == 0 || step.rfind("reloadgatee2e=", 0) == 0;
    // reloadgatee2e=<gun>: fire a few rounds, then press the stock reload; the
    // host's reload gate must refuse it while physical reload owns the gun.
    const bool gateProbe = step.rfind("reloadgatee2e=", 0) == 0;
    const bool spareReturn = step == "sparereturne2e";
    const bool slap = step == "slape2e" || spareReturn;
    // magholde2e=<gun>: the spare in a natural insert pose (see below).
    const bool hold = step.rfind("magholde2e=", 0) == 0;
    // magpulle2e=<gun>: pull the seated magazine out with the free hand's
    // trigger (slap guns), put it back, pull it again and drop it, reload.
    const bool pullStep = step.rfind("magpulle2e=", 0) == 0;
    // freetriggere2e=<gun>: the free hand's trigger never fires or shoves, and
    // its grip takes no magazine (trigger interactions).
    const bool freeTrigger = step.rfind("freetriggere2e=", 0) == 0;
    // slapmotione2e=<motion>: one AK-47 strike of a given direction and speed;
    // down, forward, diag, cant (gun canted 50 deg, struck straight down),
    // cantalong (canted, along the magazine) and fast slap the old magazine
    // out; slowinsert, slowdown, up and wide must not.
    const bool slapMotion = step.rfind("slapmotione2e=", 0) == 0;
    if (!shells && !magazine && !slap && !hold && !pullStep && !freeTrigger && !slapMotion)
    {
        SelfTestReport(step.c_str(), false, "unknown step");
        return true;
    }
    const std::string gun = (slap || slapMotion) ? std::string("rifle_ak47") : step.substr(step.find('=') + 1);
    const std::string motion = slapMotion ? step.substr(step.find('=') + 1) : std::string();
    int expectedId = 0;
    if (gun == "rifle") expectedId = 5;
    else if (gun == "pistol") expectedId = 1;
    else if (gun == "pistol_magnum") expectedId = 32;
    else if (gun == "smg") expectedId = 2;
    else if (gun == "rifle_ak47") expectedId = 26;
    else if (gun == "pumpshotgun") expectedId = 3;
    else if (gun == "shotgun_chrome") expectedId = 8;
    else if (gun == "autoshotgun") expectedId = 4;
    else if (gun == "shotgun_spas") expectedId = 11;
    else if (gun == "smg_silenced") expectedId = 7;
    else if (gun == "smg_mp5") expectedId = 33;
    else if (gun == "rifle_desert") expectedId = 9;
    else if (gun == "rifle_sg552") expectedId = 34;
    else if (gun == "sniper_military") expectedId = 10;
    else if (gun == "hunting_rifle") expectedId = 6;
    if (!expectedId)
    {
        SelfTestReport(step.c_str(), false, "unknown weapon");
        return true;
    }
    const bool pump = expectedId == 3 || expectedId == 8;
    const bool shellGun = pump || expectedId == 4 || expectedId == 11;
    const bool semiAuto = expectedId == 1 || expectedId == 32;

    ReloadSelfTestGeometry g{};
    GetReloadSelfTestGeometry(g);
    const float inPhase = (nowMs - m_SelfTestPhaseStartMs) / 1000.0f;
    auto go = [&](int next) { m_SelfTestPhase = next; m_SelfTestPhaseStartMs = nowMs; };
    auto label = [&](const char* text)
    {
        std::snprintf(name, sizeof(name), "%s %s", gun.c_str(), text);
        return name;
    };
    auto finish = [&]()
    {
        button(m_ActionOffHandGrip, false);
        button(m_ActionSecondaryAttack, false);
        button(m_ActionPrimaryAttack, false);
        button(m_ActionMagazineRelease, false);
        return true;
    };
    auto fail = [&](const char* what, const char* why)
    {
        SelfTestReport(label(what), false, why);
        return finish();
    };
    auto state = [&]() { return l4d2vr_reload_log::StateName(g.state); };
    // The off hand glides between targets (world units, linear).
    auto leftNow = [&]() { return m_SelfTestLeftWorldValid ? m_SelfTestLeftWorld : m_LeftControllerPosAbs; };
    auto glide = [&](const Vector& to, float seconds)
    {
        m_SelfTestGlideFrom = leftNow();
        m_SelfTestGlideTo = to;
        m_SelfTestGlideStartMs = nowMs;
        m_SelfTestGlideMs = static_cast<uint64_t>((std::max)(0.01f, seconds) * 1000.0f);
        m_SelfTestLeftWorldValid = true;
    };
    if (m_SelfTestGlideMs)
    {
        const float u = (std::min)(1.0f, (nowMs - m_SelfTestGlideStartMs) / static_cast<float>(m_SelfTestGlideMs));
        m_SelfTestLeftWorld = m_SelfTestGlideFrom + (m_SelfTestGlideTo - m_SelfTestGlideFrom) * u;
        m_SelfTestLeftWorldValid = true;
    }
    auto glideDone = [&](float settle) { return !m_SelfTestGlideMs || nowMs >= m_SelfTestGlideStartMs + m_SelfTestGlideMs + static_cast<uint64_t>(settle * 1000.0f); };
    auto stopGlide = [&]() { m_SelfTestGlideMs = 0; };
    auto meters = [&](const Vector& a, const Vector& b) { return (a - b).Length() / scale; };
    // Drives the held magazine or shell (its centre) onto `target`, turning
    // the hand so a magazine lines up with the well. Returns the residual.
    // The held pose trails the scripted hand by a frame or more, so the hand
    // turns only once the last turn shows in the held pose; turning every
    // frame against the stale pose flips the hand back and forth (a magazine
    // held 180 degrees off its well never settles).
    auto steerHeld = [&](const Vector& target, bool align, float& errorDeg)
    {
        stopGlide();
        errorDeg = 0.0f;
        if (align)
        {
            QAngle aligned{};
            if (SelfTestAlignHeldMagazine(m_SelfTestLeftAngWorld, aligned, errorDeg) &&
                (!m_SelfTestLeftAngWorldValid || nowMs >= m_SelfTestAlignAtMs + 200u))
            {
                m_SelfTestLeftAngWorld = aligned;
                m_SelfTestLeftAngWorldValid = true;
                m_SelfTestAlignAtMs = nowMs;
            }
        }
        if (!g.heldValid)
            return -1.0f;
        m_SelfTestLeftWorld = leftNow() + (target - g.heldCenter) * 0.5f;
        m_SelfTestLeftWorldValid = true;
        return meters(target, g.heldCenter);
    };
    auto pullBack = [&](float metersBack) { return leftNow() - g.gunForward * (metersBack * scale); };

    // ---- Common start: draw the gun -------------------------------------------
    if (m_SelfTestPhase == 0)
    {
        button(m_ActionOffHandGrip, false);
        button(m_ActionSecondaryAttack, false);
        button(m_ActionMagazineRelease, false);
        button(m_ActionPrimaryAttack, false);
        m_SelfTestLeftAngWorldValid = false;
        if (gun != "pistol")
        {
            // Another gun first, so the step always starts with a new gun of its
            // kind (a magazine an earlier step left out stays with the old one).
            std::snprintf(detail, sizeof(detail), "give %s", gun == "smg" ? "rifle" : "smg");
            cmd(detail);
            std::snprintf(detail, sizeof(detail), "give %s", gun.c_str());
            cmd(detail);
        }
        go(1);
        return false;
    }
    if (m_SelfTestPhase == 1)
    {
        if (inPhase < 1.0f) return false;
        std::snprintf(detail, sizeof(detail), "use weapon_%s", gun.c_str());
        cmd(detail);
        SelfTestHoldRight(SelfTestVec(0.4f, 0.2f, -0.25f), QAngle(0.0f, 0.0f, 0.0f));
        go(2);
        return false;
    }
    if (m_SelfTestPhase == 2)
    {
        // Boxes are published by the viewmodel draw: wait for fresh ones.
        const bool ready = g.weaponId == expectedId && g.magazineBoxFresh &&
            (!shellGun || g.state == static_cast<int>(MagazineInteractionManualState::WaitingForFreshMagazine)) &&
            (shellGun || g.state == 0);
        if (!ready)
        {
            if (inPhase < 6.0f) return false;
            std::snprintf(detail, sizeof(detail), "weapon=%d (want %d) magazine box fresh=%d age=%.2fs model=%s state=%s",
                g.weaponId, expectedId, g.magazineBoxFresh ? 1 : 0, g.magazineBoxAge, g.modelName.c_str(), state());
            return fail("ready", detail);
        }
        m_SelfTestClip0 = g.clip;
        m_SelfTestReserve0 = g.reserve;
        m_SelfTestCycle = 0;
        m_SelfTestMeasureB = 0.0f;
        m_SelfTestValueC = 0; // shells: shots the host did not fire
        m_SelfTestValueD = 0;
        Game::logMsg("[VR][SelfTest] %s: clip=%d reserve=%d box age=%.3fs model=%s", step.c_str(), g.clip, g.reserve,
            g.magazineBoxAge, g.modelName.c_str());
        go(hold ? 300 : pullStep ? 400 : freeTrigger ? 500 : slapMotion ? 600 : magazine ? 10 : redraw ? 400 : shells ? 100 : 200);
        return false;
    }

    // Shared by the hand-pull and motion steps: push the held magazine into the
    // well (lined up below it first); true once it left the hand.
    auto seatHeld = [&](float& closest, float& misalignment) -> bool
    {
        if (g.state != static_cast<int>(MagazineInteractionManualState::HoldingFreshMagazine))
            return true;
        if (!g.socketValid || !g.heldValid)
            return false;
        const float approach = (std::min)(1.0f, (std::max)(0.0f, (inPhase - 0.7f) / 0.5f));
        const Vector target = g.socketCenter + g.socketOutward * ((1.0f - approach) * 0.10f * scale);
        float error = 0.0f;
        steerHeld(target, true, error);
        closest = (std::min)(closest, meters(g.socketCenter, g.heldCenter));
        if (error > 0.0f) misalignment = error;
        return false;
    };
    const int idleState = static_cast<int>(MagazineInteractionManualState::Idle);
    const int holdingSpareState = static_cast<int>(MagazineInteractionManualState::HoldingFreshMagazine);
    const int holdingOldState = static_cast<int>(MagazineInteractionManualState::HoldingOldMagazine);
    const int holdingForSlapState = static_cast<int>(MagazineInteractionManualState::HoldingFreshMagazineForSlap);
    const int waitingSpareState = static_cast<int>(MagazineInteractionManualState::WaitingForFreshMagazine);
    const int waitingServerState = static_cast<int>(MagazineInteractionManualState::WaitingForServerMagazine);

    // ---- Hand pull: the seated magazine out by the free hand's trigger -------------
    if (pullStep)
    {
        auto ejects = [&]() { return m_MagazineEjectSerial.load(std::memory_order_acquire) - m_SelfTestSerial; };
        switch (m_SelfTestPhase)
        {
        case 400: // first the grip on the seated magazine: it must not pull it
            if (inPhase < 0.8f) return false;
            if (!g.magazineBoxFresh)
            {
                if (inPhase < 3.0f) return false;
                return fail("hand pull", "no seated magazine box");
            }
            m_SelfTestSerial = m_MagazineEjectSerial.load(std::memory_order_acquire);
            m_SelfTestSerialB = m_DroppedMagazineSpawnSerial.load(std::memory_order_acquire);
            m_SelfTestClip0 = g.clip;
            m_SelfTestReserve0 = g.reserve;
            m_SelfTestPoint = g.magazineCenter;
            glide(g.magazineCenter, 0.4f);
            go(m_MagazineInteractionTriggerInteractions ? 401 : 404);
            return false;
        case 401:
            if (!glideDone(0.15f)) return false;
            m_SelfTestMeasure = meters(g.supportHand, g.magazineCenter);
            button(m_ActionOffHandGrip, true);
            go(402);
            return false;
        case 402:
            if (inPhase < 0.25f) return false;
            glide(leftNow() + g.socketOutward * (0.15f * scale), 0.3f);
            go(403);
            return false;
        case 403:
            if (!glideDone(0.3f)) return false;
            button(m_ActionOffHandGrip, false);
            std::snprintf(detail, sizeof(detail), "hand %.3f m from the magazine, gripped and pulled 0.15 m: %u ejection(s), clip %d -> %d, state %s, foregrip=%d",
                m_SelfTestMeasure, ejects(), m_SelfTestClip0, g.clip, state(), m_VrHandsTwoHandedGripActive ? 1 : 0);
            // (Holding the gun at the magazine well with the grip may take the
            // two-handed grip; it must never take or pull the magazine.)
            SelfTestReport(label("the grip does not take the seated magazine (trigger interactions)"),
                ejects() == 0u && g.clip == m_SelfTestClip0 && g.state == idleState, detail);
            m_SelfTestValueD = m_VrHandsTwoHandedGripActive ? 1 : 0;
            if (m_SelfTestValueD)
                button(m_ActionOffHandGrip, true); // toggle the foregrip off again
            go(410);
            return false;
        case 410:
            if (inPhase < 0.2f) return false;
            button(m_ActionOffHandGrip, false);
            // Off to the side, then back.
            glide(leftNow() + g.gunRight * (-0.30f * scale), 0.3f);
            go(404);
            return false;
        case 404:
            if (!glideDone(0.3f)) return false;
            m_SelfTestPoint = g.magazineCenter;
            glide(g.magazineCenter, 0.45f);
            go(405);
            return false;
        case 405:
            if (!glideDone(0.15f)) return false;
            if (meters(g.supportHand, g.magazineCenter) > 0.04f && inPhase < 1.2f)
            {
                glide(g.magazineCenter, 0.1f); // the body drifted: follow the gun
                return false;
            }
            m_SelfTestMeasure = meters(g.supportHand, g.magazineCenter);
            m_SelfTestClip0 = g.clip;
            m_SelfTestReserve0 = g.reserve;
            button(reloadHand, true);
            go(406);
            return false;
        case 406:
            if (g.state != holdingOldState)
            {
                if (inPhase < 0.8f) return false;
                std::snprintf(detail, sizeof(detail), "hand %.3f m from the magazine, state %s", m_SelfTestMeasure, state());
                return fail(m_SelfTestCycle == 0 ? "trigger takes hold of the seated magazine" : "trigger takes the magazine again", detail);
            }
            if (m_SelfTestCycle == 0)
            {
                std::snprintf(detail, sizeof(detail), "hand %.3f m from the magazine", m_SelfTestMeasure);
                SelfTestReport(label("trigger takes hold of the seated magazine"), true, detail);
            }
            // Out along the well, a natural 12 cm pull.
            glide(leftNow() + g.socketOutward * (0.12f * scale), 0.35f);
            go(407);
            return false;
        case 407:
        {
            const bool settled = ejects() >= 1u + static_cast<uint32_t>(m_SelfTestCycle) &&
                g.state != waitingServerState && g.state != holdingOldState;
            if (!settled && inPhase < 3.0f) return false;
            const bool held = g.state == holdingSpareState && m_MagazineInteractionRetainedMagazineHeld;
            const bool conserved = g.clip + g.reserve == m_SelfTestClip0 + m_SelfTestReserve0;
            std::snprintf(detail, sizeof(detail), "%u ejection(s), clip %d -> %d, reserve %d -> %d, state %s, held=%d",
                ejects(), m_SelfTestClip0, g.clip, m_SelfTestReserve0, g.reserve, state(), held ? 1 : 0);
            if (m_SelfTestCycle == 0)
            {
                SelfTestReport(label("trigger pull removes the magazine: one ejection, rounds back to the reserve"),
                    ejects() == 1u && g.clip <= 1 && conserved, detail);
                SelfTestReport(label("the pulled magazine stays in the hand"), held, detail);
                SelfTestRequestShot(gun + "_hand_pulled", 1);
                if (!held)
                    return finish();
                m_SelfTestClip0 = g.clip;
                m_SelfTestReserve0 = g.reserve;
                // All the way out (the ejection fires a few centimetres into
                // the pull), clear of where the well catches a magazine.
                glide(leftNow() + g.socketOutward * (0.12f * scale), 0.3f);
                go(420);
                return false;
            }
            // Second pull: let go away from the gun, it drops.
            SelfTestReport(label("trigger pulls it out again"), ejects() == 2u && conserved, detail);
            m_SelfTestSerialB = m_DroppedMagazineSpawnSerial.load(std::memory_order_acquire);
            glide(leftNow() + g.gunRight * (-0.25f * scale) + Vector(0.0f, 0.0f, -0.10f * scale), 0.3f);
            go(411);
            return false;
        }
        case 420:
            if (!glideDone(0.2f)) return false;
            m_SelfTestLeftAngWorld = m_LeftControllerTrackedAngAbs;
            m_SelfTestLeftAngWorldValid = true;
            m_SelfTestMeasure = 1e9f;
            m_SelfTestMeasureB = 180.0f;
            go(408);
            return false;
        case 408: // put it back
        {
            if (inPhase < 0.4f) return false;
            if (!seatHeld(m_SelfTestMeasure, m_SelfTestMeasureB))
            {
                if (inPhase < 6.0f) return false;
                std::snprintf(detail, sizeof(detail), "still held %.3f m from the well, %.1f deg off", m_SelfTestMeasure, m_SelfTestMeasureB);
                return fail("the pulled magazine goes back in", detail);
            }
            go(409);
            return false;
        }
        case 409:
            if ((g.state == waitingServerState || g.clip <= m_SelfTestClip0) && inPhase < 3.0f) return false;
            button(reloadHand, false);
            m_SelfTestLeftAngWorldValid = false;
            std::snprintf(detail, sizeof(detail), "magazine %.3f m from the well: clip %d -> %d, reserve %d -> %d, state %s",
                m_SelfTestMeasure, m_SelfTestClip0, g.clip, m_SelfTestReserve0, g.reserve, state());
            SelfTestReport(label("the pulled magazine goes back in, nothing lost"), g.clip > m_SelfTestClip0 &&
                g.clip + g.reserve == m_SelfTestClip0 + m_SelfTestReserve0, detail);
            SelfTestRequestShot(gun + "_hand_pull_reseated", 1);
            m_SelfTestCycle = 1;
            glide(leftNow() + g.gunRight * (-0.30f * scale), 0.3f);
            go(404);
            return false;
        case 411: // let go of the second one away from the gun
            if (!glideDone(0.1f)) return false;
            button(reloadHand, false);
            go(412);
            return false;
        case 412:
        {
            if (inPhase < 0.6f) return false;
            const uint32_t drops = m_DroppedMagazineSpawnSerial.load(std::memory_order_acquire) - m_SelfTestSerialB;
            std::snprintf(detail, sizeof(detail), "%u dropped, state %s, clip %d reserve %d", drops, state(), g.clip, g.reserve);
            SelfTestReport(label("letting go drops the pulled magazine"), g.state == waitingSpareState &&
                (drops == 1u || !m_MagazineDropEnabled), detail);
            if (!g.pickupValid)
                return fail("spare after the hand pull", "no pouch position");
            m_SelfTestClip0 = g.clip;
            m_SelfTestReserve0 = g.reserve;
            glide(g.pickupCenter, 0.5f);
            go(413);
            return false;
        }
        case 413:
            if (!glideDone(0.2f)) return false;
            button(reloadHand, true);
            go(414);
            return false;
        case 414:
            if (g.state != holdingSpareState)
            {
                if (inPhase < 1.0f) return false;
                std::snprintf(detail, sizeof(detail), "state %s", state());
                return fail("spare after the hand pull", detail);
            }
            m_SelfTestLeftAngWorld = m_LeftControllerTrackedAngAbs;
            m_SelfTestLeftAngWorldValid = true;
            m_SelfTestMeasure = 1e9f;
            m_SelfTestMeasureB = 180.0f;
            go(415);
            return false;
        case 415:
            if (inPhase < 0.3f) return false;
            if (!seatHeld(m_SelfTestMeasure, m_SelfTestMeasureB))
            {
                if (inPhase < 6.0f) return false;
                return fail("spare seated after the hand pull", "still held");
            }
            go(416);
            return false;
        case 416:
            if ((g.state == waitingServerState || g.clip <= m_SelfTestClip0) && inPhase < 3.0f) return false;
            button(reloadHand, false);
            m_SelfTestLeftAngWorldValid = false;
            std::snprintf(detail, sizeof(detail), "clip %d -> %d, reserve %d -> %d, state %s",
                m_SelfTestClip0, g.clip, m_SelfTestReserve0, g.reserve, state());
            SelfTestReport(label("spare seated after the hand pull"), g.clip > m_SelfTestClip0 &&
                g.clip + g.reserve == m_SelfTestClip0 + m_SelfTestReserve0, detail);
            m_SelfTestClip0 = g.clip;
            glide(leftNow() + Vector(0.0f, 0.0f, -0.15f * scale), 0.3f);
            go(417);
            return false;
        case 417:
            if (inPhase < 0.5f) return false;
            if (g.state != idleState)
            {
                // An empty chamber wants the rack first (the round went with a magazine).
                if (inPhase < 1.5f) return false;
            }
            button(m_ActionPrimaryAttack, true);
            go(418);
            return false;
        case 418:
            if (inPhase < 0.15f) return false;
            button(m_ActionPrimaryAttack, false);
            go(419);
            return false;
        case 419:
            if (inPhase < 0.6f) return false;
            std::snprintf(detail, sizeof(detail), "clip %d -> %d, state %s", m_SelfTestClip0, g.clip, state());
            SelfTestReport(label("fires after the hand-pull reload"), g.clip < m_SelfTestClip0, detail);
            return finish();
        default:
            return fail("phase", "lost");
        }
    }

    // ---- The free hand's trigger never fires or shoves ---------------------------
    if (freeTrigger)
    {
        switch (m_SelfTestPhase)
        {
        case 500:
            if (inPhase < 0.8f) return false;
            m_SelfTestClip0 = g.clip;
            m_SelfTestReserve0 = g.reserve;
            m_SelfTestSerial = m_FreeHandTriggerLeakSerial.load(std::memory_order_acquire);
            m_SelfTestValueC = 0; // +attack2 seen while the trigger was down
            m_SelfTestValueD = 0;
            button(m_ActionSecondaryAttack, true); // in the air, beside the gun
            go(501);
            return false;
        case 501:
            if (m_SecondaryAttackCmdOwned || m_PrimaryAttackCmdOwned) m_SelfTestValueC = 1;
            if (inPhase < 0.6f) return false;
            button(m_ActionSecondaryAttack, false);
            // Then with the hand on the seated magazine and on the bolt.
            glide(g.magazineCenter + g.socketOutward * (0.03f * scale), 0.4f);
            go(502);
            return false;
        case 502:
            if (m_SecondaryAttackCmdOwned || m_PrimaryAttackCmdOwned) m_SelfTestValueC = 1;
            if (!glideDone(0.1f)) return false;
            button(m_ActionSecondaryAttack, true);
            go(503);
            return false;
        case 503:
            if (m_SecondaryAttackCmdOwned || m_PrimaryAttackCmdOwned) m_SelfTestValueC = 1;
            if (inPhase < 0.4f) return false;
            // Let go before moving: on the AK-47 the trigger holds the magazine,
            // and moving off while still holding it pulls it out.
            button(m_ActionSecondaryAttack, false);
            go(511);
            return false;
        case 511:
            if (m_SecondaryAttackCmdOwned || m_PrimaryAttackCmdOwned) m_SelfTestValueC = 1;
            if (inPhase < 0.25f) return false;
            if (g.boltBoxFresh)
                glide(g.boltCenter, 0.35f);
            go(504);
            return false;
        case 504:
            if (m_SecondaryAttackCmdOwned || m_PrimaryAttackCmdOwned) m_SelfTestValueC = 1;
            if (!glideDone(0.1f)) return false;
            button(m_ActionSecondaryAttack, true);
            go(505);
            return false;
        case 505:
        {
            if (m_SecondaryAttackCmdOwned || m_PrimaryAttackCmdOwned) m_SelfTestValueC = 1;
            if (inPhase < 0.4f) return false;
            button(m_ActionSecondaryAttack, false);
            if (inPhase < 1.2f) return false;
            // A hand pull on the AK-47 puts the magazine back by letting go
            // early (under the pull distance): nothing may have changed.
            const uint32_t leaks = m_FreeHandTriggerLeakSerial.load(std::memory_order_acquire) - m_SelfTestSerial;
            std::snprintf(detail, sizeof(detail), "pressed in the air, on the magazine and on the bolt: clip %d -> %d, reserve %d -> %d, "
                "fire/shove command seen=%d, leaks=%u, state %s", m_SelfTestClip0, g.clip, m_SelfTestReserve0, g.reserve,
                m_SelfTestValueC, leaks, state());
            SelfTestReport(label("the free hand's trigger never fires or shoves"),
                g.clip == m_SelfTestClip0 && g.reserve == m_SelfTestReserve0 && m_SelfTestValueC == 0 && leaks == 0u, detail);
            if (!g.pickupValid)
                return fail("grip at the pouch", "no pouch position");
            glide(g.pickupCenter, 0.5f);
            go(506);
            return false;
        }
        case 506:
            if (!glideDone(0.2f)) return false;
            m_SelfTestMeasure = meters(g.supportHand, g.pickupCenter);
            button(m_ActionOffHandGrip, true);
            go(507);
            return false;
        case 507:
            if (g.state != idleState) m_SelfTestValueD = g.state;
            if (inPhase < 0.8f) return false;
            button(m_ActionOffHandGrip, false);
            std::snprintf(detail, sizeof(detail), "hand %.3f m from the pouch (range %.2f m): state while gripping %s, now %s",
                m_SelfTestMeasure, g.pouchRange, l4d2vr_reload_log::StateName(m_SelfTestValueD), state());
            if (m_MagazineInteractionTriggerInteractions)
                SelfTestReport(label("the grip takes no magazine from the pouch (trigger interactions)"),
                    m_SelfTestValueD == 0 && g.state == idleState, detail);
            go(508);
            return false;
        case 508: // the gun hand's trigger still fires
            if (inPhase < 0.6f) return false;
            m_SelfTestClip0 = g.clip;
            glide(leftNow() + g.gunRight * (-0.25f * scale), 0.3f);
            button(m_ActionPrimaryAttack, true);
            go(509);
            return false;
        case 509:
            if (inPhase < 0.15f) return false;
            button(m_ActionPrimaryAttack, false);
            go(510);
            return false;
        case 510:
            if (inPhase < 0.6f) return false;
            std::snprintf(detail, sizeof(detail), "clip %d -> %d", m_SelfTestClip0, g.clip);
            SelfTestReport(label("the gun hand's trigger still fires"), g.clip < m_SelfTestClip0, detail);
            return finish();
        default:
            return fail("phase", "lost");
        }
    }

    // ---- One AK-47 slap motion ---------------------------------------------------------
    if (slapMotion)
    {
        struct Motion { const char* name; float speed; bool canted; bool hit; };
        static const Motion kMotions[] = {
            { "down", 1.2f, false, true }, { "forward", 1.0f, false, true }, { "diag", 0.9f, false, true },
            { "fast", 2.5f, false, true }, { "cant", 1.0f, true, true }, { "cantalong", 0.8f, true, true },
            { "slowinsert", 0.3f, false, false }, { "slowdown", 0.25f, false, false },
            { "up", 1.5f, false, false }, { "wide", 1.5f, false, false }, { "stock", 1.5f, false, false },
        };
        const Motion* m = nullptr;
        for (const Motion& candidate : kMotions)
            if (motion == candidate.name)
                m = &candidate;
        if (!m)
            return fail("slap motion", "unknown motion");
        // The strike direction in the world, from the seated magazine's outward
        // axis and the barrel (square to it).
        auto strikeDirection = [&]() -> Vector
        {
            Vector out = g.socketOutward;
            Vector forward = g.gunForward - out * DotProduct(g.gunForward, out);
            if (forward.Length() > 0.001f) forward = forward * (1.0f / forward.Length());
            Vector dir = out;
            if (motion == "forward") dir = forward;
            else if (motion == "stock") dir = forward * -1.0f;
            else if (motion == "diag") dir = out + forward;
            else if (motion == "cant") dir = Vector(0.0f, 0.0f, -1.0f);
            else if (motion == "slowinsert" || motion == "up") dir = out * -1.0f;
            const float len = dir.Length();
            return len > 0.001f ? dir * (1.0f / len) : out;
        };
        auto slaps = [&]() { return m_MagazineSlapSerial.load(std::memory_order_acquire) - static_cast<uint32_t>(m_SelfTestValueA); };
        auto ejects = [&]() { return m_MagazineEjectSerial.load(std::memory_order_acquire) - m_SelfTestSerial; };
        // The old magazine on its way out: it drops when the host confirms the
        // ejection; photographed 0.05 s and 0.2 s later.
        auto slapShots = [&]()
        {
            if (m_SelfTestValueD == 1 && ejects() > 0u)
            {
                m_SelfTestSerialB = static_cast<uint32_t>(nowMs);
                m_SelfTestValueD = 2;
            }
            const uint32_t since = static_cast<uint32_t>(nowMs) - m_SelfTestSerialB;
            if (m_SelfTestValueD == 2 && since >= 50u)
            {
                SelfTestRequestShot(gun + "_slap_" + motion + "_flying1", 1);
                m_SelfTestValueD = 3;
            }
            else if (m_SelfTestValueD == 3 && since >= 200u)
            {
                SelfTestRequestShot(gun + "_slap_" + motion + "_flying2", 1);
                m_SelfTestValueD = 4;
            }
        };
        switch (m_SelfTestPhase)
        {
        case 600:
            // Canted: the gun rolled 50 degrees (the user's logged outward axes
            // lean 25-75 degrees off vertical).
            SelfTestHoldRight(SelfTestVec(0.4f, 0.2f, -0.25f), QAngle(0.0f, 0.0f, m->canted ? 50.0f : 0.0f));
            go(601);
            return false;
        case 601:
            if (inPhase < 1.2f) return false;
            if (!g.pickupValid || !g.magazineBoxFresh)
            {
                if (inPhase < 3.0f) return false;
                return fail("slap motion", "no pouch or seated magazine");
            }
            m_SelfTestSerial = m_MagazineEjectSerial.load(std::memory_order_acquire);
            m_SelfTestValueA = static_cast<int>(m_MagazineSlapSerial.load(std::memory_order_acquire));
            m_SelfTestValueB = static_cast<int>(m_MagazineSlapNearMissSerial.load(std::memory_order_acquire));
            m_SelfTestValueD = 0; // screenshots of the slapped magazine taken
            m_SelfTestClip0 = g.clip;
            m_SelfTestReserve0 = g.reserve;
            glide(g.pickupCenter, 0.5f);
            go(602);
            return false;
        case 602:
            if (!glideDone(0.2f)) return false;
            button(reloadHand, true);
            go(603);
            return false;
        case 603:
            if (g.state != holdingForSlapState)
            {
                if (inPhase < 1.0f) return false;
                std::snprintf(detail, sizeof(detail), "state %s", state());
                return fail("slap motion: spare taken with the old magazine seated", detail);
            }
            m_SelfTestLeftAngWorld = m_LeftControllerTrackedAngAbs;
            m_SelfTestLeftAngWorldValid = true;
            m_SelfTestMeasureB = 180.0f;
            go(604);
            return false;
        case 604:
        {
            // Line the spare up beside the gun (30 cm across the strike).
            const Vector dir = strikeDirection();
            Vector across = CrossProduct(dir, g.gunUp);
            if (across.Length() < 0.2f) across = CrossProduct(dir, g.gunForward);
            across = across * (1.0f / (std::max)(0.001f, across.Length()));
            m_SelfTestPoint = g.magazineCenter;
            if (inPhase < 1.0f || (m_SelfTestMeasureB > 5.0f && inPhase < 3.0f))
            {
                float error = 0.0f;
                steerHeld(g.magazineCenter - dir * (0.12f * scale) + across * (0.30f * scale), true, error);
                m_SelfTestMeasureB = error;
                return false;
            }
            // Hold still a moment: the held pose trails the scripted hand, so
            // the strike is planned from a settled pose (a few centimetres off
            // to the side turns it into a glancing blow).
            stopGlide();
            go(613);
            return false;
        }
        case 613:
        {
            if (inPhase < 0.35f) return false;
            const Vector dir = strikeDirection();
            Vector across = CrossProduct(dir, g.gunUp);
            if (across.Length() < 0.2f) across = CrossProduct(dir, g.gunForward);
            across = across * (1.0f / (std::max)(0.001f, across.Length()));
            m_SelfTestPoint = g.magazineCenter;
            if (!g.heldValid)
                return fail("slap motion", "no held spare pose");
            const Vector contact = leftNow() + (g.magazineCenter - g.heldCenter);
            // Wide: 30 cm beside, clear of the magazine box along any axis.
            const float side = motion == "wide" ? 0.30f : 0.0f;
            // The AK-47's curved magazine box is about 25 cm long and 18 cm
            // deep: every strike starts clear of it, so the strike itself
            // makes the touch (a spare already overlapping the old magazine
            // never slaps, as in Pavlov).
            const float back = 0.35f;
            m_SelfTestMeasureB = back;
            m_SelfTestSlapStart = contact - dir * (back * scale) + across * (side * scale);
            m_SelfTestSlapEnd = contact + dir * (0.05f * scale) + across * (side * scale);
            m_SelfTestSlapPark = across * (0.30f * scale);
            glide(m_SelfTestSlapStart + m_SelfTestSlapPark, 0.2f);
            go(605);
            return false;
        }
        case 605:
            if (!glideDone(0.2f)) return false;
            glide(m_SelfTestSlapStart + (g.magazineCenter - m_SelfTestPoint), 0.6f);
            m_SelfTestStrikeCorrections = 0;
            go(606);
            return false;
        case 606:
            if (!glideDone(0.4f)) return false;
            m_SelfTestMeasure = m->speed;
            if (nowMs - m_MagazineSlapCentersAtMs < 200u && m_SelfTestStrikeCorrections < 6)
            {
                // Line the still spare up with the old magazine across the
                // strike before it starts (a strike that starts off line grazes
                // the side and rests there, which never slaps).
                const Vector dir = strikeDirection();
                Vector across = CrossProduct(dir, g.gunUp);
                if (across.Length() < 0.2f) across = CrossProduct(dir, g.gunForward);
                across = across * (1.0f / (std::max)(0.001f, across.Length()));
                const float side = motion == "wide" ? 0.30f : 0.0f;
                Vector miss = m_MagazineSlapSeatedCenter + across * (side * scale) - m_MagazineSlapHeldCenter;
                const float along = DotProduct(miss, dir);
                miss = miss - dir * along;
                // And clear of it: at least 30 cm back along the strike (the
                // boxes are 25 cm long), never resting against it.
                const float backMissing = (std::max)(0.0f, 0.30f * scale - along);
                if (miss.Length() > 0.008f * scale || backMissing > 0.01f * scale)
                {
                    ++m_SelfTestStrikeCorrections;
                    glide(leftNow() + miss - dir * (backMissing > 0.01f * scale ? backMissing + 0.04f * scale : 0.0f), 0.2f);
                    return false;
                }
            }
            m_SelfTestStrikeCorrections = 0;
            {
                // Aim from where the spare really is now (its pose trails the
                // hand): through the old magazine's centre to 5 cm past it,
                // keeping only the planned sideways offset (wide).
                const Vector dir = strikeDirection();
                Vector across = CrossProduct(dir, g.gunUp);
                if (across.Length() < 0.2f) across = CrossProduct(dir, g.gunForward);
                across = across * (1.0f / (std::max)(0.001f, across.Length()));
                const float side = motion == "wide" ? 0.30f : 0.0f;
                // The boxes the slap itself collides (the published magazine box
                // can sit a few centimetres from the one rebased to this frame).
                const bool collider = nowMs - m_MagazineSlapCentersAtMs < 200u;
                const Vector seatedCenter = collider ? m_MagazineSlapSeatedCenter : g.magazineCenter;
                const Vector heldCenter = collider ? m_MagazineSlapHeldCenter : g.heldCenter;
                const Vector target = seatedCenter + dir * (0.05f * scale) + across * (side * scale);
                const Vector end = (collider || g.heldValid) ? leftNow() + (target - heldCenter)
                    : m_SelfTestSlapEnd + (g.magazineCenter - m_SelfTestPoint);
                Game::logMsg("[VR][SelfTest] slap motion %s: spare %.3f m from the old magazine (collider boxes %d), published box %.3f m from the collided one",
                    m->name, meters(heldCenter, seatedCenter), collider ? 1 : 0, meters(g.magazineCenter, m_MagazineSlapSeatedCenter));
                m_SelfTestSlapEnd = end;
                m_SelfTestPoint = g.magazineCenter;
                // The run-up plus 5 cm past the magazine at the motion's speed
                // against a still gun hand.
                glide(end, meters(end, leftNow()) / m->speed);
            }
            go(607);
            return false;
        case 607:
            if (slaps() > 0u && m_SelfTestValueD == 0)
            {
                SelfTestRequestShot(gun + "_slap_" + motion + "_hit", 1);
                m_SelfTestValueD = 1;
            }
            slapShots();
            if (slaps() == 0u && m_SelfTestGlideMs && !glideDone(0.0f) && m_SelfTestStrikeCorrections < 40 &&
                nowMs - m_MagazineSlapCentersAtMs < 200u)
            {
                // Keep the strike on line, as a hand watching the magazine does:
                // correct only the sideways miss (the held pose trails the hand
                // along the motion), keeping the speed.
                const Vector dir = strikeDirection();
                Vector across = CrossProduct(dir, g.gunUp);
                if (across.Length() < 0.2f) across = CrossProduct(dir, g.gunForward);
                across = across * (1.0f / (std::max)(0.001f, across.Length()));
                const float side = motion == "wide" ? 0.30f : 0.0f;
                const Vector target = m_MagazineSlapSeatedCenter + across * (side * scale);
                Vector miss = (target - m_MagazineSlapHeldCenter);
                miss = miss - dir * DotProduct(miss, dir);
                const Vector planned = m_SelfTestGlideTo - leftNow();
                Vector plannedSide = planned - dir * DotProduct(planned, dir);
                const Vector correction = miss - plannedSide;
                if (correction.Length() > 0.005f * scale)
                {
                    ++m_SelfTestStrikeCorrections;
                    if (meters(m_MagazineSlapHeldCenter, m_MagazineSlapSeatedCenter) < 0.2f)
                        Game::logMsg("[VR][SelfTest] slap motion %s: %.3f m off line %.3f m from the old magazine, corrected",
                            m->name, correction.Length() / scale, meters(m_MagazineSlapHeldCenter, m_MagazineSlapSeatedCenter));
                    const Vector end = m_SelfTestGlideTo + correction;
                    m_SelfTestSlapEnd = m_SelfTestSlapEnd + correction;
                    glide(end, meters(end, leftNow()) / m->speed);
                }
            }
            if (!glideDone(0.05f)) return false;
            if (m->hit)
                glide(m_SelfTestSlapEnd + m_SelfTestSlapPark + (g.magazineCenter - m_SelfTestPoint), 0.15f);
            go(608);
            return false;
        case 608:
        {
            slapShots();
            if (inPhase < 1.2f) return false;
            const uint32_t nearMisses = m_MagazineSlapNearMissSerial.load(std::memory_order_acquire) -
                static_cast<uint32_t>(m_SelfTestValueB);
            const bool kept = g.state == holdingSpareState;
            std::snprintf(detail, sizeof(detail), "%s at %.2f m/s%s: %u slap(s), %u ejection(s), %u near miss(es), state %s, clip %d -> %d",
                m->name, m->speed, m->canted ? " (gun canted 50 deg)" : "", slaps(), ejects(), nearMisses, state(),
                m_SelfTestClip0, g.clip);
            if (m->hit)
            {
                // The well catches the spare as soon as it may go in (0.15 s
                // after the slap), so on the way back it can already be seated.
                const bool seatedAlready = g.state == idleState && g.clip > 1 &&
                    g.clip + g.reserve == m_SelfTestClip0 + m_SelfTestReserve0;
                SelfTestReport(label((std::string("slap motion '") + m->name + "' knocks the old magazine out").c_str()),
                    slaps() == 1u && ejects() == 1u && (kept || seatedAlready), detail);
                if (seatedAlready)
                {
                    SelfTestHoldRight(SelfTestVec(0.4f, 0.2f, -0.25f), QAngle(0.0f, 0.0f, 0.0f));
                    go(611);
                    return false;
                }
                if (!kept)
                    return finish();
                m_SelfTestClip0 = g.clip;
                m_SelfTestReserve0 = g.reserve;
                m_SelfTestLeftAngWorld = m_LeftControllerTrackedAngAbs;
                m_SelfTestLeftAngWorldValid = true;
                m_SelfTestMeasure = 1e9f;
                m_SelfTestMeasureB = 180.0f;
                go(609);
                return false;
            }
            SelfTestReport(label((std::string("slap motion '") + m->name + "' does not slap").c_str()), slaps() == 0u && ejects() == 0u &&
                g.state == holdingForSlapState && g.clip == m_SelfTestClip0, detail);
            button(reloadHand, false);
            go(612);
            return false;
        }
        case 609:
            if (inPhase < 0.3f) return false;
            if (!seatHeld(m_SelfTestMeasure, m_SelfTestMeasureB))
            {
                if (inPhase < 6.0f) return false;
                return fail("spare seated after the slap", "still held");
            }
            go(610);
            return false;
        case 610:
            if ((g.state == waitingServerState || g.clip <= m_SelfTestClip0) && inPhase < 3.0f) return false;
            button(reloadHand, false);
            m_SelfTestLeftAngWorldValid = false;
            std::snprintf(detail, sizeof(detail), "clip %d -> %d, reserve %d -> %d, state %s",
                m_SelfTestClip0, g.clip, m_SelfTestReserve0, g.reserve, state());
            SelfTestReport(label((std::string("slap motion '") + m->name + "': spare seated after the slap").c_str()),
                g.clip > m_SelfTestClip0 && g.clip + g.reserve == m_SelfTestClip0 + m_SelfTestReserve0, detail);
            SelfTestHoldRight(SelfTestVec(0.4f, 0.2f, -0.25f), QAngle(0.0f, 0.0f, 0.0f));
            go(611);
            return false;
        case 611:
            if (inPhase < 0.8f) return false;
            return finish();
        case 612: // a miss: letting go puts the spare back, nothing changed
            if (inPhase < 0.8f) return false;
            std::snprintf(detail, sizeof(detail), "state %s, clip %d -> %d, reserve %d -> %d",
                state(), m_SelfTestClip0, g.clip, m_SelfTestReserve0, g.reserve);
            SelfTestReport(label((std::string("slap motion '") + m->name + "': the spare goes back after the miss").c_str()), g.state == idleState &&
                g.clip == m_SelfTestClip0 && g.reserve == m_SelfTestReserve0, detail);
            SelfTestHoldRight(SelfTestVec(0.4f, 0.2f, -0.25f), QAngle(0.0f, 0.0f, 0.0f));
            go(611);
            return false;
        default:
            return fail("phase", "lost");
        }
    }

    // ---- Natural hold ---------------------------------------------------------------
    // magholde2e=<gun>: the off hand takes a spare and goes where a player holds
    // it to seat it, a pose defined from the gun alone (the hand is never turned
    // toward the well): turned as the gun hand holds the gun (pointing along the
    // barrel, thumb up, palm toward the gun) and raised 10 degrees nose-up, as
    // when a magazine comes up into the well from below, with the spare on the
    // well's insertion line 15 cm clear of it (the well catches a spare a
    // little short of it; a shell 26 cm below the port, outside its 10 cm
    // loading reach). The spare's turn against the well is measured there
    // (PASS within 15 degrees; a shell by its length, its roll does not
    // matter) and photographed (<gun>_natural_hold), then the hand pushes it
    // straight in without turning and it must seat (<gun>_natural_seated).
    if (hold)
    {
        // extraRaiseDeg / rollDeg turn the hand further for the hand-pose photos.
        auto naturalAngles = [&](float extraRaiseDeg = 0.0f, float rollDeg = 0.0f)
        {
            const float kRaiseDeg = 10.0f + extraRaiseDeg;
            // vr_tracking.inl: the off hand's pointing direction is the
            // controller's raw pose pitched 45 degrees down.
            constexpr float kOffHandCalibrationDeg = 45.0f;
            const Vector right = VectorRotate(g.gunRight, g.gunForward, rollDeg);
            const Vector forward = VectorRotate(g.gunForward, right, kRaiseDeg);
            const Vector up = VectorRotate(VectorRotate(g.gunUp, g.gunForward, rollDeg), right, kRaiseDeg);
            QAngle angles{};
            QAngle::VectorAngles(VectorRotate(forward, right, kOffHandCalibrationDeg),
                VectorRotate(up, right, kOffHandCalibrationDeg), angles);
            return angles;
        };
        auto shortOfWell = [&](float fraction)
        {
            return shellGun ? g.socketCenter + Vector(0.0f, 0.0f, -0.14f * fraction * scale)
                : g.socketCenter + g.socketOutward * ((g.spareLength + 0.03f) * fraction * scale);
        };
        auto farFromWell = [&]()
        {
            return shortOfWell(1.0f) + (shellGun ? Vector(0.0f, 0.0f, -0.12f * scale) : g.socketOutward * (0.12f * scale));
        };
        // The AK-47's magazine comes out by hand or a slap: the spare is taken
        // with the old one still in (Pavlov's tactical reload), measured, and
        // then swept down across the old one, still turned the natural way, to
        // slap it out (Pavlov: along the seated magazine's axis, out of the well).
        const bool tactical = expectedId == 26;
        const int holdingForSlap = static_cast<int>(MagazineInteractionManualState::HoldingFreshMagazineForSlap);
        const int waitingForSpare = static_cast<int>(MagazineInteractionManualState::WaitingForFreshMagazine);
        const int holdingSpare = static_cast<int>(MagazineInteractionManualState::HoldingFreshMagazine);
        switch (m_SelfTestPhase)
        {
        case 300: // the gun level in front
            SelfTestHoldRight(SelfTestVec(0.4f, 0.2f, -0.25f), QAngle(m_WeaponAimPitchOffsetDeg, 0.0f, 0.0f));
            m_SelfTestValueD = g.clip;
            go(301);
            return false;
        case 301:
            if (inPhase < 1.0f) return false;
            if (tactical)
            {
                go(303);
                return false;
            }
            // A shotgun fires one to make room for a shell; a magazine is released.
            button(shellGun ? m_ActionPrimaryAttack : m_ActionMagazineRelease, true);
            go(302);
            return false;
        case 320: // the AK-47: the slap, as slape2e strikes it but without turning the hand
        {
            if (!g.magazineBoxFresh || !g.heldValid)
            {
                if (inPhase < 1.0f) return false;
                return fail("natural hold: slap the old magazine out", "no seated or held magazine pose");
            }
            if (m_SelfTestCycle == 0)
                m_SelfTestSerial = m_MagazineEjectSerial.load(std::memory_order_acquire);
            const Vector contact = leftNow() + (g.magazineCenter - g.heldCenter);
            m_SelfTestSlapStart = contact - g.socketOutward * (0.35f * scale); // clear of the magazine
            m_SelfTestSlapEnd = contact + g.socketOutward * (0.04f * scale);
            m_SelfTestSlapPark = g.gunRight * (-0.30f * scale);
            m_SelfTestPoint = g.magazineCenter; // the strike rides with the gun
            glide(m_SelfTestSlapStart + m_SelfTestSlapPark, 0.25f);
            go(323);
            return false;
        }
        case 323:
            if (!glideDone(0.2f)) return false;
            glide(m_SelfTestSlapStart + (g.magazineCenter - m_SelfTestPoint), 0.5f);
            go(324);
            return false;
        case 324:
            if (!glideDone(0.4f)) return false;
            glide(m_SelfTestSlapEnd + (g.magazineCenter - m_SelfTestPoint), 0.25f); // 39 cm in 0.25 s
            go(325);
            return false;
        case 325:
            if (!glideDone(0.02f)) return false;
            glide(m_SelfTestSlapEnd + m_SelfTestSlapPark + (g.magazineCenter - m_SelfTestPoint), 0.12f);
            go(322);
            return false;
        case 322:
            if (m_MagazineEjectSerial.load(std::memory_order_acquire) == m_SelfTestSerial)
            {
                if (inPhase > 1.0f && g.state == holdingForSlap && m_SelfTestCycle < 2)
                {
                    ++m_SelfTestCycle; // strike again
                    Game::logMsg("[VR][SelfTest] %s natural hold: strike %d missed, again", gun.c_str(), m_SelfTestCycle);
                    go(320);
                    return false;
                }
                if (inPhase < 3.0f) return false;
                std::snprintf(detail, sizeof(detail), "state %s clip %d", state(), g.clip);
                return fail("natural hold: slap the old magazine out", detail);
            }
            go(308);
            return false;
        case 302:
            if (inPhase < 0.15f) return false;
            button(m_ActionPrimaryAttack, false);
            button(m_ActionMagazineRelease, false);
            go(303);
            return false;
        case 303:
            if (inPhase < 0.5f || !g.pickupValid || (g.state != waitingForSpare && !(tactical && g.state == 0) &&
                !(shellGun && g.state == static_cast<int>(MagazineInteractionManualState::WaitingForBoltGrab))))
            {
                if (inPhase < 3.0f) return false;
                std::snprintf(detail, sizeof(detail), "pouch=%d state %s clip %d", g.pickupValid ? 1 : 0, state(), g.clip);
                return fail("natural hold: waiting for a spare", detail);
            }
            if (shellGun && g.clip >= m_SelfTestValueD && m_SelfTestCycle++ < 3)
            {
                go(301); // the shot did not take (still deploying): fire again
                return false;
            }
            m_SelfTestClip0 = g.clip;
            m_SelfTestReserve0 = g.reserve;
            glide(g.pickupCenter, 0.5f);
            go(304);
            return false;
        case 304:
            if (!glideDone(0.2f)) return false;
            m_SelfTestMeasure = meters(g.supportHand, g.pickupCenter);
            button(reloadHand, true);
            go(305);
            return false;
        case 305:
            if (g.state != (tactical ? holdingForSlap : holdingSpare))
            {
                if (inPhase < 1.0f) return false;
                std::snprintf(detail, sizeof(detail), "hand %.3f m from the pouch (range %.2f m), state %s",
                    m_SelfTestMeasure, g.pouchRange, state());
                return fail("natural hold: take a spare from the waist", detail);
            }
            m_SelfTestLeftAngWorld = naturalAngles();
            m_SelfTestLeftAngWorldValid = true;
            Game::logMsg("[VR][SelfTest] %s spare taken: well (%.1f %.1f %.1f) outward (%.2f %.2f %.2f) spare %.3f m long, held at (%.1f %.1f %.1f), gun forward (%.2f %.2f %.2f)",
                gun.c_str(), g.socketCenter.x, g.socketCenter.y, g.socketCenter.z, g.socketOutward.x, g.socketOutward.y,
                g.socketOutward.z, g.spareLength, g.heldCenter.x, g.heldCenter.y, g.heldCenter.z,
                g.gunForward.x, g.gunForward.y, g.gunForward.z);
            m_SelfTestPoint = leftNow(); // where the spare was taken: the poses return here
            go(330);
            return false;
        // The spare in the hand, held up in view at three hand poses (the
        // insert pose, palm turned up, raised toward the face): the drawn
        // fist must close around it (<gun>_spare_pose1..3).
        case 330: case 332: case 334:
        {
            if (m_SelfTestPhase != 330 && inPhase < 0.4f) return false;
            const int pose = (m_SelfTestPhase - 330) / 2;
            // Glide there (no faster than a hand moves): a jump across the
            // seated AK magazine would strike it like a slap.
            if (pose == 0)
                glide(m_HmdPosAbs + m_HmdForward * (0.34f * scale) + m_HmdRight * (-0.05f * scale) +
                    m_HmdUp * (-0.04f * scale), 1.0f);
            m_SelfTestLeftAngWorld = pose == 0 ? naturalAngles() : pose == 1 ? naturalAngles(0.0f, -70.0f)
                : naturalAngles(35.0f, -25.0f);
            m_SelfTestLeftAngWorldValid = true;
            go(m_SelfTestPhase + 1);
            return false;
        }
        case 331: case 333: case 335:
        {
            if (!glideDone(0.6f) || inPhase < 1.0f) return false;
            const int pose = (m_SelfTestPhase - 331) / 2;
            SelfTestSpareInFist f{};
            const bool measured = SelfTestMeasureSpareInFist(f);
            const bool inFist = measured && f.lateralMeters <= f.halfThicknessMeters + 0.01f &&
                f.alongFromBase >= -0.05f && f.alongFromBase <= (shellGun ? 1.0f : 0.75f);
            std::snprintf(detail, sizeof(detail),
                "pose %d: fist %.3f m off the spare's long axis (half thickness %.3f m), %.2f of its %.3f m length from the base%s",
                pose + 1, f.lateralMeters, f.halfThicknessMeters, f.alongFromBase, f.lengthMeters,
                measured ? "" : " (no drawn fist or held spare)");
            SelfTestReport(label("spare sits in the drawn fist"), inFist, detail);
            char shotName[64] = {};
            std::snprintf(shotName, sizeof(shotName), "%s_spare_pose%d", gun.c_str(), pose + 1);
            SelfTestRequestShot(shotName, 1);
            go(m_SelfTestPhase == 335 ? 336 : m_SelfTestPhase + 1);
            return false;
        }
        case 336:
            if (inPhase < 0.4f) return false;
            glide(m_SelfTestPoint, 1.0f);
            go(337);
            return false;
        case 337:
            if (!glideDone(0.3f)) return false;
            go(306);
            return false;
        case 306: // natural pose short of the well
        {
            if (!g.socketValid || !g.heldValid)
            {
                if (inPhase < 1.0f) return false;
                return fail("natural hold", "no well or held spare pose");
            }
            m_SelfTestLeftAngWorld = naturalAngles();
            float unused = 0.0f;
            steerHeld(farFromWell(), false, unused);
            if (inPhase < 1.5f) return false;
            SelfTestHeldMagazine m{};
            if (!SelfTestMeasureHeldMagazine(m))
                return fail("natural hold", "no held or seated pose to compare");
            const float scored = m.shell ? m.longAxisDeg : m.fullDeg;
            char live[48] = "";
            if (m.shell)
                std::snprintf(live, sizeof(live), "; port's live pose %.1f", m.liveLongAxisDeg);
            std::snprintf(detail, sizeof(detail),
                "%s %.1f deg off the %s (insertion axis %.1f, long side %.1f, whole turn %.1f%s), %.3f m from its centre",
                m.shell ? "shell" : "magazine", scored, m.shell ? "port" : "well", m.insertionDeg, m.longAxisDeg, m.fullDeg,
                live, meters(g.socketCenter, g.heldCenter));
            Game::logMsg("[VR][SelfTest] %s natural hold: in the hand (x right, y up, z back) held (%.1f %.1f %.1f), seated (%.1f %.1f %.1f); "
                "seated axes x=(%.2f %.2f %.2f) y=(%.2f %.2f %.2f) z=(%.2f %.2f %.2f) long=%d",
                gun.c_str(), m.heldInHandDeg.x, m.heldInHandDeg.y, m.heldInHandDeg.z,
                m.seatedInHandDeg.x, m.seatedInHandDeg.y, m.seatedInHandDeg.z,
                m.seatedAxesInHand[0].x, m.seatedAxesInHand[0].y, m.seatedAxesInHand[0].z,
                m.seatedAxesInHand[1].x, m.seatedAxesInHand[1].y, m.seatedAxesInHand[1].z,
                m.seatedAxesInHand[2].x, m.seatedAxesInHand[2].y, m.seatedAxesInHand[2].z, m.longAxis);
            SelfTestReport(label("natural insert pose lines the spare up within 15 deg"), scored <= 15.0f, detail);
            // For the photo the hand slides (no turning) 4 cm closer, still
            // clear of where the well (or the pump's lowered port) catches it.
            m_SelfTestPoint = leftNow() + (shortOfWell(1.0f) - g.heldCenter) +
                (shellGun ? Vector(0.0f, 0.0f, -0.08f * scale) : g.socketOutward * (0.08f * scale));
            glide(m_SelfTestPoint, 0.6f);
            go(311);
            return false;
        }
        case 311:
            if (g.state != (tactical ? holdingForSlap : holdingSpare))
            {
                go(309); // it went in on the way
                return false;
            }
            if (!glideDone(0.3f)) return false;
            SelfTestRequestShot(gun + "_natural_hold", 1);
            go(312);
            return false;
        case 312: // hold still for the photo
            if (inPhase < 0.4f) return false;
            go(tactical ? 320 : 308);
            return false;
        case 308: // straight in, no turning
        {
            m_SelfTestLeftAngWorld = naturalAngles();
            const float approach = (std::min)(1.0f, inPhase / 0.8f);
            float unused = 0.0f;
            steerHeld(shortOfWell(1.0f - approach), false, unused);
            if (g.state == holdingSpare)
            {
                if (inPhase < 4.0f) return false;
                std::snprintf(detail, sizeof(detail), "still held, fit check %.3f m, state %s", g.fitDistance, state());
                return fail("natural insert pose seats the spare", detail);
            }
            go(309);
            return false;
        }
        case 309:
        {
            // The AK-47 is refilled to where it was, after one ejection.
            const bool refilled = tactical
                ? m_MagazineEjectSerial.load(std::memory_order_acquire) == m_SelfTestSerial + 1u && g.clip > 1
                : g.clip > m_SelfTestClip0;
            const bool settled = shellGun ? g.clip == m_SelfTestClip0 + 1
                : refilled && g.state != static_cast<int>(MagazineInteractionManualState::WaitingForServerMagazine) &&
                    g.state != static_cast<int>(MagazineInteractionManualState::WaitingForServerPistolCycle);
            if (!settled && inPhase < 3.0f) return false;
            std::snprintf(detail, sizeof(detail), "clip %d -> %d, reserve %d -> %d, state %s",
                m_SelfTestClip0, g.clip, m_SelfTestReserve0, g.reserve, state());
            SelfTestReport(label("natural insert pose seats the spare"), settled, detail);
            SelfTestRequestShot(gun + "_natural_seated", 1);
            go(310);
            return false;
        }
        case 310:
            if (inPhase < 0.5f) return false;
            return finish();
        default:
            return fail("phase", "lost");
        }
    }

    // ---- Detachable magazine ----------------------------------------------------
    if (magazine)
    {
        switch (m_SelfTestPhase)
        {
        case 50:
            button(m_ActionPrimaryAttack, true);
            go(51);
            return false;
        case 51:
            if (inPhase < 0.4f) return false;
            button(m_ActionPrimaryAttack, false);
            go(52);
            return false;
        case 52:
            if (inPhase < 0.5f) return false;
            m_SelfTestClip0 = g.clip;
            m_SelfTestReserve0 = g.reserve;
            m_SelfTestSerialB = m_ReloadGateGapCommands.load(std::memory_order_acquire);
            cmd("+reload");
            go(53);
            return false;
        case 53:
            if (inPhase < 0.3f) return false;
            cmd("-reload");
            go(54);
            return false;
        case 54:
            if (inPhase < 4.0f && g.clip == m_SelfTestClip0) return false;
            std::snprintf(detail, sizeof(detail), "+reload at clip %d: clip -> %d, reserve %d -> %d, %u unblocked command(s)",
                m_SelfTestClip0, g.clip, m_SelfTestReserve0, g.reserve,
                m_ReloadGateGapCommands.load(std::memory_order_acquire) - m_SelfTestSerialB);
            SelfTestReport(label("host refuses the stock reload"), g.clip == m_SelfTestClip0, detail);
            return finish();
        case 10: // run dry
            if (gateProbe)
            {
                go(50);
                return false;
            }
            if (g.clip > 0)
            {
                if (inPhase > 20.0f)
                {
                    std::snprintf(detail, sizeof(detail), "clip still %d after 20 s of firing", g.clip);
                    return fail("fire until empty", detail);
                }
                // Semi-automatic guns need a trigger press per shot.
                const bool down = semiAuto ? (static_cast<uint64_t>(inPhase * 1000.0f) % 160u) < 70u : true;
                button(m_ActionPrimaryAttack, down);
                return false;
            }
            button(m_ActionPrimaryAttack, false);
            m_SelfTestValueD = 0;
            go(41);
            return false;
        case 41: // the client predicted the last shot; wait for the host to agree
            // A host that has not fired the last round yet never fires it once
            // the trigger is up: the clip comes back with that round chambered.
            if (g.clip > 0 && (g.state == static_cast<int>(MagazineInteractionManualState::WaitingForBoltGrab) ||
                    g.state == static_cast<int>(MagazineInteractionManualState::HoldingBolt) ||
                    IsMagazineInteractionBlockingFire()))
                m_SelfTestValueD = 1;
            if (inPhase < 0.8f) return false;
            if (g.clip > 0)
            {
                std::snprintf(detail, sizeof(detail), "client predicted empty, host kept %d round(s), reserve %d -> %d: state %s, fire blocked=%d, rack demanded=%d",
                    g.clip, m_SelfTestReserve0, g.reserve, state(), IsMagazineInteractionBlockingFire() ? 1 : 0, m_SelfTestValueD);
                SelfTestReport(label("a last shot the host never fired stays chambered (no rack, no extra round)"),
                    m_SelfTestValueD == 0 && g.reserve == m_SelfTestReserve0 &&
                        g.state == static_cast<int>(MagazineInteractionManualState::Idle) && !IsMagazineInteractionBlockingFire(),
                    detail);
                if (++m_SelfTestCycle > 3)
                    return fail("fire until empty", "the host kept rounds after three trigger pulls");
                go(10); // pull the trigger again, as a player would
                return false;
            }
            std::snprintf(detail, sizeof(detail), "clip %d -> 0, reserve %d", m_SelfTestClip0, g.reserve);
            SelfTestReport(label("fire until empty"), true, detail);
            m_SelfTestReserve0 = g.reserve;
            m_SelfTestSerialB = m_ReloadGateGapCommands.load(std::memory_order_acquire);
            go(watch ? 40 : 11);
            return false;
        case 40: // hands off: an empty gun stays empty
            if (inPhase < 5.0f) return false;
            std::snprintf(detail, sizeof(detail), "after %.1f s untouched: clip 0 -> %d, reserve %d -> %d, %u command(s) without the reload block (last 0x%X)",
                inPhase, g.clip, m_SelfTestReserve0, g.reserve,
                m_ReloadGateGapCommands.load(std::memory_order_acquire) - m_SelfTestSerialB,
                m_ReloadGateGapReason.load(std::memory_order_acquire));
            SelfTestReport(label("an empty gun stays empty"), g.clip == 0 && g.reserve == m_SelfTestReserve0, detail);
            return finish();
        case 11: // rack with the empty magazine still seated
            if (inPhase < 0.6f) return false;
            if (!g.boltBoxFresh)
            {
                if (inPhase < 4.0f) return false;
                std::snprintf(detail, sizeof(detail), "bolt box fresh=0 age=%.2fs", g.boltBoxAge);
                return fail("rack without a new magazine", detail);
            }
            m_SelfTestPoint = g.boltCenter;
            glide(g.boltCenter, 0.35f);
            go(12);
            return false;
        case 12:
            if (!glideDone(0.15f)) return false;
            m_SelfTestMeasure = meters(g.supportHand, m_SelfTestPoint);
            button(reloadHand, true);
            go(13);
            return false;
        case 13:
            if (inPhase < 0.2f) return false;
            glide(pullBack(0.12f), 0.3f);
            go(14);
            return false;
        case 14:
            if (!glideDone(0.1f)) return false;
            button(reloadHand, false);
            glide(m_SelfTestPoint, 0.2f);
            go(15);
            return false;
        case 15:
            if (inPhase < 1.5f) return false;
            std::snprintf(detail, sizeof(detail), "hand %.3f m from the bolt, pulled 0.12 m and let go: clip 0 -> %d, reserve %d -> %d, state %s",
                m_SelfTestMeasure, g.clip, m_SelfTestReserve0, g.reserve, state());
            SelfTestReport(label("rack with the empty magazine seated does not reload"),
                g.clip == 0 && g.reserve == m_SelfTestReserve0, detail);
            m_SelfTestSerial = m_MagazineEjectSerial.load(std::memory_order_acquire);
            button(m_ActionMagazineRelease, true);
            go(16);
            return false;
        case 16:
            if (inPhase < 0.15f) return false;
            button(m_ActionMagazineRelease, false);
            go(17);
            return false;
        case 17:
            if (g.state != static_cast<int>(MagazineInteractionManualState::WaitingForFreshMagazine) ||
                RemoteMagazineRequestPending() || PistolMagazineRequestPending())
            {
                if (inPhase < 3.0f) return false;
                std::snprintf(detail, sizeof(detail), "state %s clip %d", state(), g.clip);
                return fail("release ejects the empty magazine", detail);
            }
            std::snprintf(detail, sizeof(detail), "%u ejection(s), clip %d, reserve %d -> %d",
                m_MagazineEjectSerial.load(std::memory_order_acquire) - m_SelfTestSerial, g.clip, m_SelfTestReserve0, g.reserve);
            SelfTestReport(label("release ejects the empty magazine"),
                g.clip == 0 && m_MagazineEjectSerial.load(std::memory_order_acquire) - m_SelfTestSerial == 1u, detail);
            m_SelfTestReserve0 = g.reserve;
            go(18);
            return false;
        case 18: // rack with the magazine out
            if (inPhase < 0.4f) return false;
            if (g.boltBoxFresh)
                m_SelfTestPoint = g.boltCenter;
            glide(m_SelfTestPoint, 0.35f);
            go(19);
            return false;
        case 19:
            if (!glideDone(0.15f)) return false;
            m_SelfTestMeasure = meters(g.supportHand, m_SelfTestPoint);
            m_SelfTestMeasureB = g.pickupValid ? meters(g.supportHand, g.pickupCenter) : -1.0f;
            button(reloadHand, true);
            go(20);
            return false;
        case 20:
            if (inPhase < 0.2f) return false;
            glide(pullBack(0.12f), 0.3f);
            go(21);
            return false;
        case 21:
            if (!glideDone(0.1f)) return false;
            button(reloadHand, false);
            glide(m_SelfTestPoint, 0.2f);
            go(22);
            return false;
        case 22:
            if (inPhase < 1.5f) return false;
            std::snprintf(detail, sizeof(detail), "hand %.3f m from the bolt (pouch %.3f m away), pulled 0.12 m: clip 0 -> %d, reserve %d -> %d, state %s",
                m_SelfTestMeasure, m_SelfTestMeasureB, g.clip, m_SelfTestReserve0, g.reserve, state());
            SelfTestReport(label("rack with the magazine out does not reload"),
                g.clip == 0 && g.reserve == m_SelfTestReserve0 &&
                g.state == static_cast<int>(MagazineInteractionManualState::WaitingForFreshMagazine), detail);
            go(23);
            return false;
        case 23: // a spare from the waist
            if (!g.pickupValid)
            {
                if (inPhase < 2.0f) return false;
                return fail("take a spare from the waist", "no pouch position");
            }
            m_SelfTestPoint = g.pickupCenter;
            glide(g.pickupCenter, 0.5f);
            go(24);
            return false;
        case 24:
            if (!glideDone(0.2f)) return false;
            m_SelfTestMeasure = meters(g.supportHand, g.pickupCenter);
            button(reloadHand, true);
            go(25);
            return false;
        case 25:
            if (g.state != static_cast<int>(MagazineInteractionManualState::HoldingFreshMagazine))
            {
                if (inPhase < 1.0f) return false;
                std::snprintf(detail, sizeof(detail), "hand %.3f m from the pouch (range %.2f m), state %s",
                    m_SelfTestMeasure, g.pouchRange, state());
                return fail("take a spare from the waist", detail);
            }
            std::snprintf(detail, sizeof(detail), "hand %.3f m from the pouch (range %.2f m)", m_SelfTestMeasure, g.pouchRange);
            SelfTestReport(label("take a spare from the waist"), true, detail);
            SelfTestRequestShot(gun + "_spare_in_hand", 1);
            m_SelfTestLeftAngWorld = m_LeftControllerTrackedAngAbs;
            m_SelfTestLeftAngWorldValid = true;
            m_SelfTestMeasure = 1e9f;
            m_SelfTestMeasureB = 180.0f;
            go(26);
            return false;
        case 26: // into the well: line up below it, then push it home
        {
            if (g.state != static_cast<int>(MagazineInteractionManualState::HoldingFreshMagazine))
            {
                if (g.fitDistance >= 0.0f) m_SelfTestMeasure = (std::min)(m_SelfTestMeasure, g.fitDistance);
                go(27);
                return false;
            }
            if (!g.socketValid || !g.heldValid)
            {
                if (inPhase < 1.0f) return false;
                return fail("seat the spare", "no well or held magazine pose");
            }
            const float approach = (std::min)(1.0f, (std::max)(0.0f, (inPhase - 0.7f) / 0.5f));
            const Vector target = g.socketCenter + g.socketOutward * ((1.0f - approach) * 0.10f * scale);
            float error = 0.0f;
            const float residual = steerHeld(target, true, error);
            m_SelfTestMeasure = (std::min)(m_SelfTestMeasure, meters(g.socketCenter, g.heldCenter));
            if (error > 0.0f) m_SelfTestMeasureB = error;
            if (g.state == static_cast<int>(MagazineInteractionManualState::HoldingFreshMagazine))
            {
                if (inPhase < 5.0f) return false;
                std::snprintf(detail, sizeof(detail), "still held: magazine %.3f m from the well (residual %.3f, fit check %.3f m), misaligned %.1f deg",
                    m_SelfTestMeasure, residual, g.fitDistance, m_SelfTestMeasureB);
                return fail("seat the spare", detail);
            }
            go(27);
            return false;
        }
        case 27:
            if (g.state == static_cast<int>(MagazineInteractionManualState::WaitingForServerMagazine) ||
                g.state == static_cast<int>(MagazineInteractionManualState::WaitingForServerPistolCycle) || g.clip <= 0)
            {
                if (inPhase < 3.0f) return false;
                std::snprintf(detail, sizeof(detail), "state %s clip %d reserve %d", state(), g.clip, g.reserve);
                return fail("seat the spare", detail);
            }
            button(reloadHand, false);
            m_SelfTestLeftAngWorldValid = false;
            {
                const bool infinite = expectedId == 1 || expectedId == 32;
                std::snprintf(detail, sizeof(detail), "magazine %.3f m from the well, %.1f deg off when seated: clip 0 -> %d, reserve %d -> %d, state %s",
                    m_SelfTestMeasure, m_SelfTestMeasureB, g.clip, m_SelfTestReserve0, g.reserve, state());
                SelfTestRequestShot(gun + "_spare_seated", 1);
                SelfTestReport(label("spare seated, refilled from the reserve"), g.clip > 0 &&
                    (infinite || g.clip + g.reserve == m_SelfTestReserve0), detail);
            }
            m_SelfTestClip0 = g.clip;
            go(28);
            return false;
        case 28: // empty chamber: the trigger does nothing until the rack
            if (inPhase < 0.4f) return false;
            button(m_ActionPrimaryAttack, true);
            go(29);
            return false;
        case 29:
            if (inPhase < 0.25f) return false;
            button(m_ActionPrimaryAttack, false);
            go(30);
            return false;
        case 30:
            if (inPhase < 0.4f) return false;
            std::snprintf(detail, sizeof(detail), "state %s, fire blocked=%d, clip %d -> %d", state(), g.fireBlocked ? 1 : 0,
                m_SelfTestClip0, g.clip);
            SelfTestReport(label("empty chamber holds fire until racked"), g.clip == m_SelfTestClip0 &&
                g.state == static_cast<int>(MagazineInteractionManualState::WaitingForBoltGrab), detail);
            if (!g.boltStage)
                return fail("rack after seating", "no bolt stage armed");
            m_SelfTestPoint = g.boltGrabCenter;
            glide(g.boltGrabCenter, 0.35f);
            go(31);
            return false;
        case 31:
            if (!glideDone(0.1f)) return false;
            m_SelfTestMeasure = meters(g.supportHand, g.boltGrabCenter);
            button(reloadHand, true);
            go(32);
            return false;
        case 32:
            if (g.state != static_cast<int>(MagazineInteractionManualState::HoldingBolt))
            {
                if (inPhase < 0.6f) return false;
                std::snprintf(detail, sizeof(detail), "hand %.3f m from the bolt (range %.2f m), state %s",
                    m_SelfTestMeasure, g.boltRange, state());
                return fail("rack after seating", detail);
            }
            m_SelfTestMeasureB = g.boltRequiredPull;
            glide(pullBack(g.boltRequiredPull + 0.05f), 0.35f);
            go(33);
            return false;
        case 33:
            m_SelfTestMeasure = (std::max)(0.0f, g.boltPull);
            if (!glideDone(0.1f)) return false;
            button(reloadHand, false);
            go(34);
            return false;
        case 34:
            if (inPhase < 0.5f) return false;
            std::snprintf(detail, sizeof(detail), "pulled %.3f m (needs %.3f m) and let go: state %s, fire blocked=%d",
                m_SelfTestMeasure, m_SelfTestMeasureB, state(), g.fireBlocked ? 1 : 0);
            SelfTestReport(label("rack chambers a round"), g.state == 0 && !g.fireBlocked, detail);
            m_SelfTestClip0 = g.clip;
            glide(m_LeftControllerPosAbs + Vector(0.0f, 0.0f, -0.15f * scale), 0.3f);
            button(m_ActionPrimaryAttack, true);
            go(35);
            return false;
        case 35:
            if (inPhase < 0.12f) return false;
            button(m_ActionPrimaryAttack, false);
            go(36);
            return false;
        case 36:
            if (inPhase < 0.6f) return false;
            std::snprintf(detail, sizeof(detail), "clip %d -> %d", m_SelfTestClip0, g.clip);
            SelfTestReport(label("fires after the rack"), g.clip < m_SelfTestClip0, detail);
            return finish();
        default:
            return fail("phase", "lost");
        }
    }

    // ---- Shotguns -----------------------------------------------------------------
    if (shells)
    {
        switch (m_SelfTestPhase)
        {
        case 100: // fire one (once the gun has finished deploying)
            if (m_SelfTestCycle == 0 && inPhase < 1.0f) return false;
            m_SelfTestClip0 = g.clip;
            m_SelfTestReserve0 = g.reserve;
            button(m_ActionPrimaryAttack, true);
            go(101);
            return false;
        case 101:
            if (inPhase < 0.12f) return false;
            button(m_ActionPrimaryAttack, false);
            go(102);
            return false;
        case 102:
        {
            if (inPhase < 0.7f) return false;
            // A shot the host never fired (it had not finished the draw) comes
            // back; that is not a failure, so fire again (twice at most).
            if (g.clip == m_SelfTestClip0 && g.reserve == m_SelfTestReserve0 && m_SelfTestValueC < 2 &&
                g.state != static_cast<int>(MagazineInteractionManualState::WaitingForBoltGrab))
            {
                ++m_SelfTestValueC;
                Game::logMsg("[VR][SelfTest] %s: the host did not fire the shot (clip %d); firing again", gun.c_str(), g.clip);
                go(100);
                return false;
            }
            const bool pumpWaits = g.state == static_cast<int>(MagazineInteractionManualState::WaitingForBoltGrab);
            std::snprintf(detail, sizeof(detail), "cycle %d: clip %d -> %d, state %s, fire blocked=%d, waist shell shown=%d",
                m_SelfTestCycle + 1, m_SelfTestClip0, g.clip, state(), g.fireBlocked ? 1 : 0,
                ShouldDrawMagazineInteractionDetachedMagazine() ? 1 : 0);
            SelfTestReport(label("fire one"), g.clip == m_SelfTestClip0 - 1 && (!pump || (pumpWaits && g.fireBlocked)), detail);
            if (pump)
                SelfTestReport(label("shell stays at the waist while the pump waits"),
                    ShouldDrawMagazineInteractionDetachedMagazine(), detail);
            if (g.clip != m_SelfTestClip0 - 1)
                return finish();
            // The second time the shell goes in before the pump is cycled.
            m_SelfTestValueD = pump && m_SelfTestCycle == 1 ? 1 : 0;
            go(pump && m_SelfTestCycle != 1 ? 110 : 120);
            return false;
        }
        case 110: // pump: back the Pavlov distance, then forward
            if (!g.boltStage)
            {
                if (inPhase < 1.5f) return false;
                std::snprintf(detail, sizeof(detail), "state %s", state());
                return fail("pump", detail);
            }
            m_SelfTestPoint = g.boltGrabCenter;
            glide(g.boltGrabCenter, 0.35f);
            go(111);
            return false;
        case 111:
            if (!glideDone(0.1f)) return false;
            // Glide targets are world points; the player can be pushed while
            // the hand travels. Follow the pump if it moved (up to 1.5 s).
            if (meters(g.supportHand, g.boltGrabCenter) > 0.04f && inPhase < 1.5f)
            {
                m_SelfTestPoint = g.boltGrabCenter;
                glide(g.boltGrabCenter, 0.1f);
                return false;
            }
            m_SelfTestMeasure = meters(g.supportHand, m_SelfTestPoint);
            button(reloadHand, true);
            go(112);
            return false;
        case 112:
            if (g.state != static_cast<int>(MagazineInteractionManualState::HoldingBolt))
            {
                if (inPhase < 0.6f) return false;
                std::snprintf(detail, sizeof(detail), "hand %.3f m from the pump (range %.2f m), state %s",
                    m_SelfTestMeasure, g.boltRange, state());
                return fail("pump", detail);
            }
            m_SelfTestPoint = leftNow();
            m_SelfTestMeasureB = g.boltRequiredPull;
            m_SelfTestMeasure = 0.0f;
            glide(pullBack(g.boltRequiredPull + 0.05f), 0.3f);
            go(113);
            return false;
        case 113:
            m_SelfTestMeasure = (std::max)(m_SelfTestMeasure, g.boltPull);
            if (!glideDone(0.1f)) return false;
            // A push on the player during the stroke shortens it against the
            // gun; keep pulling until the pump is all the way back.
            if (m_SelfTestMeasure < m_SelfTestMeasureB && inPhase < 1.5f &&
                g.state == static_cast<int>(MagazineInteractionManualState::HoldingBolt))
            {
                glide(pullBack(m_SelfTestMeasureB - g.boltPull + 0.03f), 0.15f);
                return false;
            }
            // All the way forward: the stroke the hand made (the pull plus the
            // overshoot behind the stop) and a little past where it was taken.
            glide(leftNow() + g.gunForward * (((std::max)(g.boltPull, m_SelfTestMeasureB) + 0.05f + 0.06f) * scale), 0.3f);
            go(114);
            return false;
        case 114:
            m_SelfTestMeasure = (std::max)(m_SelfTestMeasure, g.boltPull);
            if (!glideDone(0.2f)) return false;
            // Not home yet (the player was pushed): keep pushing.
            if (g.boltPull > 0.01f && inPhase < 1.5f &&
                g.state == static_cast<int>(MagazineInteractionManualState::HoldingBolt))
            {
                glide(leftNow() + g.gunForward * ((g.boltPull + 0.04f) * scale), 0.15f);
                return false;
            }
            button(reloadHand, false);
            go(115);
            return false;
        case 115:
        {
            if (inPhase < 0.3f) return false;
            const bool cycled = !g.fireBlocked && g.state != static_cast<int>(MagazineInteractionManualState::WaitingForBoltGrab) &&
                g.state != static_cast<int>(MagazineInteractionManualState::HoldingBolt);
            std::snprintf(detail, sizeof(detail), "cycle %d: pulled %.3f m (needs %.3f m) and pushed home: state %s, fire blocked=%d",
                m_SelfTestCycle + 1, m_SelfTestMeasure, m_SelfTestMeasureB, state(), g.fireBlocked ? 1 : 0);
            SelfTestReport(label("pump cycled"), cycled, detail);
            if (!cycled)
                return finish();
            go(redraw ? 410 : m_SelfTestValueD && m_SelfTestCycle == 1 ? 140 : 120);
            return false;
        }
        // ---- pumpredrawe2e ----
        case 400: // fire one once the gun has finished deploying
            if (inPhase < 1.5f) return false;
            m_SelfTestClip0 = g.clip;
            m_SelfTestReserve0 = g.reserve;
            button(m_ActionPrimaryAttack, true);
            go(401);
            return false;
        case 401:
            if (inPhase < 0.12f) return false;
            button(m_ActionPrimaryAttack, false);
            go(402);
            return false;
        case 402:
        {
            if (inPhase < 1.5f) return false;
            const bool pumpWaits = g.state == static_cast<int>(MagazineInteractionManualState::WaitingForBoltGrab);
            if (g.clip == m_SelfTestClip0 && !pumpWaits && m_SelfTestValueC < 2)
            {
                ++m_SelfTestValueC;
                go(400);
                return false;
            }
            std::snprintf(detail, sizeof(detail), "clip %d -> %d, state %s, pump stage %d", m_SelfTestClip0, g.clip, state(),
                g.boltStage ? 1 : 0);
            SelfTestReport(label("fire one: the pump waits"), g.clip == m_SelfTestClip0 - 1 && pumpWaits && g.boltStage, detail);
            if (!pumpWaits || !g.boltStage)
                return finish();
            // The pump in the gun hand's frame (the gun follows that hand).
            const Vector d = g.boltGrabCenter - m_RightControllerPosAbs;
            m_SelfTestPumpInGun = Vector(DotProduct(d, g.gunForward), DotProduct(d, g.gunRight), DotProduct(d, g.gunUp)) * (1.0f / scale);
            SelfTestRequestShot((m_SelfTestFreezeAnyFrame ? "anyframe_" : "") + gun + "_pump_waits", 1);
            go(403);
            return false;
        }
        case 403:
        {
            if (inPhase < 0.5f) return false;
            const Vector d = g.boltGrabCenter - m_RightControllerPosAbs;
            const Vector now = Vector(DotProduct(d, g.gunForward), DotProduct(d, g.gunRight), DotProduct(d, g.gunUp)) * (1.0f / scale);
            Game::logMsg("[VR][SelfTest] %s pump before the switch: (%.3f %.3f %.3f) m, at the shot +1.5 s (%.3f %.3f %.3f) m",
                gun.c_str(), now.x, now.y, now.z, m_SelfTestPumpInGun.x, m_SelfTestPumpInGun.y, m_SelfTestPumpInGun.z);
            m_SelfTestPumpInGun = now;
            cmd("use weapon_pistol");
            go(404);
            return false;
        }
        case 404:
            if (inPhase < 1.5f) return false;
            std::snprintf(detail, sizeof(detail), "use weapon_%s", gun.c_str());
            cmd(detail);
            go(405);
            return false;
        case 405: // drawn again: the pump still waits
            if (g.weaponId != expectedId || !g.boltStage)
            {
                if (inPhase < 3.0f) return false;
                std::snprintf(detail, sizeof(detail), "weapon %d, state %s, pump stage %d", g.weaponId, state(), g.boltStage ? 1 : 0);
                return fail("drawn again with the pump waiting", detail);
            }
            if (inPhase < 0.3f) return false;
            SelfTestRequestShot((m_SelfTestFreezeAnyFrame ? "anyframe_" : "") + gun + "_redraw_early", 1);
            go(406);
            return false;
        case 406:
        {
            if (inPhase < 1.7f) return false;
            const Vector d = g.boltGrabCenter - m_RightControllerPosAbs;
            const Vector now = Vector(DotProduct(d, g.gunForward), DotProduct(d, g.gunRight), DotProduct(d, g.gunUp)) * (1.0f / scale);
            const float moved = (now - m_SelfTestPumpInGun).Length();
            std::snprintf(detail, sizeof(detail),
                "pump in the gun hand's frame (%.3f %.3f %.3f) m before the switch, (%.3f %.3f %.3f) m after: %.3f m apart, state %s",
                m_SelfTestPumpInGun.x, m_SelfTestPumpInGun.y, m_SelfTestPumpInGun.z, now.x, now.y, now.z, moved, state());
            SelfTestReport(label("the redrawn gun sits in the hand, pump where it was"), moved < 0.03f, detail);
            SelfTestRequestShot((m_SelfTestFreezeAnyFrame ? "anyframe_" : "") + gun + "_redraw", 1);
            go(110);
            return false;
        }
        case 410: // pumped: it fires
            if (inPhase < 0.3f) return false;
            m_SelfTestClip0 = g.clip;
            button(m_ActionPrimaryAttack, true);
            go(411);
            return false;
        case 411:
            if (inPhase < 0.12f) return false;
            button(m_ActionPrimaryAttack, false);
            go(412);
            return false;
        case 412:
            if (inPhase < 0.8f) return false;
            std::snprintf(detail, sizeof(detail), "clip %d -> %d, state %s", m_SelfTestClip0, g.clip, state());
            SelfTestReport(label("fires after the pump"), g.clip == m_SelfTestClip0 - 1, detail);
            return finish();
        case 120: // a shell from the waist
            if (!g.pickupValid || (g.state != static_cast<int>(MagazineInteractionManualState::WaitingForFreshMagazine) &&
                g.state != static_cast<int>(MagazineInteractionManualState::WaitingForBoltGrab)))
            {
                if (inPhase < 2.0f) return false;
                std::snprintf(detail, sizeof(detail), "pouch=%d state %s", g.pickupValid ? 1 : 0, state());
                return fail("take a shell from the waist", detail);
            }
            glide(g.pickupCenter, 0.45f);
            go(121);
            return false;
        case 121:
            if (!glideDone(0.15f)) return false;
            // As for the pump: follow the pouch if the player was pushed.
            if (g.pickupValid && meters(g.supportHand, g.pickupCenter) > 0.06f && inPhase < 1.5f)
            {
                glide(g.pickupCenter, 0.1f);
                return false;
            }
            m_SelfTestMeasure = g.pickupValid ? meters(g.supportHand, g.pickupCenter) : -1.0f;
            m_SelfTestClip0 = g.clip;
            m_SelfTestReserve0 = g.reserve;
            button(reloadHand, true);
            go(122);
            return false;
        case 122:
            if (g.state != static_cast<int>(MagazineInteractionManualState::HoldingFreshMagazine))
            {
                if (inPhase < 0.8f) return false;
                std::snprintf(detail, sizeof(detail), "cycle %d: hand %.3f m from the pouch (range %.2f m), state %s",
                    m_SelfTestCycle + 1, m_SelfTestMeasure, g.pouchRange, state());
                return fail("take a shell from the waist", detail);
            }
            std::snprintf(detail, sizeof(detail), "cycle %d: hand %.3f m from the pouch (range %.2f m)%s",
                m_SelfTestCycle + 1, m_SelfTestMeasure, g.pouchRange,
                m_SelfTestValueD ? ", pump not cycled yet" : "");
            SelfTestReport(label("take a shell from the waist"), true, detail);
            if (m_SelfTestCycle == 0)
                SelfTestRequestShot(gun + "_shell_in_hand", 1);
            m_SelfTestMeasure = 1e9f;
            go(123);
            return false;
        case 123: // into the loading port
        {
            if (!g.socketValid || !g.heldValid)
            {
                if (g.state != static_cast<int>(MagazineInteractionManualState::HoldingFreshMagazine))
                {
                    go(124);
                    return false;
                }
                if (inPhase < 1.0f) return false;
                return fail("load the shell", "no port or held shell pose");
            }
            const float approach = (std::min)(1.0f, (std::max)(0.0f, (inPhase - 0.4f) / 0.4f));
            const Vector target = g.socketCenter + Vector(0.0f, 0.0f, -(1.0f - approach) * 0.12f * scale);
            float error = 0.0f;
            const float residual = steerHeld(target, false, error);
            m_SelfTestMeasure = (std::min)(m_SelfTestMeasure, meters(g.socketCenter, g.heldCenter));
            if (g.state == static_cast<int>(MagazineInteractionManualState::HoldingFreshMagazine))
            {
                if (inPhase < 4.0f) return false;
                std::snprintf(detail, sizeof(detail), "shell %.3f m from the port (residual %.3f, fit check %.3f m), state %s",
                    m_SelfTestMeasure, residual, g.fitDistance, state());
                return fail("load the shell", detail);
            }
            go(124);
            return false;
        }
        case 124:
            if (g.state == static_cast<int>(MagazineInteractionManualState::WaitingForServerShell) ||
                g.state == static_cast<int>(MagazineInteractionManualState::HoldingFreshMagazine) ||
                g.clip == m_SelfTestClip0)
            {
                if (inPhase < 3.0f) return false;
                std::snprintf(detail, sizeof(detail), "cycle %d: state %s clip %d -> %d reserve %d -> %d",
                    m_SelfTestCycle + 1, state(), m_SelfTestClip0, g.clip, m_SelfTestReserve0, g.reserve);
                return fail("load the shell", detail);
            }
            button(reloadHand, false);
            stopGlide();
            std::snprintf(detail, sizeof(detail), "cycle %d: shell %.3f m from the port: clip %d -> %d, reserve %d -> %d, state %s",
                m_SelfTestCycle + 1, m_SelfTestMeasure, m_SelfTestClip0, g.clip, m_SelfTestReserve0, g.reserve, state());
            if (m_SelfTestCycle == 0)
                SelfTestRequestShot(gun + "_shell_loaded", 1);
            SelfTestReport(label("shell loaded: exactly one, none lost"),
                g.clip == m_SelfTestClip0 + 1 && g.reserve == m_SelfTestReserve0 - 1, detail);
            if (g.clip != m_SelfTestClip0 + 1)
                return finish();
            go(m_SelfTestValueD ? 125 : 140);
            return false;
        case 125: // the pump still waits after the early shell
            if (inPhase < 0.5f) return false;
            std::snprintf(detail, sizeof(detail), "state %s, fire blocked=%d", state(), g.fireBlocked ? 1 : 0);
            SelfTestReport(label("pump still waits after the early shell"),
                g.state == static_cast<int>(MagazineInteractionManualState::WaitingForBoltGrab) && g.fireBlocked, detail);
            go(110);
            return false;
        case 140: // next cycle
            if (inPhase < 0.6f) return false;
            if (++m_SelfTestCycle >= 3)
                return finish();
            m_SelfTestValueD = 0;
            go(100);
            return false;
        default:
            return fail("phase", "lost");
        }
    }

    // ---- AK-47 magazine slap -------------------------------------------------------
    // How far the seated magazine moved since the strike was planned.
    auto gunDrift = [&]() { return g.magazineBoxFresh ? g.magazineCenter - m_SelfTestPoint : Vector(0.0f, 0.0f, 0.0f); };
    switch (m_SelfTestPhase)
    {
    case 230: // sparereturne2e: hold the spare a moment, then let it go
        if (inPhase < 0.5f) return false;
        button(reloadHand, false);
        glide(pullBack(0.25f), 0.3f);
        go(231);
        return false;
    case 231:
        if (inPhase < 1.0f) return false;
        std::snprintf(detail, sizeof(detail), "clip %d -> %d, reserve %d -> %d, state %s, fire blocked=%d",
            m_SelfTestClip0, g.clip, m_SelfTestReserve0, g.reserve, state(), IsMagazineInteractionBlockingFire() ? 1 : 0);
        SelfTestReport(label("letting go returns the spare; nothing changes"),
            g.clip == m_SelfTestClip0 && g.reserve == m_SelfTestReserve0 &&
                g.state == static_cast<int>(MagazineInteractionManualState::Idle) && !IsMagazineInteractionBlockingFire(),
            detail);
        button(m_ActionPrimaryAttack, true);
        go(232);
        return false;
    case 232:
        if (inPhase < 0.25f) return false;
        button(m_ActionPrimaryAttack, false);
        go(233);
        return false;
    case 233:
        if (inPhase < 0.6f) return false;
        std::snprintf(detail, sizeof(detail), "clip %d -> %d, reserve %d -> %d, state %s",
            m_SelfTestClip0, g.clip, m_SelfTestReserve0, g.reserve, state());
        SelfTestReport(label("fires with the old magazine after the spare went back"),
            g.clip < m_SelfTestClip0 && g.reserve == m_SelfTestReserve0, detail);
        return finish();
    case 200:
        if (!g.pickupValid)
        {
            if (inPhase < 2.0f) return false;
            return fail("slap", "no pouch position");
        }
        m_SelfTestSerial = m_MagazineEjectSerial.load(std::memory_order_acquire);
        m_SelfTestValueA = static_cast<int>(m_MagazineSlapSerial.load(std::memory_order_acquire));
        glide(g.pickupCenter, 0.5f);
        go(201);
        return false;
    case 201:
        if (!glideDone(0.2f)) return false;
        m_SelfTestMeasure = meters(g.supportHand, g.pickupCenter);
        button(reloadHand, true);
        go(202);
        return false;
    case 202:
    {
        if (g.state != static_cast<int>(MagazineInteractionManualState::HoldingFreshMagazineForSlap))
        {
            if (inPhase < 1.0f) return false;
            std::snprintf(detail, sizeof(detail), "hand %.3f m from the pouch (range %.2f m), state %s",
                m_SelfTestMeasure, g.pouchRange, state());
            return fail("spare taken with the old magazine seated", detail);
        }
        std::snprintf(detail, sizeof(detail), "hand %.3f m from the pouch", m_SelfTestMeasure);
        SelfTestReport(label("spare taken with the old magazine seated"), true, detail);
        if (spareReturn)
        {
            m_SelfTestClip0 = g.clip;
            m_SelfTestReserve0 = g.reserve;
            go(230);
            return false;
        }
        SelfTestRequestShot(gun + "_slap_spare_in_hand", 1);
        // Hold the spare in line with the seated magazine, as a player would.
        m_SelfTestLeftAngWorld = m_LeftControllerTrackedAngAbs;
        m_SelfTestLeftAngWorldValid = true;
        go(212);
        return false;
    }
    case 212:
    {
        // Strike 30 cm before to 4 cm past the seated magazine along its
        // outward axis; reach the start sideways so only the strike counts.
        MagazineInteractionBoxSnapshot box{};
        Vector pouch{}, held{};
        bool heldValid = false;
        if (!GetMagazineInteractionBox(box) || !GetMagazineSlapSelfTestPoints(pouch, held, heldValid) || !heldValid)
            return fail("slap", "no seated or held magazine pose");
        l4d2vr_mag_slap::Box seated{};
        seated.origin = { box.origin.x, box.origin.y, box.origin.z };
        seated.axisX = { box.axisX.x, box.axisX.y, box.axisX.z };
        seated.axisY = { box.axisY.x, box.axisY.y, box.axisY.z };
        seated.axisZ = { box.axisZ.x, box.axisZ.y, box.axisZ.z };
        seated.mins = { box.mins.x, box.mins.y, box.mins.z };
        seated.maxs = { box.maxs.x, box.maxs.y, box.maxs.z };
        const Vector insertion = m_MagazineInteractionMagazineInsertionAxisLocal;
        l4d2vr_mag_slap::Vec3 outward{}, side{};
        if (!l4d2vr_mag_slap::OutwardAxis(seated, { insertion.x, insertion.y, insertion.z }, outward))
            return fail("slap", "no outward axis");
        if (!l4d2vr_mag_slap::Normalize(l4d2vr_mag_slap::Sub(seated.axisX,
                l4d2vr_mag_slap::Scale(outward, l4d2vr_mag_slap::Dot(seated.axisX, outward))), side))
            side = { 1.0f, 0.0f, 0.0f };
        const Vector out(outward.x, outward.y, outward.z), across(side.x, side.y, side.z);
        const l4d2vr_mag_slap::Vec3 seatedCenter = l4d2vr_mag_slap::Center(seated);
        const Vector center(seatedCenter.x, seatedCenter.y, seatedCenter.z);
        // Line up for at least 1 s, longer (up to 3 s) until the spare is in line.
        if (inPhase < 1.0f || (m_SelfTestMeasureB > 5.0f && inPhase < 3.0f))
        {
            float error = 0.0f;
            steerHeld(center - out * (0.10f * scale) + across * (0.30f * scale), true, error);
            m_SelfTestMeasureB = error;
            return false;
        }
        const Vector contact = m_LeftControllerPosAbs + (center - held);
        // 35 cm back: clear of the 25 cm magazine, so the strike makes the touch.
        m_SelfTestSlapStart = contact - out * (0.35f * scale);
        m_SelfTestSlapEnd = contact + out * (0.04f * scale);
        m_SelfTestSlapPark = across * (0.30f * scale);
        // The strike points ride with the gun: the body can drift a few cm
        // while the hand winds up, more than the 3 cm contact distance.
        m_SelfTestPoint = g.magazineCenter;
        Game::logMsg("[VR][SelfTest] slap: spare lined up %.1f deg off the seated magazine", m_SelfTestMeasureB);
        SelfTestRequestShot(gun + "_slap_spare_lined_up", 1);
        glide(m_SelfTestSlapStart + m_SelfTestSlapPark, 0.2f);
        go(203);
        return false;
    }
    case 203:
        if (!glideDone(0.2f)) return false;
        glide(m_SelfTestSlapStart + gunDrift(), 0.5f);
        m_SelfTestStrikeCorrections = 0;
        go(204);
        return false;
    case 204:
    {
        if (!glideDone(0.4f)) return false;
        // As in slapmotione2e: line the still spare up across the strike and
        // at least 30 cm back, then strike through the old magazine's centre.
        Vector dir = g.socketOutward * (1.0f / (std::max)(0.001f, g.socketOutward.Length()));
        const bool collider = nowMs - m_MagazineSlapCentersAtMs < 200u;
        if (collider && m_SelfTestStrikeCorrections < 6)
        {
            Vector miss = m_MagazineSlapSeatedCenter - m_MagazineSlapHeldCenter;
            const float along = DotProduct(miss, dir);
            miss = miss - dir * along;
            const float backMissing = (std::max)(0.0f, 0.30f * scale - along);
            if (miss.Length() > 0.008f * scale || backMissing > 0.01f * scale)
            {
                ++m_SelfTestStrikeCorrections;
                glide(leftNow() + miss - dir * (backMissing > 0.01f * scale ? backMissing + 0.04f * scale : 0.0f), 0.2f);
                return false;
            }
        }
        if (collider)
            m_SelfTestSlapEnd = leftNow() + (m_MagazineSlapSeatedCenter + dir * (0.04f * scale) - m_MagazineSlapHeldCenter);
        else
            m_SelfTestSlapEnd = m_SelfTestSlapEnd + gunDrift();
        m_SelfTestPoint = g.magazineCenter;
        m_SelfTestStrikeCorrections = 0;
        glide(m_SelfTestSlapEnd, meters(m_SelfTestSlapEnd, leftNow()) / 1.6f); // 1.6 m/s against a still gun hand
        go(205);
        return false;
    }
    case 205:
        if (m_MagazineSlapSerial.load(std::memory_order_acquire) == static_cast<uint32_t>(m_SelfTestValueA) &&
            m_SelfTestGlideMs && !glideDone(0.0f) && m_SelfTestStrikeCorrections < 40 &&
            nowMs - m_MagazineSlapCentersAtMs < 200u)
        {
            // Keep the strike on line (sideways only, keeping the speed).
            const Vector dir = g.socketOutward * (1.0f / (std::max)(0.001f, g.socketOutward.Length()));
            Vector miss = m_MagazineSlapSeatedCenter - m_MagazineSlapHeldCenter;
            miss = miss - dir * DotProduct(miss, dir);
            const Vector planned = m_SelfTestGlideTo - leftNow();
            const Vector correction = miss - (planned - dir * DotProduct(planned, dir));
            if (correction.Length() > 0.005f * scale)
            {
                ++m_SelfTestStrikeCorrections;
                m_SelfTestSlapEnd = m_SelfTestGlideTo + correction;
                glide(m_SelfTestSlapEnd, meters(m_SelfTestSlapEnd, leftNow()) / 1.6f);
            }
        }
        if (!glideDone(0.02f)) return false;
        glide(m_SelfTestSlapEnd + m_SelfTestSlapPark + gunDrift(), 0.12f);
        go(206);
        return false;
    case 206:
    {
        if (inPhase < 1.2f) return false;
        const uint32_t slaps = m_MagazineSlapSerial.load(std::memory_order_acquire) - static_cast<uint32_t>(m_SelfTestValueA);
        const uint32_t ejects = m_MagazineEjectSerial.load(std::memory_order_acquire) - m_SelfTestSerial;
        const bool kept = g.state == static_cast<int>(MagazineInteractionManualState::HoldingFreshMagazine);
        std::snprintf(detail, sizeof(detail), "%u slap(s), %u ejection(s), spare %s: clip %d -> %d, reserve %d -> %d",
            slaps, ejects, kept ? "in hand" : "not in hand", m_SelfTestClip0, g.clip, m_SelfTestReserve0, g.reserve);
        SelfTestReport(label("slap knocks the old magazine out, spare kept"), slaps == 1u && ejects == 1u && kept, detail);
        if (!kept)
            return finish();
        m_SelfTestReserve0 = g.reserve;
        m_SelfTestClip0 = g.clip;
        m_SelfTestLeftAngWorld = m_LeftControllerTrackedAngAbs;
        m_SelfTestLeftAngWorldValid = true;
        m_SelfTestMeasure = 1e9f;
        m_SelfTestMeasureB = 180.0f;
        go(207);
        return false;
    }
    case 207:
    {
        if (g.state != static_cast<int>(MagazineInteractionManualState::HoldingFreshMagazine))
        {
            if (g.fitDistance >= 0.0f) m_SelfTestMeasure = (std::min)(m_SelfTestMeasure, g.fitDistance);
            go(208);
            return false;
        }
        if (!g.socketValid || !g.heldValid)
        {
            if (inPhase < 1.0f) return false;
            return fail("seat the spare after the slap", "no well or held magazine pose");
        }
        const float approach = (std::min)(1.0f, (std::max)(0.0f, (inPhase - 0.7f) / 0.5f));
        const Vector target = g.socketCenter + g.socketOutward * ((1.0f - approach) * 0.10f * scale);
        float error = 0.0f;
        steerHeld(target, true, error);
        m_SelfTestMeasure = (std::min)(m_SelfTestMeasure, meters(g.socketCenter, g.heldCenter));
        if (error > 0.0f) m_SelfTestMeasureB = error;
        if (g.state == static_cast<int>(MagazineInteractionManualState::HoldingFreshMagazine))
        {
            if (inPhase < 5.0f) return false;
            std::snprintf(detail, sizeof(detail), "magazine %.3f m from the well, misaligned %.1f deg",
                m_SelfTestMeasure, m_SelfTestMeasureB);
            return fail("seat the spare after the slap", detail);
        }
        go(208);
        return false;
    }
    case 208:
        if (g.state == static_cast<int>(MagazineInteractionManualState::WaitingForServerMagazine))
        {
            if (inPhase < 3.0f) return false;
            return fail("seat the spare after the slap", "no reply");
        }
        button(reloadHand, false);
        m_SelfTestLeftAngWorldValid = false;
        std::snprintf(detail, sizeof(detail), "magazine %.3f m from the well, %.1f deg off: clip %d -> %d, reserve %d -> %d, state %s",
            m_SelfTestMeasure, m_SelfTestMeasureB, m_SelfTestClip0, g.clip, m_SelfTestReserve0, g.reserve, state());
        SelfTestReport(label("spare seated after the slap"), g.clip > m_SelfTestClip0 &&
            g.clip + g.reserve == m_SelfTestClip0 + m_SelfTestReserve0, detail);
        m_SelfTestClip0 = g.clip;
        glide(m_LeftControllerPosAbs + Vector(0.0f, 0.0f, -0.15f * scale), 0.3f);
        go(209);
        return false;
    case 209:
        if (inPhase < 0.5f) return false;
        button(m_ActionPrimaryAttack, true);
        go(210);
        return false;
    case 210:
        if (inPhase < 0.12f) return false;
        button(m_ActionPrimaryAttack, false);
        go(211);
        return false;
    case 211:
        if (inPhase < 0.6f) return false;
        std::snprintf(detail, sizeof(detail), "clip %d -> %d, state %s", m_SelfTestClip0, g.clip, state());
        SelfTestReport(label("fires after the slap reload"), g.clip < m_SelfTestClip0, detail);
        return finish();
    default:
        return fail("phase", "lost");
    }
}

// Live grenade projectiles in the client entity list (entity index, origin).
void VR::SelfTestListProjectiles(std::vector<std::pair<int, Vector>>& out)
{
    out.clear();
    if (!m_Game || !m_Game->m_ClientEntityList)
        return;
    const int highest = (std::min)(2048, m_Game->m_ClientEntityList->GetHighestEntityIndex());
    for (int i = 1; i <= highest; ++i)
    {
        C_BaseEntity* const entity = m_Game->GetClientEntity(i);
        if (!entity)
            continue;
        const char* const name = m_Game->GetNetworkClassName(reinterpret_cast<uintptr_t*>(entity));
        if (!name || (!std::strstr(name, "PipeBombProjectile") && !std::strstr(name, "MolotovProjectile") &&
            !std::strstr(name, "VomitJarProjectile")))
            continue;
        Vector origin{};
        if (VR_TryGetEntityAbsOrigin(entity, origin))
            out.emplace_back(i, origin);
    }
}

// ---- Hand throws: handthrow=pipe|molotov|bile --------------------------------
// Draws the grenade from the chest holster with the grip (hold-to-grip), taps
// the trigger (pin), then throws it three times with fresh grenades: an
// overhand swing, an underhand toss and a plain drop, letting go of the grip
// while the hand still moves. Checks, per throw:
//   * HandThrow servers: the client sent the release with a measured hand
//     velocity; the projectile is created at the hand inside the release
//     command (listen host: the server-side spawn record, release -> spawn
//     time; dedicated: the projectile seen near the hand); its velocity points
//     along the swing (drop: no hand speed); the hand is empty right after.
//   * Servers without hand throws: reports the native throw's delay (INFO).
// A screenshot is taken ~70 ms after each release (l4d2vr_selftest_ht_*.png).
bool VR::RunHandThrowStep(const std::string& step, float t, C_BasePlayer* player)
{
    (void)t;
    using l4d2vr_selftest::Key;
    auto cmd = [&](const char* text) { if (m_Game) m_Game->ClientCmd_Unrestricted(text); };
    auto button = [&](vr::VRActionHandle_t action, bool down) { l4d2vr_selftest::SetButton(action, down); };
    char detail[360];

    const std::string item = step.substr(10);
    const bool pipe = item == "pipe", molotov = item == "molotov", bile = item == "bile";
    if (!pipe && !molotov && !bile)
    {
        SelfTestReport("hand throw", false, "unknown item (use pipe, molotov or bile)");
        return true;
    }
    const int expectedId = pipe ? C_WeaponCSBase::PIPE_BOMB : molotov ? C_WeaponCSBase::MOLOTOV : C_WeaponCSBase::VOMITJAR;
    const char* const give = pipe ? "give pipe_bomb" : molotov ? "give molotov" : "give vomitjar";
    const char* const use = pipe ? "use weapon_pipe_bomb" : molotov ? "use weapon_molotov" : "use weapon_vomitjar";
    static const char* const kStyles[3] = { "overhand", "underhand", "drop" };

    struct Throw
    {
        bool released = false;
        uint64_t releaseMs = 0u;
        Vector releaseHand{};
        Vector swingStart{};
        Vector swingDir{};            // scripted hand direction in the world
        bool drawn = false;
        uint32_t serverSpawns = 0u;
        uint32_t clientThrows = 0u;
        bool seen = false;
        uint64_t seenMs = 0u;
        Vector seenPos{};
        Vector seenPos2{};
        uint64_t seenMs2 = 0u;
        bool shot = false;
        bool shot2 = false;
        bool watching = false;
        uint64_t pinMs = 0u;
        std::unordered_set<int> before;
    };
    static Throw s{};
    static bool s_savedToggle = true;
    static bool s_handMode = false;
    // Stage timing is relative to the previous stage and needs a few game
    // frames, so a game-thread stall (content loading) cannot skip a stage.
    static int s_stage = -1;
    static uint64_t s_stageMs = 0u;
    static int s_stageFrames = 0;
    const uint64_t nowMs = GetTickCount64();
    if (m_SelfTestStage != s_stage)
    {
        s_stage = m_SelfTestStage;
        s_stageMs = nowMs;
        s_stageFrames = 0;
    }
    ++s_stageFrames;
    auto next = [&](uint64_t minMs, int minFrames = 4) -> bool
    {
        if (nowMs - s_stageMs < minMs || s_stageFrames < minFrames)
            return false;
        ++m_SelfTestStage;
        return true;
    };

    const int throwIndex = m_SelfTestStage / 100;
    const int stage = m_SelfTestStage % 100;
    if (throwIndex >= 3)
    {
        m_GripToggleEnabled = s_savedToggle;
        return true;
    }
    const char* const style = kStyles[throwIndex];
    const bool drop = throwIndex == 2;

    // Watch for the projectile from the swing on (every frame).
    if (s.watching)
    {
        std::vector<std::pair<int, Vector>> now;
        SelfTestListProjectiles(now);
        for (const auto& p : now)
        {
            if (s.before.count(p.first))
                continue;
            if (!s.seen)
            {
                s.seen = true;
                s.seenMs = nowMs;
                s.seenPos = p.second;
            }
            else if (!s.seenMs2 && nowMs - s.seenMs >= 60u)
            {
                s.seenMs2 = nowMs;
                s.seenPos2 = p.second;
            }
        }
        // The hand right after the release, then the grenade in flight (the
        // client draws new entities one interpolation interval late).
        if (s.released && !s.shot && nowMs - s.releaseMs >= 70u)
        {
            s.shot = true;
            SelfTestRequestShot(std::string("ht_") + item + "_" + style, 1);
        }
        if (s.released && !s.shot2 && nowMs - s.releaseMs >= 220u)
        {
            s.shot2 = true;
            SelfTestRequestShot(std::string("ht_") + item + "_" + style + "_flight", 1);
        }
    }

    // Head-local swing keys (x forward, y toward the weapon hand, z up).
    static const l4d2vr_selftest::Vec3 kWind[3] = { { -0.15f, 0.25f, 0.10f }, { -0.05f, 0.22f, -0.62f }, { 0.35f, 0.20f, -0.30f } };
    static const l4d2vr_selftest::Vec3 kEnd[3] = { { 0.60f, 0.15f, 0.25f }, { 0.45f, 0.18f, -0.20f }, { 0.35f, 0.20f, -0.30f } };

    switch (stage)
    {
    case 0:
        if (throwIndex == 0)
        {
            s_savedToggle = m_GripToggleEnabled;
            m_GripToggleEnabled = false;   // hold-to-grip: letting go of the grip is the release
        }
        s = Throw{};
        button(m_ActionWeaponGrip, false);
        button(m_ActionPrimaryAttack, false);
        cmd(give);
        ++m_SelfTestStage;
        return false;
    case 1:
        next(1000u);
        return false;
    case 2:
        SelfTestReachHolster(2);
        if (next(1000u, 20)) button(m_ActionWeaponGrip, true);
        return false;
    case 3:
        SelfTestReachHolster(2);
        if (next(500u, 10))
        {
            const int id = SelfTestActiveWeaponId(player);
            s.drawn = id == expectedId;
            std::snprintf(detail, sizeof(detail), "%s: active weapon id=%d (grip draw from the chest)", style, id);
            SelfTestReport("hand throw: grip draws the grenade", s.drawn, detail);
            if (!s.drawn)
                cmd(use);
            const l4d2vr_selftest::Vec3& w = kWind[throwIndex];
            SelfTestHoldRight(w, QAngle(throwIndex == 0 ? -20.0f : throwIndex == 1 ? 30.0f : 0.0f, 0.0f, 0.0f));
        }
        return false;
    case 4:
        if (next(700u, 10)) { button(m_ActionPrimaryAttack, true); s.pinMs = nowMs; }   // pull the pin
        return false;
    case 5:
        if (next(100u, 3)) button(m_ActionPrimaryAttack, false);
        return false;
    case 6:
        if (next(500u, 10))
        {
            s_handMode = HandThrowServerReady();
            s.swingStart = GetRightControllerViewmodelAbsPos();
            // The scripted direction in the world: head-local -> tracking -> world.
            const l4d2vr_selftest::Vec3 d{ kEnd[throwIndex].x - kWind[throwIndex].x, kEnd[throwIndex].y - kWind[throwIndex].y,
                kEnd[throwIndex].z - kWind[throwIndex].z };
            const l4d2vr_selftest::Vec3 tr = l4d2vr_selftest::HeadLocalToTracking(SelfTestVec(0.0f, 0.0f, 0.0f),
                m_HmdPose.TrackedDeviceAng.y, d);
            const l4d2vr_hand_throw::V3 wd = l4d2vr_hand_throw::TrackingToWorld(l4d2vr_hand_throw::V3(tr.x, tr.y, tr.z),
                m_RotationOffset, 1.0f);
            s.swingDir = Vector(wd.x, wd.y, wd.z);
            VectorNormalize(s.swingDir);
            std::vector<std::pair<int, Vector>> now;
            SelfTestListProjectiles(now);
            for (const auto& p : now)
                s.before.insert(p.first);
            s.watching = true;
            s.serverSpawns = m_HandThrowServerSpawnCount.load(std::memory_order_acquire);
            s.clientThrows = m_HandThrowCount.load(std::memory_order_acquire);
            if (throwIndex == 0)
                SelfTestStartPath(
                    { Key{ 0.0f, kWind[0] }, Key{ 0.22f, kEnd[0], false }, Key{ 0.40f, SelfTestVec(0.72f, 0.10f, 0.0f) } },
                    { Key{ 0.0f, SelfTestVec(-20.0f, 0.0f, 0.0f) }, Key{ 0.22f, SelfTestVec(-35.0f, 0.0f, 0.0f), false } });
            else if (throwIndex == 1)
                SelfTestStartPath(
                    { Key{ 0.0f, kWind[1] }, Key{ 0.30f, kEnd[1], false }, Key{ 0.50f, SelfTestVec(0.55f, 0.15f, 0.0f) } },
                    { Key{ 0.0f, SelfTestVec(30.0f, 0.0f, 0.0f) }, Key{ 0.30f, SelfTestVec(-10.0f, 0.0f, 0.0f), false } });
        }
        return false;
    case 7:
        // Let go while the hand still moves (drop: the hand is still).
        if (next(throwIndex == 0 ? 150u : throwIndex == 1 ? 220u : 300u, 2))
        {
            button(m_ActionWeaponGrip, false);
            s.released = true;
            s.releaseMs = nowMs;
            s.releaseHand = GetRightControllerViewmodelAbsPos();
        }
        return false;
    case 8:
        if (!next(2500u, 20))
            return false;
        break;
    default:
        return false;
    }

    // ---- Report this throw ----
    const bool slotEmpty = player->Weapon_GetSlot(2) == nullptr;
    const int activeId = SelfTestActiveWeaponId(player);
    const double seenAfter = s.seen ? static_cast<double>(s.seenMs - s.releaseMs) : -1.0;
    const float seenFromHand = s.seen ? (s.seenPos - s.releaseHand).Length() : -1.0f;
    Vector seenVelocity{};
    if (s.seen && s.seenMs2 > s.seenMs)
        seenVelocity = (s.seenPos2 - s.seenPos) * (1000.0f / static_cast<float>(s.seenMs2 - s.seenMs));
    if (!s_handMode)
    {
        // No hand throws on this server: the trigger tap started the native
        // throw (pin-pull animation + throw timer); report its timing only.
        Game::logMsg("[VR][SelfTest] INFO hand throw %s %s without the HandThrow feature: trigger tap -> projectile seen %.0f ms "
            "(grip let go %.0f ms after the tap), %.0f units from the hand at the release, slot %s", item.c_str(), style,
            s.seen ? static_cast<double>(s.seenMs - s.pinMs) : -1.0, static_cast<double>(s.releaseMs - s.pinMs), seenFromHand,
            slotEmpty ? "empty" : "occupied");
    }
    else
    {
        Vector releaseVelocity{}, spawnOrigin{}, spawnVelocity{}, spawnBody{};
        int spawnCommand = 0;
        {
            std::lock_guard<std::mutex> lock(m_HandThrowMutex);
            releaseVelocity = m_HandThrowLastReleaseVelocity;
            spawnOrigin = m_HandThrowServerSpawnOrigin;
            spawnVelocity = m_HandThrowServerSpawnVelocity;
            spawnBody = m_HandThrowServerSpawnBody;
            spawnCommand = m_HandThrowServerSpawnCommand;
        }
        const bool released = m_HandThrowCount.load(std::memory_order_acquire) == s.clientThrows + 1u;
        const bool serverSeen = m_HandThrowServerSpawnCount.load(std::memory_order_acquire) == s.serverSpawns + 1u;
        const uint64_t releaseMs = m_HandThrowLastReleaseMs.load(std::memory_order_acquire);
        const double releaseToSpawn = serverSeen
            ? static_cast<double>(m_HandThrowServerSpawnMs.load(std::memory_order_acquire)) - static_cast<double>(releaseMs)
            : -1.0;
        const float handSpeed = releaseVelocity.Length();
        const float releaseSpeedMps = handSpeed / (std::max)(1.0f, m_VRScale);
        const float handAlong = handSpeed > 1.0f ? DotProduct(releaseVelocity, s.swingDir) / handSpeed : 0.0f;
        std::snprintf(detail, sizeof(detail), "%s: release by %s, hand %.2f m/s (%.0f %.0f %.0f), %.2f of it along the scripted swing",
            style, s.drawn ? "grip" : "trigger", releaseSpeedMps, releaseVelocity.x, releaseVelocity.y, releaseVelocity.z, handAlong);
        SelfTestReport("hand throw: release with the measured hand velocity", released &&
            (drop ? releaseSpeedMps < 0.3f : releaseSpeedMps > 1.0f && handAlong > 0.9f), detail);

        if (serverSeen)
        {
            std::snprintf(detail, sizeof(detail),
                "%s: server spawn %.0f ms after the release (command %d), %.1f units from the release hand",
                style, releaseToSpawn, spawnCommand, (spawnOrigin - s.releaseHand).Length());
            // One frame to send, the host's next tick, and the command itself.
            SelfTestReport("hand throw: spawned from the hand without delay", releaseToSpawn >= 0.0 &&
                releaseToSpawn <= 50.0 + 34.0 && (spawnOrigin - s.releaseHand).Length() < 12.0f, detail);
            // The server applied exactly hand * scale + body (below the clamp).
            const Vector handPart = spawnVelocity - spawnBody;
            const Vector expected = releaseVelocity * m_ManualThrowHandVelocityScale;
            const float error = (handPart - expected).Length();
            std::snprintf(detail, sizeof(detail),
                "%s: velocity (%.0f %.0f %.0f) speed %.0f = body (%.0f %.0f %.0f) + hand x%.1f (%.0f %.0f %.0f), error %.1f",
                style, spawnVelocity.x, spawnVelocity.y, spawnVelocity.z, spawnVelocity.Length(), spawnBody.x, spawnBody.y,
                spawnBody.z, m_ManualThrowHandVelocityScale, expected.x, expected.y, expected.z, error);
            SelfTestReport(drop ? "hand throw: a drop falls from the hand with the body" :
                throwIndex == 0 ? "hand throw: overhand gets the hand's velocity" : "hand throw: underhand gets the hand's velocity",
                error < 3.0f + 0.02f * expected.Length() && (drop ? handPart.Length() < 30.0f : handPart.Length() > 150.0f), detail);
        }
        else
        {
            std::snprintf(detail, sizeof(detail),
                "%s: projectile seen %.0f ms after the release, %.1f units from the release hand, moving (%.0f %.0f %.0f) "
                "(dedicated server: its log has 'hand throw spawn')", style, seenAfter, seenFromHand,
                seenVelocity.x, seenVelocity.y, seenVelocity.z);
            SelfTestReport("hand throw: spawned from the hand without delay", s.seen && seenAfter <= 400.0 &&
                seenFromHand >= 0.0f && seenFromHand < 60.0f, detail);
        }
        std::snprintf(detail, sizeof(detail), "%s: grenade slot %s, active weapon id %d", style,
            slotEmpty ? "empty" : "occupied", activeId);
        SelfTestReport("hand throw: the hand is empty after the throw", slotEmpty && activeId != expectedId, detail);
    }
    s = Throw{};
    m_SelfTestStage = (throwIndex + 1) * 100;
    if (throwIndex + 1 >= 3)
    {
        m_GripToggleEnabled = s_savedToggle;
        s_stage = -1;
        return true;
    }
    return false;
}

// ---- Self-test screenshots ---------------------------------------------------

void VR::SelfTestRequestShot(const std::string& name, int eyes)
{
    {
        std::lock_guard<std::mutex> lock(m_SelfTestShotMutex);
        m_SelfTestShotName = name;
        m_SelfTestShotEyes = eyes;
    }
    m_SelfTestShotRequest.fetch_add(1u, std::memory_order_acq_rel);
}

// Render thread, after both eyes of a frame are complete and before they are
// submitted: saves the requested eyes. deviceLocked: the caller already holds
// the DXVK device lock.
void VR::SelfTestCaptureEyes(IDirect3DSurface9* left, IDirect3DSurface9* right, bool deviceLocked)
{
    const uint32_t request = m_SelfTestShotRequest.load(std::memory_order_acquire);
    if (!m_SelfTestEnabled || request == m_SelfTestShotDone.load(std::memory_order_acquire))
        return;
    std::string name;
    int eyes = 1;
    {
        std::lock_guard<std::mutex> lock(m_SelfTestShotMutex);
        name = m_SelfTestShotName;
        eyes = m_SelfTestShotEyes;
    }
    bool ok = true;
    for (int eye = 0; eye < 2; ++eye)
    {
        if ((eyes & (1 << eye)) == 0)
            continue;
        std::string path = "l4d2vr_selftest_" + name;
        if (eyes == 3)
            path += eye == 0 ? "_l" : "_r";
        path += ".png";
        ok = SelfTestSaveEyePng(eye == 0 ? left : right, eye, deviceLocked, path) && ok;
    }
    m_SelfTestShotOk.store(ok, std::memory_order_release);
    m_SelfTestShotDone.store(request, std::memory_order_release);
}

bool VR::SelfTestSaveEyePng(IDirect3DSurface9* source, int eyeIndex, bool deviceLocked, const std::string& path)
{
    if (!source || !g_D3DVR9)
        return false;
    IDirect3DDevice9* device = nullptr;
    if (FAILED(source->GetDevice(&device)) || !device)
        return false;
    D3DSURFACE_DESC desc{};
    if (FAILED(source->GetDesc(&desc)) || desc.Width == 0 || desc.Height == 0)
    {
        device->Release();
        return false;
    }
    // The eye's visible area (the SteamVR texture bounds), scaled to at most
    // 1024 pixels wide.
    const vr::VRTextureBounds_t& bounds = m_TextureBounds[eyeIndex == 0 ? 0 : 1];
    float u0 = std::clamp((std::min)(bounds.uMin, bounds.uMax), 0.0f, 1.0f);
    float u1 = std::clamp((std::max)(bounds.uMin, bounds.uMax), 0.0f, 1.0f);
    float v0 = std::clamp((std::min)(bounds.vMin, bounds.vMax), 0.0f, 1.0f);
    float v1 = std::clamp((std::max)(bounds.vMin, bounds.vMax), 0.0f, 1.0f);
    if (u1 - u0 < 0.05f || v1 - v0 < 0.05f)
    {
        u0 = v0 = 0.0f;
        u1 = v1 = 1.0f;
    }
    RECT sourceRect{};
    sourceRect.left = static_cast<LONG>(u0 * desc.Width);
    sourceRect.right = static_cast<LONG>(u1 * desc.Width);
    sourceRect.top = static_cast<LONG>(v0 * desc.Height);
    sourceRect.bottom = static_cast<LONG>(v1 * desc.Height);
    const int sourceWidth = (std::max)(1, static_cast<int>(sourceRect.right - sourceRect.left));
    const int sourceHeight = (std::max)(1, static_cast<int>(sourceRect.bottom - sourceRect.top));
    const int width = (std::min)(sourceWidth, 1024);
    const int height = (std::max)(1, sourceHeight * width / sourceWidth);

    std::vector<std::uint8_t> rgb;
    IDirect3DSurface9* scaled = nullptr;
    IDirect3DSurface9* readback = nullptr;
    if (!deviceLocked)
        g_D3DVR9->LockDevice();
    HRESULT hr = device->CreateRenderTarget(width, height, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &scaled, nullptr);
    if (SUCCEEDED(hr))
        hr = device->StretchRect(source, &sourceRect, scaled, nullptr, D3DTEXF_LINEAR);
    if (SUCCEEDED(hr))
        hr = device->CreateOffscreenPlainSurface(width, height, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &readback, nullptr);
    if (SUCCEEDED(hr))
        hr = device->GetRenderTargetData(scaled, readback);
    D3DLOCKED_RECT locked{};
    if (SUCCEEDED(hr))
        hr = readback->LockRect(&locked, nullptr, D3DLOCK_READONLY);
    if (SUCCEEDED(hr))
    {
        rgb.resize(static_cast<size_t>(width) * height * 3u);
        for (int y = 0; y < height; ++y)
        {
            const std::uint8_t* row = static_cast<const std::uint8_t*>(locked.pBits) + static_cast<size_t>(locked.Pitch) * y;
            std::uint8_t* out = rgb.data() + static_cast<size_t>(width) * 3u * y;
            for (int x = 0; x < width; ++x)
            {
                out[x * 3 + 0] = row[x * 4 + 2];
                out[x * 3 + 1] = row[x * 4 + 1];
                out[x * 3 + 2] = row[x * 4 + 0];
            }
        }
        readback->UnlockRect();
    }
    if (readback)
        readback->Release();
    if (scaled)
        scaled->Release();
    if (!deviceLocked)
        g_D3DVR9->UnlockDevice();
    device->Release();

    bool saved = false;
    if (SUCCEEDED(hr))
    {
        const std::vector<std::uint8_t> png = l4d2vr_selftest::EncodePngRgb(rgb.data(), width, height);
        if (FILE* file = std::fopen(path.c_str(), "wb"))
        {
            saved = std::fwrite(png.data(), 1, png.size(), file) == png.size();
            saved = std::fclose(file) == 0 && saved;
        }
    }
    Game::logMsg("[VR][SelfTest] screenshot %s eye=%d %dx%d of %ux%u format=%d hr=0x%08X %s", path.c_str(), eyeIndex,
        width, height, desc.Width, desc.Height, static_cast<int>(desc.Format), static_cast<unsigned int>(hr),
        saved ? "saved" : "failed");
    return saved;
}

bool VR::GetWalkAxisForMove(float& x, float& y)
{
    if (m_SelfTestEnabled && m_SelfTestWalkActive)
    {
        x = m_SelfTestWalkX;
        y = m_SelfTestWalkY;
        return true;
    }
    return GetWalkAxis(x, y);
}

void VR::SelfTestTrackHolsterDraw(int slot, const Vector& itemOrigin, const Vector& eyeOrigin)
{
    if (slot < 0 || slot >= l4d2vr_holster::kZoneCount)
        return;
    l4d2vr_holster::BodyFrame frame{};
    if (!GetHolsterBodyFrame(frame) || frame.unitsPerMeter <= 0.001f)
        return;
    const Vector delta = itemOrigin - eyeOrigin;
    const float s = 1.0f / frame.unitsPerMeter;
    const Vector local(DotProduct(delta, frame.forward) * s, DotProduct(delta, frame.dominant) * s, delta.z * s);
    const int eye = DotProduct(eyeOrigin - m_HmdPosAbs, frame.dominant) > 0.0f ? 1 : 0;
    std::lock_guard<std::mutex> lock(m_SelfTestHolsterTrackMutex);
    SelfTestHolsterTrack& track = m_SelfTestHolsterTrack;
    if (!track.active)
        return;
    if (track.n[eye][slot]++ == 0u)
    {
        track.mn[eye][slot] = local;
        track.mx[eye][slot] = local;
        return;
    }
    Vector& mn = track.mn[eye][slot];
    Vector& mx = track.mx[eye][slot];
    mn.x = (std::min)(mn.x, local.x); mn.y = (std::min)(mn.y, local.y); mn.z = (std::min)(mn.z, local.z);
    mx.x = (std::max)(mx.x, local.x); mx.y = (std::max)(mx.y, local.y); mx.z = (std::max)(mx.z, local.z);
}

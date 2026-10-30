// Self-test steps for Pavlov recoil, exact aim and iron sights.
//
//   sightcheck=<give name>[:<metres>]  recoil off; turns toward a wall about that
//       far away, holds the gun so the left eye looks down its sights (rear
//       sight 12 cm in front of the eye, sight line along the view), fires one
//       shot and measures where the fired ray hits against the sight line.
//       Saves sight_<gun>_<m>m (the sight picture with the impact).
//   recoiltest=<give name>:<1|2>[:<seconds>]  fires with one hand or with the
//       support hand on the foregrip; measures the aim excursion per frame, the
//       fired rays against the kick they were fired with, the recovery, and
//       compares the kick with the recoil model replayed at the same shot
//       times. Saves recoil_<gun>_<n>h_rest and _kick.

namespace
{
    float VR_RecoilAngleDiff(float a, float b)
    {
        float d = a - b;
        while (d > 180.0f) d -= 360.0f;
        while (d < -180.0f) d += 360.0f;
        return d;
    }

    bool VR_RecoilTrace(Game* game, C_BasePlayer* player, const Vector& start, const Vector& direction, Vector& hit)
    {
        if (!game || !game->m_EngineTrace)
            return false;
        __try
        {
            CGameTrace trace;
            Ray_t ray;
            CTraceFilterSkipSelf filter(reinterpret_cast<IHandleEntity*>(player), 0);
            ray.Init(start, start + direction * 8192.0f);
            game->m_EngineTrace->TraceRay(ray, STANDARD_TRACE_MASK, &filter, &trace);
            if (!(trace.fraction < 1.0f))
                return false;
            hit = trace.endpos;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // Distance (game units) of a point from a ray's line.
    float VR_RecoilLineDistance(const Vector& origin, const Vector& unitDir, const Vector& point)
    {
        const Vector d = point - origin;
        const Vector along = unitDir * DotProduct(d, unitDir);
        return VectorLength(d - along);
    }

    bool VR_RecoilAutoWeapon(int id)
    {
        switch (id)
        {
        case 2: case 7: case 33: case 5: case 26: case 9: case 34: case 37: return true;
        default: return false;
        }
    }
}

void VR::RecordRecoilShotRay(const Vector& origin, const QAngle& angles, bool server, int weaponId)
{
    RecoilShotRecord record{};
    record.ms = GetTickCount64();
    record.origin = origin;
    record.angles = angles;
    record.server = server;
    record.weaponId = weaponId;
    record.offset = m_RecoilHands[0].offset;
    std::lock_guard<std::mutex> lock(m_RecoilShotLogMutex);
    if (m_RecoilShotLog.size() < 512)
        m_RecoilShotLog.push_back(record);
}

void VR::RecoilTestAlignSightsToEye(bool leftGun)
{
    Vector rear{}, dir{};
    if (leftGun ? !GetLeftPistolSightRay(rear, dir, true) : !GetIronSightRay(rear, dir, nullptr, true))
        return;
    const Vector eye = GetViewOriginLeft();
    Vector eyeForward = m_HmdForward;
    if (VectorNormalize(eyeForward) < 0.5f)
        return;
    QAngle want{}, have{};
    QAngle::VectorAngles(eyeForward, want);
    QAngle::VectorAngles(dir, have);
    const Vector desired = eye + eyeForward * (m_RecoilTestEyeRelief * m_VRScale);
    if (leftGun)
    {
        if (!m_SelfTestLeftAngWorldValid)
        {
            m_SelfTestLeftAngWorld = m_LeftControllerTrackedAngAbs;
            m_SelfTestLeftAngWorldValid = true;
        }
        m_SelfTestLeftAngWorld.x += 0.5f * VR_RecoilAngleDiff(want.x, have.x);
        m_SelfTestLeftAngWorld.y += 0.5f * VR_RecoilAngleDiff(want.y, have.y);
        if (!m_SelfTestLeftWorldValid)
        {
            m_SelfTestLeftWorld = m_LeftControllerPosAbs;
            m_SelfTestLeftWorldValid = true;
        }
        m_SelfTestLeftWorld += (desired - rear) * 0.5f;
        return;
    }
    m_SelfTestRightAngLocal.x += 0.5f * VR_RecoilAngleDiff(want.x, have.x);
    m_SelfTestRightAngLocal.y += 0.5f * VR_RecoilAngleDiff(want.y, have.y);
    if (!m_SelfTestRightWorldValid)
    {
        m_SelfTestRightWorld = m_RightControllerPosAbs;
        m_SelfTestRightWorldValid = true;
    }
    m_SelfTestRightWorld += (desired - rear) * 0.5f;
}

bool VR::RunRecoilSelfTestStep(const std::string& step, float t, C_BasePlayer* player, bool& handled)
{
    // ironsight=0|1: fire along the sight line (1) or the old bore alignment
    // (0); forgets the measured per-model corrections so they are re-measured.
    if (step.rfind("ironsight=", 0) == 0)
    {
        handled = true;
        m_IronSightAimEnabled = step.substr(10) != "0";
        m_SightCorrections.clear();
        m_SightCalibrationModel.clear();
        Game::logMsg("[VR][SelfTest] iron sight aim %s", m_IronSightAimEnabled ? "on" : "off (bore alignment)");
        return true;
    }
    // pellets=<give>:<1|0>: Pavlov (1) or stock (0) shotgun pellets: sights on
    // a wall ~6 m away, one shot, a photograph of the pellet holes, and the
    // count of pellet-loop draws the client and the listen host replaced.
    if (step.rfind("pellets=", 0) == 0)
    {
        handled = true;
        const std::string rest = step.substr(8);
        const size_t colon = rest.find(':');
        const std::string gun = rest.substr(0, colon);
        const bool pavlov = colon == std::string::npos || rest.substr(colon + 1) != "0";
        bool fired = false;
        auto atP = [&](int stage, float when) { if (fired || m_SelfTestStage != stage || t < when) return false; m_SelfTestStage = stage + 1; fired = true; return true; };
        if (atP(0, 0.0f))
        {
            m_RecoilTestSavedEnabled = m_RecoilEnabled;
            m_RecoilEnabled = false;
            m_ShotgunPavlovPattern = pavlov;
            char text[96];
            std::snprintf(text, sizeof(text), "give %s", gun.c_str());
            if (m_Game) m_Game->ClientCmd_Unrestricted(text);
            std::snprintf(text, sizeof(text), "use weapon_%s", gun.c_str());
            if (m_Game) m_Game->ClientCmd_Unrestricted(text);
            m_RecoilTestAligning = true;
            m_SelfTestRightWorldValid = false;
        }
        if (m_RecoilTestAligning)
            RecoilTestAlignSightsToEye();
        if (atP(1, 4.0f))
        {
            m_SelfTestValueA = static_cast<int>(m_PelletDrawsClient.load());
            m_SelfTestValueB = static_cast<int>(m_PelletDrawsServer.load());
            l4d2vr_selftest::SetButton(m_ActionPrimaryAttack, true);
        }
        if (atP(2, 4.1f))
            l4d2vr_selftest::SetButton(m_ActionPrimaryAttack, false);
        if (atP(3, 4.6f))
        {
            char name[96];
            std::snprintf(name, sizeof(name), "pellets_%s_%s", gun.c_str(), pavlov ? "pavlov" : "stock");
            SelfTestRequestShot(name, 1);
            m_SelfTestSerial = m_SelfTestShotRequest.load(std::memory_order_acquire);
        }
        if (m_SelfTestStage == 4 && (m_SelfTestShotDone.load(std::memory_order_acquire) == m_SelfTestSerial || t > 9.0f))
        {
            m_SelfTestStage = 5;
            m_RecoilTestAligning = false;
            m_RecoilEnabled = m_RecoilTestSavedEnabled;
            const int client = static_cast<int>(m_PelletDrawsClient.load()) - m_SelfTestValueA;
            const int server = static_cast<int>(m_PelletDrawsServer.load()) - m_SelfTestValueB;
            const l4d2vr_shotgun_pattern::Weapon* w = l4d2vr_shotgun_pattern::WeaponFor(
                player && player->GetActiveWeapon() ? static_cast<int>(static_cast<C_WeaponCSBase*>(player->GetActiveWeapon())->GetWeaponID()) : 0);
            char detail[192];
            std::snprintf(detail, sizeof(detail), "%s %s: pellet draws replaced client=%d server=%d (pattern active=%d, opt-in sent=%d)",
                gun.c_str(), pavlov ? "Pavlov" : "stock", client, server, ShotgunPatternActiveForLocalShot(w ? w->weaponId : 0) ? 1 : 0,
                m_ShotgunPatternSent);
            // Pavlov: every pellet after the first replaces two draws on both
            // sides (an even count > 0); stock: none.
            const bool ok = w && (pavlov ? (client > 0 && client % 2 == 0 && (server == client || server == 0))
                                         : (client == 0 && server == 0));
            SelfTestReport(pavlov ? "shotgun fires Pavlov's pellet pattern" : "shotgun keeps stock pellets", ok, detail);
            m_ShotgunPavlovPattern = true;
        }
        return m_SelfTestStage >= 5;
    }
    const bool sight = step.rfind("sightcheck=", 0) == 0;
    const bool recoil = step.rfind("recoiltest=", 0) == 0;
    handled = sight || recoil;
    if (!handled)
        return false;
    auto cmd = [&](const char* text) { if (m_Game) m_Game->ClientCmd_Unrestricted(text); };
    bool stageFired = false;
    auto at = [&](int stage, float when) -> bool
    {
        if (stageFired || m_SelfTestStage != stage || t < when) return false;
        m_SelfTestStage = stage + 1;
        stageFired = true;
        return true;
    };
    auto button = [&](vr::VRActionHandle_t action, bool down) { l4d2vr_selftest::SetButton(action, down); };
    char detail[192];

    // Arguments: <give name>[:<a>[:<b>]].
    std::vector<std::string> args;
    {
        std::string rest = step.substr(11);
        size_t pos = 0;
        while (true)
        {
            const size_t colon = rest.find(':', pos);
            args.push_back(rest.substr(pos, colon == std::string::npos ? std::string::npos : colon - pos));
            if (colon == std::string::npos) break;
            pos = colon + 1;
        }
    }
    const std::string give = args[0];
    C_WeaponCSBase* weapon = player ? static_cast<C_WeaponCSBase*>(player->GetActiveWeapon()) : nullptr;
    const int weaponId = weapon ? static_cast<int>(weapon->GetWeaponID()) : 0;
    auto aimDir = [&]() { Vector d = m_AimLineEnd - m_AimLineStart; VectorNormalize(d); return d; };
    auto shots = [&]()
    {
        std::lock_guard<std::mutex> lock(m_RecoilShotLogMutex);
        return m_RecoilShotLog;
    };

    if (sight)
    {
        const float metres = args.size() > 1 ? std::clamp(static_cast<float>(std::atof(args[1].c_str())), 2.0f, 60.0f) : 15.0f;
        // "dual_left": the dual pistols' left gun, aimed with the left hand and
        // fired with the left trigger (dual pistols must already be held).
        const bool leftGun = give == "dual_left";
        if (at(0, 0.0f))
        {
            m_RecoilTestSavedEnabled = m_RecoilEnabled;
            m_RecoilEnabled = false;
            char text[96];
            // A second pistol makes dual pistols: "pistol" uses the one held.
            if (give != "pistol" && !leftGun)
            {
                std::snprintf(text, sizeof(text), "give %s", give.c_str());
                cmd(text);
            }
            std::snprintf(text, sizeof(text), "use weapon_%s", leftGun ? "pistol" : give.c_str());
            cmd(text);
            m_RecoilTestLeftGun = leftGun;
        }
        if (at(1, 1.0f))
        {
            // Turn toward a wall about `metres` away (level ray from the eye).
            const Vector eye = GetViewOriginLeft();
            float bestDelta = 0.0f, bestError = 1.0e9f, bestDistance = 0.0f;
            for (int i = 0; i < 72; ++i)
            {
                const float yaw = m_HmdAngAbs.y + i * 5.0f;
                Vector dir{};
                QAngle::AngleVectors(QAngle(0.0f, yaw, 0.0f), &dir, nullptr, nullptr);
                Vector hit{};
                if (!VR_RecoilTrace(m_Game, player, eye, dir, hit)) continue;
                const float d = VectorLength(hit - eye) / m_VRScale;
                if (std::fabs(d - metres) < bestError) { bestError = std::fabs(d - metres); bestDelta = i * 5.0f; bestDistance = d; }
            }
            m_RotationOffset += bestDelta;
            m_RotationOffset -= 360.0f * std::floor(m_RotationOffset / 360.0f);
            Game::logMsg("[VR][SelfTest] sightcheck %s: turned %.0f deg toward a wall %.1f m away (wanted %.0f)",
                give.c_str(), bestDelta, bestDistance, metres);
        }
        if (at(2, 1.5f))
        {
            m_RecoilTestAligning = true;
            m_SelfTestRightWorldValid = false;
            m_SelfTestLeftWorldValid = false;
            m_SelfTestLeftAngWorldValid = false;
        }
        if (m_RecoilTestAligning)
            RecoilTestAlignSightsToEye(leftGun);
        if (at(3, 5.5f))
        {
            Vector rear{}, dir{};
            const bool ok = leftGun ? GetLeftPistolSightRay(rear, dir, true) : GetIronSightRay(rear, dir, nullptr, true);
            m_RecoilTestSightRear = rear;
            m_RecoilTestSightDir = dir;
            Vector hit{};
            m_SelfTestValueA = ok && VR_RecoilTrace(m_Game, player, rear, dir, hit) ? 1 : 0;
            m_RecoilTestSightHit = hit;
            const Vector eye = GetViewOriginLeft();
            const float eyeOff = VR_RecoilLineDistance(rear, dir, eye) / m_VRScale * 100.0f;
            Game::logMsg("[VR][SelfTest] sightcheck %s: sight ray ok=%d rear=(%.2f %.2f %.2f) dir=(%.4f %.4f %.4f) "
                "eye %.2f cm off the sight line, sight hits %.2f m away; rear/front in hand (%.2f %.2f %.2f)/(%.2f %.2f %.2f)",
                give.c_str(), ok ? 1 : 0, rear.x, rear.y, rear.z, dir.x, dir.y, dir.z, eyeOff,
                VectorLength(hit - rear) / m_VRScale, m_SightRearInHand.x, m_SightRearInHand.y, m_SightRearInHand.z,
                m_SightFrontInHand.x, m_SightFrontInHand.y, m_SightFrontInHand.z);
            {
                std::lock_guard<std::mutex> lock(m_RecoilShotLogMutex);
                m_RecoilShotLog.clear();
            }
            m_RecoilShotLogActive.store(true, std::memory_order_release);
            button(leftGun ? m_ActionSecondaryAttack : m_ActionPrimaryAttack, true);
        }
        if (at(4, 5.6f))
            button(leftGun ? m_ActionSecondaryAttack : m_ActionPrimaryAttack, false);
        if (at(5, 6.0f))
        {
            char name[96];
            std::snprintf(name, sizeof(name), "sight_%s%s_%.0fm", give.c_str(),
                !leftGun && give == "pistol" && m_DualPistolsActive.load(std::memory_order_acquire) ? "_dual_right" : "", metres);
            SelfTestRequestShot(name, 1);
            m_SelfTestSerial = m_SelfTestShotRequest.load(std::memory_order_acquire);
        }
        if (m_SelfTestStage == 6 && (m_SelfTestShotDone.load(std::memory_order_acquire) == m_SelfTestSerial || t > 11.0f))
        {
            m_SelfTestStage = 7;
            m_RecoilShotLogActive.store(false, std::memory_order_release);
            m_RecoilTestAligning = false;
            m_RecoilEnabled = m_RecoilTestSavedEnabled;
            m_SelfTestLeftWorldValid = false;
            m_SelfTestLeftAngWorldValid = false;
            const auto log = shots();
            const float distance = VectorLength(m_RecoilTestSightHit - m_RecoilTestSightRear) / m_VRScale;
            float worst = -1.0f;
            int server = 0, client = 0;
            for (const RecoilShotRecord& r : log)
            {
                Vector dir{};
                QAngle::AngleVectors(r.angles, &dir, nullptr, nullptr);
                Vector hit{};
                if (!VR_RecoilTrace(m_Game, player, r.origin, dir, hit)) continue;
                const float offCm = VR_RecoilLineDistance(m_RecoilTestSightRear, m_RecoilTestSightDir, hit) / m_VRScale * 100.0f;
                Game::logMsg("[VR][SelfTest] sightcheck %s: %s shot hits %.2f m away, %.2f cm from the sight line",
                    give.c_str(), r.server ? "server" : "client", VectorLength(hit - r.origin) / m_VRScale, offCm);
                (r.server ? server : client)++;
                // The server's ray is what hits; the client's prediction must agree.
                worst = std::max(worst, offCm);
            }
            const float limitCm = 1.0f + 0.0025f * distance * 100.0f; // 1 cm + 0.15 deg
            std::snprintf(detail, sizeof(detail), "%s at %.1f m: worst %.2f cm from the sight line (limit %.2f), %d server + %d client ray(s)",
                give.c_str(), distance, worst, limitCm, server, client);
            SelfTestReport("bullet lands on the sight picture", m_SelfTestValueA == 1 && worst >= 0.0f && worst <= limitCm, detail);
        }
        return m_SelfTestStage >= 7 && t >= 7.0f;
    }

    // recoiltest=<give>:<hands>[:<seconds>]
    const int hands = args.size() > 1 ? std::clamp(std::atoi(args[1].c_str()), 1, 2) : 1;
    const float seconds = args.size() > 2 ? std::clamp(static_cast<float>(std::atof(args[2].c_str())), 0.1f, 6.0f) : 1.5f;
    if (at(0, 0.0f))
    {
        m_RecoilTestSavedEnabled = m_RecoilEnabled;
        m_RecoilEnabled = true;
        char text[96];
        if (give != "pistol")
        {
            std::snprintf(text, sizeof(text), "give %s", give.c_str());
            cmd(text);
        }
        std::snprintf(text, sizeof(text), "use weapon_%s", give.c_str());
        cmd(text);
        SelfTestHoldRight(SelfTestVec(0.40f, 0.12f, -0.22f), QAngle(0.0f, 0.0f, 0.0f));
        m_SelfTestPhase = 0;
    }
    // The support hand rides on the foregrip box of the drawn gun.
    if (hands == 2 && m_SelfTestStage >= 1 && m_SelfTestStage < 6)
    {
        MagazineInteractionBoxSnapshot box{};
        if (GetVrHandsTwoHandedGripWeaponBox(box))
        {
            const Vector c = (box.mins + box.maxs) * 0.5f;
            m_SelfTestLeftWorld = box.origin + box.axisX * c.x + box.axisY * c.y + box.axisZ * c.z;
            m_SelfTestLeftWorldValid = true;
        }
    }
    if (at(1, 1.5f)) {}
    if (hands == 2 && m_SelfTestStage == 2 && t >= 3.0f && m_SelfTestPhase == 0 && !m_VrHandsTwoHandedGripActive)
    {
        button(m_ActionOffHandGrip, true);
        m_SelfTestPhase = 1;
    }
    if (m_SelfTestPhase == 1 && t >= 3.15f)
    {
        button(m_ActionOffHandGrip, false);
        m_SelfTestPhase = 2;
    }
    if (at(2, 4.0f))
    {
        m_SelfTestValueA = m_VrHandsTwoHandedGripActive ? 1 : 0;
        m_RecoilTestRefDir = aimDir();
        m_RecoilTestPeakPitch = m_RecoilTestPeakYaw = m_RecoilTestPeakBack = 0.0f;
        m_RecoilTestShotTimes.clear();
        {
            std::lock_guard<std::mutex> lock(m_RecoilShotLogMutex);
            m_RecoilShotLog.clear();
        }
        m_RecoilShotLogActive.store(true, std::memory_order_release);
        char name[96];
        std::snprintf(name, sizeof(name), "recoil_%s_%dh_rest", give.c_str(), hands);
        SelfTestRequestShot(name, 1);
        Game::logMsg("[VR][SelfTest] recoiltest %s: %d hand(s), foregrip=%d, weapon %d (%s)", give.c_str(), hands,
            m_SelfTestValueA, weaponId, l4d2vr_recoil::ProfileForWeapon(weaponId) ? l4d2vr_recoil::ProfileForWeapon(weaponId)->pavlovGun : "none");
    }
    const float fireStart = 4.6f;
    const bool automatic = VR_RecoilAutoWeapon(weaponId);
    const bool pump = weaponId == 3 || weaponId == 8;
    if (m_SelfTestStage == 3 && t >= fireStart)
    {
        const float ft = t - fireStart;
        bool down = false;
        if (automatic) down = ft < seconds;
        else if (pump) down = ft < 0.08f;
        else down = ft < seconds && std::fmod(ft, 0.35f) < 0.08f;
        button(m_ActionPrimaryAttack, down);
        // Excursion of the aim (the drawn sights) from where the hand points.
        const Vector d = aimDir();
        QAngle ref{}, now{};
        QAngle::VectorAngles(m_RecoilTestRefDir, ref);
        QAngle::VectorAngles(d, now);
        m_RecoilTestPeakPitch = std::max(m_RecoilTestPeakPitch, -VR_RecoilAngleDiff(now.x, ref.x));
        m_RecoilTestPeakYaw = std::max(m_RecoilTestPeakYaw, std::fabs(VR_RecoilAngleDiff(now.y, ref.y)));
        m_RecoilTestPeakBack = std::max(m_RecoilTestPeakBack, std::fabs(m_RecoilHands[0].offset.backCm));
        // Photograph the first kick near its peak (~60 ms after the first shot).
        const auto log = shots();
        if (!log.empty() && m_SelfTestPhase < 10 && GetTickCount64() - log.front().ms >= 60u)
        {
            char name[96];
            std::snprintf(name, sizeof(name), "recoil_%s_%dh_kick", give.c_str(), hands);
            SelfTestRequestShot(name, 1);
            m_SelfTestPhase = 10;
        }
        if (ft >= seconds + 0.1f)
        {
            button(m_ActionPrimaryAttack, false);
            m_SelfTestStage = 4;
        }
    }
    if (at(4, fireStart + seconds + 2.0f))
    {
        m_RecoilShotLogActive.store(false, std::memory_order_release);
        const auto log = shots();
        // Rest: the gun is back where the hand points.
        const Vector d = aimDir();
        QAngle ref{}, now{};
        QAngle::VectorAngles(m_RecoilTestRefDir, ref);
        QAngle::VectorAngles(d, now);
        const float rest = std::sqrt(VR_RecoilAngleDiff(now.x, ref.x) * VR_RecoilAngleDiff(now.x, ref.x) +
            VR_RecoilAngleDiff(now.y, ref.y) * VR_RecoilAngleDiff(now.y, ref.y));

        // Fired rays against the kick they were fired with (server rays on a
        // listen host, else the client's predicted ones) and the impact walk.
        bool haveServer = false;
        for (const auto& r : log) haveServer |= r.server;
        float worstRayErr = 0.0f, firstX = 0.0f, firstY = 0.0f, maxRise = 0.0f, maxSide = 0.0f;
        int used = 0;
        std::vector<uint64_t> times;
        const l4d2vr_recoil::Profile* profile = l4d2vr_recoil::ProfileForWeapon(weaponId);
        Vector right{}, up{};
        Vector fwd = m_RecoilTestRefDir;
        QAngle refAng{};
        QAngle::VectorAngles(fwd, refAng);
        QAngle::AngleVectors(refAng, nullptr, &right, &up);
        for (const auto& r : log)
        {
            if (!r.server) times.push_back(r.ms);
            if (r.server != haveServer) continue;
            const float firedPitch = -VR_RecoilAngleDiff(r.angles.x, ref.x);
            const float firedYaw = -VR_RecoilAngleDiff(r.angles.y, ref.y);
            // The shot left along the kicked gun: its elevation over the rest aim
            // equals the kick it was fired with (rotation about the grip moves
            // the line, so compare angles).
            const float err = std::sqrt((firedPitch - r.offset.pitchDeg) * (firedPitch - r.offset.pitchDeg) +
                (firedYaw - r.offset.yawDeg) * (firedYaw - r.offset.yawDeg));
            worstRayErr = std::max(worstRayErr, err);
            Vector dir{};
            QAngle::AngleVectors(r.angles, &dir, nullptr, nullptr);
            Vector hit{};
            if (VR_RecoilTrace(m_Game, player, r.origin, dir, hit))
            {
                const float x = DotProduct(hit, right) / m_VRScale * 100.0f, y = DotProduct(hit, up) / m_VRScale * 100.0f;
                if (used == 0) { firstX = x; firstY = y; }
                maxRise = std::max(maxRise, y - firstY);
                maxSide = std::max(maxSide, std::fabs(x - firstX));
            }
            ++used;
        }
        // Replay the model at the observed shot times (90 Hz) for comparison.
        float modelPitch = 0.0f, modelYaw = 0.0f;
        if (profile && !times.empty())
        {
            l4d2vr_recoil::State model;
            l4d2vr_recoil::Inputs in{};
            in.twoHanded = m_SelfTestValueA == 1;
            model.Reset(profile, in);
            size_t next = 0;
            for (uint64_t ms = times.front(); ms < times.back() + 1500u; ms += 11u)
            {
                while (next < times.size() && times[next] <= ms) { model.Fire(); ++next; }
                in.triggerHeld = automatic && next < times.size();
                const l4d2vr_recoil::Offset o = model.Tick(0.011f, in);
                modelPitch = std::max(modelPitch, o.pitchDeg);
                modelYaw = std::max(modelYaw, std::fabs(o.yawDeg));
            }
        }
        const float scale = std::clamp(m_RecoilScale, 0.0f, 4.0f);
        modelPitch *= scale;
        modelYaw *= scale;
        Game::logMsg("[VR][SelfTest] recoiltest %s %dh: shots=%zu (%s rays %d) peak pitch %.2f deg (model %.2f) yaw %.2f (model %.2f) "
            "back %.2f cm; worst fired-ray vs kick %.2f deg; impacts rise %.1f cm, side %.1f cm; rest %.2f deg",
            give.c_str(), hands, times.size(), haveServer ? "server" : "client", used, m_RecoilTestPeakPitch, modelPitch,
            m_RecoilTestPeakYaw, modelYaw, m_RecoilTestPeakBack, worstRayErr, maxRise, maxSide, rest);
        const bool gripOk = hands == 1 ? m_SelfTestValueA == 0 : m_SelfTestValueA == 1;
        const bool kickOk = !times.empty() && std::fabs(m_RecoilTestPeakPitch - modelPitch) <= 0.25f * modelPitch + 0.4f;
        std::snprintf(detail, sizeof(detail), "%s %dh: %zu shots, kick %.2f deg (model %.2f), ray error %.2f deg, rest %.2f deg, foregrip %d",
            give.c_str(), hands, times.size(), m_RecoilTestPeakPitch, modelPitch, worstRayErr, rest, m_SelfTestValueA);
        SelfTestReport("recoil kicks the gun as the model and recovers", gripOk && kickOk && worstRayErr <= 0.35f && rest <= 0.3f, detail);
        m_RecoilEnabled = m_RecoilTestSavedEnabled;
        m_SelfTestLeftWorldValid = false;
        if (hands == 2 && m_VrHandsTwoHandedGripActive)
        {
            button(m_ActionOffHandGrip, true);
            m_SelfTestPhase = 20;
        }
    }
    if (m_SelfTestPhase == 20 && t >= fireStart + seconds + 2.15f)
    {
        button(m_ActionOffHandGrip, false);
        m_SelfTestPhase = 21;
    }
    return m_SelfTestStage >= 5 && t >= fireStart + seconds + 2.5f;
}

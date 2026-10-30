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

void VR::RecordRecoilShotRay(const Vector& origin, const QAngle& angles, bool server, int weaponId, int command)
{
    RecoilShotRecord record{};
    record.command = command;
    record.handForward = m_RecoilPreKickForward;
    record.handRight = m_RecoilPreKickRight;
    record.handUp = m_RecoilPreKickUp;
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
    // pellets=<give>:<1|0>: fixed (1) or stock (0) shotgun pellets: sights on
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
            std::snprintf(name, sizeof(name), "pellets_%s_%s", gun.c_str(), pavlov ? "fixed" : "stock");
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
                gun.c_str(), pavlov ? "fixed" : "stock", client, server, ShotgunPatternActiveForLocalShot(w ? w->weaponId : 0) ? 1 : 0,
                m_ShotgunPatternSent);
            // fixed: every pellet after the first replaces two draws on both
            // sides (an even count > 0); stock: none.
            const bool ok = w && (pavlov ? (client > 0 && client % 2 == 0 && (server == client || server == 0))
                                         : (client == 0 && server == 0));
            SelfTestReport(pavlov ? "shotgun fires the fixed pellet pattern" : "shotgun keeps stock pellets", ok, detail);
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
            m_RecoilTestProjectile.clear();
            m_RecoilTestProjectileEnt = -1;
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
        // The grenade launcher fires a projectile, not bullets: follow the
        // grenade the client sees right after the shot.
        if (give == "grenade_launcher" && m_SelfTestStage >= 4 && m_SelfTestStage <= 6 && m_Game && m_Game->m_ClientEntityList &&
            m_RecoilTestProjectile.size() < 40)
        {
            if (m_RecoilTestProjectileEnt < 0)
            {
                const int highest = std::min(m_Game->m_ClientEntityList->GetHighestEntityIndex(), 4095);
                for (int i = 1; i <= highest && m_RecoilTestProjectileEnt < 0; ++i)
                {
                    C_BaseEntity* e = m_Game->GetClientEntity(i);
                    const char* cls = e ? m_Game->GetNetworkClassName(reinterpret_cast<uintptr_t*>(e)) : nullptr;
                    if (cls && std::strstr(cls, "GrenadeLauncher_Projectile"))
                        m_RecoilTestProjectileEnt = i;
                }
            }
            if (m_RecoilTestProjectileEnt >= 0)
            {
                C_BaseEntity* e = m_Game->GetClientEntity(m_RecoilTestProjectileEnt);
                if (e)
                {
                    const Vector p = e->GetAbsOrigin();
                    if (m_RecoilTestProjectile.empty() || VectorLength(p - m_RecoilTestProjectile.back().pos) > 0.01f)
                        m_RecoilTestProjectile.push_back({ GetTickCount64(), p });
                }
            }
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
            if (give == "grenade_launcher")
            {
                // The grenade flies on an arc, so only its sideways deviation from
                // the sight line (gravity never moves it sideways) is checked; the
                // drop below the line is reported.
                const auto& s = m_RecoilTestProjectile;
                Vector side = CrossProduct(m_RecoilTestSightDir, Vector(0.0f, 0.0f, 1.0f));
                Vector upPerp = CrossProduct(side, m_RecoilTestSightDir);
                VectorNormalize(side);
                VectorNormalize(upPerp);
                float lateralDeg = 0.0f, verticalDeg = 0.0f, along = 0.0f, firstOffCm = -1.0f;
                bool ok = false;
                for (const auto& sample : s)
                {
                    const Vector d = sample.pos - m_RecoilTestSightRear;
                    const float a = DotProduct(d, m_RecoilTestSightDir);
                    if (firstOffCm < 0.0f)
                        firstOffCm = VR_RecoilLineDistance(m_RecoilTestSightRear, m_RecoilTestSightDir, sample.pos) / m_VRScale * 100.0f;
                    if (a < 0.5f * m_VRScale)
                        continue;
                    along = a;
                    lateralDeg = std::atan2(DotProduct(d, side), a) * 57.29578f;
                    verticalDeg = std::atan2(DotProduct(d, upPerp), a) * 57.29578f;
                    ok = true;
                }
                std::snprintf(detail, sizeof(detail), "grenade_launcher: %zu projectile sample(s); %.2f m along the sight line it is "
                    "%.2f deg sideways and %.2f deg above (gravity: below) the line; first seen %.1f cm from it",
                    s.size(), along / m_VRScale, lateralDeg, verticalDeg, firstOffCm);
                SelfTestReport("grenade leaves along the sight line", m_SelfTestValueA == 1 && ok && std::fabs(lateralDeg) <= 0.5f &&
                    verticalDeg <= 0.3f && verticalDeg > -6.0f, detail);
                return m_SelfTestStage >= 7 && t >= 7.0f;
            }
            const float limitCm = 1.0f + 0.0025f * distance * 100.0f; // 1 cm + 0.15 deg
            std::snprintf(detail, sizeof(detail), "%s at %.1f m: worst %.2f cm from the sight line (limit %.2f), %d server + %d client ray(s)",
                give.c_str(), distance, worst, limitCm, server, client);
            SelfTestReport("bullet lands on the sight picture", m_SelfTestValueA == 1 && worst >= 0.0f && worst <= limitCm, detail);
        }
        return m_SelfTestStage >= 7 && t >= 7.0f;
    }

    // recoiltest=<give>:<hands>[:<seconds>]
    // Every angle is measured in the gun hand's own frame before the kick
    // (m_RecoilPreKick*): head or hand drift then cancels out and what is left
    // is the recoil the gun carries.
    const int hands = args.size() > 1 ? std::clamp(std::atoi(args[1].c_str()), 1, 2) : 1;
    const float seconds = args.size() > 2 ? std::clamp(static_cast<float>(std::atof(args[2].c_str())), 0.1f, 6.0f) : 1.5f;
    auto inHand = [&](const Vector& d, const Vector& f, const Vector& r, const Vector& u, float& pitch, float& yaw)
    {
        const float x = DotProduct(d, f), y = DotProduct(d, r), z = DotProduct(d, u);
        pitch = std::atan2(z, std::sqrt(x * x + y * y)) * 57.29578f;
        yaw = std::atan2(y, x) * 57.29578f;
    };
    auto aimInHand = [&](float& pitch, float& yaw)
    {
        inHand(aimDir(), m_RecoilPreKickForward, m_RecoilPreKickRight, m_RecoilPreKickUp, pitch, yaw);
    };
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
        m_RecoilTestGripSince = -1.0f;
        m_RecoilTestFireStart = 1.0e9f;
    }
    // The support hand goes onto the foregrip target and stays there once the
    // grip is taken (the gun's kick must not drag it along).
    if (hands == 2 && m_SelfTestStage >= 1 && m_SelfTestStage < 5)
    {
        Vector target{};
        if (!m_VrHandsTwoHandedGripActive && GetTwoHandedGripTargetPoint(target))
        {
            m_SelfTestLeftWorld = target;
            m_SelfTestLeftWorldValid = true;
        }
        if (m_VrHandsTwoHandedGripActive && m_RecoilTestGripSince < 0.0f)
            m_RecoilTestGripSince = t;
    }
    if (at(1, 1.5f)) {}
    // One hand: level the gun (the aim line, i.e. the sights) before the
    // reference, so the shots go into a wall and the photographs show it.
    if (hands == 1 && m_SelfTestStage >= 1 && m_SelfTestStage <= 2 && t >= 1.0f)
    {
        Vector d = aimDir();
        QAngle a{};
        QAngle::VectorAngles(d, a);
        m_SelfTestRightAngLocal.x += 0.3f * VR_RecoilAngleDiff(0.0f, a.x);
    }
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
    // Reference: one hand at 4 s; two hands 1.5 s after the grip was taken
    // (the two-hand aim line settles), at the latest at 9 s.
    const bool refReady = hands == 1 ? t >= 4.0f :
        ((m_RecoilTestGripSince >= 0.0f && t >= m_RecoilTestGripSince + 1.5f && t >= 4.0f) || t >= 9.0f);
    if (m_SelfTestStage == 2 && refReady && !stageFired)
    {
        m_SelfTestStage = 3;
        stageFired = true;
        m_SelfTestValueA = m_VrHandsTwoHandedGripActive ? 1 : 0;
        m_RecoilTestRefDir = aimDir();
        float rp = 0.0f, ry = 0.0f;
        aimInHand(rp, ry);
        m_RecoilTestRefLocal = Vector(rp, ry, 0.0f);
        {
            const Vector d = aimDir();
            m_RecoilTestRefComp = Vector(DotProduct(d, m_RecoilPreKickForward), DotProduct(d, m_RecoilPreKickRight),
                DotProduct(d, m_RecoilPreKickUp));
        }
        m_RecoilTestPeakPitch = m_RecoilTestPeakYaw = m_RecoilTestPeakBack = 0.0f;
        m_RecoilTestFireStart = t + 0.6f;
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
    const float fireStart = m_RecoilTestFireStart;
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
        float p = 0.0f, y = 0.0f;
        aimInHand(p, y);
        m_RecoilTestPeakPitch = std::max(m_RecoilTestPeakPitch, p - m_RecoilTestRefLocal.x);
        m_RecoilTestPeakYaw = std::max(m_RecoilTestPeakYaw, std::fabs(y - m_RecoilTestRefLocal.y));
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
    // Trace every 50 ms: the model's offset, the aim in the hand's frame, the
    // hand itself against the world reference and the body yaw.
    if (m_SelfTestStage >= 3 && m_SelfTestStage <= 4 && t >= fireStart - 0.2f && GetTickCount64() - m_RecoilTestTraceMs >= 50u)
    {
        m_RecoilTestTraceMs = GetTickCount64();
        float p = 0.0f, y = 0.0f;
        aimInHand(p, y);
        QAngle refWorld{}, handWorld{};
        QAngle::VectorAngles(m_RecoilTestRefDir, refWorld);
        QAngle::VectorAngles(m_RecoilPreKickForward, handWorld);
        const l4d2vr_recoil::Offset& o = m_RecoilHands[0].offset;
        Game::logMsg("[VR][SelfTest] recoiltrace t=%.2f shots=%llu offset p=%.2f y=%.2f b=%.2f | aim-in-hand p=%.2f y=%.2f | "
            "hand world p=%.2f y=%.2f (ref aim %.2f %.2f) | head yaw %.2f body yaw %.2f | foregrip=%d",
            t - fireStart, static_cast<unsigned long long>(m_RecoilHands[0].shots), o.pitchDeg, o.yawDeg, o.backCm,
            p - m_RecoilTestRefLocal.x, y - m_RecoilTestRefLocal.y, -handWorld.x, handWorld.y, -refWorld.x, refWorld.y,
            m_HmdAngAbs.y, m_RotationOffset, m_VrHandsTwoHandedGripActive ? 1 : 0);
    }
    if (m_SelfTestStage == 4 && t >= fireStart + seconds + 2.0f)
    {
        m_SelfTestStage = 5;
        m_RecoilShotLogActive.store(false, std::memory_order_release);
        const auto log = shots();
        float p = 0.0f, y = 0.0f;
        aimInHand(p, y);
        const float rest = std::sqrt((p - m_RecoilTestRefLocal.x) * (p - m_RecoilTestRefLocal.x) +
            (y - m_RecoilTestRefLocal.y) * (y - m_RecoilTestRefLocal.y));

        // Each client-predicted ray, in the hand's frame of its own frame,
        // against the reference plus the kick it was fired with; each server
        // ray (listen host) against the client ray of the same command.
        float worstRayErr = 0.0f, worstServerVsClient = -1.0f;
        int clientRays = 0, serverRays = 0, matched = 0;
        std::vector<uint64_t> times;
        float firstX = 0.0f, firstY = 0.0f, maxRise = 0.0f, maxSide = 0.0f;
        int impacts = 0;
        for (const auto& r : log)
        {
            Vector dir{};
            QAngle::AngleVectors(r.angles, &dir, nullptr, nullptr);
            if (!r.server)
            {
                ++clientRays;
                times.push_back(r.ms);
                // Where the model says the shot goes: the reference aim turned by
                // the kick it was fired with, in that frame's hand basis.
                Vector f = r.handForward, rr = r.handRight, u = r.handUp;
                l4d2vr_recoil::ApplyToBasis(r.offset, f, rr, u);
                Vector expected = f * m_RecoilTestRefComp.x + rr * m_RecoilTestRefComp.y + u * m_RecoilTestRefComp.z;
                VectorNormalize(expected);
                worstRayErr = std::max(worstRayErr,
                    std::acos(std::clamp(DotProduct(expected, dir), -1.0f, 1.0f)) * 57.29578f);
                Vector hit{};
                if (VR_RecoilTrace(m_Game, player, r.origin, dir, hit))
                {
                    // Impact walk on the wall in the hand's frame (cm).
                    const float x = DotProduct(hit - r.origin, r.handRight), z = DotProduct(hit - r.origin, r.handUp);
                    const float along = std::max(1.0f, DotProduct(hit - r.origin, r.handForward));
                    const float sx = x / along, sz = z / along; // tangent of the angles
                    if (impacts == 0) { firstX = sx; firstY = sz; }
                    maxRise = std::max(maxRise, (sz - firstY));
                    maxSide = std::max(maxSide, std::fabs(sx - firstX));
                    ++impacts;
                }
                continue;
            }
            ++serverRays;
            for (const auto& c : log)
                if (!c.server && c.command == r.command && r.command != 0)
                {
                    Vector cdir{};
                    QAngle::AngleVectors(c.angles, &cdir, nullptr, nullptr);
                    const float a = std::acos(std::clamp(DotProduct(cdir, dir), -1.0f, 1.0f)) * 57.29578f;
                    worstServerVsClient = std::max(worstServerVsClient, a);
                    ++matched;
                    break;
                }
        }
        // Replay the model at the observed shot times (90 Hz) for comparison.
        const l4d2vr_recoil::Profile* profile = l4d2vr_recoil::ProfileForWeapon(weaponId);
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
        Game::logMsg("[VR][SelfTest] recoiltest %s %dh: %d client + %d server rays (%d matched, worst server-vs-client %.2f deg); "
            "peak pitch %.2f deg (model %.2f) yaw %.2f (model %.2f) back %.2f cm; worst ray vs kick %.2f deg; "
            "impact walk up %.1f%% side %.1f%% of range; rest %.2f deg",
            give.c_str(), hands, clientRays, serverRays, matched, worstServerVsClient, m_RecoilTestPeakPitch, modelPitch,
            m_RecoilTestPeakYaw, modelYaw, m_RecoilTestPeakBack, worstRayErr, maxRise * 100.0f, maxSide * 100.0f, rest);
        const bool gripOk = hands == 1 ? m_SelfTestValueA == 0 : m_SelfTestValueA == 1;
        // Kick within 15% (+0.4 deg) of the model: frame timing and the
        // model's random recovery jitter differ between the two.
        const bool kickOk = !times.empty() && std::fabs(m_RecoilTestPeakPitch - modelPitch) <= 0.15f * modelPitch + 0.4f;
        const bool serverOk = serverRays == 0 || (matched == serverRays && worstServerVsClient <= 0.1f);
        std::snprintf(detail, sizeof(detail), "%s %dh: %zu shots, kick %.2f deg (model %.2f), ray error %.2f deg, server-client %.2f, rest %.2f deg, foregrip %d",
            give.c_str(), hands, times.size(), m_RecoilTestPeakPitch, modelPitch, worstRayErr, worstServerVsClient, rest, m_SelfTestValueA);
        SelfTestReport("recoil kicks the gun as the model and recovers",
            gripOk && kickOk && serverOk && worstRayErr <= 0.35f && rest <= 0.3f, detail);
        char name[96];
        std::snprintf(name, sizeof(name), "recoil_%s_%dh_after", give.c_str(), hands);
        SelfTestRequestShot(name, 1);
        m_RecoilEnabled = m_RecoilTestSavedEnabled;
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
        m_SelfTestLeftWorldValid = false;
    }
    if (m_SelfTestStage >= 5 && t >= fireStart + seconds + 2.5f)
    {
        m_SelfTestLeftWorldValid = false;
        return true;
    }
    return false;
}

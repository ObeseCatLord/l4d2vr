// Pavlov-style recoil and exact VR aim: shot bookkeeping, per-command shot
// poses and fixed shotgun pellets (see vr/vr_recoil.inl, vr_recoil.h and
// vr_shotgun_pattern.h).

float __cdecl Hooks::dClientSharedRandomFloat(const char* name, float minValue, float maxValue, int additionalSeed)
{
    bool replaced = false;
    const float value = PelletDraw(g_ClientPelletShot, hkClientSharedRandomFloat.fOriginal, name, minValue, maxValue, additionalSeed, replaced);
    if (replaced && m_VR) m_VR->m_PelletDrawsClient.fetch_add(1u, std::memory_order_relaxed);
    return value;
}

float __cdecl Hooks::dServerSharedRandomFloat(const char* name, float minValue, float maxValue, int additionalSeed)
{
    bool replaced = false;
    const float value = PelletDraw(g_ServerPelletShot, hkServerSharedRandomFloat.fOriginal, name, minValue, maxValue, additionalSeed, replaced);
    if (replaced && m_VR) m_VR->m_PelletDrawsServer.fetch_add(1u, std::memory_order_relaxed);
    return value;
}

void Hooks::InitRecoilHooks()
{
    if (void* client = FindSharedRandomFloat("client.dll"))
        hkClientSharedRandomFloat.createHook(client, &dClientSharedRandomFloat);
    else
        Game::logMsg("[VR][ShotgunPattern] client SharedRandomFloat not found; stock pellets");
    if (void* server = FindSharedRandomFloat("server.dll"))
        hkServerSharedRandomFloat.createHook(server, &dServerSharedRandomFloat);
    else
        Game::logMsg("[VR][ShotgunPattern] server SharedRandomFloat not found; stock pellets on a listen host");
}

void Hooks::EnableRecoilHooks()
{
    if (hkClientSharedRandomFloat.pTarget) hkClientSharedRandomFloat.enableHook();
    if (hkServerSharedRandomFloat.pTarget) hkServerSharedRandomFloat.enableHook();
    Game::logMsg("[VR][ShotgunPattern] SharedRandomFloat hooks client=%d server=%d",
        hkClientSharedRandomFloat.isEnabled ? 1 : 0, hkServerSharedRandomFloat.isEnabled ? 1 : 0);
}

bool Hooks::ListenHostShotgunPatternReady()
{
    return hkServerSharedRandomFloat.isEnabled;
}

// CreateMove: remember the pose a firing command will be fired along.
void Hooks::RecoilCacheShotPose(int commandNumber, bool attack)
{
    if (!m_VR || !m_Game || !attack || commandNumber <= 0 || !m_VR->m_IsVREnabled || m_VR->m_ForceNonVRServerMovement ||
        m_VR->m_MouseModeEnabled)
        return;
    // Firearms only: throws, melee and mounted guns keep their own poses.
    C_BasePlayer* localPlayer = m_Game->m_EngineClient
        ? reinterpret_cast<C_BasePlayer*>(m_Game->GetClientEntity(m_Game->m_EngineClient->GetLocalPlayer())) : nullptr;
    C_WeaponCSBase* weapon = localPlayer ? reinterpret_cast<C_WeaponCSBase*>(localPlayer->GetActiveWeapon()) : nullptr;
    if (!weapon || !l4d2vr_calibration::IsFirearm(static_cast<int>(weapon->GetWeaponID())) ||
        m_VR->IsUsingMountedGun(localPlayer) || m_Game->m_IsMeleeWeaponActive)
        return;
    Vector position{};
    QAngle angles{};
    if (!BuildEncodedVRUsercmdControllerPose(m_VR, m_Game, position, angles))
        return;
    if (m_VR->CanCalibrateBulletAim() && !m_VR->ScopeChangesAim())
    {
        Vector origin{}, direction{};
        if (m_VR->GetBulletAimRay(origin, direction) && IsFiniteControllerPosition(origin) && VectorNormalize(direction) > 0.0001f)
        {
            QAngle rayAngles{};
            QAngle::VectorAngles(direction, rayAngles);
            NormalizeAndClampViewAngles(rayAngles);
            if (IsFiniteViewAngle(rayAngles))
            {
                position = origin;
                angles = rayAngles;
            }
        }
    }
    StoreRecoilShotPose(commandNumber, position, angles);
}

bool Hooks::RecoilShotPoseForCommand(int commandNumber, Vector& position, QAngle& angles)
{
    return LoadRecoilShotPose(commandNumber, position, angles);
}

// Client prediction fired a shot for the local player.
void Hooks::RecoilAfterLocalShot(int weaponId, bool leftPistol, uintptr_t weaponTag)
{
    if (!m_VR)
        return;
    const int command = g_RecoilClientCommand > 0 ? g_RecoilClientCommand : m_VR->m_CurrentPredictedHitFeedbackCmdNumber;
    m_VR->OnRecoilShot(weaponId, leftPistol, command, weaponTag);
}

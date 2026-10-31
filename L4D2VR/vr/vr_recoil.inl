// Pavlov-style recoil for VR players (vr_recoil.h). Every shot starts a kick
// that UpdateWeaponRecoil turns, frame by frame, into an offset of the gun
// relative to the hand: the gun basis (drawn viewmodel, aim line, scope and
// the bullet ray) pitches up, yaws and slides back, then recovers. The
// controller itself never moves, so the player counters the kick with their
// own hands. Desktop players and mouse mode are never affected.

namespace
{
    bool VR_RecoilCrouched(const VR* vr, C_BasePlayer* player)
    {
        // Pavlov: head below 140 cm (scaled with the player).
        if (!vr || !player || !(vr->m_VRScale > 0.0f))
            return false;
        const float headHeight = vr->m_HmdPosAbs.z - player->GetAbsOrigin().z;
        return std::isfinite(headHeight) && headHeight < 1.40f * vr->m_VRScale;
    }
}

void VR::OnRecoilShot(int weaponId, bool leftPistol, int commandNumber, uintptr_t weaponTag)
{
    m_LastLocalShotMs.store(GetTickCount64(), std::memory_order_release);
    if (!m_IsVREnabled || !m_RecoilEnabled || m_MouseModeEnabled)
        return;
    const l4d2vr_recoil::Profile* profile = l4d2vr_recoil::ProfileForWeapon(weaponId);
    if (!profile)
        return;
    RecoilHand& hand = m_RecoilHands[leftPistol ? 1 : 0];
    if (!hand.gate.Accept(commandNumber))
        return;
    if (hand.weaponId != weaponId || (weaponTag && hand.weaponTag && weaponTag != hand.weaponTag))
    {
        l4d2vr_recoil::Inputs in{};
        in.twoHanded = !leftPistol && IsVrHandsTwoHandedGripPoseActive();
        hand.state.Reset(profile, in);
        hand.state.Seed(static_cast<uint32_t>(GetTickCount64()) ^ (leftPistol ? 0x5bd1e995u : 0x2545F491u));
        hand.weaponId = weaponId;
        if (weaponTag) hand.weaponTag = weaponTag;
    }
    hand.state.Fire();
    ++hand.shots;
    m_RecoilShotSerial.fetch_add(1u, std::memory_order_acq_rel);
    if (m_RecoilDebugLog)
        Game::logMsg("[VR][Recoil] shot weapon=%d (%s) hand=%s command=%d heat=%.2f",
            weaponId, profile->pavlovGun, leftPistol ? "left" : "gun", commandNumber, hand.state.Heat());
}

bool VR::ShotgunPatternActiveForLocalShot(int weaponId) const
{
    if (!m_IsVREnabled || !m_SpreadDisabled || !m_ShotgunPavlovPattern || m_ForceNonVRServerMovement ||
        !l4d2vr_shotgun_pattern::WeaponFor(weaponId))
        return false;
    // Predict the pattern only where the server fires it: the dedicated plugin
    // or listen host advertises it and this player opted in.
    return Hooks::s_ServerUnderstandsVR && ServerFeature(l4d2vr_features::ShotgunPattern) && m_ShotgunPatternSent == 1;
}

void VR::SendShotgunPatternPreference()
{
    if (!m_Game || !Hooks::s_ServerUnderstandsVR || !ServerFeature(l4d2vr_features::ShotgunPattern))
    {
        if (!Hooks::s_ServerUnderstandsVR || m_ServerFeatureMask.load(std::memory_order_acquire) == 0u)
            m_ShotgunPatternSent = -1;
        return;
    }
    const int wanted = (m_IsVREnabled && m_SpreadDisabled && m_ShotgunPavlovPattern && !m_ForceNonVRServerMovement) ? 1 : 0;
    if (wanted == m_ShotgunPatternSent)
        return;
    char command[64];
    std::snprintf(command, sizeof(command), "%s %d", l4d2vr_shotgun_pattern::kOptInCommand, wanted);
    m_Game->ServerCmd(command, true);
    m_ShotgunPatternSent = wanted;
    Game::logMsg("[VR][ShotgunPattern] sent %s", command);
}

void VR::UpdateWeaponRecoil(C_BasePlayer* localPlayer)
{
    SendShotgunPatternPreference();

    const float dt = std::clamp(m_LastFrameDuration, 0.0f, 0.1f);
    C_WeaponCSBase* weapon = localPlayer ? static_cast<C_WeaponCSBase*>(localPlayer->GetActiveWeapon()) : nullptr;
    const int weaponId = weapon ? static_cast<int>(weapon->GetWeaponID()) : 0;
    const uintptr_t weaponTag = reinterpret_cast<uintptr_t>(weapon);
    m_ActiveGunWeaponId.store(localPlayer && m_IsVREnabled ? weaponId : 0, std::memory_order_release);
    const bool eligible = m_IsVREnabled && m_RecoilEnabled && !m_MouseModeEnabled && localPlayer && weapon &&
        !IsUsingMountedGun(localPlayer) && l4d2vr_recoil::ProfileForWeapon(weaponId) != nullptr;

    // The grenade launcher fires a projectile, not FireTerrorBullets: a round
    // leaving the clip is its shot.
    if (eligible && weaponId == static_cast<int>(C_WeaponCSBase::GRENADE_LAUNCHER))
    {
        int clip = -1;
        VR_TryReadI32(reinterpret_cast<const unsigned char*>(weapon), kClip1Offset, clip);
        if (m_RecoilClipWatchWeapon == weaponTag && m_RecoilClipWatchClip >= 0 && clip == m_RecoilClipWatchClip - 1)
            OnRecoilShot(weaponId, false, 0, weaponTag);
        m_RecoilClipWatchWeapon = weaponTag;
        m_RecoilClipWatchClip = clip;
    }
    else
    {
        m_RecoilClipWatchWeapon = 0;
        m_RecoilClipWatchClip = -1;
    }

    m_RecoilPreKickForward = m_RightControllerForward;
    m_RecoilPreKickRight = m_RightControllerRight;
    m_RecoilPreKickUp = m_RightControllerUp;
    const bool dual = m_DualPistolsActive.load(std::memory_order_acquire) ||
        m_LeftHandPistolActive.load(std::memory_order_acquire);
    for (int k = 0; k < 2; ++k)
    {
        RecoilHand& hand = m_RecoilHands[k];
        const bool handEligible = eligible && (k == 0 || (dual && weaponId == static_cast<int>(C_WeaponCSBase::PISTOL)));
        if (!handEligible || hand.weaponId != weaponId || (hand.weaponTag && hand.weaponTag != weaponTag))
        {
            // Another gun (or none): its recoil does not carry over.
            if (hand.weaponId != 0 || hand.state.GetProfile())
            {
                hand.state.Reset(nullptr);
                hand.weaponId = 0;
            }
            hand.weaponTag = handEligible ? weaponTag : 0;
            hand.offset = {};
            if (!handEligible)
                continue;
            hand.weaponId = weaponId;
            l4d2vr_recoil::Inputs in{};
            in.twoHanded = k == 0 && IsVrHandsTwoHandedGripPoseActive();
            hand.state.Reset(l4d2vr_recoil::ProfileForWeapon(weaponId), in);
            hand.state.Seed(static_cast<uint32_t>(GetTickCount64()) ^ (k ? 0x5bd1e995u : 0x2545F491u));
        }

        l4d2vr_recoil::Inputs in{};
        in.twoHanded = k == 0 && (m_RecoilForceTwoHanded >= 0 ? m_RecoilForceTwoHanded == 1 : IsVrHandsTwoHandedGripPoseActive());
        in.triggerHeld = m_PrimaryAttackDown;
        in.crouched = VR_RecoilCrouched(this, localPlayer);
        l4d2vr_recoil::Offset o = hand.state.Tick(dt, in);
        const float scale = std::clamp(m_RecoilScale, 0.0f, 4.0f);
        o.backCm *= scale;
        o.pitchDeg *= scale;
        o.yawDeg *= scale;
        hand.offset = o;
        hand.peakPitch = std::max(hand.peakPitch, std::fabs(o.pitchDeg));
        hand.peakYaw = std::max(hand.peakYaw, std::fabs(o.yawDeg));
        hand.peakBack = std::max(hand.peakBack, std::fabs(o.backCm));
        if (o.backCm == 0.0f && o.pitchDeg == 0.0f && o.yawDeg == 0.0f)
            continue;

        Vector& forward = k == 0 ? m_RightControllerForward : m_LeftControllerForward;
        Vector& right = k == 0 ? m_RightControllerRight : m_LeftControllerRight;
        Vector& up = k == 0 ? m_RightControllerUp : m_LeftControllerUp;
        Vector& position = k == 0 ? m_RightControllerPosAbs : m_LeftControllerPosAbs;
        l4d2vr_recoil::ApplyToBasis(o, forward, right, up);
        VectorNormalize(forward);
        VectorNormalize(right);
        VectorNormalize(up);
        position += forward * (o.backCm * 0.01f * m_VRScale);
        if (k == 0)
        {
            m_RightControllerForwardUnforced = m_RightControllerForward;
            m_LastUnforcedAimDirection = m_RightControllerForward;
        }
    }
}

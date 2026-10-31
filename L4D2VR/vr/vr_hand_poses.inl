// Finger poses of the survivor's own viewmodel hands (vr_hand_pose_library.h).
// Included by vr.cpp. The update thread decides what each gameplay hand is
// doing (free, gun grip, support grip, holding a magazine/shell, on a slide,
// charging handle, pump or bolt) and blends its finger pose; the viewmodel bone
// hooks (hooks_misc.inl) read the result on the render thread and write it to
// the hand's finger bones. Only the local player's own first-person hands are
// posed: other players and desktop players see nothing new.

namespace
{
    const char* HandPoseActivityName(l4d2vr_handpose::HandActivity a)
    {
        using A = l4d2vr_handpose::HandActivity;
        switch (a)
        {
        case A::Free: return "free";
        case A::GunGrip: return "gun";
        case A::Animated: return "animated";
        case A::Support: return "support";
        case A::Magazine: return "magazine";
        case A::Shell: return "shell";
        case A::Handle: return "handle";
        case A::GunGripPosed: return "gunposed";
        }
        return "?";
    }

    bool HandPoseActivityFromName(const std::string& name, l4d2vr_handpose::HandActivity& out)
    {
        using A = l4d2vr_handpose::HandActivity;
        static const std::pair<const char*, A> kNames[] = {
            { "free", A::Free }, { "gun", A::GunGrip }, { "animated", A::Animated }, { "support", A::Support },
            { "magazine", A::Magazine }, { "shell", A::Shell }, { "handle", A::Handle },
            { "gunposed", A::GunGripPosed } };
        for (const auto& entry : kNames)
            if (name == entry.first) { out = entry.second; return true; }
        return false;
    }
}

// Gameplay side: 0 = the support (viewmodel left) arm, 1 = the item (viewmodel right) arm.
l4d2vr_handpose::HandInput VR::ResolveHandPoseInput(int gameplaySide) const
{
    using A = l4d2vr_handpose::HandActivity;
    namespace hp = l4d2vr_handpose;
    hp::HandInput in{};
    const int weaponId = m_ActiveGunWeaponId.load(std::memory_order_acquire);
    in.weaponId = weaponId;

    const bool emptyHands = m_ManualInventoryEmptyHandsActive.load(std::memory_order_acquire);
    const bool dual = m_DualPistolsActive.load(std::memory_order_acquire) &&
        weaponId == static_cast<int>(C_WeaponCSBase::PISTOL);
    // The hand doing the reload (the free hand; with dual pistols the other pistol's hand).
    const int reloadSide = MagazineReloadSupportIsLeft() ? 0 : 1;
    const bool reloadPoseActive = m_MagazineInteractionLeftHandPoseActive.load(std::memory_order_acquire) != 0u;

    if (reloadPoseActive && gameplaySide == reloadSide && !emptyHands)
    {
        const auto state = m_MagazineInteractionState;
        const int reloadWeapon = m_MagazineInteractionWeaponId != 0 ? m_MagazineInteractionWeaponId : weaponId;
        in.weaponId = reloadWeapon;
        if (state == MagazineInteractionManualState::HoldingBolt)
            in.activity = A::Handle;
        else if (m_MagazineInteractionShotgunShellMode)
            in.activity = A::Shell;
        else
            in.activity = A::Magazine;
        return in;
    }

    if (gameplaySide == 1)
    {
        if (emptyHands)
            in.activity = A::Free;
        else if (hp::IsGun(weaponId))
            in.activity = A::GunGrip;
        else
            in.activity = A::Animated;
        return in;
    }

    if (dual && !emptyHands)
    {
        in.activity = A::GunGrip;  // the left pistol: the game's animated grip plus the trigger finger
        return in;
    }
    if (IsVrHandsTwoHandedGripPoseActive())
    {
        in.activity = A::Support;
        return in;
    }
    in.activity = A::Free;
    return in;
}

void VR::UpdateHandPoseTargets()
{
    namespace hp = l4d2vr_handpose;
    const auto now = std::chrono::steady_clock::now();
    float dt = 0.0f;
    if (m_HandPoseLastUpdate.time_since_epoch().count() != 0)
        dt = std::chrono::duration<float>(now - m_HandPoseLastUpdate).count();
    m_HandPoseLastUpdate = now;

    if (!m_HandPosesEnabled || !m_IsVREnabled)
    {
        std::lock_guard<std::mutex> lock(m_HandPoseMutex);
        for (int side = 0; side < 2; ++side)
        {
            m_HandPoseBlenders[side].Reset();
            m_HandPoseOutputs[side] = hp::HandOutput{};
            m_HandPoseOutputValid[side] = false;
        }
        return;
    }

    // Curls per gameplay hand: the item hand's cache follows the item hand; the
    // support hand is the other physical controller.
    std::array<float, 5> itemCurls{};
    const bool itemCurlsValid = GetNativeViewmodelRightHandOpenVRFingerCurls(itemCurls);
    std::array<float, 5> supportCurls{};
    bool supportCurlsValid = false;
    if (!ItemHandIsPhysicalLeft())
        supportCurlsValid = GetNativeViewmodelLeftHandOpenVRFingerCurls(supportCurls);
    else
        supportCurlsValid = GetWorldPoseAnatomicalRightHandOpenVRFingerCurls(supportCurls);

    hp::HandOutput outputs[2];
    for (int side = 0; side < 2; ++side)
    {
        hp::HandInput in = ResolveHandPoseInput(side);
        const std::array<float, 5>& curls = side == 1 ? itemCurls : supportCurls;
        in.curlsValid = side == 1 ? itemCurlsValid : supportCurlsValid;
        for (int f = 0; f < hp::kFingers; ++f)
            in.curls[f] = curls[static_cast<size_t>(f)];

        // Self-test handact=: force an activity for a photograph.
        const int forced = m_SelfTestHandPoseForced[side].load(std::memory_order_acquire);
        if (forced >= 0)
        {
            in.activity = static_cast<hp::HandActivity>(forced & 0xff);
            const int forcedWeapon = forced >> 8;
            if (forcedWeapon > 0)
                in.weaponId = forcedWeapon;
        }

        outputs[side] = m_HandPoseBlenders[side].Step(in, dt, m_HandPoseBlendSeconds, m_HandPoseTriggerFingerScale);

        if (m_HandPoseDebugLog && outputs[side].activity != m_HandPoseLoggedActivity[side])
        {
            m_HandPoseLoggedActivity[side] = outputs[side].activity;
            Game::logMsg("[VR][HandPose] %s hand: %s weapon=%d curls=%d (%.2f %.2f %.2f %.2f %.2f)",
                side == 0 ? "support" : "item", HandPoseActivityName(outputs[side].activity), in.weaponId,
                in.curlsValid ? 1 : 0, in.curls[0], in.curls[1], in.curls[2], in.curls[3], in.curls[4]);
        }
    }

    std::lock_guard<std::mutex> lock(m_HandPoseMutex);
    for (int side = 0; side < 2; ++side)
    {
        m_HandPoseOutputs[side] = outputs[side];
        m_HandPoseOutputValid[side] = true;
        m_HandPoseOutputAt[side] = now;
    }
}

bool VR::GetHandPoseOutput(int viewmodelSide, l4d2vr_handpose::HandOutput& out) const
{
    if (!m_HandPosesEnabled || (viewmodelSide != -1 && viewmodelSide != 1))
        return false;
    const int side = viewmodelSide < 0 ? 0 : 1;
    std::lock_guard<std::mutex> lock(m_HandPoseMutex);
    if (!m_HandPoseOutputValid[side] ||
        std::chrono::duration<float>(std::chrono::steady_clock::now() - m_HandPoseOutputAt[side]).count() > 0.5f)
        return false;
    out = m_HandPoseOutputs[side];
    return true;
}

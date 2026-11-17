// Gameplay mode (2.0.13, vr_gameplay_mode.h): Physical or Classic.
//
// Classic is the keyou91-style simple control set on servers without the
// L4D2VR plugin (or always, with GameplayMode=classic). It is applied in one
// place: the options below read as their classic values while classic is
// active (the parsed config values are kept and come back with Physical), the
// "non-VR server" input path aims the usercmd from the controller exactly as
// upstream does, and the wire policy keeps every usercmd and command stock.

namespace
{
    // Options that read as these values in classic. Every one of them is a
    // config.txt key (tests/test_gameplay_mode_overrides.py checks the names).
    struct ClassicBoolOverride
    {
        const char* key;
        bool VR::* member;
        bool classicValue;
    };

    const ClassicBoolOverride kClassicBoolOverrides[] = {
        // Reload: the reload button, the game's own auto reload on an empty clip,
        // no magazines, shells, bolts, pumps, pouches or magazine glow.
        { "MagazineInteractionEnabled", &VR::m_MagazineInteractionEnabled, false },
        { "MagazineInteractionSuppressEmptyClipAutoReload", &VR::m_MagazineInteractionSuppressEmptyClipAutoReload, false },
        { "PhysicalReloadOnUnsupportedServers", &VR::m_PhysicalReloadOnUnsupportedServers, false },
        { "MagazineEmptyGlowEnabled", &VR::m_MagazineEmptyGlowEnabled, false },
        { "ManualPumpEnabled", &VR::m_ManualPumpEnabled, false },
        { "MagazineSlapEnabled", &VR::m_MagazineSlapEnabled, false },
        { "MagazineDropEnabled", &VR::m_MagazineDropEnabled, false },
        { "MagazineHandPullEnabled", &VR::m_MagazineHandPullEnabled, false },
        // No true akimbo: the dual pistols fire natively from the gun hand.
        { "DualPistolsIndependentHandsEnabled", &VR::m_DualPistolsIndependentHandsEnabled, false },
        { "DualPistolsAkimboOnAnyServer", &VR::m_DualPistolsAkimboOnAnyServer, false },
        { "DualPistolsPhysicalReloadEnabled", &VR::m_DualPistolsPhysicalReloadEnabled, false },
        // No recoil system: the game's spread, shown by the spread crosshair.
        { "RecoilEnabled", &VR::m_RecoilEnabled, false },
        { "SpreadDisabled", &VR::m_SpreadDisabled, false },
        { "ShotgunPavlovPattern", &VR::m_ShotgunPavlovPattern, false },
        { "VrHandsRealBulletSpreadEnabled", &VR::m_VrHandsRealBulletSpreadEnabled, false },
        { "CrosshairEnabled", &VR::m_CrosshairEnabled, true },
        // Holsters select weapons (use / slotN) with the stock draw; the gun hand
        // always holds the active weapon.
        { "GripToggle", &VR::m_GripToggleEnabled, false },
        { "GripReleaseDropEnabled", &VR::m_GripReleaseDropEnabled, false },
        { "HolsterReleaseDrop", &VR::m_HolsterReleaseDropEnabled, false },
        { "HolsterInstantDraw", &VR::m_HolsterInstantDraw, false },
        { "HolsterGrabRequiresEmptyHand", &VR::m_HolsterGrabRequiresEmptyHand, false },
        { "AmbidextrousItemHands", &VR::m_AmbidextrousItemHands, false },
        { "HolsterEmptyHands", &VR::m_HolsterEmptyHands, false },
        { "ChestSlingEnabled", &VR::m_ChestSlingEnabled, false },
        // Throwables and consumables on the game's own buttons.
        { "ManualThrowEnabled", &VR::m_ManualThrowEnabled, false },
        { "GrenadeThrowAssist", &VR::m_GrenadeThrowAssist, false },
        { "ConsumableGesturesEnabled", &VR::m_ConsumableGesturesEnabled, false },
        { "ConsumableGiveEnabled", &VR::m_ConsumableGiveEnabled, false },
        { "ObjectPullEnabled", &VR::m_ObjectPullEnabled, false },
        // Button shove and melee: no motion melee, gun bash or gestures.
        { "MotionGesturesEnabled", &VR::m_MotionGesturesEnabled, false },
        { "PhysicalShoveEnabled", &VR::m_PhysicalShoveEnabled, false },
        { "GunBashSweepEnabled", &VR::m_GunBashSweepEnabled, false },
        { "MotionOnlyMeleeAndShove", &VR::m_MotionOnlyMeleeAndShove, false },
        { "MeleeInertiaEnabled", &VR::m_MeleeInertiaEnabled, false },
        // No input filtering: the trigger fires like the mouse button.
        { "BlockFireOnFriendlyAimEnabled", &VR::m_BlockFireOnFriendlyAimEnabled, false },
        // No automated inputs and no target highlighting.
        { "AutoFlashlightEnabled", &VR::m_AutoFlashlightEnabled, false },
        { "EffectiveAttackRangeIndicatorEnabled", &VR::m_EffectiveAttackRangeIndicatorEnabled, false },
        // Needs the plugin.
        { "Roomscale1To1ServerMove", &VR::m_Roomscale1To1ServerMove, false },
        // The flashlight follows the gun (the game's own beam along the aim).
        { "FlashlightFollowHmd", &VR::m_FlashlightFollowHmd, false },
    };
    constexpr std::size_t kClassicBoolOverrideCount = sizeof(kClassicBoolOverrides) / sizeof(kClassicBoolOverrides[0]);
    // The parsed (physical) values while classic is applied.
    bool g_ClassicSavedBools[kClassicBoolOverrideCount]{};
    // GrenadeThrowMode=arc: the native throw along the shown arc.
    bool g_ClassicSavedGrenadeThrowArc = true;
}

l4d2vr_gameplay_mode::WirePolicy VR::GameplayWirePolicy() const
{
    const bool poseCapable = m_Game && m_Game->m_VRPoseServerCapable.load(std::memory_order_acquire);
    const auto effective = m_IsVREnabled ? GameplayModeEffective() : l4d2vr_gameplay_mode::Effective::Vanilla;
    return l4d2vr_gameplay_mode::PolicyFor(effective, m_ServerFeatureMask.load(std::memory_order_acquire), poseCapable);
}

void VR::ApplyGameplayModeOverrides(bool classic)
{
    if (classic == m_ClassicOverridesApplied)
        return;
    if (classic)
    {
        for (std::size_t i = 0; i < kClassicBoolOverrideCount; ++i)
        {
            g_ClassicSavedBools[i] = this->*kClassicBoolOverrides[i].member;
            this->*kClassicBoolOverrides[i].member = kClassicBoolOverrides[i].classicValue;
        }
        g_ClassicSavedGrenadeThrowArc = m_GrenadeThrowArc;
        m_GrenadeThrowArc = true;
    }
    else
    {
        for (std::size_t i = 0; i < kClassicBoolOverrideCount; ++i)
            this->*kClassicBoolOverrides[i].member = g_ClassicSavedBools[i];
        m_GrenadeThrowArc = g_ClassicSavedGrenadeThrowArc;
    }
    m_ClassicOverridesApplied = classic;
}

// Everything the physical mechanics may hold when classic starts, so nothing
// stays stuck: reloads, held and stowed items, grips, the sling, holster
// draws, dual pistols, melee motion, object pull, throws, mod-held buttons.
void VR::ResetPhysicalStateForClassic()
{
    CancelMagazineInteractionManual();
    ResetMagazineInteractionSession();
    // Server transactions in flight are dropped (their replies are ignored);
    // the protocols themselves stay offered for a return to physical.
    CancelRemoteMagazineRequest();
    CancelRemoteShellRequest();
    m_ManualPumpBlockingFire.store(false, std::memory_order_release);
    m_PhysicalReloadOwnsStockInput.store(false, std::memory_order_release);
    UpdateDualPistolReload(nullptr, false, DualReloadButtons{});
    m_DualPistolsActive.store(false, std::memory_order_release);
    m_LeftHandPistolActive.store(false, std::memory_order_release);
    m_PistolOwnershipMask.store(0u, std::memory_order_release);
    if (m_ObjectPullPhase != ObjectPullClientPhase::Idle || m_ObjectPullDesiredWireCommand != kObjectPullWireNone)
        ResetObjectPullInput(false);
    m_ObjectPullActionDownPrev = false;
    m_ObjectPullCatchActionSuppressMask = 0;
    CancelMeleeMotion();
    // Toggle grip and the item it held.
    m_GripToggle = l4d2vr_grip_toggle::Toggle{};
    m_GripToggleHeldTag.store(0u, std::memory_order_release);
    m_GripToggleRehold.store(false, std::memory_order_release);
    // Stowed weapon (sling / empty hands): the game keeps it active, so it is
    // simply shown in the gun hand again.
    if (m_ChestSlungWeapon.exchange(0u, std::memory_order_acq_rel) != 0u)
        Game::logMsg("[VR][GameplayMode] classic: the stowed weapon is back in the gun hand");
    // Holster draws in flight.
    m_HolsterInstantDrawSlot.store(-1, std::memory_order_release);
    m_HolsterInstantDrawUntilMs.store(0u, std::memory_order_release);
    m_LeftHolsterRequestSlot = -1;
    m_HolsterDrawWatchSlot = -1;
    // The held item goes back to the dominant hand (a holster draw is the only
    // other way an item changes hands, and classic has none).
    if (ItemHandIsPhysicalLeft() != m_LeftHanded)
        RequestItemHand(m_LeftHanded, l4d2vr_item_hand::Reason::Reset);
    // Hand throws, pill gives, consumable holds.
    m_PillGiveImpulsePending.store(false, std::memory_order_release);
    m_PillGiveHoldUntil = {};
    m_ConsumableUseHoldUntil = {};
    m_PhysicalShoveImpulsePending.store(false, std::memory_order_release);
    m_MagazineEmptyGlowActive.store(false, std::memory_order_release);
    m_ReloadHintMask.store(0u, std::memory_order_release);
    Game::logMsg("[VR][GameplayMode] physical state reset for classic (reload, grips, holsters, sling, akimbo, melee, throws)");
}

void VR::UpdateGameplayMode(bool inGame)
{
    using namespace l4d2vr_gameplay_mode;
    static bool s_WasInGame = false;
    static bool s_AnnouncePending = false;
    const uint64_t nowMs = GetTickCount64();
    if (inGame && !s_WasInGame)
    {
        m_GameplayModeJoinMs = nowMs;
        s_AnnouncePending = true;   // log and show the mode of every map / connection
    }
    if (!inGame)
        m_ListenHostActive.store(false, std::memory_order_release);
    s_WasInGame = inGame;

    Inputs in;
    in.config = m_GameplayModeConfig;
    in.vrActive = m_IsVREnabled;
    in.inGame = inGame;
    in.listenHost = m_ListenHostActive.load(std::memory_order_acquire);
    in.serverAck = Hooks::s_ServerUnderstandsVR.load(std::memory_order_acquire);
    in.graceExpired = inGame && nowMs - m_GameplayModeJoinMs >= kGameplayModeAckGraceMs;
    // A listen host also decodes its own commands: the hooks set the ack flag
    // there too, so a listen host reads as acknowledged.
    const Result r = Resolve(in);
    const Effective previous = GameplayModeEffective();
    const bool changed = r.effective != previous;
    const bool classic = ClassicLike(r.effective);
    if (changed && classic && previous == Effective::Physical)
        ResetPhysicalStateForClassic();
    // Idempotent: also applies the overlay at start-up (the first resolved
    // mode equals the initial PendingClassic) and after a config reload.
    ApplyGameplayModeOverrides(classic);
    if (changed)
    {
        m_GameplayModeEffective.store(static_cast<uint8_t>(r.effective), std::memory_order_release);
        m_GameplayModeGeneration.fetch_add(1u, std::memory_order_acq_rel);
    }
    m_GameplayModeReason = r.reason;
    // Classic uses the upstream "non-VR server" input path (aim from the
    // controller into the stock usercmd).
    if (ClassicLike(r.effective))
        m_ForceNonVRServerMovement = true;

    const bool settled = inGame && r.effective != Effective::PendingClassic;
    if ((changed && inGame) || (s_AnnouncePending && settled))
    {
        s_AnnouncePending = !settled;
        const uint32_t features = m_ServerFeatureMask.load(std::memory_order_acquire);
        Game::logMsg("[VR][GameplayMode] effective=%s config=%s reason=%s serverAck=%d listenHost=%d features=0x%X",
            EffectiveName(r.effective), ConfigName(m_GameplayModeConfig), ReasonName(r.reason),
            in.serverAck ? 1 : 0, in.listenHost ? 1 : 0, features);
        if (settled)
        {
            std::lock_guard<std::mutex> lock(m_GameplayModeNoticeMutex);
            m_GameplayModeNoticeText = r.effective == Effective::Physical
                ? "Physical mode: L4D2VR server"
                : (r.reason == Reason::ForcedClassic ? "Classic mode" : "Classic mode: server without L4D2VR");
            m_GameplayModeNoticeUntilMs = nowMs + kGameplayModeNoticeMs;
        }
    }
    else if (changed)
    {
        Game::logMsg("[VR][GameplayMode] effective=%s config=%s reason=%s",
            EffectiveName(r.effective), ConfigName(m_GameplayModeConfig), ReasonName(r.reason));
    }
}

bool VR::GetGameplayModeNotice(std::string& text) const
{
    std::lock_guard<std::mutex> lock(m_GameplayModeNoticeMutex);
    if (m_GameplayModeNoticeText.empty() || GetTickCount64() >= m_GameplayModeNoticeUntilMs)
        return false;
    text = m_GameplayModeNoticeText;
    return true;
}

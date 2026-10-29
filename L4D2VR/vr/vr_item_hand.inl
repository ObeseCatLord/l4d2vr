// Ambidextrous items: the hand that takes an item holds and uses it
// (AmbidextrousItemHands, vr_item_hand.h).

bool VR::ItemHandSwappedWithinMs(uint64_t ms) const
{
    const uint64_t at = m_ItemHandSwapAtMs.load(std::memory_order_acquire);
    return at != 0u && GetTickCount64() - at < ms;
}

void VR::RequestItemHand(bool physicalLeft, l4d2vr_item_hand::Reason reason)
{
    if (!m_AmbidextrousItemHands && reason != l4d2vr_item_hand::Reason::Reset)
        return;
    m_ItemHandSwitch.Request(l4d2vr_item_hand::SwappedForPhysicalHand(m_LeftHanded, physicalLeft), reason);
}

vr::VRActionHandle_t VR::ItemHandRoleAction(vr::VRActionHandle_t action) const
{
    // The item hand's trigger fires and its grip is the weapon grip.
    return l4d2vr_item_hand::RemapRole(action, m_ItemHandSwapped.load(std::memory_order_acquire),
        m_ActionPrimaryAttack, m_ActionSecondaryAttack, m_ActionWeaponGrip, m_ActionOffHandGrip,
        vr::k_ulInvalidActionHandle);
}

void VR::CommitItemHandSwitch()
{
    if (!m_AmbidextrousItemHands && m_ItemHandSwapped.load(std::memory_order_acquire) && !m_ItemHandSwitch.Pending())
        m_ItemHandSwitch.Request(false, l4d2vr_item_hand::Reason::Reset);
    bool swapped = m_ItemHandSwapped.load(std::memory_order_acquire);
    l4d2vr_item_hand::Reason reason = l4d2vr_item_hand::Reason::None;
    if (!m_ItemHandSwitch.Commit(swapped, reason))
        return;
    const uint64_t nowMs = GetTickCount64();
    m_ItemHandSwapped.store(swapped, std::memory_order_release);
    m_ItemHandSwapAtMs.store(nowMs, std::memory_order_release);
    m_ItemHandSwapSerial.fetch_add(1u, std::memory_order_acq_rel);
    // The gameplay hands trade controllers this frame: forget per-hand motion
    // history so no detector reads the jump as a swing, bash, shove or throw.
    m_MeleeMotionWeapon = nullptr; // UpdateMeleeMotion resets swing, thrust, bash and inertia
    m_GestureSampler.Reset(); // tracking-space hand history (vr_gesture.h)
    m_MotionGestureInitialized = false;
    if (nowMs + 500u > m_GunBashGripBlockUntilMs.load(std::memory_order_acquire))
        m_GunBashGripBlockUntilMs.store(nowMs + 500u, std::memory_order_release);
    // The hand on the foregrip now holds the item itself.
    m_VrHandsTwoHandedGripActive = false;
    m_VrHandsTwoHandedGripWeaponId = 0;
    m_VrHandsTwoHandedGripWeaponTag = 0;
    // Mirrored-binding caches hold the previous source's state per action.
    m_LeftHandedDigitalActionFrameCache.clear();
    m_LeftHandedAnalogActionFrameCache.clear();
    m_ItemHandAdoptGrip = reason == l4d2vr_item_hand::Reason::HandOff ||
        reason == l4d2vr_item_hand::Reason::ReleaseTransfer;
    m_ItemHandForceGripUpFrame = reason == l4d2vr_item_hand::Reason::HolsterDraw;
    if (reason == l4d2vr_item_hand::Reason::HolsterDraw)
    {
        // The new item hand is over the holster it squeezed (the holster block
        // refreshes these from its position later this frame); until then the
        // old hand's flags would read as "let go in the open" and drop the item.
        m_HolsterHandOverOtherZone.store(true, std::memory_order_release);
        m_HolsterHandInOwnZone.store(false, std::memory_order_release);
    }
    Game::logMsg("[VR][ItemHand] %s: item now in the %s hand (swapped=%d)", l4d2vr_item_hand::ReasonName(reason),
        ItemHandIsPhysicalLeft() ? "left" : "right", swapped ? 1 : 0);
}


bool VR::ItemHandReady(C_BasePlayer* localPlayer, bool isObserverOrIdle) const
{
    return m_AmbidextrousItemHands && m_IsVREnabled && !m_MouseModeEnabled && localPlayer && !isObserverOrIdle &&
        !m_SuppressPlayerInput && m_FirstPersonControlReady.load(std::memory_order_acquire) &&
        !m_RenderPlayerControlledBySI.load(std::memory_order_relaxed);
}

// Runs right after the weapon-hand grip (toggle) is resolved each frame.
void VR::UpdateItemHandGripTransitions(C_BasePlayer* localPlayer, bool isObserverOrIdle, bool akimbo,
    bool toggleApplies, bool gripValid, bool& gripDown, bool& gripJustPressed, uint32_t& weaponGripInputState)
{
    static bool s_PrevVirtual = false;
    // Nobody holds anything: the next life (or the next VR session) starts with
    // the configured hands.
    if (m_ItemHandSwapped.load(std::memory_order_acquire) && !m_ItemHandSwitch.Pending() &&
        (!m_AmbidextrousItemHands || !m_IsVREnabled || m_MouseModeEnabled || !localPlayer || isObserverOrIdle))
    {
        RequestItemHand(m_LeftHanded, l4d2vr_item_hand::Reason::Reset);
    }

    if (m_ItemHandForceGripUpFrame)
    {
        // A holster draw by the free hand: that hand's squeeze reaches the
        // weapon-hand holster logic as a fresh press on the next frame.
        m_ItemHandForceGripUpFrame = false;
        gripDown = false;
        gripJustPressed = false;
        weaponGripInputState = gripValid ? 1u : 0u;
    }

    bool virtualNow = gripValid && gripDown;
    const uint32_t support = m_PistolOffhandGripState.load(std::memory_order_acquire);
    const bool supportSqueezing = (support & 3u) == 3u;
    l4d2vr_item_hand::ReleaseInput release{};
    release.enabled = m_AmbidextrousItemHands && !akimbo && !m_ItemHandSwitch.Pending() &&
        !IsMagazineInteractionManualActive();
    release.ready = ItemHandReady(localPlayer, isObserverOrIdle) && localPlayer->GetActiveWeapon() != nullptr;
    release.released = s_PrevVirtual && !virtualNow;
    release.twoHandGrip = m_VrHandsTwoHandedGripActive;
    release.overHolster = m_HolsterHandInOwnZone.load(std::memory_order_acquire) ||
        m_HolsterHandOverOtherZone.load(std::memory_order_acquire);
    release.supportHolds = toggleApplies ? (!m_VrHandsTwoHandedGripHeldMode || supportSqueezing) : supportSqueezing;
    if (l4d2vr_item_hand::ReleaseTransfer(release))
    {
        // Letting go with the item hand while the other hand holds the
        // foregrip leaves the item in that hand instead of dropping it.
        RequestItemHand(IsGameplayHandLeftPhysical(true), l4d2vr_item_hand::Reason::ReleaseTransfer);
        m_ItemHandAdoptHeld = toggleApplies;
        gripDown = true;
        gripJustPressed = false;
        weaponGripInputState = 3u;
        virtualNow = true;
        Game::logMsg("[VR][ItemHand] item hand let go on the foregrip: moving the item to the %s hand",
            IsGameplayHandLeftPhysical(true) ? "left" : "right");
    }
    s_PrevVirtual = virtualNow;
}

// The free hand squeezed its grip and nothing else (holster, pouch, magazine,
// bolt or slide, foregrip) took the squeeze: with the hands together, the item
// moves to the free hand.
void VR::UpdateItemHandOff(C_BasePlayer* localPlayer, bool isObserverOrIdle, bool akimbo, bool pressEdge)
{
    // Did the foregrip or magazine logic (UpdateMagazineInteraction, just run)
    // react to this squeeze? Compared with the state after last frame's run.
    const int magazineState = static_cast<int>(m_MagazineInteractionState);
    const bool claimed = m_VrHandsTwoHandedGripActive != m_ItemHandOffTwoHandPrev ||
        magazineState != m_ItemHandOffMagazineStatePrev;
    m_ItemHandOffTwoHandPrev = m_VrHandsTwoHandedGripActive;
    m_ItemHandOffMagazineStatePrev = magazineState;
    l4d2vr_item_hand::HandOffInput in{};
    in.enabled = m_AmbidextrousItemHands && !akimbo && !m_ItemHandSwitch.Pending() && !ItemHandSwappedWithinMs(400u);
    in.ready = ItemHandReady(localPlayer, isObserverOrIdle);
    in.pressEdge = pressEdge;
    in.claimed = claimed || IsMagazineInteractionManualActive() || m_MagazineInteractionLeftHandHolding ||
        MagazineInteractionOwnsGrip(true);
    in.itemInHand = localPlayer && localPlayer->GetActiveWeapon() != nullptr;
    in.handDistanceMeters = (m_LeftControllerPosAbs - m_RightControllerPosAbs).Length() / (std::max)(0.001f, m_VRScale);
    in.radiusMeters = m_ItemHandOffRadiusMeters;
    if (pressEdge && m_AmbidextrousItemHands)
        Game::logMsg("[VR][ItemHand] free-hand squeeze: hands %.3f m apart (hand-off within %.2f m) claimed=%d ready=%d",
            in.handDistanceMeters, in.radiusMeters, in.claimed ? 1 : 0, in.ready ? 1 : 0);
    if (!l4d2vr_item_hand::HandOff(in))
        return;
    m_ItemHandAdoptHeld = m_GripToggle.Held();
    RequestItemHand(IsGameplayHandLeftPhysical(true), l4d2vr_item_hand::Reason::HandOff);
}

// Self-test steps (see docs/physical-overhaul.md):
//   ambie2e     left-hand draw, left trigger fire and aim, hand-off, left grenade
//               throw, left pills, right-hand draw back; pouch side; no bashes.
//   switchanim  every kind of weapon switch with the viewmodel trace and
//               screenshots: no frame may show the draw animation.
//   lfire=<s>   hold the physical left trigger.
bool VR::RunItemHandSelfTestStep(const std::string& step, float t, C_BasePlayer* player)
{
    using l4d2vr_selftest::Key;
    using l4d2vr_selftest::Vec3;
    namespace body = l4d2vr_selftest::body;
    auto cmd = [&](const char* text) { if (m_Game) m_Game->ClientCmd_Unrestricted(text); };
    // At most one stage per frame: after a hitch, a press and its release
    // must not land in the same frame (the press would never be seen).
    bool stageFiredThisFrame = false;
    auto at = [&](int stage, float when) -> bool
    {
        if (stageFiredThisFrame || m_SelfTestStage != stage || t < when)
            return false;
        m_SelfTestStage = stage + 1;
        stageFiredThisFrame = true;
        return true;
    };
    // Physical buttons (default bindings): the right controller's grip and
    // trigger are WeaponGrip / PrimaryAttack, the left's OffHandGrip /
    // SecondaryAttack, whichever hand holds the item.
    auto button = [&](vr::VRActionHandle_t action, bool down) { l4d2vr_selftest::SetButton(action, down); };
    auto holdLeft = [&](const Vec3& local, const QAngle& angles)
    {
        m_SelfTestLeftPathActive = false;
        m_SelfTestLeftWorldValid = false;
        m_SelfTestSceneLeftValid = false;
        m_SelfTestLeftAngWorldValid = false;
        m_SelfTestLeftLocal = local;
        m_SelfTestLeftAngLocal = angles;
    };
    auto clip = [&]() -> int
    {
        int value = -1;
        C_BaseCombatWeapon* const weapon = player ? player->GetActiveWeapon() : nullptr;
        if (weapon)
            VR_TryReadI32(reinterpret_cast<const unsigned char*>(weapon), kClip1Offset, value);
        return value;
    };
    auto side = [&](const Vector& point)
    {
        return DotProduct(point - m_HmdPosAbs, m_HmdRight) / (std::max)(0.001f, m_VRScale);
    };
    auto aimYawFromHead = [&]()
    {
        QAngle aim{};
        QAngle::VectorAngles(m_RightControllerForward, aim);
        QAngle head{};
        QAngle::VectorAngles(m_HmdForward, head);
        float d = aim.y - head.y;
        while (d > 180.0f) d -= 360.0f;
        while (d < -180.0f) d += 360.0f;
        return d;
    };
    const char* hand = ItemHandIsPhysicalLeft() ? "left" : "right";
    char detail[256];
    static bool s_SavedToggle = true;
    static uint32_t s_ShoveBaseline = 0u;
    static uint32_t s_SwitchHeldBaseline = 0u, s_SwitchShownBaseline = 0u;

    if (step == "switchtotal")
    {
        // Every switch since the game started (holster draws, hand-offs, throws,
        // pills and the switchanim list).
        const uint32_t held = m_VmSwitchFramesHeld.load(std::memory_order_acquire);
        const uint32_t shown = m_VmSwitchFramesShown.load(std::memory_order_acquire);
        std::snprintf(detail, sizeof(detail), "%u draw frame(s) kept off the screen, %u shown off the idle pose", held, shown);
        SelfTestReport("switchtotal: no draw animation in the whole session", held > 0u && shown == 0u, detail);
        return true;
    }

    if (step.rfind("lfire=", 0) == 0)
    {
        const float seconds = std::clamp(static_cast<float>(std::atof(step.c_str() + 6)), 0.1f, 30.0f);
        if (at(0, 0.0f)) { button(m_ActionSecondaryAttack, true); }
        if (at(1, seconds)) { button(m_ActionSecondaryAttack, false); }
        return t >= seconds + 1.0f;
    }

    if (step == "ambie2e")
    {
        if (at(0, 0.0f))
        {
            s_SavedToggle = m_GripToggleEnabled;
            m_GripToggleEnabled = true;
            cmd("give rifle"); cmd("give pipe_bomb"); cmd("give pain_pills");
            button(m_ActionWeaponGrip, false); button(m_ActionOffHandGrip, false);
            button(m_ActionPrimaryAttack, false); button(m_ActionSecondaryAttack, false);
            SelfTestHoldRight(body::RightReady(), QAngle(0.0f, 0.0f, 0.0f));
            holdLeft(body::LeftReady(), QAngle(0.0f, 0.0f, 0.0f));
            m_VmAnimTraceUntilMs.store(GetTickCount64() + 35000u, std::memory_order_release);
        }
        if (at(1, 1.5f)) { cmd("use weapon_pistol"); }
        if (at(2, 3.0f))
        {
            s_ShoveBaseline = m_PhysicalShoveSerial.load(std::memory_order_acquire);
            std::snprintf(detail, sizeof(detail), "item hand %s, active slot %d", hand, SelfTestActiveSlot(player) + 1);
            SelfTestReport("ambidextrous: starts in the right hand", !ItemHandIsPhysicalLeft() &&
                SelfTestActiveSlot(player) == 1, detail);
            // The rifle hangs on the dominant (right) half of the back; the left
            // hand reaches it over the right shoulder.
            SelfTestReachLeft(body::OverRightShoulder(), SelfTestVec(-60.0f, 0.0f, 0.0f), 1.0f);
        }
        if (at(3, 4.4f)) { button(m_ActionOffHandGrip, true); }
        if (at(4, 4.6f)) { button(m_ActionOffHandGrip, false); }
        if (at(5, 5.4f))
        {
            Vector pouch{}, held{};
            bool heldValid = false;
            const bool pouchValid = GetMagazineSlapSelfTestPoints(pouch, held, heldValid);
            std::snprintf(detail, sizeof(detail), "active slot %d (rifle is 1), item hand %s",
                SelfTestActiveSlot(player) + 1, hand);
            SelfTestReport("ambidextrous: left hand draws the rifle into the left hand",
                SelfTestActiveSlot(player) == 0 && ItemHandIsPhysicalLeft() && ActiveItemHand() == l4d2vr_dual::Hand::Left, detail);
            std::snprintf(detail, sizeof(detail), "pouch %.2f m %s of the head (free hand is the right)",
                std::fabs(side(pouch)), side(pouch) < 0.0f ? "left" : "right");
            SelfTestReport("ambidextrous: pouch on the free (right) hip", pouchValid && side(pouch) > 0.05f, detail);
            SelfTestReachLeft(SelfTestVec(0.42f, -0.12f, -0.32f), SelfTestVec(0.0f, 25.0f, 0.0f), 0.8f);
            SelfTestHoldRight(body::RightReady(), QAngle(0.0f, -25.0f, 0.0f));
        }
        if (at(6, 6.6f))
        {
            // The rifle came over the right shoulder; once the left hand is back
            // in front-left, the gun must be there with it.
            const float gunSide = side(m_RightControllerPosAbs);
            std::snprintf(detail, sizeof(detail), "gun hand %.2f m %s of the head",
                std::fabs(gunSide), gunSide < 0.0f ? "left" : "right");
            SelfTestReport("ambidextrous: the rifle follows the left hand", ItemHandIsPhysicalLeft() && gunSide < -0.05f, detail);
            SelfTestRequestShot("ambi_left_rifle", 1);
        }
        if (at(7, 7.2f)) { m_SelfTestValueA = clip(); button(m_ActionSecondaryAttack, true); }
        if (at(8, 7.5f)) { button(m_ActionSecondaryAttack, false); }
        if (at(9, 8.2f))
        {
            const float yaw = aimYawFromHead();
            std::snprintf(detail, sizeof(detail), "clip %d -> %d; aim %.1f deg from the head (left hand points +25, right -25)",
                m_SelfTestValueA, clip(), yaw);
            SelfTestReport("ambidextrous: left trigger fires the left-held rifle", clip() >= 0 && clip() < m_SelfTestValueA, detail);
            SelfTestReport("ambidextrous: aim follows the left controller", std::fabs(yaw - 25.0f) < 8.0f, detail);
            m_SelfTestValueB = clip();
            button(m_ActionPrimaryAttack, true);
        }
        if (at(10, 8.5f)) { button(m_ActionPrimaryAttack, false); }
        if (at(11, 9.2f))
        {
            std::snprintf(detail, sizeof(detail), "clip %d -> %d", m_SelfTestValueB, clip());
            SelfTestReport("ambidextrous: right trigger does not fire the left-held gun", clip() == m_SelfTestValueB, detail);
            SelfTestRequestShot("ambi_left_fired", 1);
            // Bring the free right hand to the rifle in the left hand.
            SelfTestReachRight(SelfTestVec(0.37f, -0.10f, -0.30f), SelfTestVec(0.0f, 25.0f, 0.0f), 0.8f);
        }
        if (at(12, 11.0f)) { m_SelfTestValueA = clip(); button(m_ActionWeaponGrip, true); }
        if (at(13, 11.3f)) { button(m_ActionWeaponGrip, false); }
        if (at(14, 12.0f))
        {
            std::snprintf(detail, sizeof(detail), "item hand %s, active slot %d, clip %d -> %d", hand,
                SelfTestActiveSlot(player) + 1, m_SelfTestValueA, clip());
            SelfTestReport("ambidextrous: squeezing the rifle with the free hand hands it over",
                !ItemHandIsPhysicalLeft() && SelfTestActiveSlot(player) == 0 && clip() == m_SelfTestValueA, detail);
            SelfTestReachRight(body::RightReady(), SelfTestVec(0.0f, -25.0f, 0.0f), 0.8f);
            SelfTestReachLeft(body::LeftReady(), SelfTestVec(0.0f, 25.0f, 0.0f), 0.8f);
        }
        if (at(15, 13.0f)) { m_SelfTestValueA = clip(); button(m_ActionPrimaryAttack, true); }
        if (at(16, 13.3f)) { button(m_ActionPrimaryAttack, false); }
        if (at(17, 14.0f))
        {
            const float yaw = aimYawFromHead();
            std::snprintf(detail, sizeof(detail), "clip %d -> %d; aim %.1f deg from the head", m_SelfTestValueA, clip(), yaw);
            SelfTestReport("ambidextrous: the right hand fires after the hand-off",
                clip() >= 0 && clip() < m_SelfTestValueA && std::fabs(yaw + 25.0f) < 8.0f, detail);
            SelfTestReachLeft(body::LeftChest(), SelfTestVec(30.0f, -40.0f, 0.0f), 0.5f);
        }
        if (at(18, 15.4f)) { button(m_ActionOffHandGrip, true); }
        if (at(19, 15.6f)) { button(m_ActionOffHandGrip, false); }
        if (at(20, 16.4f))
        {
            std::snprintf(detail, sizeof(detail), "active weapon id %d (pipe bomb %d), item hand %s",
                SelfTestActiveWeaponId(player), static_cast<int>(C_WeaponCSBase::PIPE_BOMB), hand);
            SelfTestReport("ambidextrous: left hand draws the pipe bomb into the left hand",
                SelfTestActiveWeaponId(player) == C_WeaponCSBase::PIPE_BOMB && ItemHandIsPhysicalLeft(), detail);
            holdLeft(SelfTestVec(-0.12f, -0.25f, 0.08f), QAngle(-20.0f, 0.0f, 0.0f));
        }
        if (at(21, 17.0f)) { SelfTestRequestShot("ambi_left_grenade", 1); }
        if (at(22, 17.4f)) { button(m_ActionSecondaryAttack, true); }
        if (at(23, 18.2f))
        {
            // Overhand throw with the left hand, released at the end.
            m_SelfTestLeftPath = { Key{ 0.0f, SelfTestVec(-0.12f, -0.25f, 0.08f) }, Key{ 0.2f, SelfTestVec(0.55f, -0.15f, 0.3f), false } };
            m_SelfTestLeftAngPath = { Key{ 0.0f, SelfTestVec(-20.0f, 0.0f, 0.0f) }, Key{ 0.2f, SelfTestVec(-35.0f, 0.0f, 0.0f), false } };
            m_SelfTestLeftWorldValid = false;
            m_SelfTestLeftPathStartMs = GetTickCount64();
            m_SelfTestLeftPathActive = true;
        }
        if (at(24, 18.4f)) { button(m_ActionSecondaryAttack, false); }
        if (at(25, 20.4f))
        {
            void* const grenade = player->Weapon_GetSlot(2);
            std::snprintf(detail, sizeof(detail), "grenade slot %s; afterwards weapon id %d in the %s hand",
                grenade ? "still occupied" : "empty", SelfTestActiveWeaponId(player), hand);
            SelfTestReport("ambidextrous: left hand throws the pipe bomb", grenade == nullptr, detail);
            cmd("hurtme 40"); // pills are only taken below full health
            SelfTestReachLeft(body::RightChest(), SelfTestVec(30.0f, 40.0f, 0.0f), 0.5f);
        }
        if (at(26, 21.8f)) { button(m_ActionOffHandGrip, true); }
        if (at(27, 22.0f)) { button(m_ActionOffHandGrip, false); }
        if (at(28, 22.8f))
        {
            std::snprintf(detail, sizeof(detail), "active weapon id %d (pills %d), item hand %s",
                SelfTestActiveWeaponId(player), static_cast<int>(C_WeaponCSBase::PAIN_PILLS), hand);
            SelfTestReport("ambidextrous: left hand takes the pills",
                SelfTestActiveWeaponId(player) == C_WeaponCSBase::PAIN_PILLS && ItemHandIsPhysicalLeft(), detail);
            holdLeft(SelfTestVec(0.45f, -0.05f, -0.25f), QAngle(0.0f, 0.0f, 0.0f));
            m_SelfTestValueC = 0;
        }
        if (at(29, 23.4f))
        {
            m_SelfTestLeftPath = { Key{ 0.0f, SelfTestVec(0.45f, -0.05f, -0.25f) }, Key{ 0.6f, SelfTestVec(0.07f, 0.0f, -0.07f) } };
            m_SelfTestLeftAngPath.clear();
            m_SelfTestLeftWorldValid = false;
            m_SelfTestLeftPathStartMs = GetTickCount64();
            m_SelfTestLeftPathActive = true;
        }
        if (m_SelfTestStage >= 30 && std::chrono::steady_clock::now() < m_ConsumableUseHoldUntil)
            m_SelfTestValueC = 1;
        if (at(30, 27.5f))
        {
            void* const pills = player->Weapon_GetSlot(4);
            std::snprintf(detail, sizeof(detail), "gesture %s, pills %s", m_SelfTestValueC ? "fired" : "not fired",
                pills ? "still held" : "used");
            SelfTestReport("ambidextrous: pills to the mouth with the left hand", m_SelfTestValueC == 1 && !pills, detail);
            holdLeft(body::LeftReady(), QAngle(0.0f, 0.0f, 0.0f));
            SelfTestReachRight(body::RightHip(), SelfTestVec(60.0f, 0.0f, 0.0f), 0.8f);
        }
        if (at(31, 29.2f)) { button(m_ActionWeaponGrip, true); }
        if (at(32, 29.4f)) { button(m_ActionWeaponGrip, false); }
        if (at(33, 30.2f))
        {
            std::snprintf(detail, sizeof(detail), "active slot %d (pistol 2), item hand %s", SelfTestActiveSlot(player) + 1, hand);
            SelfTestReport("ambidextrous: right hand draws the pistol back into the right hand",
                SelfTestActiveSlot(player) == 1 && !ItemHandIsPhysicalLeft(), detail);
            const uint32_t shoves = m_PhysicalShoveSerial.load(std::memory_order_acquire) - s_ShoveBaseline;
            std::snprintf(detail, sizeof(detail), "%u bash(es) or shove(s) while the item changed hands", shoves);
            SelfTestReport("ambidextrous: no bash from a hand change", shoves == 0u, detail);
            SelfTestReachRight(body::RightReady(), SelfTestVec(0.0f, 0.0f, 0.0f), 0.8f);
            m_GripToggleEnabled = s_SavedToggle;
            return true;
        }
        return false;
    }

    if (step == "ambimelee")
    {
        // The left hand draws the melee weapon from the hip and swings it; the
        // still right hand swings nothing.
        if (at(0, 0.0f))
        {
            s_SavedToggle = m_GripToggleEnabled;
            m_GripToggleEnabled = true;
            for (const char* melee : { "give machete", "give fireaxe", "give crowbar", "give baseball_bat", "give katana" })
                cmd(melee);
            button(m_ActionWeaponGrip, false); button(m_ActionOffHandGrip, false);
            SelfTestHoldRight(body::RightReady(), QAngle(0.0f, 0.0f, 0.0f));
            holdLeft(body::LeftReady(), QAngle(0.0f, 0.0f, 0.0f));
        }
        if (at(1, 1.2f)) { cmd("use weapon_rifle"); }
        if (at(2, 2.4f)) { SelfTestReachLeft(body::RightHip(), SelfTestVec(60.0f, 0.0f, 0.0f), 0.8f); }
        if (at(3, 3.8f)) { button(m_ActionOffHandGrip, true); }
        if (at(4, 4.0f)) { button(m_ActionOffHandGrip, false); }
        if (at(5, 4.8f))
        {
            std::snprintf(detail, sizeof(detail), "active weapon id %d (melee %d), item hand %s", SelfTestActiveWeaponId(player),
                static_cast<int>(C_WeaponCSBase::MELEE), hand);
            SelfTestReport("ambidextrous: left hand draws the melee weapon into the left hand",
                SelfTestActiveWeaponId(player) == C_WeaponCSBase::MELEE && ItemHandIsPhysicalLeft(), detail);
            SelfTestReachLeft(SelfTestVec(0.2f, -0.35f, 0.2f), SelfTestVec(-60.0f, -30.0f, 0.0f), 0.8f);
            m_SelfTestSerial = m_MeleeSwingStartSerial.load(std::memory_order_acquire);
        }
        if (at(6, 6.6f))
        {
            // Diagonal downward slash with the left hand at about 5 m/s.
            m_SelfTestLeftPath = { Key{ 0.0f, SelfTestVec(0.2f, -0.35f, 0.2f) }, Key{ 0.16f, SelfTestVec(0.55f, 0.15f, -0.35f), false } };
            m_SelfTestLeftAngPath = { Key{ 0.0f, SelfTestVec(-60.0f, -30.0f, 0.0f) }, Key{ 0.16f, SelfTestVec(50.0f, 30.0f, 0.0f), false } };
            m_SelfTestLeftWorldValid = false;
            m_SelfTestLeftPathStartMs = GetTickCount64();
            m_SelfTestLeftPathActive = true;
        }
        if (at(7, 8.0f))
        {
            const uint32_t swings = m_MeleeSwingStartSerial.load(std::memory_order_acquire) - m_SelfTestSerial;
            std::snprintf(detail, sizeof(detail), "%u swing(s) for one left-hand slash (the right hand stayed still)", swings);
            SelfTestReport("ambidextrous: the left hand swings the melee weapon", swings >= 1u, detail);
            holdLeft(body::LeftReady(), QAngle(0.0f, 0.0f, 0.0f));
            m_GripToggleEnabled = s_SavedToggle;
            return true;
        }
        return false;
    }

    if (step == "ambimedkit")
    {
        // The left hand takes the medkit from the back and heals with the left
        // trigger, then takes adrenaline and stabs it into the chest.
        auto health = [&]() { int value = -1; if (player) VR_TryReadI32(reinterpret_cast<const unsigned char*>(player), kHealthOffset, value); return value; };
        if (at(0, 0.0f))
        {
            s_SavedToggle = m_GripToggleEnabled;
            m_GripToggleEnabled = true;
            cmd("give first_aid_kit"); cmd("give adrenaline"); cmd("hurtme 50");
            button(m_ActionWeaponGrip, false); button(m_ActionOffHandGrip, false); button(m_ActionSecondaryAttack, false);
            SelfTestHoldRight(body::RightReady(), QAngle(0.0f, 0.0f, 0.0f));
            holdLeft(body::LeftReady(), QAngle(0.0f, 0.0f, 0.0f));
        }
        if (at(1, 1.2f)) { cmd("use weapon_rifle"); }
        // The kit hangs on the off (left) half of the back.
        if (at(2, 2.0f)) { SelfTestReachLeft(body::OverLeftShoulder(), SelfTestVec(-60.0f, 0.0f, 0.0f), 1.0f); }
        if (at(3, 3.4f)) { button(m_ActionOffHandGrip, true); }
        if (at(4, 3.6f)) { button(m_ActionOffHandGrip, false); }
        if (at(5, 4.4f))
        {
            std::snprintf(detail, sizeof(detail), "active weapon id %d (medkit %d), item hand %s", SelfTestActiveWeaponId(player),
                static_cast<int>(C_WeaponCSBase::FIRST_AID_KIT), hand);
            SelfTestReport("ambidextrous: left hand takes the medkit",
                SelfTestActiveWeaponId(player) == C_WeaponCSBase::FIRST_AID_KIT && ItemHandIsPhysicalLeft(), detail);
            holdLeft(SelfTestVec(0.40f, -0.12f, -0.35f), QAngle(0.0f, 0.0f, 0.0f));
            m_SelfTestValueA = health();
        }
        if (at(6, 5.0f)) { button(m_ActionSecondaryAttack, true); }
        if (at(7, 11.5f)) { button(m_ActionSecondaryAttack, false); }
        if (at(8, 12.5f))
        {
            void* const kit = player->Weapon_GetSlot(3);
            std::snprintf(detail, sizeof(detail), "health %d -> %d, medkit %s", m_SelfTestValueA, health(), kit ? "still held" : "used");
            SelfTestReport("ambidextrous: the left trigger heals with the left-held medkit", !kit && health() > m_SelfTestValueA, detail);
            SelfTestReachLeft(body::RightChest(), SelfTestVec(30.0f, 40.0f, 0.0f), 0.5f);
        }
        if (at(9, 14.0f)) { button(m_ActionOffHandGrip, true); }
        if (at(10, 14.2f)) { button(m_ActionOffHandGrip, false); }
        if (at(11, 15.0f))
        {
            std::snprintf(detail, sizeof(detail), "active weapon id %d (adrenaline %d), item hand %s", SelfTestActiveWeaponId(player),
                static_cast<int>(C_WeaponCSBase::ADRENALINE), hand);
            SelfTestReport("ambidextrous: left hand takes the adrenaline",
                SelfTestActiveWeaponId(player) == C_WeaponCSBase::ADRENALINE && ItemHandIsPhysicalLeft(), detail);
            holdLeft(SelfTestVec(0.50f, -0.05f, -0.26f), QAngle(0.0f, 180.0f, 0.0f));
            m_SelfTestValueC = 0;
        }
        if (at(12, 15.6f))
        {
            // Needle pointing back at the chest; the tip lands on it at about 2.3 m/s.
            m_SelfTestLeftPath = { Key{ 0.0f, SelfTestVec(0.50f, -0.05f, -0.26f) }, Key{ 0.12f, SelfTestVec(0.22f, 0.0f, -0.30f), false } };
            m_SelfTestLeftAngPath = { Key{ 0.0f, SelfTestVec(0.0f, 180.0f, 0.0f) }, Key{ 0.12f, SelfTestVec(0.0f, 180.0f, 0.0f) } };
            m_SelfTestLeftWorldValid = false;
            m_SelfTestLeftPathStartMs = GetTickCount64();
            m_SelfTestLeftPathActive = true;
        }
        if (m_SelfTestStage >= 13 && std::chrono::steady_clock::now() < m_ConsumableUseHoldUntil)
            m_SelfTestValueC = 1;
        if (at(13, 19.0f))
        {
            void* const item = player->Weapon_GetSlot(4);
            std::snprintf(detail, sizeof(detail), "gesture %s, adrenaline %s", m_SelfTestValueC ? "fired" : "not fired",
                item ? "still held" : "used");
            SelfTestReport("ambidextrous: adrenaline stab with the left hand", m_SelfTestValueC == 1 && !item, detail);
            holdLeft(body::LeftReady(), QAngle(0.0f, 0.0f, 0.0f));
            m_GripToggleEnabled = s_SavedToggle;
            return true;
        }
        return false;
    }

    if (step == "switchanim")
    {
        // Switches of every kind; three photographs 0.1, 0.4 and 0.7 s after
        // each, and the per-frame viewmodel trace ([VR][VmAnim]).
        struct Switch { float at; const char* command; const char* name; };
        // The idle poses learned so far are forgotten first, so the first draw
        // of each model takes the "no idle pose yet" path and the repeats the
        // held-idle-pose path.
        static const Switch kSwitches[] = {
            { 1.0f, "use weapon_rifle", "sw_rifle_first" },
            { 3.5f, "use weapon_pistol", "sw_pistol_first" },
            { 6.0f, "use weapon_rifle", "sw_rifle_again" },
            { 8.5f, "use weapon_first_aid_kit", "sw_medkit_first" },
            { 11.0f, "use weapon_pistol", "sw_pistol_again" },
            { 13.5f, "give smg", "sw_pickup_smg" },              // a new weapon arriving (server switch)
            { 16.0f, "use weapon_pipe_bomb", "sw_pipe_first" },
            { 18.5f, "use weapon_first_aid_kit", "sw_medkit_again" },
        };
        constexpr int kCount = static_cast<int>(sizeof(kSwitches) / sizeof(kSwitches[0]));
        if (at(0, 0.0f))
        {
            cmd("give rifle"); cmd("give first_aid_kit"); cmd("give pipe_bomb");
            m_VmIdlePosesClearRequest.store(true, std::memory_order_release);
            SelfTestHoldRight(body::RightReady(), QAngle(0.0f, 0.0f, 0.0f));
            holdLeft(body::LeftReady(), QAngle(0.0f, 0.0f, 0.0f));
            m_VmAnimTraceUntilMs.store(GetTickCount64() + 60000u, std::memory_order_release);
            s_SwitchHeldBaseline = m_VmSwitchFramesHeld.load(std::memory_order_acquire);
            s_SwitchShownBaseline = m_VmSwitchFramesShown.load(std::memory_order_acquire);
        }
        // Stages 1..4*kCount: command, then three shots.
        for (int i = 0; i < kCount; ++i)
        {
            const Switch& s = kSwitches[i];
            const int base = 1 + i * 4;
            if (at(base, s.at))
            {
                cmd(s.command);
                Game::logMsg("[VR][SelfTest] switch %d: %s", i + 1, s.command);
            }
            for (int k = 0; k < 3; ++k)
                if (at(base + 1 + k, s.at + 0.1f + 0.3f * k))
                    SelfTestRequestShot(std::string(s.name) + "_" + std::to_string(k + 1), 1);
        }
        if (at(1 + kCount * 4, 21.5f))
        {
            const uint32_t held = m_VmSwitchFramesHeld.load(std::memory_order_acquire) - s_SwitchHeldBaseline;
            const uint32_t shown = m_VmSwitchFramesShown.load(std::memory_order_acquire) - s_SwitchShownBaseline;
            std::snprintf(detail, sizeof(detail), "%u draw/deploy frame(s) held in the hand, %u shown (see [VR][VmAnim])",
                held, shown);
            SelfTestReport("switchanim: no weapon switch shows the draw animation", held > 0u && shown == 0u, detail);
            return true;
        }
        return false;
    }
    return true;
}

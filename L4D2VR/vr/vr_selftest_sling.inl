// Chest sling self-test (slinge2e, docs/physical-overhaul.md). Included by vr.cpp.
//   slinge2e  (toggle grips) draw the rifle from the chest sling with the right
//             hand, squeeze with the hand at the sling (slung, still the active
//             weapon, the trigger does not fire), take it by the handle with the
//             LEFT hand (left trigger fires), squeeze at the sling (slung again),
//             take it with the right hand, then squeeze with the arm stretched
//             out (dropped).
//   handswitch (toggle grips) no hand switch from a held gun: the other hand on
//             its handle (nothing) and foregrip (support only), the sling as the
//             only way to the other hand, left-arm model and triggers, the
//             owner's squeeze away from the sling drops, the pistol at the hip. Screenshots
//             sling_held, sling_hung, sling_taken_left, sling_hung_again.
bool VR::RunChestSlingSelfTestStep(const std::string& step, float t, C_BasePlayer* player)
{
    using l4d2vr_selftest::Vec3;
    namespace body = l4d2vr_selftest::body;
    auto cmd = [&](const char* text) { if (m_Game) m_Game->ClientCmd_Unrestricted(text); };
    bool stageFiredThisFrame = false;
    auto at = [&](int stage, float when) -> bool
    {
        if (stageFiredThisFrame || m_SelfTestStage != stage || t < when)
            return false;
        m_SelfTestStage = stage + 1;
        stageFiredThisFrame = true;
        return true;
    };
    auto button = [&](vr::VRActionHandle_t action, bool down) { l4d2vr_selftest::SetButton(action, down); };
    auto clip = [&]() -> int
    {
        int value = -1;
        C_BaseCombatWeapon* const weapon = player ? player->GetActiveWeapon() : nullptr;
        if (weapon)
            VR_TryReadI32(reinterpret_cast<const unsigned char*>(weapon), kClip1Offset, value);
        return value;
    };
    auto holdHandAtHandle = [&](bool left)
    {
        Vector handle{}, foregrip{};
        if (!GetChestSlingPoint(handle, foregrip))
            return false;
        if (left)
        {
            m_SelfTestLeftPathActive = false;
            m_SelfTestLeftWorld = handle;
            m_SelfTestLeftWorldValid = true;
        }
        else
        {
            m_SelfTestPathActive = false;
            m_SelfTestRightWorld = handle;
            m_SelfTestRightWorldValid = true;
        }
        return true;
    };
    char detail[256];
    const bool slung = ChestSlingActive();
    const int slot = SelfTestActiveSlot(player);
    const char* hand = ItemHandIsPhysicalLeft() ? "left" : "right";

    if (step == "stow")
    {
        // stow: puts the active item away in its own holster (both hands
        // empty), for holster screenshots. PASS when it is stowed.
        if (at(0, 0.0f))
        {
            C_BaseCombatWeapon* const active = player ? player->GetActiveWeapon() : nullptr;
            StowActiveWeapon(reinterpret_cast<uintptr_t>(active), slot, "self-test stow");
        }
        if (t < 0.5f)
            return false;
        std::snprintf(detail, sizeof(detail), "active slot %d, stowed slot %d", slot + 1, StowedSlot() + 1);
        SelfTestReport("stow: the active item is put away", slot >= 0 && StowedSlot() == slot, detail);
        return true;
    }
    if (step == "emptyhands")
        return RunEmptyHandsSelfTestStep(t, player);
    if (step == "handswitch")
    {
        // handswitch (2.0.6): toggle grips (the default). A held gun never
        // changes hands: the other hand's squeeze on its handle does nothing,
        // on its foregrip it is the support grip. The only way to move it to
        // the other hand is from the body: put it on the sling, take it with
        // that hand. Letting go of the grip never drops; the owner's squeeze
        // away from the sling drops it, at the hip it holsters the pistol.
        auto holdAt = [&](bool left, const Vector& world)
        {
            if (left)
            {
                m_SelfTestLeftPathActive = false;
                m_SelfTestLeftWorld = world;
                m_SelfTestLeftWorldValid = true;
            }
            else
            {
                m_SelfTestPathActive = false;
                m_SelfTestRightWorld = world;
                m_SelfTestRightWorldValid = true;
            }
        };
        // The free hand on the held gun's handle: 2 cm beside the item hand.
        auto onHandle = [&](bool left)
        {
            holdAt(left, m_RightControllerPosAbs + m_HmdRight * (0.02f * (std::max)(1.0f, m_VRScale)));
        };
        static bool s_SavedToggle = true;
        static uint32_t s_SwapBase = 0u, s_RemapBase = 0u;
        const uint32_t swaps = m_ItemHandSwapSerial.load(std::memory_order_acquire) - s_SwapBase;
        void* const primary = player ? player->Weapon_GetSlot(0) : nullptr;
        if (at(0, 0.0f))
        {
            s_SavedToggle = m_GripToggleEnabled;
            m_GripToggleEnabled = true;
            cmd("give rifle");
            button(m_ActionWeaponGrip, false); button(m_ActionOffHandGrip, false);
            button(m_ActionPrimaryAttack, false); button(m_ActionSecondaryAttack, false);
            SelfTestHoldRight(body::RightReady(), QAngle(0.0f, 0.0f, 0.0f));
            m_SelfTestLeftPathActive = false;
            m_SelfTestLeftWorldValid = false;
            m_SelfTestSceneLeftValid = false;
            m_SelfTestLeftAngWorldValid = false;
            m_SelfTestLeftLocal = body::LeftReady();
            m_SelfTestLeftAngLocal = QAngle(0.0f, 0.0f, 0.0f);
            RequestItemHand(false, l4d2vr_item_hand::Reason::SelfTest); // start in the right hand
        }
        if (at(1, 1.0f)) cmd("use weapon_pistol");
        if (at(2, 2.0f)) SelfTestReachRight(body::SlungRifleGrip(), SelfTestVec(30.0f, 0.0f, 0.0f), 1.0f);
        if (at(3, 3.4f)) button(m_ActionWeaponGrip, true);
        if (at(4, 3.6f)) button(m_ActionWeaponGrip, false);
        if (at(5, 4.4f))
        {
            std::snprintf(detail, sizeof(detail), "active slot %d, slung %d, item hand %s", slot + 1, slung ? 1 : 0, hand);
            SelfTestReport("handswitch: the right hand draws the rifle from the sling", slot == 0 && !slung && !ItemHandIsPhysicalLeft(), detail);
            SelfTestReachRight(body::RightReady(), SelfTestVec(0.0f, 0.0f, 0.0f), 0.8f);
        }
        if (at(6, 6.0f))
        {
            std::snprintf(detail, sizeof(detail), "2.4 s after letting go of the grip: active slot %d, slung %d, rifle %s, toggle %s",
                slot + 1, slung ? 1 : 0, primary ? "kept" : "gone", m_GripToggle.Held() ? "held" : "released");
            SelfTestReport("handswitch: letting go of the grip keeps the rifle (toggle grip)",
                slot == 0 && !slung && primary != nullptr && m_GripToggle.Held(), detail);
            s_SwapBase = m_ItemHandSwapSerial.load(std::memory_order_acquire);
            onHandle(true);
        }
        // The left hand squeezes the held rifle's handle.
        if (at(7, 6.8f)) button(m_ActionOffHandGrip, true);
        if (at(8, 7.0f)) button(m_ActionOffHandGrip, false);
        if (at(9, 7.8f))
        {
            std::snprintf(detail, sizeof(detail), "item hand %s, active slot %d, slung %d, hand changes %u, two-hand %d",
                hand, slot + 1, slung ? 1 : 0, swaps, m_VrHandsTwoHandedGripActive ? 1 : 0);
            SelfTestReport("handswitch: the left hand's squeeze on the held rifle's handle does not take it",
                !ItemHandIsPhysicalLeft() && slot == 0 && !slung && primary != nullptr && swaps == 0u, detail);
            SelfTestRequestShot("hs_left_on_handle", 1);
            Vector target{};
            if (GetTwoHandedGripTargetPoint(target))
                holdAt(true, target);
        }
        // ... and its foregrip: the support grip only.
        if (at(10, 8.8f)) button(m_ActionOffHandGrip, true);
        if (at(11, 9.0f)) button(m_ActionOffHandGrip, false);
        if (at(12, 9.8f))
        {
            std::snprintf(detail, sizeof(detail), "two-hand grip %d, item hand %s, hand changes %u",
                m_VrHandsTwoHandedGripActive ? 1 : 0, hand, swaps);
            SelfTestReport("handswitch: the left hand on the foregrip is the support grip, no switch",
                m_VrHandsTwoHandedGripActive && !ItemHandIsPhysicalLeft() && swaps == 0u, detail);
            SelfTestRequestShot("hs_foregrip", 1);
        }
        if (at(13, 10.2f)) button(m_ActionOffHandGrip, true);   // foregrip toggle: let go again
        if (at(14, 10.4f)) button(m_ActionOffHandGrip, false);
        if (at(15, 11.2f))
        {
            std::snprintf(detail, sizeof(detail), "two-hand grip %d, active slot %d, slung %d, rifle %s",
                m_VrHandsTwoHandedGripActive ? 1 : 0, slot + 1, slung ? 1 : 0, primary ? "kept" : "gone");
            SelfTestReport("handswitch: the support hand letting go never drops or holsters the rifle",
                !m_VrHandsTwoHandedGripActive && slot == 0 && !slung && primary != nullptr, detail);
            SelfTestReachLeft(body::LeftReady(), SelfTestVec(0.0f, 0.0f, 0.0f), 0.8f);
            holdHandAtHandle(false);
        }
        // The right hand puts it on the sling.
        if (at(16, 12.2f)) button(m_ActionWeaponGrip, true);
        if (at(17, 12.4f)) button(m_ActionWeaponGrip, false);
        if (at(18, 13.2f))
        {
            std::snprintf(detail, sizeof(detail), "slung %d, active slot %d", slung ? 1 : 0, slot + 1);
            SelfTestReport("handswitch: squeezing at the sling puts the rifle away", slung && slot == 0, detail);
            SelfTestReachRight(body::RightReady(), SelfTestVec(0.0f, 0.0f, 0.0f), 0.8f);
            holdHandAtHandle(true);
        }
        // The left hand takes it from the sling: now it is the left hand's.
        if (at(19, 14.2f)) button(m_ActionOffHandGrip, true);
        if (at(20, 14.4f)) button(m_ActionOffHandGrip, false);
        if (at(21, 15.2f))
        {
            std::snprintf(detail, sizeof(detail), "slung %d, item hand %s, active slot %d, hand changes %u",
                slung ? 1 : 0, hand, slot + 1, swaps);
            SelfTestReport("handswitch: the left hand takes the rifle from the sling", !slung && ItemHandIsPhysicalLeft() &&
                slot == 0 && swaps == 1u, detail);
            SelfTestReachLeft(SelfTestVec(0.42f, -0.12f, -0.32f), SelfTestVec(0.0f, 25.0f, 0.0f), 0.8f);
            s_RemapBase = m_LeftHandRemapDraws.load(std::memory_order_acquire);
        }
        if (at(22, 16.4f)) { SelfTestRequestShot("hs_left_rifle", 1); m_SelfTestValueA = clip(); button(m_ActionSecondaryAttack, true); }
        if (at(23, 16.7f)) button(m_ActionSecondaryAttack, false);
        if (at(24, 17.4f))
        {
            const uint32_t remaps = m_LeftHandRemapDraws.load(std::memory_order_acquire) - s_RemapBase;
            std::snprintf(detail, sizeof(detail), "clip %d -> %d; left-arm model draws %u (ItemHandLeftArmModel %d)",
                m_SelfTestValueA, clip(), remaps, m_ItemHandLeftArmModel ? 1 : 0);
            SelfTestReport("handswitch: the left trigger fires the left-held rifle", clip() >= 0 && clip() < m_SelfTestValueA, detail);
            SelfTestReport("handswitch: the left-held rifle is drawn on the survivor's left arm",
                !m_ItemHandLeftArmModel || remaps > 0u, detail);
            onHandle(false);
        }
        // The right hand squeezes the left-held rifle's handle: still the left's.
        if (at(25, 18.2f)) button(m_ActionWeaponGrip, true);
        if (at(26, 18.4f)) button(m_ActionWeaponGrip, false);
        if (at(27, 19.2f))
        {
            std::snprintf(detail, sizeof(detail), "item hand %s, active slot %d, slung %d, hand changes %u",
                hand, slot + 1, slung ? 1 : 0, swaps);
            SelfTestReport("handswitch: the right hand's squeeze on the left-held rifle does not take it",
                ItemHandIsPhysicalLeft() && slot == 0 && !slung && primary != nullptr && swaps == 1u, detail);
            SelfTestRequestShot("hs_right_on_handle", 1);
            m_SelfTestValueB = clip();
            button(m_ActionPrimaryAttack, true);
        }
        if (at(28, 19.5f)) button(m_ActionPrimaryAttack, false);
        if (at(29, 20.2f))
        {
            std::snprintf(detail, sizeof(detail), "clip %d -> %d", m_SelfTestValueB, clip());
            SelfTestReport("handswitch: the free right trigger does not fire the left-held rifle", clip() == m_SelfTestValueB, detail);
            SelfTestReachRight(body::RightReady(), SelfTestVec(0.0f, 0.0f, 0.0f), 0.8f);
            holdHandAtHandle(true);
        }
        // Back: the left hand puts it on the sling, the right hand takes it.
        if (at(30, 21.2f)) button(m_ActionOffHandGrip, true);
        if (at(31, 21.4f)) button(m_ActionOffHandGrip, false);
        if (at(32, 22.2f))
        {
            std::snprintf(detail, sizeof(detail), "slung %d, active slot %d", slung ? 1 : 0, slot + 1);
            SelfTestReport("handswitch: the left hand puts it on the sling", slung && slot == 0, detail);
            SelfTestReachLeft(body::LeftReady(), SelfTestVec(0.0f, 0.0f, 0.0f), 0.8f);
            holdHandAtHandle(false);
        }
        if (at(33, 23.2f)) button(m_ActionWeaponGrip, true);
        if (at(34, 23.4f)) button(m_ActionWeaponGrip, false);
        if (at(35, 24.2f))
        {
            std::snprintf(detail, sizeof(detail), "slung %d, item hand %s, active slot %d, hand changes %u",
                slung ? 1 : 0, hand, slot + 1, swaps);
            SelfTestReport("handswitch: the right hand takes it back from the sling", !slung && !ItemHandIsPhysicalLeft() &&
                slot == 0 && swaps == 2u, detail);
            SelfTestReachRight(body::RightReady(), SelfTestVec(0.0f, 0.0f, 0.0f), 0.8f);
        }
        if (at(36, 25.4f)) { SelfTestRequestShot("hs_right_again", 1); m_SelfTestValueA = clip(); button(m_ActionPrimaryAttack, true); }
        if (at(37, 25.7f)) button(m_ActionPrimaryAttack, false);
        if (at(38, 26.4f))
        {
            std::snprintf(detail, sizeof(detail), "clip %d -> %d", m_SelfTestValueA, clip());
            SelfTestReport("handswitch: the right trigger fires it again", clip() >= 0 && clip() < m_SelfTestValueA, detail);
        }
        // The owner's squeeze in front of the body (away from the sling) drops it.
        if (at(39, 26.8f)) button(m_ActionWeaponGrip, true);
        if (at(40, 27.0f)) button(m_ActionWeaponGrip, false);
        if (at(41, 28.6f))
        {
            std::snprintf(detail, sizeof(detail), "rifle %s, slung %d, active slot %d", primary ? "still held" : "dropped",
                slung ? 1 : 0, slot + 1);
            SelfTestReport("handswitch: the owner's squeeze away from the sling drops the rifle", primary == nullptr && !slung, detail);
            SelfTestRequestShot("hs_dropped", 1);
            SelfTestReachRight(body::RightBelt(), SelfTestVec(60.0f, 0.0f, 0.0f), 1.0f);
        }
        // The pistol (now in hand) goes into the hip holster with a squeeze there.
        if (at(42, 30.2f)) button(m_ActionWeaponGrip, true);
        if (at(43, 30.4f)) button(m_ActionWeaponGrip, false);
        if (at(44, 31.2f))
        {
            std::snprintf(detail, sizeof(detail), "stowed slot %d, active slot %d", StowedSlot() + 1, slot + 1);
            SelfTestReport("handswitch: squeezing the pistol at the hip holsters it", slung && StowedSlot() == 1 && slot == 1, detail);
        }
        if (at(45, 32.0f)) button(m_ActionWeaponGrip, true);
        if (at(46, 32.2f)) button(m_ActionWeaponGrip, false);
        if (at(47, 33.0f))
        {
            std::snprintf(detail, sizeof(detail), "stowed %d, active slot %d, item hand %s", slung ? 1 : 0, slot + 1, hand);
            SelfTestReport("handswitch: the right hand draws the pistol again", !slung && slot == 1 && !ItemHandIsPhysicalLeft(), detail);
            SelfTestReachRight(body::RightReady(), SelfTestVec(0.0f, 0.0f, 0.0f), 0.8f);
            m_GripToggleEnabled = s_SavedToggle;
            return true;
        }
        return false;
    }
    if (step != "slinge2e")
        return true;
    static bool s_SlingSavedToggle = true;
    if (at(0, 0.0f))
    {
        s_SlingSavedToggle = m_GripToggleEnabled;
        m_GripToggleEnabled = true;
        cmd("give rifle"); // survivors spawn with a pistol (a second one would be akimbo)
        button(m_ActionWeaponGrip, false); button(m_ActionOffHandGrip, false);
        button(m_ActionPrimaryAttack, false); button(m_ActionSecondaryAttack, false);
        SelfTestHoldRight(body::RightReady(), QAngle(0.0f, 0.0f, 0.0f));
        m_SelfTestLeftPathActive = false;
        m_SelfTestLeftWorldValid = false;
        m_SelfTestSceneLeftValid = false;
        m_SelfTestLeftAngWorldValid = false;
        m_SelfTestLeftLocal = body::LeftReady();
        m_SelfTestLeftAngLocal = QAngle(0.0f, 0.0f, 0.0f);
        m_SelfTestValueA = static_cast<int>(m_ChestSlingHangCount.load(std::memory_order_acquire));
    }
    if (at(1, 1.0f)) cmd("use weapon_pistol");
    // Draw the rifle from the chest sling by its grip.
    if (at(2, 2.0f)) SelfTestReachRight(body::SlungRifleGrip(), SelfTestVec(30.0f, 0.0f, 0.0f), 1.0f);
    if (at(3, 3.4f)) button(m_ActionWeaponGrip, true);
    if (at(4, 3.6f)) button(m_ActionWeaponGrip, false);
    if (at(5, 4.4f))
    {
        std::snprintf(detail, sizeof(detail), "active slot %d, item hand %s", slot + 1, hand);
        SelfTestReport("sling: right hand draws the rifle", slot == 0 && !slung, detail);
        SelfTestReachRight(body::RightReady(), SelfTestVec(0.0f, 0.0f, 0.0f), 0.8f);
    }
    if (at(6, 5.6f)) { SelfTestRequestShot("sling_held", 1); holdHandAtHandle(false); }
    // Put it away: the owner's squeeze with the hand at the sling (toggle grip).
    if (at(7, 6.0f)) button(m_ActionWeaponGrip, true);
    if (at(8, 6.2f)) button(m_ActionWeaponGrip, false);
    if (at(9, 7.0f))
    {
        std::snprintf(detail, sizeof(detail), "slung=%d, active slot %d (rifle is 1), hangs %u", slung ? 1 : 0, slot + 1,
            m_ChestSlingHangCount.load(std::memory_order_acquire) - static_cast<uint32_t>(m_SelfTestValueA));
        SelfTestReport("sling: squeezing at the sling hangs the rifle on the chest", slung && slot == 0, detail);
        SelfTestRequestShot("sling_hung", 1);
        m_SelfTestValueB = clip();
    }
    if (at(10, 7.4f)) button(m_ActionPrimaryAttack, true);
    if (at(11, 7.8f)) button(m_ActionPrimaryAttack, false);
    if (at(12, 8.4f))
    {
        std::snprintf(detail, sizeof(detail), "clip %d -> %d", m_SelfTestValueB, clip());
        SelfTestReport("sling: the slung rifle does not fire", clip() == m_SelfTestValueB && slung, detail);
        if (!holdHandAtHandle(true))
            SelfTestReport("sling: handle position", false, "no body frame");
    }
    // The LEFT hand takes it by the handle.
    if (at(13, 9.4f)) button(m_ActionOffHandGrip, true);
    if (at(14, 9.6f)) button(m_ActionOffHandGrip, false);
    if (at(15, 10.4f))
    {
        std::snprintf(detail, sizeof(detail), "slung=%d item hand %s, active slot %d", slung ? 1 : 0, hand, slot + 1);
        SelfTestReport("sling: the left hand takes the slung rifle by its handle", !slung && ItemHandIsPhysicalLeft() &&
            slot == 0, detail);
        SelfTestReachLeft(SelfTestVec(0.42f, -0.12f, -0.32f), SelfTestVec(0.0f, 25.0f, 0.0f), 0.8f);
    }
    if (at(16, 11.6f)) { SelfTestRequestShot("sling_taken_left", 1); m_SelfTestValueB = clip(); }
    if (at(17, 12.0f)) button(m_ActionSecondaryAttack, true);
    if (at(18, 12.3f)) button(m_ActionSecondaryAttack, false);
    if (at(19, 13.0f))
    {
        std::snprintf(detail, sizeof(detail), "clip %d -> %d", m_SelfTestValueB, clip());
        SelfTestReport("sling: the left trigger fires it", clip() >= 0 && clip() < m_SelfTestValueB, detail);
        holdHandAtHandle(true);
    }
    // The left hand squeezes at the sling: slung again.
    if (at(20, 13.4f)) button(m_ActionOffHandGrip, true);
    if (at(21, 13.6f)) button(m_ActionOffHandGrip, false);
    if (at(22, 14.4f))
    {
        std::snprintf(detail, sizeof(detail), "slung=%d active slot %d", slung ? 1 : 0, slot + 1);
        SelfTestReport("sling: the left hand puts it back on the sling", slung && slot == 0, detail);
        SelfTestRequestShot("sling_hung_again", 1);
        holdHandAtHandle(false);
    }
    // The right hand takes it back.
    if (at(23, 15.4f)) button(m_ActionWeaponGrip, true);
    if (at(24, 15.6f)) button(m_ActionWeaponGrip, false);
    if (at(25, 16.4f))
    {
        std::snprintf(detail, sizeof(detail), "slung=%d item hand %s, active slot %d", slung ? 1 : 0, hand, slot + 1);
        SelfTestReport("sling: the right hand takes it back", !slung && !ItemHandIsPhysicalLeft() && slot == 0, detail);
        // Arm stretched out, away from the body.
        SelfTestReachRight(SelfTestVec(0.80f, 0.35f, -0.10f), SelfTestVec(0.0f, 0.0f, 0.0f), 1.0f);
    }
    if (at(26, 17.6f)) button(m_ActionWeaponGrip, true);
    if (at(27, 17.8f)) button(m_ActionWeaponGrip, false);
    if (at(28, 19.0f))
    {
        void* const primary = player ? player->Weapon_GetSlot(0) : nullptr;
        std::snprintf(detail, sizeof(detail), "slung=%d primary slot %s, active slot %d", slung ? 1 : 0,
            primary ? "still held" : "empty", slot + 1);
        SelfTestReport("sling: squeezing at arm's length drops it", !slung && primary == nullptr, detail);
        SelfTestHoldRight(body::RightReady(), QAngle(0.0f, 0.0f, 0.0f));
        m_SelfTestLeftWorldValid = false;
        m_GripToggleEnabled = s_SlingSavedToggle;
        return true;
    }
    return false;
}

// emptyhands (docs/physical-overhaul.md, Empty hands): Forefront / Pavlov empty
// hands with hold grips along realistic body paths. The pistol is put away in
// the hip holster (the hand is empty, it cannot fire), the empty hand draws
// the rifle from the chest sling and puts it back (both hands empty), the
// free left hand takes it off the sling and lets go (slung again), the right
// hand draws the pistol and lets go near the body (back to the hip), the left
// hand takes the pills from its hip and puts them back, and the right hand
// draws the pistol again. Screenshots empty_*.
bool VR::RunEmptyHandsSelfTestStep(float t, C_BasePlayer* player)
{
    using l4d2vr_selftest::Vec3;
    namespace body = l4d2vr_selftest::body;
    auto cmd = [&](const char* text) { if (m_Game) m_Game->ClientCmd_Unrestricted(text); };
    bool stageFiredThisFrame = false;
    auto at = [&](int stage, float when) -> bool
    {
        if (stageFiredThisFrame || m_SelfTestStage != stage || t < when)
            return false;
        m_SelfTestStage = stage + 1;
        stageFiredThisFrame = true;
        return true;
    };
    auto button = [&](vr::VRActionHandle_t action, bool down) { l4d2vr_selftest::SetButton(action, down); };
    auto clip = [&]() -> int
    {
        int value = -1;
        C_BaseCombatWeapon* const weapon = player ? player->GetActiveWeapon() : nullptr;
        if (weapon)
            VR_TryReadI32(reinterpret_cast<const unsigned char*>(weapon), kClip1Offset, value);
        return value;
    };
    static bool s_SavedToggle = true;
    static uint32_t s_ShoveBaseline = 0u;
    char detail[256];
    const bool stowed = ChestSlingActive();
    const int slot = SelfTestActiveSlot(player);
    const int stowedSlot = StowedSlot();
    const bool leftHolds = ItemHandIsPhysicalLeft();
    const Vec3 level = SelfTestVec(0.0f, 0.0f, 0.0f);
    const Vec3 down60 = SelfTestVec(60.0f, 0.0f, 0.0f);
    const Vec3 down30 = SelfTestVec(30.0f, 0.0f, 0.0f);

    if (at(0, 0.0f))
    {
        s_SavedToggle = m_GripToggleEnabled;
        m_GripToggleEnabled = false;   // hold grips: the hand keeps what the grip holds
        cmd("give rifle"); cmd("give pain_pills"); cmd("hurtme 30");
        button(m_ActionWeaponGrip, false); button(m_ActionOffHandGrip, false);
        button(m_ActionPrimaryAttack, false); button(m_ActionSecondaryAttack, false);
        SelfTestHoldRight(body::RightReady(), QAngle(0.0f, 0.0f, 0.0f));
        m_SelfTestLeftPathActive = false;
        m_SelfTestLeftWorldValid = false;
        m_SelfTestSceneLeftValid = false;
        m_SelfTestLeftAngWorldValid = false;
        m_SelfTestLeftLocal = body::LeftReady();
        m_SelfTestLeftAngLocal = QAngle(0.0f, 0.0f, 0.0f);
        // The right hand holds the pistol (an earlier step may have left the
        // item in the left hand: the free hand cannot put the other hand's
        // item away).
        RequestItemHand(false, l4d2vr_item_hand::Reason::SelfTest);
    }
    if (at(1, 1.0f))
    {
        cmd("use weapon_pistol");
        // After the hands moved to their start poses (that jump is no bash).
        s_ShoveBaseline = m_PhysicalShoveSerial.load(std::memory_order_acquire);
    }
    // 1) The pistol goes back into the hip holster: the hand is empty.
    if (at(2, 2.0f)) SelfTestReachRight(body::RightBelt(), down60, 1.0f);
    if (at(3, 3.4f)) button(m_ActionWeaponGrip, true);
    if (at(4, 3.7f)) button(m_ActionWeaponGrip, false);
    if (at(5, 4.5f))
    {
        void* const pistol = player ? player->Weapon_GetSlot(1) : nullptr;
        std::snprintf(detail, sizeof(detail), "stowed slot %d, active slot %d, pistol %s", stowedSlot + 1, slot + 1,
            pistol ? "kept" : "gone");
        SelfTestReport("emptyhands: putting the pistol away in the hip holster leaves the hand empty",
            stowed && stowedSlot == 1 && slot == 1 && pistol != nullptr, detail);
        SelfTestReachRight(body::RightReady(), level, 0.8f);
        m_SelfTestLookActive = true;
        m_SelfTestLookPitch = 55.0f;
    }
    if (at(6, 5.6f)) { SelfTestRequestShot("empty_pistol_holstered", 1); m_SelfTestValueA = clip(); button(m_ActionPrimaryAttack, true); }
    if (at(7, 6.0f)) button(m_ActionPrimaryAttack, false);
    if (at(8, 6.6f))
    {
        std::snprintf(detail, sizeof(detail), "clip %d -> %d, stowed %d", m_SelfTestValueA, clip(), stowed ? 1 : 0);
        SelfTestReport("emptyhands: the holstered pistol cannot fire", stowed && clip() == m_SelfTestValueA, detail);
        SelfTestReachRight(body::SlungRifleGrip(), down30, 1.0f);
    }
    // 2) The empty hand draws the rifle from the chest sling and puts it back.
    if (at(9, 8.0f)) button(m_ActionWeaponGrip, true);
    if (at(10, 8.6f))
    {
        std::snprintf(detail, sizeof(detail), "active slot %d, stowed %d, item hand %s", slot + 1, stowed ? 1 : 0,
            leftHolds ? "left" : "right");
        SelfTestReport("emptyhands: the empty hand draws the rifle from the chest sling", slot == 0 && !stowed && !leftHolds, detail);
        SelfTestReachRight(body::RightReady(), level, 0.8f);
    }
    if (at(11, 9.8f)) { SelfTestRequestShot("empty_rifle_drawn", 1); SelfTestReachRight(body::SlungRifleGrip(), down30, 0.8f); }
    if (at(12, 11.2f)) button(m_ActionWeaponGrip, false);
    if (at(13, 12.0f))
    {
        void* const rifle = player ? player->Weapon_GetSlot(0) : nullptr;
        std::snprintf(detail, sizeof(detail), "stowed slot %d, active slot %d, rifle %s", stowedSlot + 1, slot + 1,
            rifle ? "kept" : "gone");
        SelfTestReport("emptyhands: the rifle goes back on the sling, both hands empty",
            stowed && stowedSlot == 0 && slot == 0 && rifle != nullptr, detail);
        SelfTestRequestShot("empty_both", 1);
        SelfTestReachRight(body::RightReady(), level, 0.8f);
        SelfTestReachLeft(body::SlungRifleForegrip(), down30, 0.8f);
    }
    // 3) The free left hand takes it off the sling by the handguard, then lets go.
    if (at(14, 13.6f)) button(m_ActionOffHandGrip, true);
    if (at(15, 14.4f))
    {
        std::snprintf(detail, sizeof(detail), "stowed %d, active slot %d, item hand %s", stowed ? 1 : 0, slot + 1,
            leftHolds ? "left" : "right");
        SelfTestReport("emptyhands: the left hand takes the rifle off the sling", !stowed && slot == 0 && leftHolds, detail);
        SelfTestReachLeft(SelfTestVec(0.42f, -0.12f, -0.32f), SelfTestVec(0.0f, 25.0f, 0.0f), 0.8f);
    }
    if (at(16, 15.4f)) SelfTestRequestShot("empty_left_rifle", 1);
    if (at(17, 15.8f)) button(m_ActionOffHandGrip, false);
    if (at(18, 16.6f))
    {
        std::snprintf(detail, sizeof(detail), "stowed slot %d, active slot %d", stowedSlot + 1, slot + 1);
        SelfTestReport("emptyhands: the left hand lets go near the body: on the sling again", stowed && stowedSlot == 0, detail);
        SelfTestReachLeft(body::LeftReady(), level, 0.8f);
        SelfTestReachRight(body::RightBelt(), down60, 1.0f);
    }
    // 4) The right hand draws the pistol and lets go near the body: back to the hip.
    if (at(19, 18.0f)) button(m_ActionWeaponGrip, true);
    if (at(20, 18.6f))
    {
        std::snprintf(detail, sizeof(detail), "active slot %d, stowed %d, item hand %s", slot + 1, stowed ? 1 : 0,
            leftHolds ? "left" : "right");
        SelfTestReport("emptyhands: the right hand draws the pistol", slot == 1 && !stowed && !leftHolds, detail);
        SelfTestReachRight(body::RightReady(), level, 0.8f);
    }
    if (at(21, 19.8f)) button(m_ActionWeaponGrip, false);
    if (at(22, 20.6f))
    {
        void* const pistol = player ? player->Weapon_GetSlot(1) : nullptr;
        std::snprintf(detail, sizeof(detail), "stowed slot %d, active slot %d, pistol %s", stowedSlot + 1, slot + 1,
            pistol ? "kept" : "gone");
        SelfTestReport("emptyhands: letting go of the pistol near the body puts it back on the hip",
            stowed && stowedSlot == 1 && pistol != nullptr, detail);
        SelfTestReachLeft(body::LeftBelt(), down60, 1.0f);
    }
    // 5) The free left hand takes the pills from its hip and puts them back.
    if (at(23, 22.0f)) button(m_ActionOffHandGrip, true);
    if (at(24, 22.8f))
    {
        std::snprintf(detail, sizeof(detail), "active slot %d (pills 5), stowed %d, item hand %s", slot + 1, stowed ? 1 : 0,
            leftHolds ? "left" : "right");
        SelfTestReport("emptyhands: the left hand draws the pills from the left hip", slot == 4 && !stowed && leftHolds, detail);
    }
    if (at(25, 23.4f)) button(m_ActionOffHandGrip, false);
    if (at(26, 24.2f))
    {
        void* const pills = player ? player->Weapon_GetSlot(4) : nullptr;
        std::snprintf(detail, sizeof(detail), "stowed slot %d, active slot %d, pills %s", stowedSlot + 1, slot + 1,
            pills ? "kept" : "gone");
        SelfTestReport("emptyhands: putting the pills back on the hip leaves both hands empty",
            stowed && stowedSlot == 4 && pills != nullptr, detail);
        SelfTestRequestShot("empty_pills_back", 1);
        SelfTestReachLeft(body::LeftReady(), level, 0.8f);
        SelfTestReachRight(body::RightBelt(), down60, 1.0f);
    }
    // 6) Drawing again: the pistol goes into the right hand.
    if (at(27, 25.8f)) button(m_ActionWeaponGrip, true);
    if (at(28, 26.6f))
    {
        std::snprintf(detail, sizeof(detail), "active slot %d, stowed %d, item hand %s", slot + 1, stowed ? 1 : 0,
            leftHolds ? "left" : "right");
        SelfTestReport("emptyhands: drawing again puts the pistol in the right hand", slot == 1 && !stowed && !leftHolds, detail);
        SelfTestReachRight(body::RightReady(), level, 0.8f);
    }
    if (at(29, 27.6f)) SelfTestRequestShot("empty_pistol_again", 1);
    if (at(30, 28.0f))
    {
        const uint32_t shoves = m_PhysicalShoveSerial.load(std::memory_order_acquire) - s_ShoveBaseline;
        std::snprintf(detail, sizeof(detail), "%u bash(es) or shove(s), stows %u, takes back %u", shoves,
            m_HolsterStowCount.load(std::memory_order_acquire), m_HolsterUnstowCount.load(std::memory_order_acquire));
        SelfTestReport("emptyhands: no bash while putting items away", shoves == 0u, detail);
        // Keep holding the pistol with the grip through the end of the step.
        m_SelfTestLookActive = false;
        m_SelfTestLookPitch = 0.0f;
        m_GripToggleEnabled = s_SavedToggle;
        return true;
    }
    return false;
}

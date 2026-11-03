// Chest sling self-test (slinge2e, docs/physical-overhaul.md). Included by vr.cpp.
//   slinge2e  draw the rifle from the back with the right hand, let go in front
//             of the body (slung, still the active weapon, the trigger does not
//             fire), take it by the handle with the LEFT hand (left trigger
//             fires), let go (slung again), take it with the right hand, then
//             let go with the arm stretched out (dropped). Screenshots
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

    if (step != "slinge2e")
        return true;
    if (at(0, 0.0f))
    {
        cmd("give rifle"); cmd("give pistol");
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
    // Draw the rifle from the back over the right shoulder.
    if (at(2, 2.0f)) SelfTestReachRight(body::OverRightShoulder(), SelfTestVec(-60.0f, 0.0f, 0.0f), 1.0f);
    if (at(3, 3.4f)) button(m_ActionWeaponGrip, true);
    if (at(4, 3.6f)) button(m_ActionWeaponGrip, false);
    if (at(5, 4.4f))
    {
        std::snprintf(detail, sizeof(detail), "active slot %d, item hand %s", slot + 1, hand);
        SelfTestReport("sling: right hand draws the rifle", slot == 0 && !slung, detail);
        SelfTestReachRight(body::RightReady(), SelfTestVec(0.0f, 0.0f, 0.0f), 0.8f);
    }
    if (at(6, 5.6f)) SelfTestRequestShot("sling_held", 1);
    // Let go in front of the body (toggle grip: the next squeeze lets go).
    if (at(7, 6.0f)) button(m_ActionWeaponGrip, true);
    if (at(8, 6.2f)) button(m_ActionWeaponGrip, false);
    if (at(9, 7.0f))
    {
        std::snprintf(detail, sizeof(detail), "slung=%d, active slot %d (rifle is 1), hangs %u", slung ? 1 : 0, slot + 1,
            m_ChestSlingHangCount.load(std::memory_order_acquire) - static_cast<uint32_t>(m_SelfTestValueA));
        SelfTestReport("sling: letting go near the body hangs the rifle on the chest", slung && slot == 0, detail);
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
    }
    // The left hand lets go: slung again.
    if (at(20, 13.4f)) button(m_ActionOffHandGrip, true);
    if (at(21, 13.6f)) button(m_ActionOffHandGrip, false);
    if (at(22, 14.4f))
    {
        std::snprintf(detail, sizeof(detail), "slung=%d active slot %d", slung ? 1 : 0, slot + 1);
        SelfTestReport("sling: the left hand lets go: slung again", slung && slot == 0, detail);
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
        SelfTestReport("sling: letting go at arm's length drops it", !slung && primary == nullptr, detail);
        SelfTestHoldRight(body::RightReady(), QAngle(0.0f, 0.0f, 0.0f));
        m_SelfTestLeftWorldValid = false;
        return true;
    }
    return false;
}

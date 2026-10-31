// Self-test steps for the survivor hand poses (vr/vr_hand_poses.inl).
// Included by vr.cpp after vr_selftest_hands.inl; RunHandSelfTestStep calls it first.
//
//   handact=<left|right>:<activity>[:<weaponId>] | <hand>:off | off
//                     force a hand's pose activity (free, gun, animated, support, magazine,
//                     shell, handle, gunposed) for a photograph;
//                     left = the support hand, right = the item hand
//   foregrip=1|0      1: the support hand goes onto the held gun's foregrip target and squeezes
//                     the grip until the two-handed grip holds (PASS/FAIL); 0: lets go again
//   posecheck=<left|right>:<activity>
//                     PASS when that hand's pose activity is <activity> (waits up to 2 s)
//   posephotos=1|0    the reload steps photograph the hand on the bolt/slide/pump
//                     (<gun>_bolt_held, <gun>_bolt_pulled, <gun>_pump_held)

bool VR::RunHandPoseSelfTestStep(const std::string& step, float t, bool& handled)
{
    handled = true;
    char detail[192];
    auto sideOf = [](const std::string& name) { return name == "left" ? 0 : name == "right" ? 1 : -1; };

    if (step.rfind("handact=", 0) == 0)
    {
        const std::string arg = step.substr(8);
        if (arg == "off")
        {
            m_SelfTestHandPoseForced[0].store(-1, std::memory_order_release);
            m_SelfTestHandPoseForced[1].store(-1, std::memory_order_release);
            Game::logMsg("[VR][SelfTest] hand pose activities released");
            return true;
        }
        const size_t c1 = arg.find(':');
        const int side = sideOf(arg.substr(0, c1));
        const std::string rest = c1 == std::string::npos ? std::string() : arg.substr(c1 + 1);
        const size_t c2 = rest.find(':');
        const std::string activityName = rest.substr(0, c2);
        const int weaponId = c2 == std::string::npos ? 0 : std::atoi(rest.c_str() + c2 + 1);
        if (side < 0)
        {
            SelfTestReport(step.c_str(), false, "handact=<left|right>:<activity>[:<weaponId>]|off");
            return true;
        }
        if (activityName == "off")
        {
            m_SelfTestHandPoseForced[side].store(-1, std::memory_order_release);
            return true;
        }
        l4d2vr_handpose::HandActivity activity{};
        if (!HandPoseActivityFromName(activityName, activity))
        {
            SelfTestReport(step.c_str(), false, "unknown activity");
            return true;
        }
        m_SelfTestHandPoseForced[side].store(static_cast<int>(activity) | (std::clamp(weaponId, 0, 255) << 8),
            std::memory_order_release);
        Game::logMsg("[VR][SelfTest] %s hand pose forced to %s weapon=%d", side == 0 ? "support" : "item",
            activityName.c_str(), weaponId);
        return t >= 0.3f;
    }

    if (step.rfind("posecheck=", 0) == 0)
    {
        const std::string arg = step.substr(10);
        const size_t colon = arg.find(':');
        const int side = sideOf(arg.substr(0, colon));
        l4d2vr_handpose::HandActivity want{};
        if (side < 0 || colon == std::string::npos || !HandPoseActivityFromName(arg.substr(colon + 1), want))
        {
            SelfTestReport(step.c_str(), false, "posecheck=<left|right>:<activity>");
            return true;
        }
        l4d2vr_handpose::HandOutput out{};
        const bool valid = GetHandPoseOutput(side == 0 ? -1 : 1, out);
        if ((!valid || out.activity != want) && t < 2.0f)
            return false;
        std::snprintf(detail, sizeof(detail), "%s hand pose %s (want %s)%s", side == 0 ? "support" : "item",
            valid ? HandPoseActivityName(out.activity) : "none", HandPoseActivityName(want), valid ? "" : ", no output");
        SelfTestReport(step.c_str(), valid && out.activity == want, detail);
        return true;
    }

    if (step.rfind("posephotos=", 0) == 0)
    {
        m_SelfTestHandPosePhotos = step.substr(11) == "1";
        return true;
    }

    if (step == "foregrip=1")
    {
        // Follow the foregrip target until the grip holds, then keep the hand there.
        Vector target{};
        if (!m_VrHandsTwoHandedGripActive && GetTwoHandedGripTargetPoint(target))
        {
            m_SelfTestLeftWorld = target;
            m_SelfTestLeftWorldValid = true;
        }
        if (m_VrHandsTwoHandedGripActive)
        {
            l4d2vr_selftest::SetButton(m_ActionOffHandGrip, false);
            if (m_SelfTestPhase == 0)
            {
                m_SelfTestPhase = 1;
                m_SelfTestHandPoseGripAt = t;
                std::snprintf(detail, sizeof(detail), "two-handed grip held after %.1f s", t);
                SelfTestReport("foregrip", true, detail);
            }
            return t >= m_SelfTestHandPoseGripAt + 0.6f;  // let the pose blend in first
        }
        if (t >= 1.0f && t < 1.15f)
            l4d2vr_selftest::SetButton(m_ActionOffHandGrip, true);
        else if (t >= 1.15f && t < 2.5f)
            l4d2vr_selftest::SetButton(m_ActionOffHandGrip, false);
        else if (t >= 2.5f && t < 2.65f)
            l4d2vr_selftest::SetButton(m_ActionOffHandGrip, true);
        else
            l4d2vr_selftest::SetButton(m_ActionOffHandGrip, false);
        if (t < 4.5f)
            return false;
        SelfTestReport("foregrip", false, "the two-handed grip never held");
        m_SelfTestLeftWorldValid = false;
        return true;
    }

    if (step == "foregrip=0")
    {
        if (m_VrHandsTwoHandedGripActive)
        {
            if (t < 0.15f) { l4d2vr_selftest::SetButton(m_ActionOffHandGrip, true); return false; }
            l4d2vr_selftest::SetButton(m_ActionOffHandGrip, false);
            if (t < 1.0f) return false;
        }
        l4d2vr_selftest::SetButton(m_ActionOffHandGrip, false);
        m_SelfTestLeftWorldValid = false;
        return t >= 0.3f;
    }

    handled = false;
    return false;
}

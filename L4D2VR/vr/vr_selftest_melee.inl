// Self-test step for the melee feel (welded grip, rotation-only lag, swing gates).
//
//   meleelag=<melee script name>  draws that melee weapon (katana, knife, ...)
//       and slashes it across the body at four speeds: 0.6 s (slow, must not
//       count), 0.3 s, 0.2 s and 0.14 s (must count once each), each after a
//       slow wind-up that must not count either. For every slash it records the
//       weapon's lag behind the controller on the update thread and on the
//       drawn weapon (the root bone of the viewmodel as submitted for drawing,
//       after every viewmodel bone hook; VR::RecordMeleeDrawnBones), how far
//       the drawn grip left the hand, how far the drawn weapon is from the
//       pose the usercmd sends to the server, and compares the lag with the lag
//       spring replayed on the same path at 90 Hz. Then photographed slashes at
//       0.2 s: meleelag_<w>_rest (held still), _mid (halfway through the
//       slash), _late (three quarters) and _mid_nolag (halfway, with the lag
//       turned off for the comparison).

namespace
{
    struct MeleeLagSwing
    {
        float seconds = 0.0f;   // slash duration
        bool counts = false;    // must register as a swing
        float laggedPeak = 0.0f, drawnPeak = 0.0f, drift = 0.0f, model = 0.0f, serverMismatch = 0.0f;
        uint32_t swings = 0u, frames = 0u;
    };

    // Head-local slash: from the right shoulder across the body to the left
    // hip, the blade turning 120 degrees (yaw -70 -> +50) with it.
    const l4d2vr_selftest::Vec3 kMeleeLagFrom{ 0.30f, 0.40f, -0.20f };
    const l4d2vr_selftest::Vec3 kMeleeLagTo{ 0.45f, -0.30f, -0.30f };
    const l4d2vr_selftest::Vec3 kMeleeLagFromAng{ -10.0f, -70.0f, 0.0f };
    const l4d2vr_selftest::Vec3 kMeleeLagToAng{ -10.0f, 50.0f, 0.0f };

    // Peak lag of the lag spring on the scripted angle path at 90 Hz (smoothstep keys).
    float MeleeLagModelPeak(const l4d2vr_melee_inertia::Params& params, float seconds)
    {
        namespace mi = l4d2vr_melee_inertia;
        mi::Spring spring(params);
        auto orientation = [](float yaw, float pitch) {
            Vector f{}, r{}, u{};
            QAngle::AngleVectors(QAngle(pitch, yaw, 0.0f), &f, &r, &u);
            mi::Quat q{};
            mi::QuatFromAxes(f, r, u, q);
            return q;
        };
        spring.Reset(mi::Pose{ Vector(0.0f, 0.0f, 0.0f), orientation(kMeleeLagFromAng.y, kMeleeLagFromAng.x) });
        float peak = 0.0f;
        const float dt = 1.0f / 90.0f;
        for (float t = dt; t <= seconds + 0.3f; t += dt)
        {
            const float u = l4d2vr_selftest::SmoothStep(t / seconds);
            const float yaw = kMeleeLagFromAng.y + (kMeleeLagToAng.y - kMeleeLagFromAng.y) * u;
            spring.Step(mi::Pose{ Vector(0.0f, 0.0f, 0.0f), orientation(yaw, kMeleeLagFromAng.x) }, dt);
            peak = std::max(peak, spring.LagDeg());
        }
        return peak;
    }

    float s_MeleeLagFastestPeak[2] = { -1.0f, -1.0f }; // [0] long weapon, [1] short weapon, for the comparison

    // A melee weapon with the same lag spring and swing gates (same weapon
    // class), for a weapon the map does not offer: the server only creates
    // the melee weapons the campaign's mission file lists (Dead Air has no
    // machete and no fire axe). machete -> tonfa, fireaxe -> electric_guitar.
    std::string MeleeLagStandIn(const std::string& name)
    {
        namespace mi = l4d2vr_melee_inertia;
        std::size_t count = 0;
        const mi::MeleeProfile* const profiles = mi::MeleeProfiles(count);
        const mi::MeleeProfile* asked = nullptr;
        for (std::size_t i = 0; i < count; ++i)
            if (name == profiles[i].name) asked = &profiles[i];
        if (!asked)
            return std::string();
        const bool askedShort = asked->reach < mi::kShortWeaponReach;
        for (std::size_t i = 0; i < count; ++i)
        {
            const mi::MeleeProfile& p = profiles[i];
            if (&p != asked && p.hz == asked->hz && p.zeta == asked->zeta && p.maxLagDeg == asked->maxLagDeg &&
                (p.reach < mi::kShortWeaponReach) == askedShort && name != "chainsaw" && std::string(p.name) != "chainsaw")
                return p.name;
        }
        return std::string();
    }
}

bool VR::RunMeleeSelfTestStep(const std::string& step, float stepTime, C_BasePlayer* player, bool& handled)
{
    if (step.rfind("meleelag=", 0) != 0)
        return false;
    handled = true;
    using l4d2vr_selftest::Key;
    const std::string weapon = step.substr(9);
    static std::vector<MeleeLagSwing> s_Swings;
    static uint32_t s_SerialBase = 0u;
    static float s_LaggedPeak = 0.0f;
    static float s_SwingStart = 0.0f;
    static bool s_Measuring = false;
    static bool s_InertiaWasEnabled = true;
    static std::string s_ScriptName;
    static std::string s_StandIn;   // the stand-in being drawn instead of `weapon`, if any
    static float s_TimeShift = 0.0f; // the timeline restarts when a stand-in is drawn
    if (m_SelfTestStage == 0)
    {
        s_TimeShift = 0.0f;
        s_StandIn.clear();
    }
    const float t = stepTime - s_TimeShift;
    bool fired = false;
    auto at = [&](int stage, float when) {
        if (fired || m_SelfTestStage != stage || t < when) return false;
        m_SelfTestStage = stage + 1;
        fired = true;
        return true;
    };
    auto cmd = [&](const std::string& text) { if (m_Game) m_Game->ClientCmd_Unrestricted(text.c_str()); };
    auto vec = [](const l4d2vr_selftest::Vec3& v) { return v; };
    // Slow return to the wind-up pose (0.7 s), then hold.
    auto windUp = [&]() {
        SelfTestStartPath({ Key{ 0.0f, m_SelfTestRightNow }, Key{ 0.7f, vec(kMeleeLagFrom) } },
            { Key{ 0.0f, m_SelfTestRightAngNow }, Key{ 0.7f, vec(kMeleeLagFromAng) } });
    };
    auto slash = [&](float seconds) {
        SelfTestStartPath({ Key{ 0.0f, vec(kMeleeLagFrom) }, Key{ seconds, vec(kMeleeLagTo) } },
            { Key{ 0.0f, vec(kMeleeLagFromAng) }, Key{ seconds, vec(kMeleeLagToAng) } });
    };

    if (at(0, 0.0f))
    {
        s_Swings.clear();
        for (float seconds : { 0.6f, 0.3f, 0.2f, 0.14f })
        {
            MeleeLagSwing swing{};
            swing.seconds = seconds;
            swing.counts = seconds < 0.5f;
            s_Swings.push_back(swing);
        }
        s_Measuring = false;
        cmd("give " + weapon);
        SelfTestHoldRight(SelfTestVec(0.35f, 0.25f, -0.30f), QAngle(-20.0f, -20.0f, 0.0f));
    }
    if (at(1, 1.2f))
        cmd("use weapon_melee");
    if (at(2, 2.4f))
    {
        s_ScriptName = m_MeleeMotionScriptName;
        const bool melee = SelfTestActiveWeaponId(player) == C_WeaponCSBase::MELEE;
        Game::logMsg("[VR][SelfTest] INFO meleelag: asked for %s, holding %s (weapon id %d)", weapon.c_str(),
            s_ScriptName.empty() ? "nothing" : s_ScriptName.c_str(), SelfTestActiveWeaponId(player));
        const std::string& wanted = s_StandIn.empty() ? weapon : s_StandIn;
        if (!melee || s_ScriptName != wanted)
        {
            const std::string standIn = s_StandIn.empty() ? MeleeLagStandIn(weapon) : std::string();
            if (!standIn.empty())
            {
                // Not on this map's melee list: measure the weapon with the same
                // lag spring and swing gates instead, from a fresh timeline.
                Game::logMsg("[VR][SelfTest] INFO meleelag: %s is not on this map's melee list (holding '%s'); measuring %s, which has the same lag spring and swing gates",
                    weapon.c_str(), s_ScriptName.c_str(), standIn.c_str());
                s_StandIn = standIn;
                cmd("give " + standIn);
                s_TimeShift = stepTime;
                m_SelfTestStage = 1;
                return false;
            }
            char detail[192];
            std::snprintf(detail, sizeof(detail), "%s is not available on this map (holding '%s')", wanted.c_str(),
                s_ScriptName.c_str());
            SelfTestReport("meleelag: weapon drawn", false, detail);
            return true;
        }
        if (!s_StandIn.empty())
            Game::logMsg("[VR][SelfTest] INFO meleelag: measuring %s in place of %s (its checks below carry the stand-in's name)",
                s_StandIn.c_str(), weapon.c_str());
        windUp();
    }

    // Four measured slashes, 2.2 s apart: wind-up at +0, slash at +1.1, read at +1.1 + T + 0.45.
    const float base = 2.4f;
    for (int i = 0; i < 4; ++i)
    {
        MeleeLagSwing& swing = s_Swings[static_cast<size_t>(i)];
        const float start = base + 2.2f * static_cast<float>(i);
        if (i > 0 && at(3 + i * 3 - 1, start))
            windUp();
        if (at(3 + i * 3, start + 1.1f))
        {
            s_SerialBase = m_MeleeSwingStartSerial.load(std::memory_order_acquire);
            s_LaggedPeak = 0.0f;
            m_MeleeDrawnLagMaxDeg.store(0.0f, std::memory_order_release);
            m_MeleeDrawnGripDriftMax.store(0.0f, std::memory_order_release);
            m_MeleeDrawnServerMismatchMaxDeg.store(0.0f, std::memory_order_release);
            m_MeleeDrawnFrames.store(0u, std::memory_order_release);
            m_MeleeDrawnLagRecord.store(true, std::memory_order_release);
            s_SwingStart = t;
            s_Measuring = true;
            slash(swing.seconds);
        }
        if (at(3 + i * 3 + 1, start + 1.1f + swing.seconds + 0.45f))
        {
            s_Measuring = false;
            m_MeleeDrawnLagRecord.store(false, std::memory_order_release);
            swing.laggedPeak = s_LaggedPeak;
            swing.drawnPeak = m_MeleeDrawnLagMaxDeg.load(std::memory_order_acquire);
            swing.drift = m_MeleeDrawnGripDriftMax.load(std::memory_order_acquire);
            swing.serverMismatch = m_MeleeDrawnServerMismatchMaxDeg.load(std::memory_order_acquire);
            swing.frames = m_MeleeDrawnFrames.load(std::memory_order_acquire);
            swing.swings = m_MeleeSwingStartSerial.load(std::memory_order_acquire) - s_SerialBase;
            swing.model = MeleeLagModelPeak(m_MeleeMotionParams, swing.seconds);
            Game::logMsg("[VR][SelfTest] INFO meleelag %s: %.2f s slash (peak %.0f deg/s, hand %.1f m/s): lag %.1f deg (drawn %.1f, model %.1f), grip drift %.2f cm, drawn vs usercmd pose %.2f deg over %u drawn frames, %u swing(s)",
                s_ScriptName.c_str(), swing.seconds, 1.5f * 120.0f / swing.seconds,
                1.5f * l4d2vr_selftest::Distance(kMeleeLagFrom, kMeleeLagTo) / swing.seconds, swing.laggedPeak,
                swing.drawnPeak, swing.model, swing.drift * 100.0f, swing.serverMismatch, swing.frames, swing.swings);
        }
    }
    if (s_Measuring)
        s_LaggedPeak = std::max(s_LaggedPeak, m_MeleeLagDeg.load(std::memory_order_acquire));

    // Photographed slashes (a picture stalls the game, so none is measured).
    const float photos = base + 2.2f * 4.0f;
    // The last one is the mid-slash photo again with the lag turned off, so the
    // two pictures show what the lag does to the drawn weapon.
    const char* const names[4] = { "rest", "mid", "late", "mid_nolag" };
    for (int i = 0; i < 4; ++i)
    {
        const float start = photos + 2.0f * static_cast<float>(i);
        const int stage = 14 + i * 3;
        if (at(stage, start))
        {
            if (i == 3)
            {
                s_InertiaWasEnabled = m_MeleeInertiaEnabled;
                m_MeleeInertiaEnabled = false;
            }
            windUp();
        }
        if (at(stage + 1, start + 1.1f))
        {
            if (i == 0)
                SelfTestRequestShot("meleelag_" + s_ScriptName + "_rest", 1);
            else
                slash(0.2f);
        }
        if (at(stage + 2, start + 1.1f + (i == 2 ? 0.15f : i == 0 ? 0.5f : 0.10f)))
        {
            if (i > 0)
                SelfTestRequestShot("meleelag_" + s_ScriptName + "_" + names[i], 1);
        }
    }

    if (at(26, photos + 8.5f))
    {
        m_MeleeInertiaEnabled = s_InertiaWasEnabled;
        char detail[256];
        const MeleeLagSwing& slow = s_Swings[0];
        bool countsOk = true, modelOk = true, drawnOk = true, weldOk = true, serverOk = true, rising = true;
        std::string counts, lags;
        for (size_t i = 0; i < s_Swings.size(); ++i)
        {
            const MeleeLagSwing& s = s_Swings[i];
            countsOk = countsOk && s.swings == (s.counts ? 1u : 0u);
            modelOk = modelOk && std::fabs(s.laggedPeak - s.model) <= std::max(3.0f, 0.25f * s.model);
            drawnOk = drawnOk && s.frames > 0u && std::fabs(s.drawnPeak - s.laggedPeak) <= std::max(4.0f, 0.2f * s.laggedPeak);
            weldOk = weldOk && s.frames > 0u && s.drift < 0.005f;
            serverOk = serverOk && s.frames > 0u && s.serverMismatch <= 2.0f;
            if (i > 0) rising = rising && s.laggedPeak > s_Swings[i - 1].laggedPeak;
            char part[96];
            std::snprintf(part, sizeof(part), "%s%.2fs:%u", i ? " " : "", s.seconds, s.swings);
            counts += part;
            std::snprintf(part, sizeof(part), "%s%.2fs %.1f/%.1f/%.1f", i ? ", " : "", s.seconds, s.laggedPeak, s.drawnPeak, s.model);
            lags += part;
        }
        std::snprintf(detail, sizeof(detail), "swings per slash %s (the %.1f s slash and the wind-ups must not count)", counts.c_str(), slow.seconds);
        SelfTestReport(("meleelag " + s_ScriptName + ": slow swings and wind-ups never count, fast slashes once each").c_str(), countsOk, detail);
        std::snprintf(detail, sizeof(detail), "peak lag deg (update/drawn/model): %s", lags.c_str());
        SelfTestReport(("meleelag " + s_ScriptName + ": lag grows with swing speed and matches the lag spring").c_str(),
            modelOk && rising, detail);
        SelfTestReport(("meleelag " + s_ScriptName + ": the drawn weapon shows the lag").c_str(), drawnOk, detail);
        float drift = 0.0f, mismatch = 0.0f;
        uint32_t frames = 0u;
        for (const MeleeLagSwing& s : s_Swings)
        {
            drift = std::max(drift, s.drift);
            mismatch = std::max(mismatch, s.serverMismatch);
            frames += s.frames;
        }
        std::snprintf(detail, sizeof(detail), "the drawn grip moved at most %.2f cm off the hand (%u drawn frames)",
            drift * 100.0f, frames);
        SelfTestReport(("meleelag " + s_ScriptName + ": grip welded to the hand").c_str(), weldOk, detail);
        std::snprintf(detail, sizeof(detail), "drawn weapon at most %.2f deg from the pose the usercmd sends (%u drawn frames)",
            mismatch, frames);
        SelfTestReport(("meleelag " + s_ScriptName + ": the server's melee sweep follows the drawn weapon").c_str(), serverOk, detail);
        // Short weapons trail less at full speed (lower lag limit, snappier spring).
        const bool shortWeapon = l4d2vr_melee_inertia::ShortWeapon(m_MeleeMotionParams);
        s_MeleeLagFastestPeak[shortWeapon ? 1 : 0] = s_Swings.back().laggedPeak;
        if (s_MeleeLagFastestPeak[0] >= 0.0f && s_MeleeLagFastestPeak[1] >= 0.0f)
        {
            std::snprintf(detail, sizeof(detail), "fastest slash: long weapon %.1f deg, short weapon %.1f deg",
                s_MeleeLagFastestPeak[0], s_MeleeLagFastestPeak[1]);
            SelfTestReport("meleelag: the short weapon trails less than the long one", s_MeleeLagFastestPeak[1] < s_MeleeLagFastestPeak[0], detail);
        }
        SelfTestHoldRight(SelfTestVec(0.35f, 0.2f, -0.3f), QAngle(0.0f, 0.0f, 0.0f));
        return true;
    }
    return false;
}

// Self-test for the physical dual-pistol reload (vr_dual_reload.h), driven
// through the real hand path: the scripted controllers move onto the published
// points (the other pistol hand, the pouch, the magazine wells, the slides) and
// the real buttons are pressed; nothing calls the reload logic directly.
//
//   dualreloade2e=left      run both guns dry, release, the LEFT hand moves into
//                           the right one (stack), double magazine from the pouch,
//                           seat, rack both slides one at a time, take the gun
//                           back, both guns fire.
//   dualreloade2e=right     the same with the RIGHT hand moving into the left one.
//   dualreloade2e=tactical  fire four rounds (both guns still chambered), reload:
//                           no slide needs pulling.
//   dualreloade2e=cancel    release, stack, take the gun back before the magazines
//                           (unstack), switch weapons mid-reload (cancel: nothing
//                           committed, the magazines are back), then a full reload.
// Run under cfg=LeftHanded=true for the left-handed check. Screenshots
// dual_<variant>_<stage> at every stage.

namespace
{
    struct DualReloadTestHandGlide
    {
        Vector from{}, to{};
        uint64_t startMs = 0u, durationMs = 0u;
        bool active = false;
    };
    DualReloadTestHandGlide s_DualGlide[2];
    Vector s_DualRest[2];
    uint32_t s_DualDropSerial = 0u, s_DualEvents0[14] = {};
    int s_DualClipBefore = -1;
    bool s_DualCancelDone = false;
}

bool VR::RunDualReloadSelfTestStep(const std::string& step, float t, C_BasePlayer* player, bool& handled)
{
    (void)t;
    handled = step.rfind("dualreloade2e=", 0) == 0;
    if (!handled) return false;
    namespace dr = l4d2vr_dual_reload;
    const std::string variant = step.substr(step.find('=') + 1);
    if (variant != "left" && variant != "right" && variant != "tactical" && variant != "cancel")
    {
        SelfTestReport(step.c_str(), false, "unknown variant");
        return true;
    }
    const uint64_t nowMs = GetTickCount64();
    const float scale = (std::max)(0.001f, m_VRScale);
    const float inPhase = (nowMs - m_SelfTestPhaseStartMs) / 1000.0f;
    auto go = [&](int next) { m_SelfTestPhase = next; m_SelfTestPhaseStartMs = nowMs; };
    auto cmd = [&](const char* text) { if (m_Game) m_Game->ClientCmd_Unrestricted(text); };
    auto button = [&](vr::VRActionHandle_t action, bool down) { l4d2vr_selftest::SetButton(action, down); };
    char detail[400];
    char name[128];
    auto label = [&](const char* text) { std::snprintf(name, sizeof(name), "dual %s: %s", variant.c_str(), text); return name; };
    auto shot = [&](const char* stage)
    { SelfTestRequestShot(std::string(m_LeftHanded ? "dual_lh_" : "dual_") + variant + "_" + stage, 1); };
    // The moving hand and the receiving hand (gameplay 0 right, 1 left).
    const int moving = variant == "right" ? 0 : 1;
    const int receiving = 1 - moving;
    const vr::VRActionHandle_t trigger[2] = { m_ActionPrimaryAttack, m_ActionSecondaryAttack };
    const vr::VRActionHandle_t grip[2] = { m_ActionWeaponGrip, m_ActionOffHandGrip };
    const vr::VRActionHandle_t grab[2] = {
        m_MagazineInteractionTriggerInteractions ? m_ActionPrimaryAttack : m_ActionWeaponGrip,
        m_MagazineInteractionTriggerInteractions ? m_ActionSecondaryAttack : m_ActionOffHandGrip };
    auto releaseAll = [&]()
    {
        for (int h = 0; h < 2; ++h) { button(trigger[h], false); button(grip[h], false); }
        button(m_ActionMagazineRelease, false);
    };
    auto finish = [&]() { releaseAll(); s_DualGlide[0].active = s_DualGlide[1].active = false; return true; };
    auto fail = [&](const char* what, const char* why) { SelfTestReport(label(what), false, why); return finish(); };

    DualReloadSelfTestPoints p{};
    GetDualReloadSelfTestPoints(p);
    const dr::Stage stage = static_cast<dr::Stage>(p.stage);
    auto events = [&](int bit) { return m_DualReloadEventCount[bit].load(std::memory_order_acquire) - s_DualEvents0[bit]; };
    auto handNow = [&](int h) { return h == 0 ? m_RightControllerPosAbs : m_LeftControllerPosAbs; };
    // Where the script holds a hand (the tracked pose trails it by a frame or more).
    auto scripted = [&](int h)
    {
        if (h == 0) return m_SelfTestRightWorldValid ? m_SelfTestRightWorld : handNow(0);
        return m_SelfTestLeftWorldValid ? m_SelfTestLeftWorld : handNow(1);
    };
    // Scripted hands: world targets with straight glides.
    auto setHand = [&](int h, const Vector& world)
    {
        if (h == 0) { m_SelfTestPathActive = false; m_SelfTestRightWorld = world; m_SelfTestRightWorldValid = true; }
        else { m_SelfTestLeftPathActive = false; m_SelfTestLeftWorld = world; m_SelfTestLeftWorldValid = true; }
    };
    auto glide = [&](int h, const Vector& to, float seconds)
    {
        auto& g = s_DualGlide[h];
        g.from = h == 0 ? (m_SelfTestRightWorldValid ? m_SelfTestRightWorld : handNow(0))
                        : (m_SelfTestLeftWorldValid ? m_SelfTestLeftWorld : handNow(1));
        g.to = to; g.startMs = nowMs; g.durationMs = static_cast<uint64_t>((std::max)(0.01f, seconds) * 1000.0f);
        g.active = true;
    };
    for (int h = 0; h < 2; ++h)
    {
        auto& g = s_DualGlide[h];
        if (!g.active) continue;
        const float u = (std::min)(1.0f, (nowMs - g.startMs) / static_cast<float>(g.durationMs));
        const float s = u * u * (3.0f - 2.0f * u);
        setHand(h, g.from + (g.to - g.from) * s);
    }
    auto glideDone = [&](int h, float settle)
    {
        const auto& g = s_DualGlide[h];
        return !g.active || nowMs >= g.startMs + g.durationMs + static_cast<uint64_t>(settle * 1000.0f);
    };
    auto stageName = [&]() { return dr::StageName(stage); };

    switch (m_SelfTestPhase)
    {
    case 0:
        releaseAll();
        s_DualGlide[0].active = s_DualGlide[1].active = false;
        s_DualCancelDone = false;
        m_SelfTestLeftAngWorldValid = false;
        m_SelfTestRightAngLocal = QAngle(0.0f, 0.0f, 0.0f);
        m_SelfTestLeftAngLocal = QAngle(0.0f, 0.0f, 0.0f);
        SelfTestHoldRight(SelfTestVec(0.40f, 0.13f, -0.28f), QAngle(0.0f, 0.0f, 0.0f));
        m_SelfTestLeftWorldValid = false;
        m_SelfTestLeftPathActive = false;
        m_SelfTestLeftLocal = SelfTestVec(0.40f, -0.13f, -0.28f);
        if (variant == "cancel") cmd("give smg");
        cmd("give pistol");
        go(1);
        return false;
    case 1:
        if (inPhase < 1.0f) return false;
        if (!m_DualPistolsActive.load(std::memory_order_acquire)) cmd("give pistol");
        go(2);
        return false;
    case 2:
        if (inPhase < 1.0f) return false;
        cmd("use weapon_pistol");
        go(3);
        return false;
    case 3:
    {
        const bool ready = m_DualPistolsActive.load(std::memory_order_acquire) && p.enabled && p.geometryFresh &&
            stage == dr::Stage::Idle && p.clip >= 0;
        if (!ready)
        {
            if (inPhase < 8.0f) return false;
            std::snprintf(detail, sizeof(detail), "dual=%d enabled=%d geometry=%d stage=%s clip=%d",
                m_DualPistolsActive.load(std::memory_order_acquire) ? 1 : 0, p.enabled ? 1 : 0, p.geometryFresh ? 1 : 0,
                stageName(), p.clip);
            return fail("ready", detail);
        }
        if (inPhase < 1.0f) return false;
        s_DualRest[0] = handNow(0);
        s_DualRest[1] = handNow(1);
        setHand(0, s_DualRest[0]);
        setHand(1, s_DualRest[1]);
        m_SelfTestClip0 = p.clip;
        m_SelfTestReserve0 = p.reserve;
        m_SelfTestCycle = 0;
        Game::logMsg("[VR][SelfTest] %s: clip=%d reserve=%d host transaction=%d gap=%.3fm", step.c_str(), p.clip, p.reserve,
            p.hostTxn ? 1 : 0, (s_DualRest[0] - s_DualRest[1]).Length() / scale);
        go(variant == "tactical" ? 20 : 10);
        return false;
    }
    case 10: // run both guns dry, alternating the triggers
    {
        if (p.clip == 0)
        {
            releaseAll();
            if (inPhase < 0.6f) return false;
            go(30);
            return false;
        }
        if (inPhase > 25.0f)
        {
            std::snprintf(detail, sizeof(detail), "clip still %d", p.clip);
            return fail("run dry", detail);
        }
        const int tick = static_cast<int>(inPhase / 0.09f);
        const int hand = (tick / 2) % 2;
        button(trigger[hand], (tick % 2) == 0);
        button(trigger[1 - hand], false);
        return false;
    }
    case 20: // tactical: four rounds, both guns stay chambered
    {
        if (p.clip <= m_SelfTestClip0 - 4)
        {
            releaseAll();
            if (inPhase < 0.6f) return false;
            go(30);
            return false;
        }
        if (inPhase > 10.0f)
        {
            std::snprintf(detail, sizeof(detail), "clip %d -> %d", m_SelfTestClip0, p.clip);
            return fail("fire four rounds", detail);
        }
        const int tick = static_cast<int>(inPhase / 0.12f);
        const int hand = (tick / 2) % 2;
        button(trigger[hand], (tick % 2) == 0);
        button(trigger[1 - hand], false);
        return false;
    }
    case 30: // release both magazines
        if (inPhase < 0.3f) return false;
        shot(s_DualCancelDone ? "before2" : "before");
        for (int bit = 0; bit < 14; ++bit) s_DualEvents0[bit] = m_DualReloadEventCount[bit].load(std::memory_order_acquire);
        s_DualDropSerial = m_DroppedMagazineSpawnSerial.load(std::memory_order_acquire);
        s_DualClipBefore = p.clip;
        button(m_ActionMagazineRelease, true);
        go(31);
        return false;
    case 31:
    {
        if (inPhase >= 0.15f) button(m_ActionMagazineRelease, false);
        if (stage != dr::Stage::Ejected)
        {
            if (inPhase < 2.0f) return false;
            std::snprintf(detail, sizeof(detail), "stage %s", stageName());
            return fail("release drops both magazines", detail);
        }
        if (inPhase < 0.5f) return false;
        const uint32_t drops = m_DroppedMagazineSpawnSerial.load(std::memory_order_acquire) - s_DualDropSerial;
        std::snprintf(detail, sizeof(detail), "stage %s, %u magazine(s) dropped, clip %d (unchanged until the new ones seat)",
            stageName(), drops, p.clip);
        SelfTestReport(label("release drops both magazines"), events(0) == 1u &&
            (drops == 2u || !m_MagazineDropEnabled) && p.hideClips && p.clip == s_DualClipBefore, detail);
        shot(s_DualCancelDone ? "ejected2" : "ejected");
        // The triggers do nothing while the magazines are out.
        m_SelfTestValueA = p.clip;
        button(trigger[0], true);
        button(trigger[1], true);
        go(32);
        return false;
    }
    case 32:
        if (inPhase < 0.4f) return false;
        button(trigger[0], false);
        button(trigger[1], false);
        if (inPhase < 0.8f) return false;
        std::snprintf(detail, sizeof(detail), "clip %d -> %d with both triggers held", m_SelfTestValueA, p.clip);
        SelfTestReport(label("no shot while the magazines are out"), p.clip == m_SelfTestValueA, detail);
        {
            // Into the other hand from its side, guns parallel.
            const Vector lateral = VrHandMath::Normalize(s_DualRest[0] - s_DualRest[1]);
            const Vector target = scripted(receiving) + lateral * ((moving == 1 ? -0.065f : 0.065f) * scale);
            glide(moving, target, 0.6f);
        }
        go(33);
        return false;
    case 33:
    {
        if (stage != dr::Stage::Stacked)
        {
            if (!glideDone(moving, 2.0f)) return false;
            std::snprintf(detail, sizeof(detail), "stage %s, hands %.3f m apart",
                stageName(), (handNow(0) - handNow(1)).Length() / scale);
            return fail("the pistols stack", detail);
        }
        if (!glideDone(moving, 0.3f)) return false;
        std::snprintf(detail, sizeof(detail), "receiving hand %d (want %d), free hand %d, hands %.3f m apart",
            p.receiving, receiving, p.freeHand, (handNow(0) - handNow(1)).Length() / scale);
        SelfTestReport(label(moving == 1 ? "the left pistol stacks into the right hand" : "the right pistol stacks into the left hand"),
            p.receiving == receiving && p.freeHand == moving && events(1) == 1u, detail);
        shot(s_DualCancelDone ? "stacked2" : "stacked");
        if (variant == "cancel" && !s_DualCancelDone) { go(80); return false; }
        // Off the stack, then to the pouch.
        glide(moving, scripted(moving) + (scripted(moving) - scripted(receiving)) * 1.5f, 0.3f);
        go(34);
        return false;
    }
    case 34:
        if (!glideDone(moving, 0.1f)) return false;
        if (!p.pouchValid) return fail("double magazine from the pouch", "no pouch");
        glide(moving, p.pouchTarget, 0.6f);
        go(35);
        return false;
    case 35:
        if (!glideDone(moving, 0.2f)) return false;
        button(grab[moving], true);
        go(36);
        return false;
    case 36:
        if (stage != dr::Stage::HoldingMags)
        {
            if (inPhase < 1.0f) return false;
            std::snprintf(detail, sizeof(detail), "stage %s, hand %.3f m from the pouch", stageName(), p.pouchDistanceM);
            return fail("double magazine from the pouch", detail);
        }
        if (inPhase < 0.3f) return false;
        std::snprintf(detail, sizeof(detail), "hand %.3f m from the pouch", p.pouchDistanceM);
        SelfTestReport(label("the free hand takes the double magazine from the pouch"), events(3) >= 1u, detail);
        shot("mags_in_hand");
        if (!p.seatValid) return fail("seat", "no magazine wells");
        // Below the wells first, then straight up into them.
        glide(moving, p.freeSeatTarget - Vector(0.0f, 0.0f, 0.12f * scale), 0.6f);
        go(37);
        return false;
    case 37:
        if (!glideDone(moving, 0.2f)) return false;
        shot("mags_below_wells");
        glide(moving, p.freeSeatTarget, 0.4f);
        go(38);
        return false;
    case 38:
        if (stage == dr::Stage::HoldingMags)
        {
            if (!glideDone(moving, 0.3f)) return false;
            if (inPhase < 2.0f) { glide(moving, p.freeSeatTarget, 0.1f); return false; }
            std::snprintf(detail, sizeof(detail), "still held %.3f m from the wells, %.1f deg off", p.seatDistanceM, p.holdAngleDeg);
            return fail("both magazines seat", detail);
        }
        if (stage != dr::Stage::Committing && stage != dr::Stage::Racking && stage != dr::Stage::Ready)
        {
            std::snprintf(detail, sizeof(detail), "stage %s", stageName());
            return fail("both magazines seat", detail);
        }
        button(grab[moving], false);
        shot("seated");
        go(39);
        return false;
    case 39:
    {
        if (stage == dr::Stage::Committing)
        {
            if (inPhase < 4.0f) return false;
            return fail("the rounds go in", "still committing after 4 s");
        }
        if (inPhase < 0.3f) return false;
        // A gun needs its slide only when it had no round before (the shared clip split as the ledger does).
        int beforeRight = 0, beforeLeft = 0;
        dr::Split(s_DualClipBefore, beforeRight, beforeLeft);
        const bool wantRack[2] = { beforeRight == 0, beforeLeft == 0 };
        std::snprintf(detail, sizeof(detail), "stage %s, slides needed %d/%d (want %d/%d, right/left), host transaction %d, clip %d -> %d",
            stageName(), p.needsRack[0] ? 1 : 0, p.needsRack[1] ? 1 : 0, wantRack[0] ? 1 : 0, wantRack[1] ? 1 : 0,
            p.lastCommitByHost ? 1 : 0, s_DualClipBefore, p.clip);
        SelfTestReport(label(!wantRack[0] && !wantRack[1] ? "both seat; a chambered pair needs no slide"
                : "both seat; each empty gun needs its slide"),
            p.needsRack[0] == wantRack[0] && p.needsRack[1] == wantRack[1] &&
            stage == (wantRack[0] || wantRack[1] ? dr::Stage::Racking : dr::Stage::Ready), detail);
        // Clear of the guns before reaching for a slide.
        glide(moving, scripted(moving) - Vector(0.0f, 0.0f, 0.05f * scale), 0.2f);
        go(stage == dr::Stage::Racking ? 40 : 50);
        return false;
    }
    case 40: // rack the next slide
    {
        if (!glideDone(moving, 0.1f)) return false;
        const int gun = p.needsRack[0] ? 0 : p.needsRack[1] ? 1 : -1;
        if (gun < 0) { go(50); return false; }
        if (!p.slideValid[gun]) return fail("slide", "no slide point");
        m_SelfTestValueB = gun;
        glide(moving, p.slideTarget[gun], 0.4f);
        go(41);
        return false;
    }
    case 41:
        if (!glideDone(moving, 0.15f)) return false;
        button(grab[moving], true);
        go(42);
        return false;
    case 42:
        if (p.slideHeld != m_SelfTestValueB)
        {
            if (inPhase < 0.8f) return false;
            std::snprintf(detail, sizeof(detail), "gun %d: slide held %d, hand %.3f m from the slide",
                m_SelfTestValueB, p.slideHeld, p.slideDistanceM[m_SelfTestValueB]);
            return fail("free hand takes the slide", detail);
        }
        glide(moving, scripted(moving) + p.slideBack[m_SelfTestValueB] * (0.045f * scale), 0.3f);
        go(43);
        return false;
    case 43:
    {
        if (!glideDone(moving, 0.1f)) return false;
        const int gun = m_SelfTestValueB;
        std::snprintf(detail, sizeof(detail), "%s slide pulled %.3f m: still needed %d, stage %s", gun == 0 ? "right" : "left",
            p.slidePullM, p.needsRack[gun] ? 1 : 0, stageName());
        SelfTestReport(label(gun == 0 ? "the right gun's slide racks" : "the left gun's slide racks"), !p.needsRack[gun], detail);
        shot(gun == 0 ? "slide_right" : "slide_left");
        button(grab[moving], false);
        go(44);
        return false;
    }
    case 44:
        if (inPhase < 0.3f) return false;
        glide(moving, scripted(moving) - Vector(0.0f, 0.0f, 0.04f * scale), 0.15f);
        go(p.needsRack[0] || p.needsRack[1] ? 40 : 50);
        return false;
    case 50: // ready: the rounds are in, the triggers still do nothing
    {
        if (stage != dr::Stage::Ready)
        {
            if (inPhase < 1.0f) return false;
            std::snprintf(detail, sizeof(detail), "stage %s", stageName());
            return fail("ready to take the gun back", detail);
        }
        const int want = 30;
        if (p.clip != want && inPhase < 5.0f) return false;
        std::snprintf(detail, sizeof(detail), "clip %d -> %d (want %d, L4D2's own dual reload), reserve %d -> %d, %s",
            s_DualClipBefore, p.clip, want, m_SelfTestReserve0, p.reserve,
            p.lastCommitByHost ? "host transaction" : "stock reload pulse");
        SelfTestReport(label("the reload fills the pair like L4D2's dual reload"), p.clip == want, detail);
        m_SelfTestValueA = p.clip;
        button(trigger[receiving], true);
        go(51);
        return false;
    }
    case 51:
        if (inPhase < 0.3f) return false;
        button(trigger[receiving], false);
        if (inPhase < 0.6f) return false;
        std::snprintf(detail, sizeof(detail), "clip %d -> %d", m_SelfTestValueA, p.clip);
        SelfTestReport(label("no shot while stacked"), p.clip == m_SelfTestValueA, detail);
        {
            const Vector lateral = VrHandMath::Normalize(s_DualRest[0] - s_DualRest[1]);
            glide(moving, scripted(receiving) + lateral * ((moving == 1 ? -0.07f : 0.07f) * scale), 0.5f);
        }
        go(52);
        return false;
    case 52:
        if (!glideDone(moving, 0.2f)) return false;
        button(grip[moving], true);
        go(53);
        return false;
    case 53:
        if (stage != dr::Stage::Idle)
        {
            if (inPhase < 1.0f) return false;
            std::snprintf(detail, sizeof(detail), "stage %s, hands %.3f m apart", stageName(), (handNow(0) - handNow(1)).Length() / scale);
            return fail("the free hand takes its pistol back", detail);
        }
        button(grip[moving], false);
        glide(moving, s_DualRest[moving], 0.5f);
        go(54);
        return false;
    case 54:
        if (!glideDone(moving, 0.3f)) return false;
        std::snprintf(detail, sizeof(detail), "stage %s, done events %u", stageName(), events(10));
        SelfTestReport(label("the free hand takes its pistol back: akimbo again"), stage == dr::Stage::Idle && events(10) == 1u, detail);
        shot("done");
        m_SelfTestValueA = p.clip;
        button(trigger[0], true);
        go(55);
        return false;
    case 55:
        if (inPhase < 0.15f) return false;
        button(trigger[0], false);
        if (inPhase < 0.45f) return false;
        button(trigger[1], true);
        go(56);
        return false;
    case 56:
        if (inPhase < 0.15f) return false;
        button(trigger[1], false);
        if (p.clip > m_SelfTestValueA - 2 && inPhase < 1.5f) return false;
        std::snprintf(detail, sizeof(detail), "clip %d -> %d after one shot per trigger", m_SelfTestValueA, p.clip);
        SelfTestReport(label("both guns fire after the reload"), p.clip == m_SelfTestValueA - 2, detail);
        return finish();

    // ---- cancel: unstack, then a weapon switch mid-reload ----------------------------------------
    case 80:
        button(grip[moving], true);
        go(81);
        return false;
    case 81:
        if (stage != dr::Stage::Ejected)
        {
            if (inPhase < 1.0f) return false;
            std::snprintf(detail, sizeof(detail), "stage %s", stageName());
            return fail("the free hand takes its gun back before the magazines", detail);
        }
        button(grip[moving], false);
        SelfTestReport(label("the free hand takes its gun back before the magazines (unstack)"),
            events(2) == 1u && p.hideClips, "stage ejected, magazines still out");
        glide(moving, s_DualRest[moving], 0.4f);
        shot("unstacked");
        go(82);
        return false;
    case 82:
        if (!glideDone(moving, 0.3f)) return false;
        cmd("use weapon_smg");
        go(83);
        return false;
    case 83:
        if (stage != dr::Stage::Idle || m_DualPistolsActive.load(std::memory_order_acquire))
        {
            if (inPhase < 3.0f) return false;
            std::snprintf(detail, sizeof(detail), "stage %s dual=%d", stageName(), m_DualPistolsActive.load() ? 1 : 0);
            return fail("a weapon switch cancels the reload", detail);
        }
        if (inPhase < 0.8f) return false;
        SelfTestReport(label("a weapon switch cancels the reload"), events(11) >= 1u && !DualReloadActive(), "stage idle");
        cmd("use weapon_pistol");
        go(84);
        return false;
    case 84:
        if (!m_DualPistolsActive.load(std::memory_order_acquire) || !p.geometryFresh)
        {
            if (inPhase < 4.0f) return false;
            return fail("dual pistols back after the cancel", "not drawn");
        }
        if (inPhase < 1.5f) return false;
        // With the host transaction the stock reload is gated, so nothing may
        // change. Without it (plain servers, older plugins) the game's own
        // auto reload may refill the empty pair when it is drawn again.
        std::snprintf(detail, sizeof(detail), "clip %d (was %d), magazines %s, stage %s, %s",
            p.clip, s_DualClipBefore, p.hideClips ? "hidden" : "back in", stageName(),
            p.hostTxn ? "host transaction: the stock reload is gated" : "no host transaction: the stock auto reload may refill an empty pair on the draw");
        SelfTestReport(label("after the cancel nothing was committed and the magazines are back"),
            (p.hostTxn ? p.clip == s_DualClipBefore : (p.clip == s_DualClipBefore || p.clip == 30)) &&
            !p.hideClips && stage == dr::Stage::Idle, detail);
        shot("redrawn");
        s_DualCancelDone = true;
        s_DualRest[0] = handNow(0);
        s_DualRest[1] = handNow(1);
        go(30);
        return false;
    default:
        return fail("phase", "unknown");
    }
}

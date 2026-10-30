// Physical dual-pistol reload: the per-frame driver around
// l4d2vr_dual_reload::Machine (vr_dual_reload.h). Included at the end of
// vr_hand_vr_bridge.cpp so it shares the magazine helpers (pouch, sounds,
// controller frames, native ammo reads). The viewmodel side (stacking the
// guns, hiding/holding/drawing the magazines, moving the slides) is
// ApplyDualReloadViewmodelPose in hooks/hooks_misc.inl.
//
// All distances the machine sees are metres in the viewmodel layer (the space
// the guns are drawn in, BuildPistolControllerWorld).

namespace
{
    constexpr float kDualReloadHoldRaiseMeters = 0.09f;   // free fist this far below the receiving fist seats the pair
    constexpr char kDualClipOutSound[] = "weapons/dual_pistol/gunother/dualpistol_clip_out_1.wav";
    constexpr char kDualClipInSound[] = "weapons/dual_pistol/gunother/dualpistol_clip_in_1.wav";
    constexpr char kDualSlideBackSound[] = "weapons/pistol/gunother/pistol_slideback_1.wav";
    constexpr char kDualSlideForwardSound[] = "weapons/pistol/gunother/pistol_slideforward_1.wav";

    l4d2vr_dual_reload::V3 DualV3(const Vector& v, float scale)
    {
        return { v.x / scale, v.y / scale, v.z / scale };
    }
    Vector DualMatrixPoint(const VrHandMatrix4& m, const Vector& local)
    {
        return MagazineInteractionMatrixOrigin(m) + MagazineInteractionMatrixAxis(m, 0) * local.x +
            MagazineInteractionMatrixAxis(m, 1) * local.y + MagazineInteractionMatrixAxis(m, 2) * local.z;
    }
    Vector DualMatrixDirection(const VrHandMatrix4& m, const Vector& local)
    {
        return MagazineInteractionMatrixAxis(m, 0) * local.x + MagazineInteractionMatrixAxis(m, 1) * local.y +
            MagazineInteractionMatrixAxis(m, 2) * local.z;
    }

    // The magazine pouch (Pavlov layout: the off-hand hip), in the viewmodel layer.
    bool DualReloadResolvePouch(const VR* vr, Vector& point, Vector& forward, Vector& right)
    {
        if (!vr) return false;
        if (!MagazineInteractionResolvePavlovPouch(vr, point, forward, right))
        {
            Vector bodyForward{}, bodyRight{};
            if (!MagazineInteractionResolveFreshPickupBodyAxes(vr, bodyForward, bodyRight)) return false;
            const Vector worldUp(0.0f, 0.0f, 1.0f);
            Vector bodyOrigin = vr->m_HmdPosAbs
                + bodyForward * (vr->m_InventoryBodyOriginOffset.x * vr->m_VRScale)
                + bodyRight * (vr->m_InventoryBodyOriginOffset.y * vr->m_VRScale)
                + worldUp * (vr->m_InventoryBodyOriginOffset.z * vr->m_VRScale);
            if (vr->m_BodyGripInventoryEnabled)
                vr->GetBodyInventoryPose(bodyOrigin, bodyForward, bodyRight);
            const Vector offset = MagazineInteractionFreshMagazinePickupOffsetMeters(vr);
            point = bodyOrigin + bodyForward * (offset.x * vr->m_VRScale) + bodyRight * (offset.y * vr->m_VRScale) +
                worldUp * (offset.z * vr->m_VRScale);
            forward = bodyForward; right = bodyRight;
        }
        Vector projected{};
        if (MagazineInteractionReprojectScenePointToViewmodelLayer(vr, point, projected)) point = projected;
        return l4d2vr_interaction::Finite(point);
    }

    void DualReloadSound(VR* vr, const char* sample, bool slot, const char* label)
    {
        if (!vr) return;
        if (slot)
            MagazineInteractionPlaySyntheticSound(vr, sample, vr->m_MagazineInteractionSyntheticClipInSample,
                vr->m_MagazineInteractionSyntheticClipInStarted, label);
        else
            MagazineInteractionPlaySyntheticSound(vr, sample, vr->m_MagazineInteractionSyntheticBoltBackSample,
                vr->m_MagazineInteractionSyntheticBoltBackStarted, label);
    }

    void SendDualReloadRequest(Game* game, const l4d2vr_dual_reload::Request& request, bool retry)
    {
        if (!game) return;
        char command[160]{};
        if (!l4d2vr_dual_reload::FormatRequest(request, command, sizeof(command))) return;
        game->ServerCmd(command, true);
        Game::logMsg("[VR][DualReload][client] %s seq=%u clip=%d reserve=%d command=%u",
            retry ? "retry" : "requested", request.sequence, request.clip, request.reserve, request.command);
    }
}

bool VR::DualReloadHostTransactionAvailable() const
{
    if (!ServerFeature(l4d2vr_features::DualMagTxn) || !RemoteMagazineProtocolSupported()) return false;
    std::lock_guard<std::mutex> lock(m_DualReloadMutex);
    return m_DualReloadRequest.Supported();
}

bool VR::DualReloadUsesPhysicalPath() const
{
    return m_DualPistolsPhysicalReloadEnabled && m_MagazineInteractionEnabled && m_DualPistolsIndependentHandsEnabled &&
        (m_VrHandsEnabled || m_NativeViewmodelHandsOnly) &&
        (m_DualPistolsNativeReloadFallbackEnabled || DualReloadHostTransactionAvailable());
}

void VR::ReceiveDualReloadReply(const l4d2vr_dual_reload::Reply& reply)
{
    std::lock_guard<std::mutex> lock(m_DualReloadMutex);
    m_DualReloadRequest.Receive(reply);
}

bool VR::GetDualReloadRenderState(DualReloadRenderState& out) const
{
    std::lock_guard<std::mutex> lock(m_DualReloadMutex);
    out = m_DualReloadRender;
    return out.enabled;
}

void VR::PublishDualReloadGeometry(const DualReloadGeometry& geometry)
{
    std::lock_guard<std::mutex> lock(m_DualReloadMutex);
    m_DualReloadGeometry = geometry;
}

void VR::UpdateDualPistolReload(C_BasePlayer* player, bool dualPistolsActive, const DualReloadButtons& buttons)
{
    namespace dr = l4d2vr_dual_reload;
    const uint64_t now = GetTickCount64();
    const float scale = (std::max)(0.001f, m_VRScale);
    C_WeaponCSBase* weapon = player ? reinterpret_cast<C_WeaponCSBase*>(player->GetActiveWeapon()) : nullptr;
    const uintptr_t owner = reinterpret_cast<uintptr_t>(player);
    uint32_t handle = 0u;
    int clip = -1, reserve = -1;
    const bool hostTxn = DualReloadHostTransactionAvailable();
    const bool physicalPath = DualReloadUsesPhysicalPath();
    if (dualPistolsActive && physicalPath && weapon && m_Game && m_Game->IsDualPistolWeapon(weapon))
    {
        handle = MagazineInteractionActiveWeaponHandle(m_Game, player, weapon);
        int ammoType = -1;
        MagazineInteractionTryReadValue(weapon, kClip1Offset, clip);
        if (MagazineInteractionTryReadValue(weapon, kPrimaryAmmoTypeOffset, ammoType) && ammoType >= 0 && ammoType < 32)
            MagazineInteractionTryReadValue(player, kAmmoArrayOffset + ammoType * static_cast<int>(sizeof(int)), reserve);
    }
    const bool paused = !m_Game || !m_Game->m_EngineClient || m_Game->m_EngineClient->IsPaused() ||
        (m_Game->m_VguiSurface && m_Game->m_VguiSurface->IsCursorVisible() &&
            !l4d2vr_selftest::Get().active.load(std::memory_order_acquire));
    const bool eligible = handle != 0u && l4d2vr_shell::ValidHandle(handle) && clip >= 0 && clip <= dr::kMaxCapacity &&
        m_IsVREnabled && !m_MouseModeEnabled && !m_SuppressPlayerInput && !paused &&
        m_FirstPersonControlReady.load(std::memory_order_acquire) &&
        !m_RenderPlayerIncap.load(std::memory_order_relaxed) &&
        !m_RenderPlayerControlledBySI.load(std::memory_order_relaxed);
    // A new gun (or a new player entity) is a new session: the machine cancels.
    const bool sameSession = owner == m_DualReloadOwner && handle == m_DualReloadHandle;
    if (!sameSession)
    {
        m_DualReloadOwner = owner;
        m_DualReloadHandle = handle;
    }

    dr::Config config{};
    config.stackDistanceM = std::clamp(m_DualPistolsStackDistanceMeters, 0.04f, 0.40f);
    config.stackMaxAngleDeg = std::clamp(m_DualPistolsStackMaxAngleDeg, 5.0f, 90.0f);
    config.pouchReachM = (std::max)(0.08f, m_MagazineInteractionFreshMagazineGrabRangeMeters + kPavlovHandRadiusMeters);

    dr::Input in{};
    in.nowMs = now;
    in.eligible = eligible && sameSession;
    in.release = buttons.release;
    // L4D2's pistols have an infinite reserve; a host with a finite one still
    // computes the real result (vr_dual_reload.h NativeReload).
    in.canReload = true;
    {
        const uint64_t packed = m_PistolAmmoCounts.load(std::memory_order_acquire);
        const int right = static_cast<int>(packed & 0xffffu), left = static_cast<int>((packed >> 16) & 0xffffu);
        if (packed != 0xffffffffffffffffull && right + left == clip) { in.roundsRight = right; in.roundsLeft = left; }
        else dr::Split(clip, in.roundsRight, in.roundsLeft);
    }
    VrHandMatrix4 controller[2]{};
    for (int h = 0; h < 2; ++h)
    {
        auto& hand = in.hands[static_cast<std::size_t>(h)];
        hand.valid = BuildPistolControllerWorld(h == 1, controller[h]);
        if (hand.valid)
        {
            hand.position = DualV3(MagazineInteractionMatrixOrigin(controller[h]), scale);
            const Vector up = MagazineInteractionMatrixAxis(controller[h], 1);
            const Vector back = MagazineInteractionMatrixAxis(controller[h], 2);
            hand.forward = { -back.x, -back.y, -back.z };
            hand.up = { up.x, up.y, up.z };
        }
        hand.grab = buttons.grab[h];
        hand.grip = buttons.grip[h];
    }
    Vector pouch{}, pouchForward{}, pouchRight{};
    in.pouchValid = DualReloadResolvePouch(this, pouch, pouchForward, pouchRight);
    if (in.pouchValid) in.pouch = DualV3(pouch, scale);

    DualReloadGeometry geometry{};
    {
        std::lock_guard<std::mutex> lock(m_DualReloadMutex);
        geometry = m_DualReloadGeometry;
    }
    const bool geometryFresh = geometry.valid && now >= geometry.atMs && now - geometry.atMs <= 400u;
    const int receiving = m_DualReload.Receiving();
    if (geometryFresh && receiving >= 0 && geometry.receiving == receiving && geometry.serial == m_DualReload.Serial())
    {
        const int freeHand = 1 - receiving;
        if (geometry.seatValid && in.hands[0].valid && in.hands[1].valid)
        {
            const Vector c = geometry.seatCenterLocal;
            const Vector seat = DualMatrixPoint(controller[receiving], c);
            const Vector held = DualMatrixPoint(controller[freeHand],
                c + Vector(0.0f, kDualReloadHoldRaiseMeters * scale, 0.0f));
            in.seatValid = true;
            in.seat = DualV3(seat, scale);
            in.held = DualV3(held, scale);
            VrHandMatrix4 a{}, b{};
            if (l4d2vr_ammo_grip::RotationOf(controller[freeHand], a) && l4d2vr_ammo_grip::RotationOf(controller[receiving], b))
                in.holdAngleDeg = l4d2vr_ammo_grip::AngleBetweenDeg(a, b);
        }
        for (int gun = 0; gun < 2; ++gun)
        {
            if (!geometry.slideValid[gun] || !in.hands[static_cast<std::size_t>(receiving)].valid) continue;
            const Vector slide = DualMatrixPoint(controller[receiving], geometry.slideLocal[gun]);
            const Vector back = VrHandMath::Normalize(DualMatrixDirection(controller[receiving], geometry.slideBackLocal[gun]));
            in.slideValid[static_cast<std::size_t>(gun)] = true;
            in.slide[static_cast<std::size_t>(gun)] = DualV3(slide, scale);
            in.slideBack[static_cast<std::size_t>(gun)] = { back.x, back.y, back.z };
        }
    }

    const dr::Stage before = m_DualReload.GetStage();
    unsigned events = m_DualReload.Update(config, in);
    const int freeHand = m_DualReload.FreeHand();
    const int recvHand = m_DualReload.Receiving();
    auto haptic = [&](int gameplayHand, float duration, float frequency, float amplitude)
    {
        if (gameplayHand < 0 || gameplayHand > 1) return;
        TriggerPhysicalHandHapticPulse(IsGameplayHandLeftPhysical(gameplayHand == 1), duration, frequency, amplitude, 2);
    };
    auto both = [&](float duration, float frequency, float amplitude)
    { haptic(0, duration, frequency, amplitude); haptic(1, duration, frequency, amplitude); };

    // ---- Committing the rounds when the magazines seat --------------------------------------------
    auto commitPredicted = [&](const char* why)
    {
        // The stock reload: the host refills the shared clip on its own timer.
        m_DualReloadNativePulse.store(true, std::memory_order_release);
        // A host with the transaction gates the stock reload: open it for this one.
        if (hostTxn) m_DualReloadNativeWindowUntilMs.store(now + 3000u, std::memory_order_release);
        m_DualReloadCommitByHost = false;
        dr::AmmoResult predicted{};
        int right = 15, left = 15;
        if (dr::NativeReload((std::max)(0, clip), (std::max)(0, reserve), true, 30, predicted))
            dr::Split(predicted.clip, right, left);
        events |= m_DualReload.Commit(right, left);
        Game::logMsg("[VR][DualReload] rounds by the stock reload (%s): clip %d reserve %d", why, clip, reserve);
    };
    auto beginHost = [&]() -> bool
    {
        dr::Request request{};
        {
            std::lock_guard<std::mutex> lock(m_DualReloadMutex);
            if (m_CurrentPredictedHitFeedbackCmdNumber <= 0 || clip < 0 || reserve < 0 ||
                !m_DualReloadRequest.Begin(handle, clip, (std::min)(reserve, dr::kMaxReserve),
                    static_cast<uint32_t>(m_CurrentPredictedHitFeedbackCmdNumber), owner, now, request))
                return false;
        }
        ++m_DualReloadCommitAttempts;
        SendDualReloadRequest(m_Game, request, false);
        return true;
    };
    if ((events & dr::kEventSeated) != 0u)
    {
        m_DualReloadCommitAttempts = 0;
        m_DualReloadClipBefore = clip;
        m_DualReloadReserveBefore = reserve;
        both(0.06f, 90.0f, 1.0f);
        DualReloadSound(this, kDualClipInSound, true, "dual clip-in");
        m_DualReloadCommitByHost = hostTxn && beginHost();
        m_DualReloadLastCommitByHost = m_DualReloadCommitByHost;
        if (!m_DualReloadCommitByHost)
            commitPredicted(hostTxn ? "the host transaction could not start" : "no host transaction");
    }
    else if (m_DualReload.GetStage() == dr::Stage::Committing && m_DualReloadCommitByHost)
    {
        dr::Reply reply{};
        dr::Request retry{};
        bool resend = false;
        dr::ClientRequest::Poll poll = dr::ClientRequest::Poll::None;
        {
            std::lock_guard<std::mutex> lock(m_DualReloadMutex);
            poll = m_DualReloadRequest.Update(handle, owner, now, clip, reserve, reply);
            if (poll == dr::ClientRequest::Poll::Waiting) resend = m_DualReloadRequest.NextRetry(now, retry);
        }
        if (resend) SendDualReloadRequest(m_Game, retry, true);
        if (poll == dr::ClientRequest::Poll::Result)
        {
            Game::logMsg("[VR][DualReload] host reply status=%u clip %d -> %d reserve %d -> %d",
                static_cast<unsigned>(reply.status), reply.request.clip, reply.clip, reply.request.reserve, reply.reserve);
            int right = 0, left = 0;
            if (reply.status == dr::Status::Applied)
            {
                dr::Split(reply.clip, right, left);
                events |= m_DualReload.Commit(right, left);
            }
            else if (reply.status == dr::Status::NoSpaceOrAmmo)
            {
                dr::Split(clip, right, left);   // full already, or nothing in the reserve
                events |= m_DualReload.Commit(right, left);
            }
            else if ((reply.status == dr::Status::Stale || reply.status == dr::Status::RateLimited) &&
                m_DualReloadCommitAttempts < 3 && beginHost())
            {
            }
            else commitPredicted("the host refused the transaction");
        }
        else if (poll == dr::ClientRequest::Poll::Timeout)
            commitPredicted("the host transaction timed out");
        else if (poll == dr::ClientRequest::Poll::None)
            commitPredicted("the host transaction was cancelled");
    }
    if (m_DualReload.GetStage() != dr::Stage::Committing && before == dr::Stage::Committing && m_DualReloadCommitByHost)
    {
        std::lock_guard<std::mutex> lock(m_DualReloadMutex);
        if ((events & dr::kEventCancelled) != 0u) m_DualReloadRequest.Cancel();
    }

    // ---- Feedback ---------------------------------------------------------------------------------
    if ((events & dr::kEventEjected) != 0u)
    {
        both(0.03f, 160.0f, 0.5f);
        DualReloadSound(this, kDualClipOutSound, false, "dual clip-out");
        if (geometry.valid && geometry.clipWorldValid && now - geometry.atMs <= 400u)
            for (int gun = 0; gun < 2; ++gun)
                SpawnDroppedMagazine(geometry.model, geometry.clipBone[gun], geometry.clipWorld[gun], gun == 1);
    }
    if ((events & dr::kEventContact) != 0u) both(0.015f, 120.0f, 0.25f);
    if ((events & dr::kEventStacked) != 0u) both(0.05f, 140.0f, 0.8f);
    if ((events & dr::kEventUnstacked) != 0u) both(0.04f, 120.0f, 0.5f);
    if ((events & dr::kEventGrabbedMags) != 0u) haptic(freeHand, 0.04f, 150.0f, 0.6f);
    if ((events & dr::kEventDroppedMags) != 0u)
    {
        haptic(freeHand, 0.02f, 120.0f, 0.3f);
        if (geometryFresh && geometry.seatValid && freeHand >= 0 && in.hands[static_cast<std::size_t>(freeHand)].valid)
        {
            VrHandMatrix4 raised = controller[freeHand];
            const Vector origin = DualMatrixPoint(controller[freeHand], Vector(0.0f, kDualReloadHoldRaiseMeters * scale, 0.0f));
            VrHandMath::Set(raised, 0, 3, origin.x); VrHandMath::Set(raised, 1, 3, origin.y); VrHandMath::Set(raised, 2, 3, origin.z);
            for (int gun = 0; gun < 2; ++gun)
                SpawnDroppedMagazine(geometry.model, geometry.clipBone[gun],
                    VrHandMath::Multiply(raised, geometry.clipSeatLocal[gun]), freeHand == 1);
        }
    }
    if ((events & dr::kEventSlideGrabbed) != 0u) haptic(freeHand, 0.02f, 200.0f, 0.4f);
    if ((events & dr::kEventSlideRacked) != 0u)
    {
        haptic(freeHand, 0.04f, 220.0f, 0.8f);
        haptic(recvHand, 0.03f, 180.0f, 0.5f);
        DualReloadSound(this, kDualSlideBackSound, false, "dual slide-back");
    }
    if ((events & dr::kEventSlideReleased) != 0u) DualReloadSound(this, kDualSlideForwardSound, true, "dual slide-forward");
    if ((events & dr::kEventDone) != 0u) both(0.04f, 140.0f, 0.6f);

    for (int bit = 0; bit < 14; ++bit)
        if ((events & (1u << bit)) != 0u) m_DualReloadEventCount[bit].fetch_add(1u, std::memory_order_acq_rel);
    const dr::Stage stage = m_DualReload.GetStage();
    if (events != 0u || stage != before)
    {
        const float gap = in.hands[0].valid && in.hands[1].valid ? dr::Distance(in.hands[0].position, in.hands[1].position) : -1.0f;
        Game::logMsg("[VR][DualReload] %s -> %s events=0x%X receiving=%d free=%d gap=%.3fm speed=%.2f/%.2f clip=%d reserve=%d "
            "chambered=%d/%d rack=%d/%d host=%d",
            dr::StageName(before), dr::StageName(stage), events, m_DualReload.Receiving(), m_DualReload.FreeHand(), gap,
            m_DualReload.Speed(0), m_DualReload.Speed(1), clip, reserve,
            m_DualReload.ChamberedBefore(0) ? 1 : 0, m_DualReload.ChamberedBefore(1) ? 1 : 0,
            m_DualReload.NeedsRack(0) ? 1 : 0, m_DualReload.NeedsRack(1) ? 1 : 0, hostTxn ? 1 : 0);
    }
    m_DualReloadLastClip = clip;
    m_DualReloadLastReserve = reserve;

    // ---- Publication ------------------------------------------------------------------------------
    const bool enabled = physicalPath && handle != 0u && dualPistolsActive;
    m_DualReloadActive.store(m_DualReload.Active(), std::memory_order_release);
    m_DualReloadHostOwned.store(enabled && hostTxn && now >= m_DualReloadNativeWindowUntilMs.load(std::memory_order_acquire),
        std::memory_order_release);
    for (int h = 0; h < 2; ++h)
        m_DualReloadHandRole[h].store(static_cast<int>(m_DualReload.Role(h)), std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(m_DualReloadMutex);
        DualReloadRenderState& render = m_DualReloadRender;
        render.enabled = enabled;
        render.active = m_DualReload.Active();
        render.stage = static_cast<unsigned>(stage);
        render.receiving = m_DualReload.Receiving();
        render.serial = m_DualReload.Serial();
        render.hideClips = m_DualReload.HideMagazines();
        render.drawHeld = m_DualReload.HoldingMagazines();
        render.drawPouch = enabled && (stage == dr::Stage::Idle || stage == dr::Stage::Ejected || stage == dr::Stage::Stacked);
        render.pouchValid = in.pouchValid;
        render.pouch = pouch; render.pouchForward = pouchForward; render.pouchRight = pouchRight;
        for (int gun = 0; gun < 2; ++gun)
            render.slidePullUnits[gun] = m_DualReload.SlideHeld() == gun ? m_DualReload.SlidePullM() * scale : 0.0f;
        render.holdRaiseUnits = kDualReloadHoldRaiseMeters * scale;
    }
}

bool VR::GetDualReloadSelfTestPoints(DualReloadSelfTestPoints& out)
{
    namespace dr = l4d2vr_dual_reload;
    out = {};
    const float scale = (std::max)(0.001f, m_VRScale);
    const uint64_t now = GetTickCount64();
    DualReloadRenderState render{};
    DualReloadGeometry geometry{};
    {
        std::lock_guard<std::mutex> lock(m_DualReloadMutex);
        render = m_DualReloadRender;
        geometry = m_DualReloadGeometry;
    }
    out.enabled = render.enabled;
    out.hideClips = render.hideClips;
    out.hostTxn = DualReloadHostTransactionAvailable();
    out.lastCommitByHost = m_DualReloadLastCommitByHost;
    out.geometryFresh = geometry.valid && now >= geometry.atMs && now - geometry.atMs <= 400u;
    out.stage = static_cast<unsigned>(m_DualReload.GetStage());
    out.receiving = m_DualReload.Receiving();
    out.freeHand = m_DualReload.FreeHand();
    out.slideHeld = m_DualReload.SlideHeld();
    out.slidePullM = m_DualReload.SlidePullM();
    out.needsRack[0] = m_DualReload.NeedsRack(0);
    out.needsRack[1] = m_DualReload.NeedsRack(1);
    out.clip = m_DualReloadLastClip;
    out.reserve = m_DualReloadLastReserve;
    VrHandMatrix4 controller[2]{};
    const bool tracked[2] = { BuildPistolControllerWorld(false, controller[0]), BuildPistolControllerWorld(true, controller[1]) };
    const Vector scene[2] = { m_RightControllerPosAbs, m_LeftControllerPosAbs };
    // The free hand (or, before stacking, the left hand) reaches the points.
    const int reacher = out.freeHand >= 0 ? out.freeHand : 1;
    if (!tracked[reacher]) return true;
    const Vector reacherDrawn = MagazineInteractionMatrixOrigin(controller[reacher]);
    auto targetFor = [&](const Vector& drawnPoint) { return scene[reacher] + (drawnPoint - reacherDrawn); };
    if (render.pouchValid)
    {
        out.pouchValid = true;
        out.pouchTarget = targetFor(render.pouch);
        out.pouchDistanceM = (render.pouch - reacherDrawn).Length() / scale;
    }
    const int r = out.receiving;
    if (r < 0 || !tracked[r] || !out.geometryFresh || geometry.receiving != r) return true;
    if (geometry.seatValid)
    {
        const Vector c = geometry.seatCenterLocal;
        const Vector seat = DualMatrixPoint(controller[r], c);
        const Vector held = DualMatrixPoint(controller[reacher], c + Vector(0.0f, kDualReloadHoldRaiseMeters * scale, 0.0f));
        out.seatValid = true;
        out.freeSeatTarget = scene[reacher] + (seat - held);
        out.seatDistanceM = (seat - held).Length() / scale;
        VrHandMatrix4 a{}, b{};
        if (l4d2vr_ammo_grip::RotationOf(controller[reacher], a) && l4d2vr_ammo_grip::RotationOf(controller[r], b))
            out.holdAngleDeg = l4d2vr_ammo_grip::AngleBetweenDeg(a, b);
    }
    for (int gun = 0; gun < 2; ++gun)
    {
        if (!geometry.slideValid[gun]) continue;
        const Vector slide = DualMatrixPoint(controller[r], geometry.slideLocal[gun]);
        out.slideValid[gun] = true;
        out.slideTarget[gun] = targetFor(slide);
        out.slideBack[gun] = VrHandMath::Normalize(DualMatrixDirection(controller[r], geometry.slideBackLocal[gun]));
        out.slideDistanceM[gun] = (slide - reacherDrawn).Length() / scale;
    }
    return true;
}

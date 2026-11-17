// Replaces the command's view angles and re-expresses forwardmove/sidemove in
// the new yaw, so the stick keeps its world direction (B44). The angles are
// normalized, pitch clamped and roll cleared like every other override.
static void SetCmdViewAnglesKeepMove(CUserCmd* cmd, QAngle angles)
{
	NormalizeAndClampViewAngles(angles);
	angles.z = 0.0f;
	QAngle oldYawOnly(0.f, cmd->viewangles.y, 0.f);
	Vector oldForward, oldRight, oldUp;
	QAngle::AngleVectors(oldYawOnly, &oldForward, &oldRight, &oldUp);
	const Vector worldMove = oldForward * cmd->forwardmove + oldRight * cmd->sidemove;
	cmd->viewangles = angles;
	QAngle newYawOnly(0.f, cmd->viewangles.y, 0.f);
	Vector newForward, newRight, newUp;
	QAngle::AngleVectors(newYawOnly, &newForward, &newRight, &newUp);
	cmd->forwardmove = DotProduct(worldMove, newForward);
	cmd->sidemove = DotProduct(worldMove, newRight);
}

static bool IsClientAttachedToLadder(Game* game, C_BasePlayer* player)
{
    if (!game || !player) return false;
    const int offset = game->FindRecvPropOffset("DT_BaseEntity", "movetype");
    if (offset <= 0) return false;
#ifdef _MSC_VER
    __try { return ReadNetvar<uint8_t>(player, offset) == 9u; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
#else
    return ReadNetvar<uint8_t>(player, offset) == 9u;
#endif
}

bool __fastcall Hooks::dCreateMove(void* ecx, void* edx, float flInputSampleTime, CUserCmd* cmd)
{
	l4d2vr_frame_perf::Scope perfCreateMoveScope(l4d2vr_frame_perf::kCreateMoveUs);
	// 2.0.11: server commands queued by other threads go out from here.
	if (m_Game) m_Game->FlushDeferredServerCmds(true);
	// No config hot reload while this command reads the settings (review B05).
	l4d2vr_config_lock::ConfigLock::MaybeReadScope configRead(m_VR ? &m_VR->m_ConfigLock : nullptr);
	// Desktop mode: put back game settings a VR session changed (review B37).
	if (m_VR && !m_VR->m_IsVREnabled) m_VR->RestoreGameCvarsAfterVrOnce();
	// Opus voice chat runs every client frame, VR or desktop (before any early return).
	if (m_Game) m_Game->UpdateOpusVoice();
	// When returning from spectator/observer back to a live player entity ("rescued"),
	// VR origin can be desynced. Force a recenter/reset once on that transition.
	// Netvars (client):
	// - m_lifeState     @ 0x147
	// - m_iObserverMode @ 0x1450
	static bool s_prevSpectatorLike = false;
	static bool s_prevSpectatorLikeInitialized = false;

	// Non-VR server melee feel state (ForceNonVRServerMovement=true only)
	static l4d2vr_grip::ReleaseLatch s_weaponGripRelease;
	// Pavlov-style grenade hold / pin / release (servers with HandThrow).
	static l4d2vr_hand_throw::ReleaseFsm s_handThrowFsm;
    static l4d2vr_physical::PickupIntent s_gripPickup;
    static l4d2vr_physical::PickupIntent s_holsterDrawPickup;
    static uint32_t s_gripDrawSerial = 0u;
    static uintptr_t s_previousGripWeapon = 0u;
    static uintptr_t s_pendingGripAdoption = 0u;
    static uint64_t s_chestSlingAdoptUntilMs = 0u;   // a hand took the gun off the sling: adopt it once its grip is down
    static l4d2vr_grip::DropReturn s_gripDropReturn;
    static l4d2vr_dual::TriggerRouter s_dualPistolTriggers;
    static l4d2vr_pistol::Ownership s_pistolOwnership;
    static l4d2vr_physical::PickupIntent s_leftPistolPickup;
    static l4d2vr_physical::ReservedGrip s_rightGripReservation;
    static l4d2vr_physical::ReservedGrip s_leftGripReservation;
    static uint32_t s_gripExtraSamples = 0u;
    l4d2vr_dual::Hand pistolInteractionHand = l4d2vr_dual::Hand::None;
	static int s_nonvrMeleeHoldTicks = 0;
	static bool s_nonvrMeleeArmed = true;
	static QAngle s_nonvrMeleeLockedAngles = { 0,0,0 };
	static std::chrono::steady_clock::time_point s_nonvrMeleeLockUntil{};
	static std::chrono::steady_clock::time_point s_nonvrMeleeCooldownUntil{};
	static bool s_nonvrMeleePending = false;
	static std::chrono::steady_clock::time_point s_nonvrMeleeFireAt{};
	static QAngle s_nonvrMeleePendingAngles = { 0,0,0 };
	static int s_nonvrMeleePendingHoldTicks = 0;
	static bool s_nonvrMeleeHasPrev = false;
	static Vector s_nonvrMeleePrevCtrlPos = { 0,0,0 };
	static Vector s_nonvrMeleePrevHmdPos = { 0,0,0 };

	// Third-person melee: lock cmd->viewangles toward rendered reticle convergence (VR-aware servers)
	static bool s_tpMeleePrevAttackDown = false;
	static std::chrono::steady_clock::time_point s_tpMeleeLockUntil{};
	static QAngle s_tpMeleeLockedAngles = { 0,0,0 };
	static uintptr_t s_vrAwareThrowableAimWeapon = 0;
	static bool s_vrAwareThrowableAimPrevAttackDown = false;
	static uintptr_t s_manualCarryThrowWeapon = 0;
	static bool s_manualCarryThrowPhysicalAttackDown = false;
	static bool s_manualInventoryThrowPhysicalUseDown = false;
	static bool s_manualInventoryThrowArmed = false;
	static l4d2vr_input::ReleaseInputGate s_manualThrowInputGate;
	static bool s_vrAwareThrowableAimPrevWeaponThrowable = false;
	static int s_vrAwareThrowableAimTicks = 0;
	static int s_lastButtons = 0;

	auto updateFirstPersonBodyLocalState = [&](bool hasRealUserCmd)
		{
			if (m_Game)
				m_Game->RefreshVRPosePlayerIdentitiesMainThread();
			C_BasePlayer* localPlayer = nullptr;
			if (m_Game && m_Game->m_EngineClient)
			{
				const int localIndex = m_Game->m_EngineClient->GetLocalPlayer();
				if (localIndex > 0)
				{
					localPlayer = reinterpret_cast<C_BasePlayer*>(
						m_Game->GetClientEntity(localIndex));
				}
			}
			if (m_VR)
				m_VR->UpdateObjectPullClientTargetMainThread(localPlayer);
			HooksFirstPersonBodyUpdateLocalStateMainThread(localPlayer);
			HooksUpdateFirstPersonControlReadyMainThread(
				m_VR,
				m_Game,
				localPlayer,
				hasRealUserCmd);
			HooksWorldPoseEnsureWeaponSetupBonesHookMainThread(
				m_VR,
				m_Game);
			HooksHolsterDrawDiagnosticsMainThread(
				m_VR,
				m_Game);
		};

    auto synchronizeGripSession = [&]()
    {
        const auto session = ReadGripGameplaySession(m_VR, m_Game);
        s_weaponGripRelease.ObserveSession(session.gameplay, session.weapon);
        if (!session.gameplay)
        {
            if (m_VR) m_VR->CancelMeleeMotion();
            s_gripPickup.Reset(); s_holsterDrawPickup.Reset(); s_leftPistolPickup.Reset();
            s_rightGripReservation.Reset(); s_leftGripReservation.Reset();
            s_pendingGripAdoption = 0u;
            s_gripDropReturn.Reset();
            s_pistolOwnership.CancelInteractions();
        }
        if (!session.liveInventory) s_pistolOwnership.Reset();
        return session;
    };
	// 2.0.13: a gameplay mode change (Physical <-> Classic) drops every
	// in-flight physical input state of this function.
	static uint32_t s_gameplayModeGeneration = 0u;
	if (m_VR)
	{
		const uint32_t generation = m_VR->m_GameplayModeGeneration.load(std::memory_order_acquire);
		if (generation != s_gameplayModeGeneration)
		{
			s_gameplayModeGeneration = generation;
			s_weaponGripRelease.Reset();
			s_handThrowFsm = l4d2vr_hand_throw::ReleaseFsm{};
			s_gripPickup.Reset(); s_holsterDrawPickup.Reset(); s_leftPistolPickup.Reset();
			s_rightGripReservation.Reset(); s_leftGripReservation.Reset();
			s_pendingGripAdoption = 0u;
			s_chestSlingAdoptUntilMs = 0u;
			s_gripDropReturn.Reset();
			s_dualPistolTriggers.Reset();
			s_pistolOwnership.Reset();
			s_nonvrMeleeHoldTicks = 0;
			s_nonvrMeleeArmed = true;
			s_nonvrMeleeLockUntil = {};
			s_nonvrMeleeCooldownUntil = {};
			s_nonvrMeleePending = false;
			s_nonvrMeleeFireAt = {};
			s_nonvrMeleePendingHoldTicks = 0;
			s_nonvrMeleeHasPrev = false;
			s_manualCarryThrowWeapon = 0;
			s_manualCarryThrowPhysicalAttackDown = false;
			s_manualInventoryThrowPhysicalUseDown = false;
			s_manualInventoryThrowArmed = false;
			s_manualThrowInputGate.Reset();
			m_VR->m_ManualThrowViewmodelInputState.store(0u, std::memory_order_release);
		}
	}
	if (!cmd || !cmd->command_number)
	{
		const bool result = hkCreateMove.fOriginal(ecx, flInputSampleTime, cmd);
		updateFirstPersonBodyLocalState(false);
        // ExtraMouseSample invokes ClientMode::CreateMove with a dummy command.
        // That camera update is not a release or a gameplay lifecycle change.
        ++s_gripExtraSamples;
        synchronizeGripSession();
		return result;
	}

	// Keep the latest real CUserCmd number latched beyond this function call.
	// On remote servers, ClientFireTerrorBullets can be reached by prediction replay
	// outside the immediate CreateMove call stack; restoring the value here makes
	// those replays look like fresh shots and causes repeated hit sounds.
	if (m_VR)
		m_VR->m_CurrentPredictedHitFeedbackCmdNumber = cmd->command_number;

	bool result = hkCreateMove.fOriginal(ecx, flInputSampleTime, cmd);

	// Keep lifecycle cleanup running even when Source emits command_number==0
	// during loading, pause, disconnect, or menu transitions (handled above).
	updateFirstPersonBodyLocalState(true);
    const auto gripSession = synchronizeGripSession();

	// Server-hook roomscale movement is applied after the originating CreateMove.
	// Consume its accepted-vs-visual correction before this command uses cached VR poses.
	if (m_VR)
		m_VR->ApplyPendingRoomscale1To1ServerVisualCorrection();

	constexpr int kAutoActionIN_USE = (1 << 5);
	const bool localUseButtonDownForAutoActions = (cmd->buttons & kAutoActionIN_USE) != 0;
	C_BasePlayer* localPlayerForAutoActions = nullptr;
	if (m_Game && m_Game->m_EngineClient)
	{
		const int lpIdx = m_Game->m_EngineClient->GetLocalPlayer();
		localPlayerForAutoActions = (lpIdx > 0) ? (C_BasePlayer*)m_Game->GetClientEntity(lpIdx) : nullptr;
	}
    // 2.0.12: no hands / no control watchdog (names the stuck gate, recovers).
    if (m_VR)
        m_VR->UpdateControlWatchdog(localPlayerForAutoActions,
            gripSession.blocked | (IsPlayerDoingUseOrReviveAction(localPlayerForAutoActions) ? 32u : 0u));

	if (m_VR && m_VR->m_TeleportVisualScoutActive)
	{
		// Detached scout view is local-only. Return before VR aim, melee helpers,
		// auto-fire and roomscale code can mutate the authoritative body command.
		// Keep command timing intact, but freeze body orientation and strip actions.
		if (m_VR->m_TeleportVisualScoutBodyViewAnglesValid)
			cmd->viewangles = m_VR->m_TeleportVisualScoutBodyViewAngles;
		cmd->forwardmove = 0.0f;
		cmd->sidemove = 0.0f;
		cmd->upmove = 0.0f;
		cmd->buttons = 0;
		cmd->impulse = 0;
		cmd->weaponselect = 0;
		cmd->weaponsubtype = 0;
		cmd->mousedx = 0;
		cmd->mousedy = 0;
		m_VR->m_PushingThumbstick = false;
		m_VR->m_LocomotionActive = false;

		// Drop transient attack helpers so returning to the body cannot replay a
		// queued melee swing, auto-repeat pulse or effective-range cycle.
		s_weaponGripRelease.Reset();
		s_nonvrMeleeHoldTicks = 0;
		s_nonvrMeleeArmed = true;
		s_nonvrMeleeLockUntil = {};
		s_nonvrMeleeCooldownUntil = {};
		s_nonvrMeleePending = false;
		s_nonvrMeleeFireAt = {};
		s_nonvrMeleePendingHoldTicks = 0;
		s_nonvrMeleeHasPrev = false;
		s_tpMeleePrevAttackDown = false;
		s_tpMeleeLockUntil = {};
		s_manualCarryThrowWeapon = 0;
		s_manualCarryThrowPhysicalAttackDown = false;
		s_manualInventoryThrowPhysicalUseDown = false;
		s_manualInventoryThrowArmed = false;
		s_manualThrowInputGate.Reset();
		m_VR->m_ManualThrowViewmodelInputState.store(0u, std::memory_order_release);
		return result;
	}

	if (m_VR->m_IsVREnabled) {
		// Detect observer -> live transition and recenter once.
	   // In L4D2, the local player entity persists while dead and enters observer modes.
	   // When rescued, m_iObserverMode usually returns to 0 and m_lifeState becomes 0.
		{
			const int lpIdx = (m_Game->m_EngineClient) ? m_Game->m_EngineClient->GetLocalPlayer() : -1;
			C_BasePlayer* lp = (lpIdx > 0) ? (C_BasePlayer*)m_Game->GetClientEntity(lpIdx) : nullptr;
			if (lp)
			{
				const int lifeState = (int)ReadNetvar<uint8_t>(lp, 0x147); // m_lifeState
				const int observerMode = (int)ReadNetvar<int>(lp, 0x1450); // m_iObserverMode
				const bool spectatorLikeNow = (lifeState != 0) || (observerMode != 0);

				if (s_prevSpectatorLikeInitialized)
				{
					// Transition: was spectator-like -> now fully alive (not observer).
					if (s_prevSpectatorLike && !spectatorLikeNow)
					{
						// Note: ResetPosition is expected to exist on VR (used by the input action).
						m_VR->ResetPosition();
					}
				}

				s_prevSpectatorLike = spectatorLikeNow;
				s_prevSpectatorLikeInitialized = true;
			}
			else
			{
				// No local player entity yet; don't latch state.
				s_prevSpectatorLikeInitialized = false;
			}
		}
		const bool treatServerAsNonVR = m_VR->m_ForceNonVRServerMovement;

		// Mouse mode: consume raw mouse deltas to drive body yaw and independent aim pitch.
		// Mouse X -> yaw (m_RotationOffset), Mouse Y -> m_MouseAimPitchOffset.
		// We zero cmd->mousedx/y so Source doesn't also apply them to viewangles.
		if (m_VR->m_MouseModeEnabled)
		{
			// Mouse mode hotkeys (keyboard). Polled here because CreateMove runs at input tick rate.
			auto pollKeyPressed = [&](const std::optional<WORD>& vk, bool& prevDown) -> bool
				{
					if (!vk.has_value())
					{
						prevDown = false;
						return false;
					}
					const SHORT state = GetAsyncKeyState((int)*vk);
					const bool down = (state & 0x8000) != 0;
					const bool pressed = down && !prevDown;
					prevDown = down;
					return pressed;
				};

			if (pollKeyPressed(m_VR->m_MouseModeScopeToggleKey, m_VR->m_MouseModeScopeToggleKeyDownPrev))
				m_VR->ToggleMouseModeScope();

			const float mouseScopeGain = m_VR->GetMouseModeScopeSensitivityScale();

			const int dx = cmd->mousedx;
			const int dy = cmd->mousedy;

			// Mouse-mode yaw (scheme A): delta-drain smoothing.
			// - If MouseModeTurnSmoothing <= 0: legacy behavior (apply yaw directly on CreateMove ticks).
			// - If MouseModeTurnSmoothing  > 0: convert mouse X to a yaw delta and accumulate it.
			//   VR::UpdateTracking will drain/apply it smoothly per-frame.
			if (m_VR->m_MouseModeTurnSmoothing <= 0.0f)
			{
				if (dx != 0)
				{
					m_VR->m_RotationOffset += (-float(dx) * m_VR->m_MouseModeYawSensitivity) * mouseScopeGain;
					// Wrap to [0, 360)
					m_VR->m_RotationOffset -= 360.0f * std::floor(m_VR->m_RotationOffset / 360.0f);
				}
				m_VR->m_MouseModeYawDeltaRemainingDeg = 0.0f;
				m_VR->m_MouseModeYawDeltaInitialized = false;
			}
			else
			{
				if (!m_VR->m_MouseModeYawDeltaInitialized)
				{
					m_VR->m_MouseModeYawDeltaRemainingDeg = 0.0f;
					m_VR->m_MouseModeYawDeltaInitialized = true;
				}
				if (dx != 0)
				{
					const float yawDeltaDeg = (-float(dx) * m_VR->m_MouseModeYawSensitivity) * mouseScopeGain;
					m_VR->m_MouseModeYawDeltaRemainingDeg += yawDeltaDeg;
				}
			}

			if (!m_VR->m_MouseAimInitialized)
			{
				// Initialize current values (used immediately) and targets (smoothed per-frame in VR::UpdateTracking).
				m_VR->m_MouseAimPitchOffset = m_VR->m_HmdAngAbs.x;
				m_VR->m_MouseModeViewPitchOffset = 0.0f;
				m_VR->m_MouseAimInitialized = true;

				m_VR->m_MouseModePitchTarget = m_VR->m_MouseAimPitchOffset;
				m_VR->m_MouseModePitchTargetInitialized = true;
				m_VR->m_MouseModeViewPitchTargetOffset = m_VR->m_MouseModeViewPitchOffset;
				m_VR->m_MouseModeViewPitchTargetOffsetInitialized = true;
			}

			// Ensure targets are initialized even if MouseAimInitialized was set elsewhere.
			if (!m_VR->m_MouseModePitchTargetInitialized)
			{
				m_VR->m_MouseModePitchTarget = m_VR->m_MouseAimPitchOffset;
				m_VR->m_MouseModePitchTargetInitialized = true;
			}
			if (!m_VR->m_MouseModeViewPitchTargetOffsetInitialized)
			{
				m_VR->m_MouseModeViewPitchTargetOffset = m_VR->m_MouseModeViewPitchOffset;
				m_VR->m_MouseModeViewPitchTargetOffsetInitialized = true;
			}

			if (dy != 0)
			{
				const float deltaPitch = float(dy) * m_VR->m_MouseModePitchSensitivity * mouseScopeGain;

				if (m_VR->m_MouseModePitchAffectsView)
				{
					// Update targets only. VR::UpdateTracking will smooth the current values per-frame.
					const float curViewPitch = m_VR->m_MouseModePitchTarget;
					float newViewPitch = curViewPitch + deltaPitch;
					if (newViewPitch > 89.f)  newViewPitch = 89.f;
					if (newViewPitch < -89.f) newViewPitch = -89.f;

					const float appliedDelta = newViewPitch - curViewPitch;
					m_VR->m_MouseModeViewPitchTargetOffset += appliedDelta;
					m_VR->m_MouseModePitchTarget = newViewPitch;

					// If pitch smoothing is disabled, keep legacy immediate behavior.
					if (m_VR->m_MouseModePitchSmoothing <= 0.0f)
					{
						m_VR->m_MouseModeViewPitchOffset = m_VR->m_MouseModeViewPitchTargetOffset;
						m_VR->m_MouseAimPitchOffset = m_VR->m_MouseModePitchTarget;
					}
				}
				else
				{
					// Aim pitch only (camera remains driven purely by HMD).
					m_VR->m_MouseModePitchTarget += deltaPitch;
					if (m_VR->m_MouseModePitchTarget > 89.f)  m_VR->m_MouseModePitchTarget = 89.f;
					if (m_VR->m_MouseModePitchTarget < -89.f) m_VR->m_MouseModePitchTarget = -89.f;

					if (m_VR->m_MouseModePitchSmoothing <= 0.0f)
					{
						m_VR->m_MouseAimPitchOffset = m_VR->m_MouseModePitchTarget;
					}
				}
			}

			cmd->mousedx = 0;
			cmd->mousedy = 0;
		}
		const QAngle originalViewAngles = cmd->viewangles;
		const bool suppressScopeWalk = m_VR->m_ScopeFovAdjustSuppressWalk || m_VR->m_TeleportTargetingActive;
		const float originalForwardMove = suppressScopeWalk ? 0.0f : cmd->forwardmove;
		const float originalSideMove = suppressScopeWalk ? 0.0f : cmd->sidemove;
		if (suppressScopeWalk)
		{
			cmd->forwardmove = 0.0f;
			cmd->sidemove = 0.0f;
			cmd->buttons &= ~((1 << 3) | (1 << 4) | (1 << 9) | (1 << 10));
		}
		bool hadWalkAxis = false;
		float walkNx = 0.f, walkNy = 0.f; 
		float walkMaxSpeed = 0.f;
		float ax = 0.f, ay = 0.f;
		if (!m_VR->m_AdjustingViewmodel && !m_VR->m_AdjustingScope && !suppressScopeWalk && m_VR->GetWalkAxisForMove(ax, ay)) {
			// Deadzone + normalization. In viewmodel adjustment mode this axis is reserved for rotation.
			const float dz = 0.2f;
			auto norm = [&](float v) {
				float a = fabsf(v);
				if (a <= dz) return 0.f;
				float t = (a - dz) / (1.f - dz);
				return v < 0 ? -t : t;
				};
			const float nx = norm(ax);
			const float ny = norm(ay);
			float moveNx = nx;
			float moveNy = ny;

			// Third-person front-view mode: keep left/right as-is.
			// Forward/back follows current camera-aligned basis (no extra sign flip).

			// Safe command speed; the server will still apply its own movement limits.
			const float maxSpeed = 250.f;
			hadWalkAxis = true;
			walkNx = moveNx;
			walkNy = moveNy;
			walkMaxSpeed = maxSpeed;

			// Track thumbstick locomotion state (used to disable 1:1 roomscale when requested).
			m_VR->m_PushingThumbstick = (fabsf(moveNx) > 0.0001f) || (fabsf(moveNy) > 0.0001f);

			// Stick locomotion is applied later once the final cmd->viewangles basis is known.
			// That keeps keyboard movement and stick movement in the same basis.

			// Also set direction button bits for compatibility.
			// IN_FORWARD=1<<3, IN_BACK=1<<4, IN_MOVELEFT=1<<9, IN_MOVERIGHT=1<<10
			if (moveNy > 0.5f)      cmd->buttons |= (1 << 3);
			else if (moveNy < -0.5f)cmd->buttons |= (1 << 4);
			if (moveNx > 0.5f)      cmd->buttons |= (1 << 10);
			else if (moveNx < -0.5f)cmd->buttons |= (1 << 9);

		}
		else
		{
			m_VR->m_PushingThumbstick = false;
		}

		// ② ★ 非 VR 服务器：把“右手手柄朝向”塞给服务器用的视角
		if (treatServerAsNonVR) {
			// Freshly solve on the input tick. The server consumes this cmd immediately,
			// so using an older render/aim-line solution can miss at close range.
			if (m_Game && m_Game->m_EngineClient)
			{
				const int lpIdxForAim = m_Game->m_EngineClient->GetLocalPlayer();
				C_BasePlayer* lpForAim = (lpIdxForAim > 0) ? (C_BasePlayer*)m_Game->GetClientEntity(lpIdxForAim) : nullptr;
				if (lpForAim)
					m_VR->UpdateNonVRAimSolution(lpForAim, true, true);
			}

			QAngle aim = m_VR->GetBulletAimAbsAngle();
			if (m_VR->m_MouseModeEnabled)
			{
				if (m_VR->m_HasNonVRAimSolution)
					aim = m_VR->m_NonVRAimAngles;
				else
				{
					if (m_VR->m_MouseModeAimFromHmd)
					{
						Vector v = m_VR->GetViewAngle();
						aim = QAngle(v.x, v.y, 0.0f);
					}
					else
					{
						aim = QAngle(m_VR->m_MouseAimPitchOffset, m_VR->m_RotationOffset, 0.0f);
					}
				}
			}
			// ForceNonVRServerMovement: prefer the eye-based solve (what the server will actually trace).
			if (m_VR->m_HasNonVRAimSolution)
				aim = m_VR->m_NonVRAimAngles;
			const bool attackDownForRealSpread = (cmd->buttons & (1 << 0)) != 0;
			if (attackDownForRealSpread && localPlayerForAutoActions)
			{
				C_WeaponCSBase* activeWeaponForSpread =
					reinterpret_cast<C_WeaponCSBase*>(localPlayerForAutoActions->GetActiveWeapon());
				m_VR->ApplyVrHandsRealBulletSpreadServerViewAngles(
					localPlayerForAutoActions,
					activeWeaponForSpread,
					aim);
			}
			// 简单夹角，避免异常值
			if (aim.x > 89.f)  aim.x = 89.f;
			if (aim.x < -89.f) aim.x = -89.f;
			// yaw 归一到 [-180,180]
			while (aim.y > 180.f)  aim.y -= 360.f;
			while (aim.y < -180.f) aim.y += 360.f;

			cmd->viewangles.x = aim.x;   // pitch
			cmd->viewangles.y = aim.y;   // yaw
			cmd->viewangles.z = 0.f;     // roll 一般不用

			// Immersive throws on servers that cannot take a custom throw velocity:
			// at release, aim the native fixed-speed throw so it lands where the
			// hand's throw would have, and hold that aim through the throw window.
			{
				static bool s_t0ThrowPrevAttack = false;
				static int s_t0ThrowGraceTicks = 0;
				static QAngle s_t0ThrowAngles{};
				C_WeaponCSBase* const throwWeapon = localPlayerForAutoActions
					? reinterpret_cast<C_WeaponCSBase*>(localPlayerForAutoActions->GetActiveWeapon()) : nullptr;
				// A HandThrow server throws from the hand itself (below).
				const bool throwable = throwWeapon && m_VR->m_ManualThrowEnabled && !m_VR->m_MouseModeEnabled &&
					!m_VR->GrenadeArcMode() && !m_VR->HandThrowServerReady() && m_VR->IsThrowableWeapon(throwWeapon);
				const bool attackDownForThrow = (cmd->buttons & (1 << 0)) != 0;
				if (throwable && attackDownForThrow && !s_t0ThrowPrevAttack)
					m_VR->TriggerPhysicalHandHapticPulse(m_VR->IsGameplayHandLeftPhysical(false), 0.025f, 140.0f, 0.2f, 2); // pin pulled
				if (throwable && !attackDownForThrow && s_t0ThrowPrevAttack && localPlayerForAutoActions)
				{
					// The hand's smoothed release velocity (tracking space, relative
					// to the body) times the same boost a hand throw gets.
					Vector releaseVelocity{};
					const bool releaseMeasured = m_VR->EstimateHandThrowVelocity(m_VR->ThrowableHandIsLeft(), releaseVelocity);
					if (!releaseMeasured)
						releaseVelocity = m_VR->m_DroppedMagazineHandVelocity[1];
					// Physics throw assistance: the native throw below is aimed to land where
					// the assisted hand throw lands (no body velocity in this model).
					if (releaseMeasured)
						m_VR->ApplyGrenadeThrowAssist(localPlayerForAutoActions, m_VR->GetRightControllerAbsPos(),
							m_VR->GetRightControllerAbsAngle(), Vector(0.0f, 0.0f, 0.0f),
							std::max(0.1f, m_VR->m_ManualThrowHandVelocityScale), releaseVelocity);
					const Vector handVelocity = releaseVelocity *
						std::max(0.1f, m_VR->m_ManualThrowHandVelocityScale);
					const l4d2vr_ballistics::NativeThrowAim solved = l4d2vr_ballistics::MapHandThrowToNative(
						m_VR->GetRightControllerAbsPos(), handVelocity, localPlayerForAutoActions->EyePosition(),
						localPlayerForAutoActions->GetAbsOrigin().z,
						l4d2vr_ballistics::DefaultNativeThrowModel(l4d2vr_ballistics::Throwable::Molotov));
					if (solved.solved || solved.fallback)
					{
						s_t0ThrowAngles = QAngle(solved.pitchDeg, solved.yawDeg, 0.0f);
						NormalizeAndClampViewAngles(s_t0ThrowAngles);
						s_t0ThrowGraceTicks = 48;
					}
				}
				s_t0ThrowPrevAttack = throwable && attackDownForThrow;
				if (s_t0ThrowGraceTicks > 0)
				{
					--s_t0ThrowGraceTicks;
					cmd->viewangles = s_t0ThrowAngles;
				}
			}

			// Third-person melee: when holding a melee weapon, steer the server-facing viewangles
			// toward the rendered reticle convergence point so swings land where the crosshair points.
			if (m_Game->m_IsMeleeWeaponActive && m_VR->IsThirdPersonCameraActive() && m_VR->m_HasAimConvergePoint && !m_VR->m_AdjustingViewmodel
				&& !m_VR->m_AdjustingScope)
			{
				Vector eyePos = (m_VR->GetViewOriginLeft() + m_VR->GetViewOriginRight()) * 0.5f;
				Vector dir = m_VR->m_AimConvergePoint - eyePos;
				if (VectorLength(dir) > 0.001f)
				{
					VectorNormalize(dir);
					QAngle tpAng;
					QAngle::VectorAngles(dir, tpAng);
					NormalizeAndClampViewAngles(tpAng);
					tpAng.z = 0.f;
					cmd->viewangles = tpAng;
				}
			}

			// Non-VR server melee feel: translate a controller swing into a normal melee attack (IN_ATTACK)
			// This only affects local *input* / presentation. The server still does normal melee resolution.
			// 2.0.13: never in classic (melee is the attack button there).
			if (m_Game->m_IsMeleeWeaponActive && !m_VR->m_AdjustingViewmodel
				&& !m_VR->m_AdjustingScope && !m_VR->ClassicGameplayActive())
			{
				using clock = std::chrono::steady_clock;
				const auto now = clock::now();

				auto lerpAngle = [](float a, float b, float t) -> float {
					float d = b - a;
					while (d > 180.f) d -= 360.f;
					while (d < -180.f) d += 360.f;
					return a + d * t;
					};

				// Aim lock: during lock window, keep viewangles stable so the melee direction doesn't jitter.
				if (now < s_nonvrMeleeLockUntil)
				{
					cmd->viewangles = s_nonvrMeleeLockedAngles;
				}

				// Pending fire: after a short delay, start IN_ATTACK and begin aim lock.
				// This makes melee feel more like a "wind-up -> hit" instead of an instant click.
				if (s_nonvrMeleePending && now >= s_nonvrMeleeFireAt)
				{
					s_nonvrMeleePending = false;

					s_nonvrMeleeLockedAngles = s_nonvrMeleePendingAngles;

					const float lockT = std::max(0.0f, m_VR->m_NonVRMeleeAimLockTime);
					s_nonvrMeleeLockUntil = now + std::chrono::duration_cast<clock::duration>(std::chrono::duration<float>(lockT));

					s_nonvrMeleeHoldTicks = std::max(s_nonvrMeleeHoldTicks, s_nonvrMeleePendingHoldTicks);

					cmd->viewangles = s_nonvrMeleeLockedAngles;
				}

				// Hold/queue: keep IN_ATTACK pressed for a few ticks to reduce "dropped" swings.
				if (s_nonvrMeleeHoldTicks > 0)
				{
					cmd->buttons |= (1 << 0); // IN_ATTACK
					--s_nonvrMeleeHoldTicks;
				}

				// Edge trigger + hysteresis: only trigger once per swing, and require speed to fall below a lower
				// threshold before re-arming.
				// Controller velocity in tracking space can include whole-body/HMD motion; remove HMD velocity for cleaner gesture.
				Vector relVel = m_VR->m_RightControllerPose.TrackedDeviceVel - m_VR->m_HmdPose.TrackedDeviceVel;

				// Fallback: derive relative velocity from position delta (some runtimes report near-zero velocity).
				const float dt = (flInputSampleTime > 0.0001f) ? flInputSampleTime : 0.011111f;
				if (s_nonvrMeleeHasPrev)
				{
					Vector dCtrl = m_VR->m_RightControllerPose.TrackedDevicePos - s_nonvrMeleePrevCtrlPos;
					Vector dHmd = m_VR->m_HmdPose.TrackedDevicePos - s_nonvrMeleePrevHmdPos;
					Vector derivedRelVel = (dCtrl - dHmd) * (1.0f / dt);

					if (VectorLength(relVel) < 0.01f && VectorLength(derivedRelVel) > 0.01f)
						relVel = derivedRelVel;
				}
				s_nonvrMeleePrevCtrlPos = m_VR->m_RightControllerPose.TrackedDevicePos;
				s_nonvrMeleePrevHmdPos = m_VR->m_HmdPose.TrackedDevicePos;
				s_nonvrMeleeHasPrev = true;

				// Gesture speed: ignore vertical to reduce false triggers from raising/lowering hands.
				Vector swingVel = relVel;
				swingVel.z = 0.0f;
				const float v = (float)VectorLength(swingVel);
				const QAngle av = m_VR->m_RightControllerPose.TrackedDeviceAngVel;
				const float angV = sqrtf(av.x * av.x + av.y * av.y + av.z * av.z); // deg/s (tracking space)

				const float thr = std::max(0.0f, m_VR->m_NonVRMeleeSwingThreshold);
				const float hyst = std::clamp(m_VR->m_NonVRMeleeHysteresis, 0.1f, 0.95f);
				const float rearmThr = thr * hyst;

				const float angThr = std::max(0.0f, m_VR->m_NonVRMeleeAngVelThreshold);

				bool above =
					(thr > 0.0f && v > thr) ||
					(angThr > 0.0f && angV > angThr);

				bool below =
					(thr <= 0.0f || v < rearmThr) &&
					(angThr <= 0.0f || angV < angThr * hyst);

				// With melee inertia on, the blade-tip swing detector decides swings
				// (it already re-arms on rest and never fires for the chainsaw).
				static uint32_t s_nonvrMeleeSeenSwingSerial = 0u;
				if (m_VR->m_MeleeInertiaEnabled)
				{
					const uint32_t swingSerial = m_VR->m_MeleeSwingStartSerial.load(std::memory_order_acquire);
					above = m_VR->m_MeleeMotionSwingEligible && swingSerial != s_nonvrMeleeSeenSwingSerial;
					below = true;
					s_nonvrMeleeSeenSwingSerial = swingSerial;
					if (above && VectorLength(m_VR->m_MeleeSwingDirection) > 0.01f)
						swingVel = m_VR->m_MeleeSwingDirection;
				}

				if (below)
					s_nonvrMeleeArmed = true;

				if (above && s_nonvrMeleeArmed && !s_nonvrMeleePending && now >= s_nonvrMeleeCooldownUntil)
				{
					s_nonvrMeleeArmed = false;

					// Hold IN_ATTACK for a few ticks so we don't miss the server-side melee window.
					const float holdTime = std::max(0.0f, m_VR->m_NonVRMeleeHoldTime);
					int holdTicks = (int)ceilf(holdTime / dt);
					holdTicks = std::clamp(holdTicks, 1, 8);

					// Capture current aim direction, optionally blend toward swing velocity direction.
					QAngle lockedAngles = cmd->viewangles;

					const float blend = std::clamp(m_VR->m_NonVRMeleeSwingDirBlend, 0.0f, 1.0f);
					if (blend > 0.0f)
					{
						Vector velDir = swingVel;
						if (!velDir.IsZero())
						{
							VectorNormalize(velDir);
							QAngle velAng;
							QAngle::VectorAngles(velDir, velAng);
							NormalizeAndClampViewAngles(velAng);

							lockedAngles.x = lerpAngle(lockedAngles.x, velAng.x, blend);
							lockedAngles.y = lerpAngle(lockedAngles.y, velAng.y, blend);
							lockedAngles.z = 0.f;
							NormalizeAndClampViewAngles(lockedAngles);
						}
					}

					// Apply cooldown window immediately so one gesture maps to one swing.
					const float cd = std::max(0.05f, m_VR->m_NonVRMeleeSwingCooldown);
					s_nonvrMeleeCooldownUntil = now + std::chrono::duration_cast<clock::duration>(std::chrono::duration<float>(cd));

					// Delay before attacking, then lock angles during the hit window.
					const float delayT = std::max(0.0f, m_VR->m_NonVRMeleeAttackDelay);
					if (delayT > 0.0f)
					{
						s_nonvrMeleePending = true;
						s_nonvrMeleeFireAt = now + std::chrono::duration_cast<clock::duration>(std::chrono::duration<float>(delayT));
						s_nonvrMeleePendingAngles = lockedAngles;
						s_nonvrMeleePendingHoldTicks = holdTicks;
					}
					else
					{
						// Fire immediately (legacy behavior)
						s_nonvrMeleeLockedAngles = lockedAngles;

						const float lockT = std::max(0.0f, m_VR->m_NonVRMeleeAimLockTime);
						s_nonvrMeleeLockUntil = now + std::chrono::duration_cast<clock::duration>(std::chrono::duration<float>(lockT));

						s_nonvrMeleeHoldTicks = std::max(s_nonvrMeleeHoldTicks, holdTicks);

						cmd->viewangles = s_nonvrMeleeLockedAngles;
						cmd->buttons |= (1 << 0); // IN_ATTACK
					}
				}
			}
			else
			{
				// Leaving melee state: clear any queued swings/locks so we don't "ghost swing" later.
				s_nonvrMeleePending = false;
				s_nonvrMeleePendingHoldTicks = 0;
				s_nonvrMeleeHoldTicks = 0;
				s_nonvrMeleeLockUntil = {};
				s_nonvrMeleeCooldownUntil = {};
				s_nonvrMeleeArmed = true;
				s_nonvrMeleeHasPrev = false;
			}

			// Re-base movement for non-VR servers:
			// - The server interprets forwardmove/sidemove in the basis of cmd->viewangles (aim).
			// - We want movement to follow a separate "movement yaw" (HMD yaw by default; optional controller yaw),
			//   not necessarily the hand aim yaw.
			// So we convert existing movement (built under originalViewAngles) into world space,
			// add VR stick movement in movement-yaw space, then project back into the final cmd basis.
			{
				// Existing movement (keyboard etc.) in world space
				QAngle origYawOnly(0.f, originalViewAngles.y, 0.f);
				Vector origForward, origRight, origUp;
				QAngle::AngleVectors(origYawOnly, &origForward, &origRight, &origUp);
				Vector worldMove = origForward * cmd->forwardmove + origRight * cmd->sidemove;

				// VR stick movement (movement basis = HMD yaw or controller yaw)
				if (hadWalkAxis)
				{
					float moveYaw = m_VR->GetMovementYawDeg();
					if (m_VR->m_ThirdPersonFrontViewEnabled && m_VR->m_IsThirdPersonCamera && m_VR->ShouldRenderScope())
					{
						moveYaw = m_VR->GetScopeCameraAbsAngle().y + 180.0f;
						moveYaw -= 360.0f * std::floor((moveYaw + 180.0f) / 360.0f);
					}
					QAngle bodyYawOnly(0.f, moveYaw, 0.f);
					Vector bodyForward, bodyRight, bodyUp;
					QAngle::AngleVectors(bodyYawOnly, &bodyForward, &bodyRight, &bodyUp);
					worldMove += bodyForward * (walkNy * walkMaxSpeed) + bodyRight * (walkNx * walkMaxSpeed);
				}

				// Project into the final cmd basis (after aim/melee lock)
				QAngle cmdYawOnly(0.f, cmd->viewangles.y, 0.f);
				Vector cmdForward, cmdRight, cmdUp;
				QAngle::AngleVectors(cmdYawOnly, &cmdForward, &cmdRight, &cmdUp);
				cmd->forwardmove = DotProduct(worldMove, cmdForward);
				cmd->sidemove = DotProduct(worldMove, cmdRight);
			}

		}
		else {
			// VR-aware servers: ensure cmd->viewangles matches HMD.
			// Otherwise forward/sidemove get interpreted in the wrong basis (push forward -> strafe).
			bool useControllerCmdView = false;
			bool useNonVRAimCmdView = false;
			const char* controllerCmdViewReason = nullptr;
			{
				const int lpIdx = (m_Game && m_Game->m_EngineClient) ? m_Game->m_EngineClient->GetLocalPlayer() : -1;
				C_BasePlayer* lp = (lpIdx > 0) ? (C_BasePlayer*)m_Game->GetClientEntity(lpIdx) : nullptr;
				C_WeaponCSBase* activeWeapon = lp ? reinterpret_cast<C_WeaponCSBase*>(lp->GetActiveWeapon()) : nullptr;
				const uintptr_t weaponTag = reinterpret_cast<uintptr_t>(activeWeapon);
				if (weaponTag != s_vrAwareThrowableAimWeapon)
				{
					if (s_vrAwareThrowableAimPrevWeaponThrowable && s_vrAwareThrowableAimPrevAttackDown)
						s_vrAwareThrowableAimTicks = std::max(s_vrAwareThrowableAimTicks, 48);
					s_vrAwareThrowableAimWeapon = weaponTag;
					s_vrAwareThrowableAimPrevAttackDown = false;
					s_vrAwareThrowableAimPrevWeaponThrowable = false;
				}

				const char* activeWeaponName = activeWeapon ? activeWeapon->GetName() : nullptr;
				const char* activeWeaponNetClass = activeWeapon ? m_Game->GetNetworkClassName((uintptr_t*)activeWeapon) : nullptr;
				const bool usingMountedWeapon = lp ? (
					ReadNetvar<bool>(lp, VR::kUsingMountedGunOffset) ||
					ReadNetvar<bool>(lp, VR::kUsingMountedWeaponOffset)
				) : false;
				const bool activeWeaponIsThrowable = IsVRThrowableWeapon(activeWeapon, activeWeaponName, activeWeaponNetClass);
				constexpr int kIN_ATTACK = (1 << 0);
				constexpr int kIN_USE = (1 << 5);
				const bool attackDown = (cmd->buttons & kIN_ATTACK) != 0;
				const bool useDown = (cmd->buttons & kIN_USE) != 0;
				const auto now = std::chrono::steady_clock::now();
				const bool useCmdAimActive = m_VR &&
					(m_VR->m_ServerUseControllerAimActive ||
						(m_VR->m_ServerUseControllerAimUntil.time_since_epoch().count() != 0 &&
							now <= m_VR->m_ServerUseControllerAimUntil));

				if (activeWeaponIsThrowable)
				{
					if (attackDown || (s_vrAwareThrowableAimPrevAttackDown && !attackDown))
						s_vrAwareThrowableAimTicks = 48;
					useControllerCmdView = attackDown || (s_vrAwareThrowableAimTicks > 0);
					if (useControllerCmdView)
					{
						controllerCmdViewReason = attackDown ? "throw" : "throw-grace";
					}
					if (s_vrAwareThrowableAimTicks > 0)
						--s_vrAwareThrowableAimTicks;
				}
				else
				{
					useControllerCmdView = s_vrAwareThrowableAimTicks > 0;
					if (useControllerCmdView)
					{
						controllerCmdViewReason = "throw-switch-grace";
					}
					if (s_vrAwareThrowableAimTicks > 0)
						--s_vrAwareThrowableAimTicks;
				}

				s_vrAwareThrowableAimPrevAttackDown = activeWeaponIsThrowable && attackDown;
				s_vrAwareThrowableAimPrevWeaponThrowable = activeWeaponIsThrowable;
				if (useCmdAimActive || useDown)
				{
					useControllerCmdView = true;
					controllerCmdViewReason = "use";
				}
				if (usingMountedWeapon)
				{
					useControllerCmdView = true;
					if (!controllerCmdViewReason)
						controllerCmdViewReason = "mounted";
				}
			}

			QAngle view;
			if (useControllerCmdView)
			{
				view = m_VR->GetRightControllerAbsAngle();

				static std::chrono::steady_clock::time_point s_lastControllerCmdViewLog{};
				const auto logNow = std::chrono::steady_clock::now();
				if (s_lastControllerCmdViewLog.time_since_epoch().count() == 0 ||
					std::chrono::duration<float>(logNow - s_lastControllerCmdViewLog).count() >= 0.50f)
				{
					s_lastControllerCmdViewLog = logNow;
					Game::logMsg(
						"[VR][UseAim] CreateMove controller cmd view reason=%s nonvrAim=%d hasAim=%d view=(%.1f %.1f %.1f)",
						controllerCmdViewReason ? controllerCmdViewReason : "unknown",
						useNonVRAimCmdView ? 1 : 0,
						m_VR->m_HasNonVRAimSolution ? 1 : 0,
						view.x, view.y, view.z);
				}
			}
			else
			{
				Vector hmdAng = m_VR->GetViewAngle();
				view = QAngle(hmdAng.x, hmdAng.y, hmdAng.z);
			}
			if (m_VR->m_MouseModeEnabled)
			{
				float yaw = m_VR->m_RotationOffset;
				while (yaw > 180.f)  yaw -= 360.f;
				while (yaw < -180.f) yaw += 360.f;
				float pitch = m_VR->m_MouseAimInitialized ? m_VR->m_MouseAimPitchOffset : view.x;
				view = QAngle(pitch, yaw, 0.f);
			}
			if (view.x > 89.f)  view.x = 89.f;
			if (view.x < -89.f) view.x = -89.f;
			while (view.y > 180.f)  view.y -= 360.f;
			while (view.y < -180.f) view.y += 360.f;
			view.z = 0.f;
			cmd->viewangles = view;

			// Re-base engine-generated locomotion (keyboard WASD, etc.) from the original
			// cmd basis into the VR-facing basis, then add stick movement in world space.
			// Without this, keyboard movement gets rotated when cmd->viewangles is replaced.
			{
				QAngle origYawOnly(0.f, originalViewAngles.y, 0.f);
				Vector origForward, origRight, origUp;
				QAngle::AngleVectors(origYawOnly, &origForward, &origRight, &origUp);
				Vector worldMove = origForward * originalForwardMove + origRight * originalSideMove;

				if (hadWalkAxis)
				{
					float moveYaw = m_VR->GetMovementYawDeg();
					if (m_VR->m_ThirdPersonFrontViewEnabled && m_VR->m_IsThirdPersonCamera && m_VR->ShouldRenderScope())
					{
						moveYaw = m_VR->GetScopeCameraAbsAngle().y + 180.0f;
						moveYaw -= 360.0f * std::floor((moveYaw + 180.0f) / 360.0f);
					}
					QAngle moveYawOnly(0.f, moveYaw, 0.f);
					Vector moveForward, moveRight, moveUp;
					QAngle::AngleVectors(moveYawOnly, &moveForward, &moveRight, &moveUp);
					worldMove += moveForward * (walkNy * walkMaxSpeed) + moveRight * (walkNx * walkMaxSpeed);
				}

				QAngle cmdYawOnly(0.f, cmd->viewangles.y, 0.f);
				Vector cmdForward, cmdRight, cmdUp;
				QAngle::AngleVectors(cmdYawOnly, &cmdForward, &cmdRight, &cmdUp);
				cmd->forwardmove = DotProduct(worldMove, cmdForward);
				cmd->sidemove = DotProduct(worldMove, cmdRight);
			}

			// Third-person melee: on VR-aware servers, melee swing direction is resolved from cmd->viewangles.
			// In third-person camera, the rendered reticle ray (m_AimConvergePoint) may not match HMD yaw,
			// causing "crosshair on target but swing misses". We lock viewangles toward the converge point
			// for a short window when a melee swing starts, and re-base movement so locomotion doesn't snap.
			{
				using clock = std::chrono::steady_clock;
				const auto now = clock::now();
				const bool tpMeleeEligible = m_Game->m_IsMeleeWeaponActive && m_VR->IsThirdPersonCameraActive() && m_VR->m_HasAimConvergePoint && !m_VR->m_AdjustingViewmodel && !m_VR->m_AdjustingScope;
				if (!tpMeleeEligible)
				{
					s_tpMeleePrevAttackDown = false;
					s_tpMeleeLockUntil = {};
				}
				else
				{
					Vector eyePos = (m_VR->GetViewOriginLeft() + m_VR->GetViewOriginRight()) * 0.5f;
					Vector dir = m_VR->m_AimConvergePoint - eyePos;
					if (VectorLength(dir) > 0.001f)
					{
						VectorNormalize(dir);
						QAngle tpAng;
						QAngle::VectorAngles(dir, tpAng);
						NormalizeAndClampViewAngles(tpAng);
						tpAng.z = 0.f;

						const bool attackDown = (cmd->buttons & (1 << 0)) != 0; // IN_ATTACK
						if (attackDown && !s_tpMeleePrevAttackDown)
						{
							s_tpMeleeLockedAngles = tpAng;
							const float lockT = std::max(0.0f, m_VR->m_NonVRMeleeAimLockTime);
							s_tpMeleeLockUntil = now + std::chrono::duration_cast<clock::duration>(std::chrono::duration<float>(lockT));
						}
						s_tpMeleePrevAttackDown = attackDown;

						if (now < s_tpMeleeLockUntil)
						{
							// Preserve movement direction: convert current cmd movement into world space (old yaw),
							// then project back into the locked yaw basis.
							QAngle oldYawOnly(0.f, cmd->viewangles.y, 0.f);
							Vector oldForward, oldRight, oldUp;
							QAngle::AngleVectors(oldYawOnly, &oldForward, &oldRight, &oldUp);
							Vector worldMove = oldForward * cmd->forwardmove + oldRight * cmd->sidemove;

							cmd->viewangles = s_tpMeleeLockedAngles;

							QAngle newYawOnly(0.f, cmd->viewangles.y, 0.f);
							Vector newForward, newRight, newUp;
							QAngle::AngleVectors(newYawOnly, &newForward, &newRight, &newUp);
							cmd->forwardmove = DotProduct(worldMove, newForward);
							cmd->sidemove = DotProduct(worldMove, newRight);
						}
					}
				}
			}

		}

	}

	// Motion melee on VR-aware servers: a detected swing presses the stock
	// attack for a few ticks so the server's melee runs (its sweep follows the
	// blade from the encoded pose). Plain servers use the swing path above.
	{
		static uint32_t s_vrMeleeSeenSwingSerial = 0u;
		static int s_vrMeleeAttackTicks = 0;
		const uint32_t swingSerial = m_VR->m_MeleeSwingStartSerial.load(std::memory_order_acquire);
		if (swingSerial != s_vrMeleeSeenSwingSerial)
		{
			s_vrMeleeSeenSwingSerial = swingSerial;
			if (m_VR->m_MotionOnlyMeleeAndShove && m_VR->m_IsVREnabled && !m_VR->m_ForceNonVRServerMovement &&
				m_VR->m_MeleeMotionSwingEligible)
				s_vrMeleeAttackTicks = 3;
		}
		if (s_vrMeleeAttackTicks > 0)
		{
			if (m_VR->m_MeleeMotionSwingEligible)
				cmd->buttons |= (1 << 0); // IN_ATTACK
			--s_vrMeleeAttackTicks;
		}
	}

	// Pills/adrenaline used by gesture: hold the stock attack until consumed.
	if (m_VR->m_ConsumableGesturesEnabled && std::chrono::steady_clock::now() < m_VR->m_ConsumableUseHoldUntil)
		cmd->buttons |= (1 << 0); // IN_ATTACK

	// Handing pills/adrenaline to a teammate. Plugin servers run the give
	// themselves (impulse 239, aimed from the eye through the hand); elsewhere
	// press the stock secondary attack aimed at the teammate's chest.
	if (m_VR->m_PillGiveImpulsePending.exchange(false, std::memory_order_acq_rel) &&
		m_VR->m_IsVREnabled && s_ServerUnderstandsVR && m_VR->ServerFeature(l4d2vr_features::PillGive) &&
		cmd->impulse == 0)
	{
		cmd->impulse = 239;
		m_VR->m_PillGiveHoldUntil = {};
	}
	if (std::chrono::steady_clock::now() < m_VR->m_PillGiveHoldUntil && !m_VR->m_AdjustingViewmodel &&
		!m_VR->m_AdjustingScope)
	{
		Vector toTarget = m_VR->m_PillGiveTargetPoint - m_VR->m_HmdPosAbs;
		if (VectorLength(toTarget) > 1.0f)
		{
			QAngle giveAngles{};
			QAngle::VectorAngles(toTarget, giveAngles);
			SetCmdViewAnglesKeepMove(cmd, giveAngles);
		}
		cmd->buttons |= (1 << 11); // IN_ATTACK2
	}

	// Physical shove (melee thrust / gun bash): aim the shove along the motion
	// for as long as it is held.
	if (m_VR->m_PhysicalShoveEnabled && m_VR->m_PhysicalShoveAimValid &&
		std::chrono::steady_clock::now() < m_VR->m_PhysicalShoveHoldUntil &&
		!m_VR->m_AdjustingViewmodel && !m_VR->m_AdjustingScope)
	{
		SetCmdViewAnglesKeepMove(cmd, m_VR->m_PhysicalShoveAngles);
	}

	// GrenadeThrowMode=arc: while a grenade is in the hand, the view angles are
	// the hand's aim (the arc's direction), so the game's own throw leaves along
	// the arc it shows (ThrowGrenade throws along the eye angles; the plugin's
	// throw eye override uses the same angles from the encoded pose). Movement
	// keeps its world direction.
	{
		C_WeaponCSBase* const arcWeapon = localPlayerForAutoActions
			? reinterpret_cast<C_WeaponCSBase*>(localPlayerForAutoActions->GetActiveWeapon()) : nullptr;
		if (m_VR->GrenadeArcMode() && arcWeapon && m_VR->IsThrowableWeapon(arcWeapon) && m_VR->m_HasThrowArc &&
			!m_VR->m_LastAimDirection.IsZero() && !m_VR->m_AdjustingViewmodel && !m_VR->m_AdjustingScope)
		{
			QAngle arcAngles{};
			QAngle::VectorAngles(m_VR->m_LastAimDirection, arcAngles);
			NormalizeAndClampViewAngles(arcAngles);
			arcAngles.z = 0.0f;
			QAngle oldYawOnly(0.f, cmd->viewangles.y, 0.f);
			Vector oldForward, oldRight, oldUp;
			QAngle::AngleVectors(oldYawOnly, &oldForward, &oldRight, &oldUp);
			const Vector worldMove = oldForward * cmd->forwardmove + oldRight * cmd->sidemove;
			cmd->viewangles = arcAngles;
			QAngle newYawOnly(0.f, cmd->viewangles.y, 0.f);
			Vector newForward, newRight, newUp;
			QAngle::AngleVectors(newYawOnly, &newForward, &newRight, &newUp);
			cmd->forwardmove = DotProduct(worldMove, newForward);
			cmd->sidemove = DotProduct(worldMove, newRight);
		}
	}

	// Keep Source's main-thread viewangles aligned with the VR audio listener while
	// third-person rendering is active. The render thread avoids SetViewAngles in
	// queued/multicore mode, so this main-thread update prevents stale visual-camera
	// angles from driving front/back sound spatialization.
	if (m_Game && m_Game->m_EngineClient && m_VR->m_IsThirdPersonCamera && !m_VR->m_AdjustingViewmodel
				&& !m_VR->m_AdjustingScope)
	{
		Vector currentAudioFallback(cmd->viewangles.x, cmd->viewangles.y, cmd->viewangles.z);
		QAngle listenerAngles = BuildVRAudioListenerAngles(m_VR, currentAudioFallback);
		m_Game->m_EngineClient->SetViewAngles(listenerAngles);
	}

	// Aim-line friendly-fire guard:
	// Compute teammate hit at input-tick rate (CreateMove) and latch suppression until attack is released.
	C_BasePlayer* ffLocalPlayer = nullptr;
	const bool localUsingMountedWeapon = !m_VR->m_ForceNonVRServerMovement && IsLocalClientUsingMountedWeapon();
	if (m_VR->m_BlockFireOnFriendlyAimEnabled && !localUsingMountedWeapon)
	{
		const int ffIdx = m_Game->m_EngineClient->GetLocalPlayer();
		if (ffIdx > 0)
			ffLocalPlayer = (C_BasePlayer*)m_Game->GetClientEntity(ffIdx);
	}

	// Do NOT suppress IN_ATTACK while the player is performing a continuous "use action"
	// (healing, giving ammo/upgrade pack, reviving, etc.). These actions intentionally target
	// teammates and require holding IN_ATTACK.
	const bool ffDoingUseAction = ffLocalPlayer ? (ReadNetvar<int>(ffLocalPlayer, 0x1ba8) != 0) : false; // m_iCurrentUseAction
	if (!localUsingMountedWeapon && !ffDoingUseAction && m_VR->ShouldSuppressPrimaryFire(cmd, ffLocalPlayer))
	{
		cmd->buttons &= ~(1 << 0); // IN_ATTACK
	}

	constexpr int kMagazineInteractionInAttack = (1 << 0);
	constexpr int kMagazineInteractionInReload = (1 << 13);
	const bool magazineInteractionBlocksFire = m_VR->IsMagazineInteractionBlockingFire();
	if (!localUsingMountedWeapon && magazineInteractionBlocksFire)
	{
		if ((cmd->buttons & kMagazineInteractionInAttack) != 0 && (s_lastButtons & kMagazineInteractionInAttack) == 0)
			m_VR->PlayMagazineInteractionBlockedFireEmptySound();
		cmd->buttons &= ~kMagazineInteractionInAttack; // IN_ATTACK
	}
	if (!localUsingMountedWeapon &&
		m_VR->IsMagazineInteractionLeftHandActive() &&
		!m_VR->IsMagazineInteractionReloadCommandActive())
	{
		cmd->buttons &= ~kMagazineInteractionInReload; // IN_RELOAD
	}
	if (!localUsingMountedWeapon &&
		m_VR->IsMagazineInteractionNativeReloadSuppressActive() &&
		!m_VR->IsMagazineInteractionReloadCommandActive())
	{
		cmd->buttons &= ~kMagazineInteractionInReload; // IN_RELOAD
	}
	const bool suppressMagazineEmptyClipAutoReload =
		m_VR->ShouldSuppressMagazineInteractionEmptyClipAutoReload(nullptr);
	if (!localUsingMountedWeapon && suppressMagazineEmptyClipAutoReload)
	{
		if ((cmd->buttons & kMagazineInteractionInAttack) != 0 && (s_lastButtons & kMagazineInteractionInAttack) == 0)
			m_VR->PlayMagazineInteractionBlockedFireEmptySound();
		cmd->buttons &= ~(kMagazineInteractionInAttack | kMagazineInteractionInReload);
	}

	if (m_VR)
	{
		// Detect control locomotion before adding physical roomscale movement.
		// Roomscale cmd movement should not disable the 1:1 camera decoupling path.
		const float fm = cmd ? cmd->forwardmove : 0.0f;
		const float sm = cmd ? cmd->sidemove : 0.0f;
		const float um = cmd ? cmd->upmove : 0.0f;
		const bool controlLocomotionActive =
			(fabsf(fm) > 0.5f) ||
			(fabsf(sm) > 0.5f) ||
			(fabsf(um) > 0.5f) ||
			m_VR->m_PushingThumbstick;
		m_VR->m_LocomotionActive = controlLocomotionActive;

		if (m_VR->m_IsVREnabled &&
			m_VR->m_Roomscale1To1Movement &&
			m_VR->m_Roomscale1To1DecoupleCamera)
		{
			const bool roomscaleMayDriveMovement =
				!controlLocomotionActive || !m_VR->m_Roomscale1To1DisableWhileThumbstick;

			const int lpIdx = (m_Game && m_Game->m_EngineClient) ? m_Game->m_EngineClient->GetLocalPlayer() : -1;
			C_BasePlayer* lp = (lpIdx > 0 && m_Game) ? (C_BasePlayer*)m_Game->GetClientEntity(lpIdx) : nullptr;
			const int lifeState = lp ? (int)ReadNetvar<uint8_t>(lp, VR::kLifeStateOffset) : 1;
			const int observerMode = lp ? (int)ReadNetvar<int>(lp, VR::kObserverModeOffset) : 1;
			const bool liveLocalPlayer = lp && lifeState == 0 && observerMode == 0;

			if (roomscaleMayDriveMovement && liveLocalPlayer && !m_VR->ShouldUseRoomscale1To1ServerMove())
			{
				Vector engineEye = lp->EyePosition();
				engineEye.z = 0.0f;

				if (m_VR->m_Roomscale1To1LastEngineEyeValid)
				{
					Vector acceptedWorldDelta = engineEye - m_VR->m_Roomscale1To1LastEngineEye;
					acceptedWorldDelta.z = 0.0f;

					const float acceptedLen = acceptedWorldDelta.Length();
					if (m_VR->m_Roomscale1To1PendingVisualWorldDeltaValid &&
						std::isfinite(acceptedLen) && acceptedLen < 96.0f &&
						m_VR->m_Roomscale1To1PrevValid && std::fabs(m_VR->m_VRScale) > 0.001f)
					{
						Vector visualWorldDelta = m_VR->m_Roomscale1To1PendingVisualWorldDelta;
						visualWorldDelta.z = 0.0f;

						Vector correctionWorld = acceptedWorldDelta - visualWorldDelta;
						correctionWorld.z = 0.0f;

						const float correctionLen = correctionWorld.Length();
						if (std::isfinite(correctionLen) && correctionLen > 0.01f && correctionLen < 96.0f)
						{
							m_VR->ApplyRoomscale1To1VisualWorldCorrection(correctionWorld);
						}
					}
				}

				m_VR->m_Roomscale1To1LastEngineEye = engineEye;
				m_VR->m_Roomscale1To1LastEngineEyeValid = true;
			}
			else
			{
				m_VR->m_Roomscale1To1LastEngineEyeValid = false;
				m_VR->m_Roomscale1To1PendingVisualWorldDeltaValid = false;
			}
		}
		else
		{
			m_VR->m_Roomscale1To1LastEngineEyeValid = false;
			m_VR->m_Roomscale1To1PendingVisualWorldDeltaValid = false;
		}

		m_VR->ApplyRoomscale1To1Move(cmd, flInputSampleTime, controlLocomotionActive);
		m_VR->ApplyMovementLedgeGuard(cmd, m_VR->m_ScopeFovAdjustSuppressWalk || m_VR->m_TeleportTargetingActive);

		// Keep the two input contracts independent:
		//   * carried props: hold the trigger and release to throw (Use stays native);
		//   * inventory items: hold Use first, then pull/release the trigger.
		// Inventory throw mode swallows both inputs and asks the listen server to use
		// CBaseCombatCharacter::Weapon_Drop. This includes grenade-slot weapons, so
		// Use+trigger drops the held item instead of arming its native effect.
		{
			constexpr uint32_t kManualCarryThrowIN_ATTACK = (1u << 0);
			constexpr uint32_t kManualInventoryThrowIN_USE = (1u << 5);
			constexpr uint32_t kManualCarryThrowWeaponShift = 26u;
			constexpr uint32_t kManualCarryThrowWeaponMask = (0x3Fu << kManualCarryThrowWeaponShift);
			C_BaseCombatWeapon* activeBaseWeapon = localPlayerForAutoActions
				? reinterpret_cast<C_BaseCombatWeapon*>(localPlayerForAutoActions->GetActiveWeapon())
				: nullptr;
			C_WeaponCSBase* activeWeapon = reinterpret_cast<C_WeaponCSBase*>(activeBaseWeapon);
			const char* activeWeaponName = activeBaseWeapon ? activeBaseWeapon->GetName() : nullptr;
			const char* activeWeaponNetClass = activeBaseWeapon
				? m_Game->GetNetworkClassName(reinterpret_cast<uintptr_t*>(activeBaseWeapon))
				: nullptr;
			const uintptr_t activeWeaponTag = reinterpret_cast<uintptr_t>(activeWeapon);
			const int activeWeaponId = activeWeapon
				? static_cast<int>(activeWeapon->GetWeaponID())
				: static_cast<int>(C_WeaponCSBase::WeaponID::NONE);
			const int carryWeaponId = ResolveVRManualCarryThrowWeaponId(
				activeWeapon,
				activeWeaponName,
				activeWeaponNetClass);
			const bool carriedProp =
				carryWeaponId != static_cast<int>(C_WeaponCSBase::WeaponID::NONE);
			const bool inventoryItem =
				!carriedProp &&
				ManualInventoryThrowWeaponIdRequiresCustomDrop(activeWeaponId);
			const bool emptyHandsPlaceholderActive =
				m_VR->m_ManualInventoryEmptyHandsActive.load(std::memory_order_acquire);
			const bool throwBackendReady = carriedProp
				? ManualCarryThrowBackendIsReady(carryWeaponId)
				: (inventoryItem && ManualInventoryThrowBackendIsReady(activeWeaponId));
			const bool physicalAttackDown = m_VR->m_PrimaryAttackDown;
			const bool physicalUseDown = localUseButtonDownForAutoActions || m_VR->m_UseCmdOwned;
			const bool throwActionsUsable = s_manualThrowInputGate.Update(
				m_VR->m_InputActionFrame.Read(), physicalAttackDown, physicalUseDown);
			const bool manualThrowInputActive =
				m_VR->m_IsVREnabled &&
				m_VR->m_ManualThrowEnabled &&
				s_ServerUnderstandsVR &&
				throwBackendReady &&
				!emptyHandsPlaceholderActive && throwActionsUsable;
			const uint32_t weaponGripInputState =
				m_VR->m_WeaponGripInputState.load(std::memory_order_acquire);
			const bool weaponGripActionActive = (weaponGripInputState & 1u) != 0u;
			const bool weaponGripDown = (weaponGripInputState & 2u) != 0u;
            const uint32_t gripNowMs = static_cast<uint32_t>(GetTickCount64());
            const uint32_t probeLeftGrip = m_VR->m_PistolOffhandGripState.load(std::memory_order_acquire);
            if (gripSession.gameplay && m_VR->m_GripReleaseDropEnabled &&
                ((weaponGripActionActive && weaponGripDown) || (probeLeftGrip & 3u) == 3u))
            {
                if (weaponGripActionActive && weaponGripDown)
                    RefreshGripPickupContact(m_VR, m_Game, localPlayerForAutoActions, false);
                if ((probeLeftGrip & 3u) == 3u)
                    RefreshGripPickupContact(m_VR, m_Game, localPlayerForAutoActions, true);
            }
            const uint32_t pickupOffer = m_VR->m_GripPickupOfferTimeMs.load(std::memory_order_acquire);
            const uint32_t leftPickupOffer = m_VR->m_LeftPistolPickupOfferTimeMs.load(std::memory_order_acquire);
            const uint32_t gripDraw = m_VR->m_GripDrawSerial.load(std::memory_order_acquire);
            // Holster drop: items drawn from a holster are held by the grip
            // (Pavlov style) even without the contact-pickup mode.
            const bool holsterGripHoldMode = m_VR->m_HolsterReleaseDropEnabled &&
                m_VR->m_BodyGripInventoryEnabled && m_VR->m_HolsterPavlovLayoutEnabled;
            const bool gripPickupEligible = gripSession.gameplay &&
                (m_VR->m_GripReleaseDropEnabled || holsterGripHoldMode) &&
                l4d2vr_wire::SupportsPhysicalVersion(m_VR->m_ServerPhysicalInteractionVersion.load(std::memory_order_acquire)) &&
                m_VR->m_IsVREnabled && m_VR->m_EncodeVRUsercmd && s_ServerUnderstandsVR &&
                !m_VR->m_ForceNonVRServerMovement && !m_VR->m_SuppressPlayerInput &&
                m_VR->m_FirstPersonControlReady.load(std::memory_order_acquire) &&
                !m_VR->m_RenderPlayerIncap.load(std::memory_order_relaxed) &&
                !m_VR->m_RenderPlayerControlledBySI.load(std::memory_order_relaxed) &&
                !IsLocalClientUsingMountedWeapon();
            const bool pistolInventorySession = gripSession.liveInventory && !emptyHandsPlaceholderActive &&
                inventoryItem && m_VR->m_DualPistolsIndependentHandsEnabled &&
                activeWeaponId == static_cast<int>(C_WeaponCSBase::WeaponID::PISTOL);
            const bool pistolGripMode = pistolInventorySession && gripPickupEligible && inventoryItem &&
                activeWeaponId == static_cast<int>(C_WeaponCSBase::WeaponID::PISTOL) &&
                m_VR->m_DualPistolsIndependentHandsEnabled && manualThrowInputActive;
            const bool nativeDual = pistolInventorySession && m_Game->IsDualPistolWeapon(activeWeapon);
            s_pistolOwnership.Observe(pistolInventorySession, activeWeaponTag, nativeDual, gripNowMs);
            const uint32_t leftGrip = m_VR->m_PistolOffhandGripState.load(std::memory_order_acquire);
            const bool leftReserved = m_VR->MagazineInteractionOwnsGrip(true);
            const bool rightReserved = m_VR->MagazineInteractionOwnsGrip(false);
            const bool leftPickupAllowed = s_leftGripReservation.AllowPickup(gripPickupEligible,
                (leftGrip & 1u) != 0u, (leftGrip & 2u) != 0u, leftReserved,
                l4d2vr_input::GripSample::Generation(leftGrip));
            const bool rightPickupAllowed = s_rightGripReservation.AllowPickup(gripPickupEligible,
                weaponGripActionActive, weaponGripDown, rightReserved,
                l4d2vr_input::GripSample::Generation(weaponGripInputState));
            if (leftReserved) s_pistolOwnership.CancelPickup(l4d2vr_dual::Hand::Left);
            if (rightReserved)
            {
                s_pistolOwnership.CancelPickup(l4d2vr_dual::Hand::Right);
                s_pendingGripAdoption = 0u;
            }
            const bool useLeft = !leftReserved && m_VR->m_PistolUseLeft.load(std::memory_order_acquire);
            const unsigned leftPickup = s_leftPistolPickup.Update(
                leftPickupAllowed && m_VR->m_DualPistolsIndependentHandsEnabled &&
                (pistolGripMode || emptyHandsPlaceholderActive) && useLeft,
                (leftGrip & 1u) != 0u, (leftGrip & 2u) != 0u,
                leftPickupOffer != 0u && gripNowMs - leftPickupOffer <= 100u, gripNowMs, activeWeaponTag);
            if ((leftPickup & l4d2vr_physical::PickupIntent::RequestUse) != 0u)
            {
                cmd->buttons |= 1u << 5;
                cmd->impulse = l4d2vr_grip::kPickupImpulse;
                pistolInteractionHand = l4d2vr_dual::Hand::Left;
                s_pistolOwnership.PickupRequested(l4d2vr_dual::Hand::Left);
                Game::logMsg("[VR][PistolPickup] left contact requested cmd=%d", cmd->command_number);
            }
            if ((leftPickup & l4d2vr_physical::PickupIntent::AdoptWeapon) != 0u && pistolGripMode)
                s_pistolOwnership.AdoptPickup(l4d2vr_dual::Hand::Left);
            // A holster draw by the weapon-hand grip: the grip took the item, so
            // once it arrives in the hand the grip owns it (a later release drops it).
            // GrabFsm only draws on a fresh squeeze inside the zone, so this does not
            // need the contact pickup's press gate; the window allows a slow server.
            const bool holsterDrawEligible = gripSession.gameplay && gripPickupEligible && !rightReserved;
            if (gripDraw != s_gripDrawSerial)
            {
                s_gripDrawSerial = gripDraw;
                if (holsterDrawEligible && weaponGripActionActive && weaponGripDown)
                    s_holsterDrawPickup.Begin(gripNowMs, s_previousGripWeapon, 1500u);
                Game::logMsg("[VR][GripDrop] holster draw seen: eligible=%d grip=%d/%d",
                    holsterDrawEligible ? 1 : 0, weaponGripActionActive ? 1 : 0, weaponGripDown ? 1 : 0);
            }
            if ((s_holsterDrawPickup.Update(holsterDrawEligible, weaponGripActionActive, weaponGripDown, false,
                    gripNowMs, activeWeaponTag) & l4d2vr_physical::PickupIntent::AdoptWeapon) != 0u)
                s_pendingGripAdoption = activeWeaponTag;
            const unsigned pickupResult = s_gripPickup.Update(rightPickupAllowed && !useLeft,
                weaponGripActionActive, weaponGripDown,
                pickupOffer != 0u && gripNowMs - pickupOffer <= 100u,
                gripNowMs, activeWeaponTag);
            if ((pickupResult & l4d2vr_physical::PickupIntent::AdoptWeapon) != 0u)
            {
                s_pendingGripAdoption = activeWeaponTag;
                m_VR->m_GripToggleAdoptSerial.fetch_add(1u, std::memory_order_acq_rel);
            }
            if ((pickupResult & l4d2vr_physical::PickupIntent::RequestUse) != 0u)
            {
                m_VR->m_GripTogglePickupSerial.fetch_add(1u, std::memory_order_acq_rel);
                cmd->buttons |= 1u << 5; // Native, server-validated IN_USE pickup.
                cmd->impulse = l4d2vr_grip::kPickupImpulse;
                pistolInteractionHand = l4d2vr_dual::Hand::Right;
                if (pistolGripMode) s_pistolOwnership.PickupRequested(l4d2vr_dual::Hand::Right);
                Game::logMsg("[VR][GripPickup] contact pickup requested cmd=%d", cmd->command_number);
            }
            s_previousGripWeapon = activeWeaponTag;

			// Hand throws: the trigger only pulls the pin, so it never holds the
			// grenade in the native attack path that would block a grip release.
			const l4d2vr_hand_throw::Kind handThrowKind = carriedProp ? l4d2vr_hand_throw::Kind::None
				: l4d2vr_hand_throw::KindFromWeaponId(activeWeaponId);
			const bool handThrowMode = handThrowKind != l4d2vr_hand_throw::Kind::None && activeWeaponTag != 0u &&
				m_VR->HandThrowServerReady();
			const bool gripDropEligible =
				l4d2vr_wire::SupportsPhysicalVersion(m_VR->m_ServerPhysicalInteractionVersion.load(std::memory_order_acquire)) &&
				gripSession.gameplay && (m_VR->m_GripReleaseDropEnabled || holsterGripHoldMode) &&
				manualThrowInputActive && inventoryItem &&
				m_VR->m_EncodeVRUsercmd && !m_VR->m_ForceNonVRServerMovement &&
				m_VR->m_FirstPersonControlReady.load(std::memory_order_acquire) &&
				!m_VR->m_SuppressPlayerInput &&
				!m_VR->m_RenderPlayerIncap.load(std::memory_order_relaxed) &&
				!m_VR->m_RenderPlayerControlledBySI.load(std::memory_order_relaxed) &&
				!IsLocalClientUsingMountedWeapon() &&
				l4d2vr_grip::CanReleaseInventoryItem(
					ManualInventoryThrowWeaponIdDoesMeleeDamage(activeWeaponId),
					handThrowMode ? false : physicalAttackDown, handThrowMode ? false : s_manualCarryThrowPhysicalAttackDown,
					physicalUseDown, s_manualInventoryThrowArmed);
            // Letting go inside the item's own holster puts it away, and over
            // another holster swaps: neither drops. The grip keeps owning what it
            // took while the hand passes through a holster on the way.
            const bool gripDropHere =
                !m_VR->m_HolsterHandInOwnZone.load(std::memory_order_acquire) &&
                !m_VR->m_HolsterHandOverOtherZone.load(std::memory_order_acquire);
            {
                // A hand took the gun off the chest sling: its grip owns it.
                static uint32_t s_chestSlingTakeSerial = 0u;
                const uint32_t takeSerial = m_VR->m_ChestSlingTakeSerial.load(std::memory_order_acquire);
                if (takeSerial != s_chestSlingTakeSerial)
                {
                    s_chestSlingTakeSerial = takeSerial;
                    s_chestSlingAdoptUntilMs = GetTickCount64() + 800u;
                }
                // The other hand's take changes hands first (next tracking
                // update): keep asking until that hand's grip is down.
                if (s_chestSlingAdoptUntilMs != 0u && GetTickCount64() < s_chestSlingAdoptUntilMs &&
                    weaponGripActionActive && weaponGripDown)
                {
                    s_pendingGripAdoption = activeWeaponTag;
                    s_chestSlingAdoptUntilMs = 0u;
                }
                else if (s_chestSlingAdoptUntilMs != 0u && GetTickCount64() >= s_chestSlingAdoptUntilMs)
                {
                    s_chestSlingAdoptUntilMs = 0u;
                }
            }
            if (!weaponGripActionActive || !weaponGripDown || s_pendingGripAdoption != activeWeaponTag)
                s_pendingGripAdoption = 0u;
            if (gripDropEligible && s_pendingGripAdoption != 0u)
            {
                s_weaponGripRelease.AdoptWeapon(activeWeaponTag);
                if (pistolGripMode) s_pistolOwnership.AdoptPickup(l4d2vr_dual::Hand::Right);
                s_pendingGripAdoption = 0u;
                Game::logMsg("[VR][GripPickup] confirmed weaponId=%d; held grip adopted", activeWeaponId);
            }
            // The item the grip let go of is back in the hand (the host kept it,
            // or picked it straight back up from the feet): the grip owns it
            // again, so the next squeeze lets it go instead of doing nothing.
            if (s_gripDropReturn.Returned(gripDropEligible && !pistolGripMode ? activeWeaponTag : 0u,
                    weaponGripActionActive && weaponGripDown, gripNowMs) && !s_weaponGripRelease.Owns())
            {
                s_weaponGripRelease.ReclaimWeapon(activeWeaponTag);
                m_VR->m_GripDropReclaims.fetch_add(1u, std::memory_order_acq_rel);
                Game::logMsg("[VR][GripDrop] the dropped item is back in the hand weaponId=%d; the grip owns it again",
                    activeWeaponId);
            }
            // 2.0.11: the toggle grip still holds the item it took (published by
            // ProcessInput once it settled) but the latch forgot it (a pause or
            // menu, Use with a grenade, a mounted gun, the runtime dashboard):
            // the grip owns it again, so the next squeeze lets it go.
            {
                const uint32_t toggleHeldTag = m_VR->m_GripToggleHeldTag.load(std::memory_order_acquire);
                if (gripDropEligible && !pistolGripMode && !s_weaponGripRelease.Owns() && toggleHeldTag != 0u &&
                    toggleHeldTag == static_cast<uint32_t>(activeWeaponTag) && weaponGripActionActive && weaponGripDown)
                {
                    s_weaponGripRelease.AdoptWeapon(activeWeaponTag);
                    if (m_VR->m_VrHandsDebugLog)
                        Game::logMsg("[VR][GripToggle] held item re-adopted by the grip weaponId=%d", activeWeaponId);
                }
            }
            // Diagnostics: did the grip own the held item before this sample's release?
            const bool gripOwnedBeforeRelease = s_weaponGripRelease.Owns() ||
                (pistolGripMode && s_pistolOwnership.Owns(l4d2vr_dual::Hand::Right));
            // A physical dual reload moves the guns between the hands: no grip
            // lets a pistol go while it runs (the grips take nothing either).
            const bool dualReloadHoldsGuns = m_VR->DualReloadActive();
            if (dualReloadHoldsGuns) s_pistolOwnership.CancelInteractions();
            l4d2vr_dual::Hand pistolDropHand = pistolGripMode && gripDropEligible && !dualReloadHoldsGuns
                ? s_pistolOwnership.Release(weaponGripActionActive, weaponGripDown,
                    (leftGrip & 1u) != 0u, (leftGrip & 2u) != 0u, gripNowMs,
                    l4d2vr_input::GripSample::Generation(weaponGripInputState),
                    l4d2vr_input::GripSample::Generation(leftGrip), gripDropHere)
                : l4d2vr_dual::Hand::None;
            m_VR->m_PistolOwnershipMask.store(s_pistolOwnership.Mask(), std::memory_order_release);
			const bool gripReleaseRequested = s_weaponGripRelease.Update(
                gripDropEligible && !pistolGripMode && !dualReloadHoldsGuns,
				weaponGripActionActive,
				weaponGripDown,
				activeWeaponTag, l4d2vr_input::GripSample::Generation(weaponGripInputState), gripDropHere) ||
                pistolDropHand != l4d2vr_dual::Hand::None;
            // Empty hands (HolsterEmptyHands): an item already put away stays
            // where it is; a release processed late must not drop it.
            const bool alreadyStowed = gripReleaseRequested && m_VR->ChestSlingActive();
            // Forefront: a single pistol (or the magnum, a melee weapon, the
            // chainsaw) let go near the body goes back to the hip holster.
            const bool sidearmCaught = gripReleaseRequested && !alreadyStowed &&
                pistolDropHand == l4d2vr_dual::Hand::Right && !nativeDual &&
                m_VR->HolsterCatchRelease(activeWeaponTag, activeWeaponId);
            if (sidearmCaught)
                pistolDropHand = l4d2vr_dual::Hand::None;
            // Chest sling: a long gun let go near the body hangs on the chest
            // instead of dropping (vr_chest_sling.h); nothing goes to the server.
            const bool slingCaught = alreadyStowed || sidearmCaught || (gripReleaseRequested &&
                pistolDropHand == l4d2vr_dual::Hand::None && !carriedProp &&
                (l4d2vr_chest_sling::IsLongGun(activeWeaponId)
                    ? m_VR->ChestSlingCatchRelease(activeWeaponTag, activeWeaponId)
                    : m_VR->HolsterCatchRelease(activeWeaponTag, activeWeaponId)));
            const bool gripDropRequested = gripReleaseRequested && !slingCaught;
			// Hand throw: tap the trigger (pin), let go of the grip to throw; or
			// hold the trigger through the swing and let go of it.
			bool handThrowNow = false;
			{
				l4d2vr_hand_throw::ReleaseInput throwInput{};
				throwInput.throwable = handThrowMode;
				throwInput.weapon = activeWeaponTag;
				throwInput.blocked = !gripSession.gameplay || !throwActionsUsable || physicalUseDown ||
					emptyHandsPlaceholderActive || m_VR->ChestSlingActive() || m_VR->m_SuppressPlayerInput || cmd->command_number <= 0 ||
					m_VR->m_RenderPlayerIncap.load(std::memory_order_relaxed) ||
					m_VR->m_RenderPlayerControlledBySI.load(std::memory_order_relaxed) || IsLocalClientUsingMountedWeapon();
				throwInput.gripOwned = gripOwnedBeforeRelease;
				throwInput.gripReleased = gripDropRequested && pistolDropHand == l4d2vr_dual::Hand::None;
				throwInput.triggerDown = physicalAttackDown;
				throwInput.pinRequired = m_VR->m_ManualThrowPinRequired;
				throwInput.nowMs = GetTickCount64();
				const l4d2vr_hand_throw::Action throwAction = s_handThrowFsm.Update(throwInput);
				if (throwAction == l4d2vr_hand_throw::Action::PinPulled)
				{
					m_VR->TriggerPhysicalHandHapticPulse(m_VR->ThrowableHandIsLeft(), 0.025f, 140.0f, 0.2f, 2);
					Game::logMsg("[VR][HandThrow] pin pulled cmd=%d weaponId=%d gripOwned=%d", cmd->command_number,
						activeWeaponId, gripOwnedBeforeRelease ? 1 : 0);
				}
				if (throwAction == l4d2vr_hand_throw::Action::Throw)
				{
					const bool leftHand = m_VR->ThrowableHandIsLeft();
					Vector handVelocity{};
					const bool measured = m_VR->EstimateHandThrowVelocity(leftHand, handVelocity);
					VR::HandThrowUsercmd entry{};
					entry.command = cmd->command_number;
					entry.position = leftHand ? m_VR->m_LeftControllerPosAbs : m_VR->GetRightControllerViewmodelAbsPos();
					entry.angles = leftHand ? m_VR->m_LeftControllerTrackedAngAbs : m_VR->GetRightControllerAbsAngle();
					// Physics throw assistance (GrenadeThrowAssist): bend the release toward
					// where the hand points when it already heads there.
					if (measured)
					{
						const int assistIdx = m_Game->m_EngineClient->GetLocalPlayer();
						C_BasePlayer* const assistPlayer = assistIdx > 0
							? reinterpret_cast<C_BasePlayer*>(m_Game->GetClientEntity(assistIdx)) : nullptr;
						if (assistPlayer)
							m_VR->ApplyGrenadeThrowAssist(assistPlayer, entry.position, entry.angles, assistPlayer->m_vecVelocity,
								m_VR->m_ManualThrowHandVelocityScale, handVelocity);
					}
					entry.velocity = handVelocity;
					entry.kind = static_cast<int>(handThrowKind);
					entry.leftHand = leftHand;
					m_VR->QueueHandThrowUsercmd(entry);
					{
						std::lock_guard<std::mutex> lock(m_VR->m_HandThrowMutex);
						m_VR->m_HandThrowLastReleasePos = entry.position;
						m_VR->m_HandThrowLastReleaseVelocity = handVelocity;
					}
					cmd->impulse = l4d2vr_hand_throw::kImpulse;
					handThrowNow = true;
					const uint64_t releaseMs = GetTickCount64();
					m_VR->m_HandThrowLastReleaseMs.store(releaseMs, std::memory_order_release);
					m_VR->m_HandThrowHideUntilMs.store(releaseMs + 1000u, std::memory_order_release);
					m_VR->m_HandThrowCount.fetch_add(1u, std::memory_order_acq_rel);
					m_VR->TriggerPhysicalHandHapticPulse(leftHand, 0.04f, 90.0f, 0.5f, 1);
					const float speedMps = handVelocity.Length() / std::max(1.0f, m_VR->m_VRScale);
					Game::logMsg("[VR][HandThrow] client release cmd=%d weaponId=%d hand=%s by=%s handVelocity=(%.0f %.0f %.0f) speed=%.2f m/s measured=%d pos=(%.1f %.1f %.1f) t=%llu",
						cmd->command_number, activeWeaponId, leftHand ? "left" : "right",
						throwInput.gripReleased ? "grip" : "trigger", handVelocity.x, handVelocity.y, handVelocity.z,
						speedMps, measured ? 1 : 0, entry.position.x, entry.position.y, entry.position.z,
						static_cast<unsigned long long>(releaseMs));
				}
				// The thrown grenade stays hidden until the server switches the hand empty.
				if (!handThrowMode)
					m_VR->m_HandThrowHideUntilMs.store(0u, std::memory_order_release);
			}
            // 2.0.12 (review s12 12/13): a release no holster caught and no throw
            // used is never a drop (vr_drop_policy.h): the item stays in the hand
            // and the grip keeps owning it, so the next squeeze behaves the same.
            // Sidearms can never be dropped. The deliberate drop of a later
            // button/chord comes in through ConsumeDeliberateDropRequest.
            l4d2vr_drop::ReleaseInput dropInput{};
            dropInput.released = gripDropRequested;
            dropInput.weaponId = pistolDropHand != l4d2vr_dual::Hand::None
                ? static_cast<int>(C_WeaponCSBase::WeaponID::PISTOL) : activeWeaponId;
            dropInput.thrown = handThrowNow;
            dropInput.carriedProp = carriedProp;
            dropInput.deliberateDrop = gripDropRequested && m_VR->ConsumeDeliberateDropRequest();
            const l4d2vr_drop::Outcome dropOutcome = l4d2vr_drop::Resolve(dropInput);
            const bool gripDropSend = dropOutcome == l4d2vr_drop::Outcome::Drop;
            if (dropOutcome == l4d2vr_drop::Outcome::Keep)
            {
                if (pistolDropHand != l4d2vr_dual::Hand::None)
                    s_pistolOwnership.AdoptPickup(pistolDropHand);
                else
                    s_weaponGripRelease.ReclaimWeapon(activeWeaponTag);
                // The toggle grip holds it again (ProcessInput).
                if (pistolDropHand != l4d2vr_dual::Hand::Left)
                    m_VR->m_GripToggleRehold.store(true, std::memory_order_release);
                m_VR->m_GripReleaseKeptCount.fetch_add(1u, std::memory_order_acq_rel);
                Game::logMsg("[VR][GripDrop] release away from a holster cmd=%d weaponId=%d hand=%s: %s (dropping is off)",
                    cmd->command_number, activeWeaponId,
                    pistolDropHand == l4d2vr_dual::Hand::Left ? "left pistol" : "item",
                    l4d2vr_drop::OutcomeName(dropOutcome));
            }
            // Sent below with the release command (not for a carried prop or a
            // pistol hand): watch whether the item comes back to the hand.
            if (gripDropSend && manualThrowInputActive && !carriedProp && pistolDropHand == l4d2vr_dual::Hand::None)
                s_gripDropReturn.Dropped(activeWeaponTag, gripNowMs);
			{
				// One line per weapon-grip release: why it did or did not drop.
				static bool s_prevGripDownForDropLog = false;
				const bool gripDownNow = weaponGripActionActive && weaponGripDown;
				if (s_prevGripDownForDropLog && !gripDownNow && (holsterGripHoldMode || m_VR->m_GripReleaseDropEnabled))
				{
					Game::logMsg("[VR][GripDrop] release: drop=%d owned=%d eligible=%d proto=%d gameplay=%d(blocked 0x%X) throwActive=%d backend=%d inventoryItem=%d ownZone=%d otherZone=%d canRelease=%d weaponId=%d",
						gripDropSend ? 1 : 0, gripOwnedBeforeRelease ? 1 : 0, gripDropEligible ? 1 : 0,
						l4d2vr_wire::SupportsPhysicalVersion(m_VR->m_ServerPhysicalInteractionVersion.load(std::memory_order_acquire)) ? 1 : 0,
						gripSession.gameplay ? 1 : 0, gripSession.blocked, manualThrowInputActive ? 1 : 0, throwBackendReady ? 1 : 0,
						inventoryItem ? 1 : 0,
						m_VR->m_HolsterHandInOwnZone.load(std::memory_order_acquire) ? 1 : 0,
						m_VR->m_HolsterHandOverOtherZone.load(std::memory_order_acquire) ? 1 : 0,
						l4d2vr_grip::CanReleaseInventoryItem(ManualInventoryThrowWeaponIdDoesMeleeDamage(activeWeaponId),
							physicalAttackDown, s_manualCarryThrowPhysicalAttackDown, physicalUseDown, s_manualInventoryThrowArmed) ? 1 : 0,
						activeWeaponId);
				}
				s_prevGripDownForDropLog = gripDownNow;
			}
			if (m_VR->m_GripReleaseDropEnabled && m_VR->m_VrHandsDebugLog)
			{
				static uint32_t previousGripGateState = ~0u;
				const uint32_t gateState = weaponGripInputState |
					(s_ServerUnderstandsVR ? 1u << 2 : 0u) |
					(throwBackendReady ? 1u << 3 : 0u) |
					(inventoryItem ? 1u << 4 : 0u) |
					(m_VR->m_ForceNonVRServerMovement ? 1u << 5 : 0u) |
					(m_VR->m_FirstPersonControlReady.load(std::memory_order_acquire) ? 1u << 6 : 0u) |
                    (gripDropEligible ? 1u << 7 : 0u) | (gripSession.blocked << 8) |
                    (m_VR->m_ServerPhysicalInteractionVersion.load(std::memory_order_acquire) == 1u ? 1u << 25 : 0u);
				if (gateState != previousGripGateState)
				{
					previousGripGateState = gateState;
					Game::logMsg("[VR][GripDrop][input] active=%d down=%d server=%d backend=%d inventory=%d fallback=%d controlReady=%d weaponId=%d eligible=%d blockers=0x%X armed=%d waitRelease=%d requested=%d extraSamples=%u weapon=%p physicalProtocol=%u",
						weaponGripActionActive ? 1 : 0, weaponGripDown ? 1 : 0,
						s_ServerUnderstandsVR ? 1 : 0, throwBackendReady ? 1 : 0,
						inventoryItem ? 1 : 0, m_VR->m_ForceNonVRServerMovement ? 1 : 0,
						m_VR->m_FirstPersonControlReady.load(std::memory_order_acquire) ? 1 : 0, activeWeaponId,
                        gripDropEligible ? 1 : 0, gripSession.blocked,
                        s_weaponGripRelease.IsArmed() ? 1 : 0, s_weaponGripRelease.WaitingForRelease() ? 1 : 0,
                        gripDropSend ? 1 : 0, s_gripExtraSamples, reinterpret_cast<void*>(activeWeaponTag),
                        m_VR->m_ServerPhysicalInteractionVersion.load(std::memory_order_acquire));
				}
			}
			if (!manualThrowInputActive)
			{
				if (!throwActionsUsable)
				{
					cmd->buttons &= ~static_cast<int>(kManualCarryThrowWeaponMask);
					if (cmd->impulse == l4d2vr_grip::kReleaseImpulse)
						cmd->impulse = 0;
				}
				s_manualCarryThrowWeapon = 0;
				s_manualCarryThrowPhysicalAttackDown = physicalAttackDown;
				s_manualInventoryThrowPhysicalUseDown = physicalUseDown;
				s_manualInventoryThrowArmed = false;
			}
			else
			{
				if (activeWeaponTag != s_manualCarryThrowWeapon)
				{
					s_manualCarryThrowWeapon = activeWeaponTag;
					s_manualCarryThrowPhysicalAttackDown = physicalAttackDown;
					s_manualInventoryThrowPhysicalUseDown = physicalUseDown;
					s_manualInventoryThrowArmed = false;
				}

				uint32_t carryButtons = static_cast<uint32_t>(cmd->buttons);
				carryButtons &= ~kManualCarryThrowWeaponMask;
				if (carriedProp)
				{
					s_manualInventoryThrowArmed = false;
					// Swallow only stock primary attack so physical trigger release
					// controls the manual throw. IN_USE must remain untouched:
					// gascans, cola bottles and scripted carry objectives rely on
					// the game's native Use interaction.
					carryButtons &= ~kManualCarryThrowIN_ATTACK;
					if (!physicalAttackDown && s_manualCarryThrowPhysicalAttackDown)
					{
						carryButtons |= kManualCarryThrowIN_ATTACK;
						const uint32_t encodedWeaponId = static_cast<uint32_t>(carryWeaponId + 1);
						carryButtons |= (encodedWeaponId & 0x3Fu) << kManualCarryThrowWeaponShift;
						Game::logMsg(
							"[VR][ManualCarryThrow] client release cmd=%d weaponId=%d weapon=%s netclass=%s",
							cmd->command_number,
							carryWeaponId,
							activeWeaponName ? activeWeaponName : "",
							activeWeaponNetClass ? activeWeaponNetClass : "");
					}
				}
				else
				{
					const bool triggerPressedAfterUse =
						physicalAttackDown && !s_manualCarryThrowPhysicalAttackDown &&
						physicalUseDown && s_manualInventoryThrowPhysicalUseDown;
					if (gripDropSend)
					{
						m_VR->CancelMagazineInteractionManual();
						m_VR->m_VrHandsTwoHandedGripActive = false;
						m_VR->m_VrHandsTwoHandedGripWeaponId = 0;
						m_VR->m_VrHandsTwoHandedGripWeaponTag = 0;
						m_VR->m_Game->ClientCmd_Unrestricted("-reload");
						m_VR->m_ReloadCmdOwned = false;
						s_manualInventoryThrowArmed = false;
						// Reuse the existing, server-validated Weapon_Drop request.
						// Clear attack/use/reload so a dropped grenade stays unarmed.
						carryButtons &= ~(kManualCarryThrowIN_ATTACK | kManualInventoryThrowIN_USE | (1u << 13));
						const uint32_t encodedWeaponId = pistolDropHand == l4d2vr_dual::Hand::Left
                            ? l4d2vr_pistol::kLeftDropMarker : pistolDropHand == l4d2vr_dual::Hand::Right
                            ? l4d2vr_pistol::kRightDropMarker : static_cast<uint32_t>(activeWeaponId + 1);
                        pistolInteractionHand = pistolDropHand != l4d2vr_dual::Hand::None
                            ? pistolDropHand : l4d2vr_dual::Hand::Right;
                        cmd->impulse = l4d2vr_grip::kReleaseImpulse;
						carryButtons |= (encodedWeaponId & 0x3Fu) << kManualCarryThrowWeaponShift;
						Game::logMsg("[VR][GripDrop] client release cmd=%d weaponId=%d", cmd->command_number, activeWeaponId);
					}
					if (!gripDropSend && !s_manualInventoryThrowArmed && triggerPressedAfterUse)
						s_manualInventoryThrowArmed = true;
					// The trigger pulls the pin; the grenade's native attack never starts.
					if (handThrowMode)
						carryButtons &= ~kManualCarryThrowIN_ATTACK;

					if (s_manualInventoryThrowArmed)
					{
						// Suppress the first trigger-down command too. This keeps
						// molotov/pipe/vomitjar out of their stock attack state.
						carryButtons &= ~(kManualCarryThrowIN_ATTACK | kManualInventoryThrowIN_USE);
					}
					if (s_manualInventoryThrowArmed &&
						!physicalAttackDown && s_manualCarryThrowPhysicalAttackDown)
					{
						const uint32_t encodedWeaponId = static_cast<uint32_t>(activeWeaponId + 1);
						carryButtons |= (encodedWeaponId & 0x3Fu) << kManualCarryThrowWeaponShift;
						Game::logMsg(
							"[VR][ManualInventoryThrow] client release cmd=%d weaponId=%d weapon=%s netclass=%s",
							cmd->command_number,
							activeWeaponId,
							activeWeaponName ? activeWeaponName : "",
							activeWeaponNetClass ? activeWeaponNetClass : "");
						s_manualInventoryThrowArmed = false;
					}
				}
				cmd->buttons = static_cast<int>(carryButtons);

				s_manualCarryThrowPhysicalAttackDown = physicalAttackDown;
				s_manualInventoryThrowPhysicalUseDown = physicalUseDown;
			}
		}

		// HandThrow servers: a grenade never enters the native pin-pull/throw
		// path (its 0.5 s pin animation and 0.2 s throw timer); the trigger
		// only pulls the pin and the throw leaves the hand (impulse 244).
		if (localPlayerForAutoActions && m_VR->HandThrowServerReady())
		{
			C_WeaponCSBase* const grenade =
				reinterpret_cast<C_WeaponCSBase*>(localPlayerForAutoActions->GetActiveWeapon());
			if (grenade && l4d2vr_hand_throw::KindFromWeaponId(static_cast<int>(grenade->GetWeaponID())) !=
				l4d2vr_hand_throw::Kind::None)
				cmd->buttons &= ~(1 << 0);
		}

		if (m_VR->m_ManualInventoryEmptyHandsActive.load(std::memory_order_acquire))
		{
			// The server repeats this filter authoritatively. Keeping the client
			// command clean also prevents muzzle flash, recoil and reload animation
			// from being predicted for the hidden placeholder pistol.
			constexpr int kEmptyHandsIN_ATTACK = (1 << 0);
			constexpr int kEmptyHandsIN_RELOAD = (1 << 13);
			cmd->buttons &= ~(kEmptyHandsIN_ATTACK | kEmptyHandsIN_RELOAD);
			s_manualInventoryThrowArmed = false;
		}

		// Publish the final command state after every helper has had a chance to
		// suppress or synthesize IN_ATTACK. The render thread uses one atomic word
		// so queued draws never observe a throwable/trigger mismatch.
		constexpr uint32_t kManualThrowViewmodelThrowableActive = 1u << 0;
		constexpr uint32_t kManualThrowViewmodelTriggerHeld = 1u << 1;
		uint32_t manualThrowViewmodelInputState = 0u;
		if (m_VR->m_IsVREnabled && m_VR->m_ManualThrowEnabled && !m_VR->GrenadeArcMode() && localPlayerForAutoActions)
		{
			C_WeaponCSBase* manualThrowWeapon =
				reinterpret_cast<C_WeaponCSBase*>(localPlayerForAutoActions->GetActiveWeapon());
			const int manualThrowWeaponId = manualThrowWeapon
				? static_cast<int>(manualThrowWeapon->GetWeaponID())
				: static_cast<int>(C_WeaponCSBase::WeaponID::NONE);
			const bool nativeThrowable = IsVRThrowableWeapon(manualThrowWeapon, nullptr, nullptr);
			const bool inventoryThrowable = ManualInventoryThrowWeaponIdIsSupported(manualThrowWeaponId);
			if (nativeThrowable || inventoryThrowable)
			{
				manualThrowViewmodelInputState |= kManualThrowViewmodelThrowableActive;
				if ((nativeThrowable && (cmd->buttons & (1 << 0)) != 0) ||
					(inventoryThrowable && s_manualInventoryThrowArmed))
					manualThrowViewmodelInputState |= kManualThrowViewmodelTriggerHeld;
			}
		}
		m_VR->m_ManualThrowViewmodelInputState.store(
			manualThrowViewmodelInputState,
			std::memory_order_release);
	}

    if (m_VR && m_VR->m_IsVREnabled && !m_VR->m_SuppressPlayerInput &&
        IsClientAttachedToLadder(m_Game, localPlayerForAutoActions))
        cmd->buttons = l4d2vr_ladder::DirectionButtons(cmd->buttons, cmd->forwardmove, cmd->sidemove);

    l4d2vr_dual::Hand dualShotHand = l4d2vr_dual::Hand::None;
    const unsigned pistolMask = m_VR ? m_VR->m_PistolOwnershipMask.load(std::memory_order_acquire) : 0u;
    C_WeaponCSBase* routingPistol = localPlayerForAutoActions
        ? reinterpret_cast<C_WeaponCSBase*>(localPlayerForAutoActions->GetActiveWeapon()) : nullptr;
    if (m_VR) m_VR->UpdatePistolAmmoSnapshot(localPlayerForAutoActions, routingPistol);
    // Per-hand triggers exist only with VR controllers: desktop mode and mouse
    // mode keep the stock IN_ATTACK (B06).
    const bool dualNegotiated = m_VR && m_VR->m_IsVREnabled && !m_VR->m_MouseModeEnabled &&
        l4d2vr_wire::SupportsPhysicalVersion(m_VR->m_ServerPhysicalInteractionVersion.load(std::memory_order_acquire));
    // Without the server protocol, akimbo still routes each trigger to its own
    // pistol; the command's view angles carry that hand's aim instead.
    const bool dualClientOnly = m_VR && !dualNegotiated && m_VR->m_DualPistolsAkimboOnAnyServer &&
        m_VR->m_IsVREnabled && !m_VR->m_MouseModeEnabled && routingPistol && m_Game->IsDualPistolWeapon(routingPistol);
    const bool dualEquipped = m_VR && routingPistol &&
        (dualNegotiated || dualClientOnly) &&
        routingPistol->GetWeaponID() == C_WeaponCSBase::WeaponID::PISTOL &&
        (m_Game->IsDualPistolWeapon(routingPistol) || pistolMask == 2u) &&
        m_VR->m_DualPistolsIndependentHandsEnabled;
    if (dualEquipped)
    {
        const uint32_t triggers = m_VR->m_DualPistolTriggerState.load(std::memory_order_acquire);
        int clip = -1;
        clip = ReadNetvar<int>(routingPistol, VR::kClip1Offset);
        const bool serverPathReady = dualClientOnly ||
            (m_VR->m_EncodeVRUsercmd && !m_VR->m_ForceNonVRServerMovement && s_ServerUnderstandsVR);
        // 2.0.12 (s12 issue 9): a downed survivor fires the dual pistols (the
        // base game allows it and the server's akimbo has no incap block);
        // only hanging from a ledge keeps them down.
        const bool eligible = serverPathReady && !m_VR->m_SuppressPlayerInput &&
            m_VR->m_FirstPersonControlReady.load(std::memory_order_acquire) &&
            !m_VR->m_RenderPlayerLedge.load(std::memory_order_relaxed) &&
            !m_VR->m_RenderPlayerControlledBySI.load(std::memory_order_relaxed) &&
            !m_VR->IsMagazineInteractionBlockingFire() && !s_pistolOwnership.Awaiting() &&
            (cmd->buttons & (1u << 5)) == 0u;
        unsigned firingMask = pistolMask ? pistolMask : 3u;
        const uint64_t handAmmo = m_VR->m_PistolAmmoCounts.load(std::memory_order_acquire);
        // While the stock reload owns the guns (downed on a server without
        // IncapReload) the per-hand magazine counts are not kept: the clip decides.
        if (m_Game->GetConVarIntDirect("sv_infinite_ammo", -1) == 0 && !m_VR->IncapBlocksPhysicalReload())
            firingMask &= l4d2vr_pistol_prediction::FiringMask(handAmmo);
        dualShotHand = s_dualPistolTriggers.Update(eligible,
            (firingMask & 1u) != 0u && (triggers & 2u) != 0u,
            (firingMask & 2u) != 0u && (triggers & 8u) != 0u, clip);
        cmd->buttons &= ~1u;
        if ((triggers & 8u) != 0u) cmd->buttons &= ~(1u << 11);
        if (dualShotHand != l4d2vr_dual::Hand::None)
        {
            cmd->buttons |= 1u;
            const bool leftShot = dualShotHand == l4d2vr_dual::Hand::Left;
            QAngle handAim{};
            if (dualClientOnly && localPlayerForAutoActions &&
                m_VR->GetHandAimViewAngles(leftShot, localPlayerForAutoActions->EyePosition(), handAim))
            {
                // The server fires from the eye: aim the eye at where this hand points.
                SetCmdViewAnglesKeepMove(cmd, handAim);
            }
            else
            {
                SetCmdViewAnglesKeepMove(cmd,
                    leftShot ? m_VR->GetLeftControllerAbsAngle() : m_VR->GetRightControllerAbsAngle());
            }
        }
    }
    else
        s_dualPistolTriggers.Reset();
    if (m_VR)
    {
        const auto poseHand = dualShotHand != l4d2vr_dual::Hand::None ? dualShotHand :
            pistolInteractionHand != l4d2vr_dual::Hand::None ? pistolInteractionHand :
            dualEquipped && pistolMask == 2u ? l4d2vr_dual::Hand::Left : l4d2vr_dual::Hand::None;
        m_VR->RecordDualPistolCommand(cmd->command_number, poseHand, dualShotHand != l4d2vr_dual::Hand::None,
            reinterpret_cast<uintptr_t>(localPlayerForAutoActions), reinterpret_cast<uintptr_t>(routingPistol));
    }

	bool manualThrowPoseRelevant = false;
	if (m_VR && m_VR->m_IsVREnabled && m_VR->m_ManualThrowEnabled &&
		m_VR->m_EncodeVRUsercmd && !m_VR->m_ForceNonVRServerMovement &&
		localPlayerForAutoActions)
	{
		C_BaseCombatWeapon* poseBaseWeapon = reinterpret_cast<C_BaseCombatWeapon*>(
			localPlayerForAutoActions->GetActiveWeapon());
		C_WeaponCSBase* poseWeapon = reinterpret_cast<C_WeaponCSBase*>(poseBaseWeapon);
		const char* poseWeaponName = poseBaseWeapon ? poseBaseWeapon->GetName() : nullptr;
		const char* poseWeaponNetClass = poseBaseWeapon
			? m_Game->GetNetworkClassName(reinterpret_cast<uintptr_t*>(poseBaseWeapon))
			: nullptr;
		const int poseWeaponId = poseWeapon
			? static_cast<int>(poseWeapon->GetWeaponID())
			: static_cast<int>(C_WeaponCSBase::WeaponID::NONE);

		manualThrowPoseRelevant =
			IsVRThrowableWeapon(poseWeapon, poseWeaponName, poseWeaponNetClass) ||
			ResolveVRManualCarryThrowWeaponId(
				poseWeapon,
				poseWeaponName,
				poseWeaponNetClass) != static_cast<int>(C_WeaponCSBase::WeaponID::NONE) ||
			ManualInventoryThrowWeaponIdIsSupported(poseWeaponId);
	}
	CacheManualThrowUsercmdControllerPose(
		m_VR,
		m_Game,
		cmd->command_number,
		manualThrowPoseRelevant);

    // Servers with the shove-aim feature run the shove themselves, aimed along
    // the hand from the encoded pose; the view-angle lock above covers the rest.
    if (m_VR && m_VR->m_PhysicalShoveImpulsePending.exchange(false, std::memory_order_acq_rel) &&
        cmd->impulse == 0 && m_VR->m_IsVREnabled && Hooks::s_ServerUnderstandsVR &&
        m_VR->ServerFeature(l4d2vr_features::ShoveAim))
        cmd->impulse = 238;

    if (m_VR && routingPistol)
    {
        // The host's reload gate follows every command: one without the block
        // lets the stock (auto) reload refill an empty gun with no magazine.
        const int gateWeaponId = static_cast<int>(routingPistol->GetWeaponID());
        uint32_t gateGap = 0u;
        if (cmd->impulse != 0) gateGap |= 1u;
        if (!l4d2vr_wire::SupportsPhysicalVersion(m_VR->m_ServerPhysicalInteractionVersion.load(std::memory_order_acquire))) gateGap |= 2u;
        if (!m_VR->m_MagazineInteractionEnabled || !m_VR->m_MagazineInteractionSuppressEmptyClipAutoReload) gateGap |= 4u;
        if (!m_VR->m_VrHandsEnabled && !m_VR->m_NativeViewmodelHandsOnly) gateGap |= 8u;
        if (m_VR->IsMagazineInteractionReloadCommandActive()) gateGap |= 16u;
        if ((m_VR->m_DualPistolNativeReloadState.load(std::memory_order_acquire) & 1u) != 0u) gateGap |= 32u;
        // 2.0.12 (s12 issue 10): downed with a pistol or the magnum, the
        // physical reload works, so the block keeps being sent; the stock
        // reload is only let through when the physical one cannot run.
        if (m_VR->m_RenderPlayerIncap.load(std::memory_order_acquire) &&
            m_VR->IncapBlocksPhysicalReload())
            gateGap |= 64u;
        if (!l4d2vr_calibration::IsFirearm(gateWeaponId)) gateGap |= 128u;
        else if (!(l4d2vr_shell::IsShotgun(gateWeaponId) ? m_VR->IsMagazineInteractionShotgunServerHookActive(gateWeaponId) :
            m_VR->IsMagazineInteractionDetachableBackendActive(gateWeaponId))) gateGap |= 256u;
        // 2.0.12 (s12 issue 14, E6, G3): with a StickyReloadGate server the
        // block stays standing per player until an explicit allow, so the
        // commands built for the previous item after a server-side weapon
        // switch (throw, heal, pickup, running dry) cannot open it.
        const bool stickyGate = Hooks::s_ServerUnderstandsVR &&
            m_VR->ServerFeature(l4d2vr_features::StickyReloadGate);
        const auto gateImpulse = l4d2vr_reload_rules::ResolveGateImpulse(gateGap, stickyGate);
        if (gateImpulse == l4d2vr_reload_rules::GateImpulse::StickyBlock)
            cmd->impulse = l4d2vr_grip::kStickyBlockNativeReloadImpulse;
        else if (gateImpulse == l4d2vr_reload_rules::GateImpulse::Allow)
            cmd->impulse = l4d2vr_grip::kAllowNativeReloadImpulse;
        if (!gateGap)
        {
            if (!stickyGate) cmd->impulse = l4d2vr_grip::kBlockNativeReloadImpulse;
        }
        else if ((gateGap & (2u | 4u | 8u | 128u | 256u)) == 0u)
        {
            // Only commands that should have carried the block count as gaps.
            m_VR->m_ReloadGateGapCommands.fetch_add(1u, std::memory_order_acq_rel);
            m_VR->m_ReloadGateGapReason.store(gateGap | (static_cast<uint32_t>(static_cast<uint8_t>(cmd->impulse)) << 16),
                std::memory_order_release);
        }
    }

	if (m_Game && m_VR)
		m_Game->PublishLocalVRPose(m_VR, localPlayerForAutoActions);
    if (m_VR) m_VR->UpdateHmdFlashlightPose(localPlayerForAutoActions);

    if (m_VR)
    {
        const bool meleeEligible = gripSession.gameplay && routingPistol &&
            routingPistol->GetWeaponID() == C_WeaponCSBase::WeaponID::MELEE &&
            m_VR->m_EncodeVRUsercmd && !m_VR->m_ForceNonVRServerMovement &&
            !m_VR->m_AdjustingViewmodel && !m_VR->m_AdjustingScope;
        m_VR->RecordMeleeCommand(cmd->command_number, meleeEligible,
            reinterpret_cast<uintptr_t>(localPlayerForAutoActions), reinterpret_cast<uintptr_t>(routingPistol));
    }
    // A gun on the chest sling cannot fire, aim-shove or reload.
    // 2.0.11: only while the stowed gun is still the active weapon.
    if (m_VR && localPlayerForAutoActions &&
        m_VR->ChestSlingHolds(reinterpret_cast<uintptr_t>(localPlayerForAutoActions->GetActiveWeapon())))
        cmd->buttons = l4d2vr_chest_sling::FilterButtons(cmd->buttons, true);
    // The pose this firing command is fired along (sent with it, and used by
    // the listen host when it runs the command).
    if (m_VR)
        RecoilCacheShotPose(cmd->command_number, (cmd->buttons & 1) != 0);
    // 2.0.13: the stock-usercmd gate (vr_gameplay_mode.h). In classic nothing
    // above should have set a protocol impulse or marker bit; if anything did,
    // it never leaves the client.
    if (m_VR && m_VR->m_IsVREnabled)
    {
        const auto wire = m_VR->GameplayWirePolicy();
        const bool releaseReloadGate = Hooks::s_ServerUnderstandsVR &&
            m_VR->ServerFeature(l4d2vr_features::StickyReloadGate);
        const int impulseBefore = cmd->impulse;
        const int buttonsBefore = cmd->buttons;
        int impulse = cmd->impulse;
        int buttons = cmd->buttons;
        if (l4d2vr_gameplay_mode::SanitizeStockUsercmd(wire, impulse, buttons, releaseReloadGate))
        {
            cmd->impulse = static_cast<decltype(cmd->impulse)>(impulse);
            cmd->buttons = buttons;
            const uint32_t hits = m_VR->m_StockWireGateHits.fetch_add(1u, std::memory_order_acq_rel) + 1u;
            if (hits <= 8u || (hits % 500u) == 0u)
                Game::logMsg("[VR][GameplayMode] stock usercmd gate: removed impulse %d / buttons 0x%X (hit %u)",
                    impulseBefore & 0xff, static_cast<unsigned>(buttonsBefore) & l4d2vr_gameplay_mode::kProtocolButtonBits, hits);
        }
    }
	s_lastButtons = cmd->buttons;

	return result;
}

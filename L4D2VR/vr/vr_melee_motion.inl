// Motion melee: the melee weapon is welded to the hand at the grip and its
// rotation lags the controller (vr_melee_inertia.h); swings are detected from
// the lagged blade with speed and travel gates (vr_gesture.h); thrusts and
// gun bashes shove. The lag is published as a delta in the viewmodel's own
// frame so the render thread can re-apply it to its fresher controller pose;
// the usercmd pose uses the same lagged pose, so the server's melee sweep
// follows what the player sees and only runs while the swing is valid.

namespace
{
    bool MeleeMotionReadScriptNameRaw(Game* game, C_WeaponCSBase* weapon, char* name, size_t capacity)
    {
        if (!game || !weapon || !name || capacity < 2 || !game->m_Offsets ||
            !game->m_Offsets->GetMeleeWeaponInfoClient.address)
            return false;
        typedef CMeleeWeaponInfoStore* (__thiscall* GetMeleeInfoFn)(void*);
        const auto getInfo = reinterpret_cast<GetMeleeInfoFn>(game->m_Offsets->GetMeleeWeaponInfoClient.address);
        __try
        {
            CMeleeWeaponInfoStore* const info = getInfo(weapon);
            if (!info)
                return false;
            size_t length = 0;
            for (; length + 1 < capacity && length < sizeof(info->meleeWeaponName); ++length)
            {
                if (info->meleeWeaponName[length] == '\0')
                    break;
                name[length] = info->meleeWeaponName[length];
            }
            name[length] = '\0';
            return length > 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    std::string MeleeMotionReadScriptName(Game* game, C_WeaponCSBase* weapon)
    {
        char name[64]{};
        return MeleeMotionReadScriptNameRaw(game, weapon, name, sizeof(name)) ? std::string(name) : std::string();
    }
}

void VR::UpdateMeleeMotion(C_BasePlayer* localPlayer)
{
    const auto now = std::chrono::steady_clock::now();
    const float dt = m_MeleeMotionLastUpdate.time_since_epoch().count() == 0 ? 0.0f :
        std::clamp(std::chrono::duration<float>(now - m_MeleeMotionLastUpdate).count(), 0.0f, 0.1f);
    m_MeleeMotionLastUpdate = now;
    const float scale = std::max(0.001f, m_VRScale);
    // Seconds since the session clock's epoch: a float of the raw steady clock
    // (seconds since boot) is quantised to 1/32 s after a few days of uptime.
    float sampleTime = 0.0f;
    if (!m_GestureSessionClock.Seconds(std::chrono::duration<double>(now.time_since_epoch()).count(), sampleTime))
    {
        m_GestureSampler.Reset();
        m_MeleeSwing.Reset();
        m_MeleeThrust.Reset();
        m_GunBash.Reset();
        m_PillsGesture.Reset();
        m_AdrenalineGesture.Reset();
        m_HandoverGesture.Reset();
    }

    C_WeaponCSBase* const weapon = localPlayer ? static_cast<C_WeaponCSBase*>(localPlayer->GetActiveWeapon()) : nullptr;
    const int weaponId = weapon ? static_cast<int>(weapon->GetWeaponID()) : 0;
    const bool meleeActive = m_IsVREnabled && !m_MouseModeEnabled && weapon &&
        (weaponId == C_WeaponCSBase::WeaponID::MELEE || weaponId == C_WeaponCSBase::WeaponID::CHAINSAW);
    if (weapon != m_MeleeMotionWeapon)
    {
        m_MeleeMotionWeapon = weapon;
        m_MeleeMotionScriptName = weaponId == C_WeaponCSBase::WeaponID::CHAINSAW ? std::string("chainsaw") :
            (weaponId == C_WeaponCSBase::WeaponID::MELEE ? MeleeMotionReadScriptName(m_Game, weapon) : std::string());
        m_MeleeMotionParams = l4d2vr_melee_inertia::ParamsForMeleeScriptName(m_MeleeMotionScriptName.c_str());
        m_MeleeInertiaSpring.SetParams(m_MeleeMotionParams);
        m_MeleeInertiaSpring.Clear();
        m_MeleeSwing.Reset();
        m_MeleeThrust.Reset();
        m_GunBash.Reset();
        if (meleeActive)
            Game::logMsg("[VR][Melee] %s: reach %.2f m (%s), lag spring %.1f Hz zeta %.2f limit %.0f deg, swing measured %.2f m out",
                m_MeleeMotionScriptName.empty() ? "unknown melee" : m_MeleeMotionScriptName.c_str(),
                m_MeleeMotionParams.reach, l4d2vr_melee_inertia::ShortWeapon(m_MeleeMotionParams) ? "short" : "long",
                m_MeleeMotionParams.rotationHz, m_MeleeMotionParams.rotationZeta, m_MeleeMotionParams.maxLagDeg,
                l4d2vr_melee_inertia::TipSampleDistance(m_MeleeMotionParams));
    }
    const l4d2vr_melee_inertia::Params& params = m_MeleeMotionParams;
    // The chainsaw runs while the trigger is held; it never swing-triggers.
    m_MeleeMotionSwingEligible = meleeActive && params.motionMelee;
    {
        // Swing gates for this weapon (config values may change at runtime).
        l4d2vr_gesture::SwingParams swingParams = m_MeleeSwing.GetParams();
        swingParams.bladeLength = l4d2vr_melee_inertia::TipSampleDistance(params);
        swingParams.startSpeed = m_MeleeSwingMinTipSpeed;
        swingParams.minHandleSpeed = l4d2vr_melee_inertia::ShortWeapon(params) ? m_MeleeSwingMinHandleSpeedShort :
            m_MeleeSwingMinHandleSpeedLong;
        swingParams.minTipTravel = m_MeleeSwingMinTipTravel;
        swingParams.minHandleTravel = m_MeleeSwingMinHandleTravel;
        m_MeleeSwing.SetParams(swingParams);
    }

    // Tracking frame (metres): the game's placement of tracked devices,
    // inverted. Smooth and snap turning pivot the world about the head, and
    // walking, teleports, ladders and pushes move the camera anchor; none of it
    // moves a point here, so neither the inertia spring nor the gesture
    // detectors see artificial locomotion as hand motion. (The old frame
    // pivoted about the camera anchor: turning swept hand and head round a
    // circle as large as the player's room offset.)
    l4d2vr_gesture::TrackingFrame trackingFrame{};
    trackingFrame.origin = m_CameraAnchor - Vector(0.0f, 0.0f, 64.0f);
    trackingFrame.yawDeg = m_RotationOffset;
    trackingFrame.hmdCorrected = m_HmdPosSmoothed;
    trackingFrame.hmdLocal = m_HmdPosLocalPrev;
    trackingFrame.scale = scale;
    auto toBodyP = [&](const Vector& p) { return trackingFrame.ToTrackingPoint(p); };
    auto toWorldP = [&](const Vector& p) { return trackingFrame.ToWorldPoint(p); };
    auto toBodyD = [&](const Vector& d) { return trackingFrame.ToTrackingDirection(d); };
    auto toWorldD = [&](const Vector& d) { return trackingFrame.ToWorldDirection(d); };
    float stickYawRate = 0.0f;
    if (dt > 0.0005f)
    {
        float yawStep = trackingFrame.yawDeg - m_GestureLastYawDeg;
        yawStep -= 360.0f * std::floor((yawStep + 180.0f) / 360.0f);
        stickYawRate = yawStep / dt;
    }
    m_GestureLastYawDeg = trackingFrame.yawDeg;

    // Inertia on the viewmodel pose (main thread copy; render re-applies the delta).
    // The spring runs on the grip (the controller the viewmodel offsets are
    // measured from): it stays welded to the hand while the weapon's rotation
    // lags and pivots about it.
    bool lagValid = false;
    l4d2vr_melee_inertia::Pose delta{};
    if (meleeActive && m_MeleeInertiaEnabled)
    {
        const Vector gripWorld = GetRightControllerViewmodelAbsPos();
        const Vector viewmodelPos = gripWorld - m_ViewmodelForward * m_ViewmodelPosOffset.x -
            m_ViewmodelRight * m_ViewmodelPosOffset.y - m_ViewmodelUp * m_ViewmodelPosOffset.z;
        l4d2vr_melee_inertia::Pose target{};
        if (l4d2vr_melee_inertia::PoseFromAxes(toBodyP(gripWorld), toBodyD(m_ViewmodelForward),
                toBodyD(m_ViewmodelRight), toBodyD(m_ViewmodelUp), target) && m_MeleeInertiaSpring.Step(target, dt))
        {
            const l4d2vr_melee_inertia::Pose laggedGrip = m_MeleeInertiaSpring.Lagged();
            // Rotation from the controller to the lagged weapon (tracking frame).
            const l4d2vr_melee_inertia::Quat bodyRotation = l4d2vr_melee_inertia::QuatNormalize(
                l4d2vr_melee_inertia::QuatMultiply(laggedGrip.orientation,
                    l4d2vr_melee_inertia::QuatConjugate(target.orientation)));
            const l4d2vr_melee_inertia::Pose targetViewmodel{ toBodyP(viewmodelPos), target.orientation };
            l4d2vr_melee_inertia::Pose laggedViewmodel =
                l4d2vr_melee_inertia::PivotAboutGrip(targetViewmodel, target.position, bodyRotation);
            laggedViewmodel.position += laggedGrip.position - target.position; // 0 while the grip is welded
            delta = l4d2vr_melee_inertia::LagDelta(targetViewmodel, laggedViewmodel);
            lagValid = l4d2vr_melee_inertia::PoseFinite(delta);
            if (lagValid)
            {
                Vector laggedForward{}, laggedRight{}, laggedUp{};
                l4d2vr_melee_inertia::QuatToAxes(laggedViewmodel.orientation, laggedForward, laggedRight, laggedUp);
                m_ViewmodelForward = toWorldD(laggedForward);
                m_ViewmodelRight = toWorldD(laggedRight);
                m_ViewmodelUp = toWorldD(laggedUp);
                // The viewmodel origin hangs off the grip along the lagged axes
                // (GetRecommendedViewmodelAbsPos); this is what remains (0 when welded).
                m_MeleeInertiaPosOffsetWorld = toWorldP(laggedViewmodel.position) - (gripWorld -
                    m_ViewmodelForward * m_ViewmodelPosOffset.x - m_ViewmodelRight * m_ViewmodelPosOffset.y -
                    m_ViewmodelUp * m_ViewmodelPosOffset.z);
                // The lagged controller pose for the usercmd (the server's melee sweep).
                Vector forward = toWorldD(l4d2vr_melee_inertia::QuatRotate(bodyRotation, toBodyD(m_RightControllerForward)));
                Vector up = toWorldD(l4d2vr_melee_inertia::QuatRotate(bodyRotation, toBodyD(m_RightControllerUp)));
                QAngle angles{};
                QAngle::VectorAngles(forward, up, angles);
                m_MeleeLaggedControllerAng = angles;
                m_MeleeLaggedControllerPos = toWorldP(laggedGrip.position);
                m_MeleeLagDeg.store(m_MeleeInertiaSpring.LagDeg(), std::memory_order_release);
            }
        }
    }
    else
    {
        m_MeleeInertiaSpring.Clear();
    }
    if (!lagValid)
        m_MeleeInertiaPosOffsetWorld = Vector(0.0f, 0.0f, 0.0f);
    {
        std::lock_guard<std::mutex> lock(m_MeleeInertiaMutex);
        m_MeleeInertiaDelta = delta;
        m_MeleeInertiaDeltaValid = lagValid;
    }
    m_MeleeLaggedPoseValid = lagValid;

    // Gesture sample from the weapon hand (the lagged pose when it exists), in
    // tracking space. Without a lag the hand and head come straight from the
    // tracked poses this frame used; velocities of both are differentiated
    // over the same interval.
    Vector handForward = m_RightControllerForward;
    if (lagValid)
        QAngle::AngleVectors(m_MeleeLaggedControllerAng, &handForward, nullptr, nullptr);
    Vector handAngularDeg{};
    if (lagValid)
        handAngularDeg = m_MeleeInertiaSpring.GetState().angularVelocity * 57.29577951f;
    else
    {
        const QAngle av = m_RightControllerPose.TrackedDeviceAngVel;
        handAngularDeg = Vector(av.x, av.y, av.z);
    }
    const l4d2vr_gesture::Sample sample = m_GestureSampler.Build(sampleTime,
        lagValid ? toBodyP(m_MeleeLaggedControllerPos) : m_RightControllerPosSmoothed, toBodyD(handForward),
        handAngularDeg, m_HmdPosLocalPrev, toBodyD(m_HmdForward), toBodyD(m_HmdUp));

    bool swingStarted = false;
    if (m_MeleeMotionSwingEligible)
    {
        const l4d2vr_gesture::SwingEvent swing = m_MeleeSwing.Update(sample);
        m_MeleeSwingActive.store(swing.swinging, std::memory_order_release);
        const uint64_t nowMs = GetTickCount64();
        if (swing.started)
        {
            swingStarted = true;
            m_MeleeSwingDirection = toWorldD(swing.direction);
            m_MeleeSwingStartSerial.fetch_add(1u, std::memory_order_acq_rel);
            if (m_MotionOnlyMeleeAndShove)
                m_ViewmodelAnimSuppressUntilMs.store(nowMs + 900u, std::memory_order_release);
            // Like Ancient Dungeon, the hit is what you feel (NotifyMeleeHitConfirmed),
            // not the start of the swing.
            if (m_MeleeSwingStartHaptics)
                TriggerMeleeSwingHaptics(false);
            Game::logMsg("[VR][Melee] swing: tip %.1f m/s hand %.1f m/s, travel tip %.2f m hand %.2f m, lag %.0f deg, quality %.2f",
                swing.tipSpeed, swing.handleSpeed, swing.tipTravel, swing.handleTravel,
                m_MeleeLagDeg.load(std::memory_order_acquire),
                l4d2vr_gesture::SwingQuality(swing.tipSpeed, swing.tipTravel, swing.handleTravel));
        }
        else if (swing.armed && (swing.fastEnough || swing.handleFastEnough) &&
            (l4d2vr_selftest::Get().active.load(std::memory_order_acquire) || nowMs - m_MeleeSwingNearMissLogMs >= 1000u))
        {
            // Fast, but a gate said no (rate limited): why a swing did not count.
            m_MeleeSwingNearMissLogMs = nowMs;
            const l4d2vr_gesture::SwingParams& gates = m_MeleeSwing.GetParams();
            Game::logMsg("[VR][Melee] swing candidate: tip %.1f/%.1f m/s hand %.1f/%.1f m/s travel tip %.2f/%.2f m hand %.2f/%.2f m axial %.2f",
                swing.tipSpeed, gates.startSpeed, swing.handleSpeed, gates.minHandleSpeed, swing.tipTravel,
                gates.minTipTravel, swing.handleTravel, gates.minHandleTravel, swing.axial);
        }
    }
    else
    {
        m_MeleeSwingActive.store(false, std::memory_order_release);
    }

    // Consumables: bring pills to the mouth, stab adrenaline into the chest or
    // the other arm. Using them holds the stock attack until the item is used.
    if (m_ConsumableGesturesEnabled && weapon &&
        (weaponId == C_WeaponCSBase::WeaponID::PAIN_PILLS || weaponId == C_WeaponCSBase::WeaponID::ADRENALINE))
    {
        bool consumed = false;
        // Hold the item against a teammate's chest to hand it over.
        bool given = false;
        if (m_ConsumableGiveEnabled && m_Game && m_Game->m_ClientEntityList)
        {
            l4d2vr_gesture::HandoverTarget targets[32];
            Vector targetsWorld[32];
            int count = 0;
            const int highest = std::min(64, m_Game->m_ClientEntityList->GetHighestEntityIndex());
            for (int i = 1; i <= highest && count < 32; ++i)
            {
                C_BaseEntity* const entity = m_Game->GetClientEntity(i);
                if (!entity || entity == reinterpret_cast<C_BaseEntity*>(localPlayer))
                    continue;
                const unsigned char* const base = reinterpret_cast<const unsigned char*>(entity);
                unsigned char lifeState = 1;
                int team = 0;
                if (!VR_TryReadU8(base, kLifeStateOffset, lifeState) || !VR_TryReadI32(base, kTeamNumOffset, team) ||
                    lifeState != 0 || team != 2)
                    continue;
                Vector origin{};
                if (!VR_TryGetEntityAbsOrigin(entity, origin))
                    continue;
                const Vector chest = origin + Vector(0.0f, 0.0f, 46.0f);
                targets[count].id = i;
                targets[count].point = toBodyP(chest);
                targetsWorld[count] = chest;
                ++count;
            }
            const l4d2vr_gesture::HandoverEvent handover = m_HandoverGesture.Update(sample, targets, count);
            if (handover.fired)
            {
                given = true;
                for (int i = 0; i < count; ++i)
                    if (targets[i].id == handover.target)
                        m_PillGiveTargetPoint = targetsWorld[i];
                m_PillGiveImpulsePending.store(true, std::memory_order_release);
                m_PillGiveHoldUntil = now + std::chrono::milliseconds(500);
                TriggerPhysicalHandHapticPulse(IsGameplayHandLeftPhysical(false), 0.04f, 90.0f, 0.4f, 2);
                Game::logMsg("[VR][Consumable] %s handed to player %d",
                    weaponId == C_WeaponCSBase::WeaponID::PAIN_PILLS ? "pills" : "adrenaline", handover.target);
            }
        }
        if (given)
        {
            m_PillsGesture.Reset();
            m_AdrenalineGesture.Reset();
        }
        else if (weaponId == C_WeaponCSBase::WeaponID::PAIN_PILLS)
        {
            consumed = m_PillsGesture.Update(sample).fired;
        }
        else
        {
            l4d2vr_gesture::StabTargets targets{};
            l4d2vr_holster::BodyFrame frame{};
            if (GetHolsterBodyFrame(frame))
            {
                targets.chest = toBodyP(l4d2vr_holster::ToWorld(frame, Vector(0.12f, 0.0f, -0.30f)));
                targets.chestValid = true;
            }
            Vector arm{};
            if (l4d2vr_gesture::ArmTargetPoint(toBodyP(m_LeftControllerPosAbs),
                    toBodyD(m_LeftControllerForward), 0.15f, arm))
            {
                targets.arm = arm;
                targets.armValid = true;
            }
            consumed = m_AdrenalineGesture.Update(sample, targets).fired;
        }
        if (consumed)
        {
            m_ConsumableUseWeapon = weapon;
            m_ConsumableUseHoldUntil = now + std::chrono::milliseconds(1500);
            TriggerPhysicalHandHapticPulse(IsGameplayHandLeftPhysical(false), 0.04f, 110.0f, 0.4f, 2);
            Game::logMsg("[VR][Consumable] %s used by gesture",
                weaponId == C_WeaponCSBase::WeaponID::PAIN_PILLS ? "pills" : "adrenaline");
        }
    }
    else
    {
        m_PillsGesture.Reset();
        m_AdrenalineGesture.Reset();
        m_HandoverGesture.Reset();
    }
    if (weapon != m_ConsumableUseWeapon)
        m_ConsumableUseHoldUntil = {};

    // Shoves: thrust a melee weapon forward, or bash with a gun.
    if (!m_PhysicalShoveEnabled || !localPlayer || !weapon)
        return;
    bool shove = false;
    Vector shoveDirection{};
    if (meleeActive && !swingStarted)
    {
        const l4d2vr_gesture::ThrustEvent thrust = m_MeleeThrust.Update(sample);
        if (thrust.fired)
        {
            shove = true;
            shoveDirection = toWorldD(thrust.direction);
        }
    }
    else if (!meleeActive && l4d2vr_calibration::IsFirearm(weaponId))
    {
        // Reaching for or drawing from a holster is not a bash: no bash while
        // the gun hand is at a holster or just squeezed / let go of the grip.
        bool bashAllowed = GetTickCount64() >= m_GunBashGripBlockUntilMs.load(std::memory_order_acquire);
        l4d2vr_holster::BodyFrame holsterFrame{};
        if (bashAllowed && m_BodyGripInventoryEnabled && m_HolsterPavlovLayoutEnabled &&
            GetHolsterBodyFrame(holsterFrame) &&
            l4d2vr_holster::NearAnyZone(holsterFrame, m_HolsterLayout, m_RightControllerPosAbs, 0.05f))
            bashAllowed = false;
        l4d2vr_gesture::GunBashParams bashParams = m_GunBash.GetParams();
        if (bashParams.sweepEnabled != m_GunBashSweepEnabled)
        {
            bashParams.sweepEnabled = m_GunBashSweepEnabled;
            m_GunBash.SetParams(bashParams);
        }
        const l4d2vr_gesture::GunBashEvent bash = m_GunBash.Update(sample, bashAllowed);
        const uint64_t nowMs = GetTickCount64();
        if (nowMs < m_VmAnimTraceUntilMs.load(std::memory_order_acquire))
            Game::logMsg("[VR][Melee] bash trace: hand=(%.3f %.3f %.3f) vel=(%.2f %.2f %.2f) hmd=(%.3f %.3f %.3f) dt=%.4f forward=%.2f level=%.2f stickYaw=%.0f allowed=%d",
                sample.handPosition.x, sample.handPosition.y, sample.handPosition.z,
                sample.handVelocity.x, sample.handVelocity.y, sample.handVelocity.z,
                sample.hmdPosition.x, sample.hmdPosition.y, sample.hmdPosition.z, dt, bash.speed, bash.levelSpeed,
                stickYawRate, bashAllowed ? 1 : 0);
        // Near misses (fast, but a gate said no), rate limited, so a headset
        // session shows why a strike did not count.
        const bool fast = bash.speed > 1.2f || bash.levelSpeed > 1.8f;
        if (fast && !bash.fired &&
            (l4d2vr_selftest::Get().active.load(std::memory_order_acquire) || nowMs - m_GunBashNearMissLogMs >= 1000u))
        {
            m_GunBashNearMissLogMs = nowMs;
            Game::logMsg("[VR][Melee] bash candidate: forward %.2f m/s fraction %.2f barrel %.2f travel %.2f m angular %.0f reach %.2f m | level %.2f m/s lateral %.2f sweep travel %.2f m height %.2f m headYaw %.0f | stickYaw %.0f deg/s allowed=%d armed=%d",
                bash.speed, bash.forwardFraction, bash.barrelDot, bash.travel, bash.peakAngular, bash.reach,
                bash.levelSpeed, bash.lateralFraction, bash.sweepTravel, bash.height, bash.headYawRate, stickYawRate,
                bashAllowed ? 1 : 0, bash.armed ? 1 : 0);
        }
        if (bash.fired && !IsMagazineInteractionManualActive())
        {
            shove = true;
            shoveDirection = toWorldD(bash.direction);
            Game::logMsg("[VR][Melee] gun bash %s: forward %.2f m/s level %.2f m/s travel %.2f/%.2f m reach %.2f m (stick turn %.0f deg/s)",
                bash.sweep ? "sweep" : "push", bash.speed, bash.levelSpeed, bash.travel, bash.sweepTravel, bash.reach,
                stickYawRate);
        }
    }
    if (shove)
    {
        m_PhysicalShoveHoldUntil = now + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<float>(std::max(0.05f, m_MotionGestureHoldDuration)));
        if (VectorLength(shoveDirection) > 0.01f)
        {
            QAngle aim{};
            QAngle::VectorAngles(shoveDirection, aim);
            NormalizeAndClampViewAngles(aim);
            m_PhysicalShoveAngles = aim;
            m_PhysicalShoveAimValid = true;
        }
        m_PhysicalShoveImpulsePending.store(true, std::memory_order_release);
        m_PhysicalShoveSerial.fetch_add(1u, std::memory_order_acq_rel);
        if (m_MotionOnlyMeleeAndShove)
            m_ViewmodelAnimSuppressUntilMs.store(GetTickCount64() + 800u, std::memory_order_release);
        TriggerShoveHaptics(false);
        Game::logMsg("[VR][Melee] physical shove (%s)", meleeActive ? "thrust" : "gun bash");
    }
}

bool VR::ApplyMeleeInertiaToViewmodel(Vector& forward, Vector& right, Vector& up, Vector& position) const
{
    l4d2vr_melee_inertia::Pose delta{};
    {
        std::lock_guard<std::mutex> lock(m_MeleeInertiaMutex);
        if (!m_MeleeInertiaDeltaValid)
            return false;
        delta = m_MeleeInertiaDelta;
    }
    const float scale = std::max(0.001f, m_VRScale);
    l4d2vr_melee_inertia::Pose current{};
    if (!l4d2vr_melee_inertia::PoseFromAxes(position * (1.0f / scale), forward, right, up, current))
        return false;
    const l4d2vr_melee_inertia::Pose lagged = l4d2vr_melee_inertia::ApplyLagDelta(current, delta);
    if (!l4d2vr_melee_inertia::PoseFinite(lagged))
        return false;
    const Vector gripBefore = position + forward * m_ViewmodelPosOffset.x + right * m_ViewmodelPosOffset.y +
        up * m_ViewmodelPosOffset.z;
    l4d2vr_melee_inertia::QuatToAxes(lagged.orientation, forward, right, up);
    position = lagged.position * scale;
    // What the drawn weapon shows, for the meleelag self-test: its angle to the
    // controller and how far the grip moved off the hand (0 when welded).
    if (m_MeleeDrawnLagRecord.load(std::memory_order_acquire))
    {
        const float lagDeg = l4d2vr_melee_inertia::QuatAngleBetween(current.orientation, lagged.orientation) *
            l4d2vr_melee_inertia::kRadToDeg;
        const Vector gripAfter = position + forward * m_ViewmodelPosOffset.x + right * m_ViewmodelPosOffset.y +
            up * m_ViewmodelPosOffset.z;
        const float drift = (gripAfter - gripBefore).Length() / scale;
        float seen = m_MeleeDrawnLagMaxDeg.load(std::memory_order_relaxed);
        while (lagDeg > seen && !m_MeleeDrawnLagMaxDeg.compare_exchange_weak(seen, lagDeg)) {}
        seen = m_MeleeDrawnGripDriftMax.load(std::memory_order_relaxed);
        while (drift > seen && !m_MeleeDrawnGripDriftMax.compare_exchange_weak(seen, drift)) {}
        m_MeleeDrawnFrames.fetch_add(1u, std::memory_order_relaxed);
    }
    return true;
}

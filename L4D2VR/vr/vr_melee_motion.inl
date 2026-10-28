// Motion melee: physics inertia on melee weapons, swing detection from the
// lagged blade, and thrust / gun-bash shoves. The lag is published as a delta
// in the viewmodel's own frame so the render thread can re-apply it to its
// fresher controller pose; the usercmd pose uses the same lagged pose, so the
// server's melee sweep follows what the player sees.

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
    const float sampleTime = std::chrono::duration<float>(now.time_since_epoch()).count();
    const float scale = std::max(0.001f, m_VRScale);

    C_WeaponCSBase* const weapon = localPlayer ? static_cast<C_WeaponCSBase*>(localPlayer->GetActiveWeapon()) : nullptr;
    const int weaponId = weapon ? static_cast<int>(weapon->GetWeaponID()) : 0;
    const bool meleeActive = m_IsVREnabled && !m_MouseModeEnabled && weapon &&
        (weaponId == C_WeaponCSBase::WeaponID::MELEE || weaponId == C_WeaponCSBase::WeaponID::CHAINSAW);
    if (weapon != m_MeleeMotionWeapon)
    {
        m_MeleeMotionWeapon = weapon;
        m_MeleeMotionScriptName = weaponId == C_WeaponCSBase::WeaponID::CHAINSAW ? std::string("chainsaw") :
            (weaponId == C_WeaponCSBase::WeaponID::MELEE ? MeleeMotionReadScriptName(m_Game, weapon) : std::string());
        m_MeleeInertiaSpring.SetParams(l4d2vr_melee_inertia::ParamsForMeleeScriptName(m_MeleeMotionScriptName.c_str()));
        m_MeleeInertiaSpring.Clear();
        m_MeleeSwing.Reset();
        m_MeleeThrust.Reset();
        m_GunBash.Reset();
    }
    const l4d2vr_melee_inertia::Params params =
        l4d2vr_melee_inertia::ParamsForMeleeScriptName(m_MeleeMotionScriptName.c_str());
    // The chainsaw runs while the trigger is held; it never swing-triggers.
    m_MeleeMotionSwingEligible = meleeActive && params.motionMelee;

    // Body frame: world minus the camera anchor, un-rotated by the stick-turn
    // offset. Turning, walking and teleports move the whole frame, so neither
    // the inertia spring nor the gesture detectors see them as hand motion.
    const Vector worldUp(0.0f, 0.0f, 1.0f);
    const float bodyYaw = m_RotationOffset;
    const Vector bodyAnchor = m_CameraAnchor;
    auto toBodyP = [&](const Vector& p) { return VectorRotate(p - bodyAnchor, worldUp, -bodyYaw); };
    auto toWorldP = [&](const Vector& p) { return VectorRotate(p, worldUp, bodyYaw) + bodyAnchor; };
    auto toBodyD = [&](const Vector& d) { return VectorRotate(d, worldUp, -bodyYaw); };
    auto toWorldD = [&](const Vector& d) { return VectorRotate(d, worldUp, bodyYaw); };

    // Inertia on the viewmodel pose (main thread copy; render re-applies the delta).
    bool lagValid = false;
    l4d2vr_melee_inertia::Pose delta{};
    l4d2vr_melee_inertia::Pose lagged{};
    if (meleeActive && m_MeleeInertiaEnabled)
    {
        const Vector viewmodelPos = GetRightControllerViewmodelAbsPos() - m_ViewmodelForward * m_ViewmodelPosOffset.x -
            m_ViewmodelRight * m_ViewmodelPosOffset.y - m_ViewmodelUp * m_ViewmodelPosOffset.z;
        l4d2vr_melee_inertia::Pose target{};
        if (l4d2vr_melee_inertia::PoseFromAxes(toBodyP(viewmodelPos) * (1.0f / scale), toBodyD(m_ViewmodelForward),
                toBodyD(m_ViewmodelRight), toBodyD(m_ViewmodelUp), target) && m_MeleeInertiaSpring.Step(target, dt))
        {
            lagged = m_MeleeInertiaSpring.Lagged();
            delta = l4d2vr_melee_inertia::LagDelta(target, lagged);
            lagValid = l4d2vr_melee_inertia::PoseFinite(delta);
            if (lagValid)
            {
                Vector laggedForward{}, laggedRight{}, laggedUp{};
                l4d2vr_melee_inertia::QuatToAxes(lagged.orientation, laggedForward, laggedRight, laggedUp);
                m_ViewmodelForward = toWorldD(laggedForward);
                m_ViewmodelRight = toWorldD(laggedRight);
                m_ViewmodelUp = toWorldD(laggedUp);
                m_MeleeInertiaPosOffsetWorld = toWorldP(lagged.position * scale) - viewmodelPos;
                // Rotation from the controller to the lagged weapon (body frame), for
                // the usercmd pose the server's melee sweep uses.
                const l4d2vr_melee_inertia::Quat bodyRotation = l4d2vr_melee_inertia::QuatMultiply(
                    lagged.orientation, l4d2vr_melee_inertia::QuatConjugate(target.orientation));
                Vector forward = toWorldD(l4d2vr_melee_inertia::QuatRotate(bodyRotation, toBodyD(m_RightControllerForward)));
                Vector up = toWorldD(l4d2vr_melee_inertia::QuatRotate(bodyRotation, toBodyD(m_RightControllerUp)));
                QAngle angles{};
                QAngle::VectorAngles(forward, up, angles);
                m_MeleeLaggedControllerAng = angles;
                m_MeleeLaggedControllerPos = GetRightControllerViewmodelAbsPos() + m_MeleeInertiaPosOffsetWorld;
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

    // Gesture sample from the weapon hand (the lagged pose when it exists).
    l4d2vr_gesture::Sample sample{};
    sample.time = sampleTime;
    const Vector handPos = lagValid ? m_MeleeLaggedControllerPos : GetRightControllerViewmodelAbsPos();
    Vector handForward = m_RightControllerForward;
    if (lagValid)
        QAngle::AngleVectors(m_MeleeLaggedControllerAng, &handForward, nullptr, nullptr);
    sample.handPosition = toBodyP(handPos) * (1.0f / scale);
    sample.handForward = toBodyD(handForward);
    if (m_MeleeMotionPrevValid && dt > 0.0005f)
        sample.handVelocity = (sample.handPosition - m_MeleeMotionPrevHand) * (1.0f / dt);
    m_MeleeMotionPrevHand = sample.handPosition;
    m_MeleeMotionPrevValid = true;
    if (lagValid)
        sample.handAngularVelocityDeg = m_MeleeInertiaSpring.GetState().angularVelocity * 57.29577951f;
    else
    {
        const QAngle av = m_RightControllerPose.TrackedDeviceAngVel;
        sample.handAngularVelocityDeg = Vector(av.x, av.y, av.z);
    }
    sample.hmdPosition = toBodyP(m_HmdPosAbs) * (1.0f / scale);
    sample.hmdForward = toBodyD(m_HmdForward);
    sample.hmdUp = toBodyD(m_HmdUp);

    bool swingStarted = false;
    if (m_MeleeMotionSwingEligible)
    {
        const l4d2vr_gesture::SwingEvent swing = m_MeleeSwing.Update(sample);
        m_MeleeSwingActive.store(swing.swinging, std::memory_order_release);
        if (swing.started)
        {
            swingStarted = true;
            m_MeleeSwingDirection = toWorldD(swing.direction);
            m_MeleeSwingStartSerial.fetch_add(1u, std::memory_order_acq_rel);
            if (m_MotionOnlyMeleeAndShove)
                m_ViewmodelAnimSuppressUntilMs.store(GetTickCount64() + 900u, std::memory_order_release);
            TriggerMeleeSwingHaptics(false);
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
                targets[count].point = toBodyP(chest) * (1.0f / scale);
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
                targets.chest = toBodyP(l4d2vr_holster::ToWorld(frame, Vector(0.12f, 0.0f, -0.30f))) * (1.0f / scale);
                targets.chestValid = true;
            }
            Vector arm{};
            if (l4d2vr_gesture::ArmTargetPoint(toBodyP(m_LeftControllerPosAbs) * (1.0f / scale),
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
        const l4d2vr_gesture::GunBashEvent bash = m_GunBash.Update(sample, bashAllowed);
        if (GetTickCount64() < m_VmAnimTraceUntilMs.load(std::memory_order_acquire))
            Game::logMsg("[VR][Melee] bash trace: hand=(%.3f %.3f %.3f) vel=(%.2f %.2f %.2f) hmd=(%.3f %.3f %.3f) dt=%.4f forward=%.2f allowed=%d",
                sample.handPosition.x, sample.handPosition.y, sample.handPosition.z,
                sample.handVelocity.x, sample.handVelocity.y, sample.handVelocity.z,
                sample.hmdPosition.x, sample.hmdPosition.y, sample.hmdPosition.z, dt, bash.speed, bashAllowed ? 1 : 0);
        if (bash.speed > 1.2f && l4d2vr_selftest::Get().active.load(std::memory_order_acquire))
            Game::logMsg("[VR][Melee] bash candidate: forward %.2f m/s fraction %.2f barrel %.2f travel %.2f m angular %.0f reach %.2f m allowed=%d armed=%d fired=%d",
                bash.speed, bash.forwardFraction, bash.barrelDot, bash.travel, bash.peakAngular, bash.reach,
                bashAllowed ? 1 : 0, bash.armed ? 1 : 0, bash.fired ? 1 : 0);
        if (bash.fired && !IsMagazineInteractionManualActive())
        {
            shove = true;
            shoveDirection = toWorldD(bash.direction);
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
    l4d2vr_melee_inertia::QuatToAxes(lagged.orientation, forward, right, up);
    position = lagged.position * scale;
    return true;
}

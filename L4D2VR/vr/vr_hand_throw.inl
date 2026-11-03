// Pavlov-style hand throws for grenades (see vr_hand_throw.h): per-frame hand
// tracking for the release velocity, the per-command wire payload, and the
// thrown grenade's viewmodel hiding until the server switches the hand empty.

double VR::HandThrowNowSeconds() const
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - m_HandThrowClockBase).count();
}

// Every headset frame: both hands in tracking space (metres). Tracking space
// excludes stick turning and locomotion, so neither can inflate a throw.
void VR::UpdateHandThrowTracking()
{
    const double now = HandThrowNowSeconds();
    const Vector& left = m_LeftControllerPose.TrackedDevicePos;
    const Vector& right = m_RightControllerPose.TrackedDevicePos;
    std::lock_guard<std::mutex> lock(m_HandThrowMutex);
    // The gameplay hands are swapped at the source when the item changes hands
    // (vr_item_hand.h); the jump between controllers must not read as a throw.
    const uint32_t itemHandSerial = m_ItemHandSwapSerial.load(std::memory_order_acquire);
    if (itemHandSerial != m_HandThrowItemHandSerial)
    {
        m_HandThrowItemHandSerial = itemHandSerial;
        m_HandThrowTracker[0].Reset();
        m_HandThrowTracker[1].Reset();
    }
    if (!m_IsVREnabled)
    {
        m_HandThrowTracker[0].Reset();
        m_HandThrowTracker[1].Reset();
        return;
    }
    m_HandThrowTracker[0].Add(now, l4d2vr_hand_throw::V3(left.x, left.y, left.z));
    m_HandThrowTracker[1].Add(now, l4d2vr_hand_throw::V3(right.x, right.y, right.z));
}

bool VR::HandThrowServerReady() const
{
    // GrenadeThrowMode=arc throws natively along the shown arc instead.
    return m_IsVREnabled && m_ManualThrowEnabled && !m_MouseModeEnabled && !m_ForceNonVRServerMovement && !GrenadeArcMode() &&
        m_EncodeVRUsercmd && Hooks::s_ServerUnderstandsVR && ServerFeature(l4d2vr_features::HandThrow);
}

// The hand that holds (and throws) the active grenade, in gameplay terms. The
// item hand is always the gameplay right hand: ambidextrous item use swaps the
// controller poses at the source when the left hand takes the item.
bool VR::ThrowableHandIsLeft() const
{
    return false;
}

bool VR::EstimateHandThrowVelocity(bool leftHand, Vector& worldRelative) const
{
    worldRelative = {};
    l4d2vr_hand_throw::V3 tracking{};
    {
        std::lock_guard<std::mutex> lock(m_HandThrowMutex);
        // The hand's peak over the release window (Pavlov), not the window's
        // average: the trigger goes up while the hand already slows down.
        if (!m_HandThrowTracker[leftHand ? 0 : 1].EstimateRelease(HandThrowNowSeconds(),
            static_cast<float>(m_ManualThrowVelocityWindowMs) / 1000.0f, tracking))
            return false;
    }
    const l4d2vr_hand_throw::V3 world = l4d2vr_hand_throw::TrackingToWorld(tracking, m_RotationOffset, m_VRScale);
    worldRelative = Vector(world.x, world.y, world.z);
    return std::isfinite(worldRelative.x) && std::isfinite(worldRelative.y) && std::isfinite(worldRelative.z);
}

void VR::QueueHandThrowUsercmd(const HandThrowUsercmd& entry)
{
    if (entry.command <= 0)
        return;
    std::lock_guard<std::mutex> lock(m_HandThrowMutex);
    m_HandThrowUsercmds[static_cast<size_t>(entry.command) % kHandThrowUsercmdCount] = entry;
}

// Backups of the release command carry the same payload (same command number).
bool VR::GetHandThrowUsercmd(int command, HandThrowUsercmd& out) const
{
    if (command <= 0)
        return false;
    std::lock_guard<std::mutex> lock(m_HandThrowMutex);
    const HandThrowUsercmd& entry = m_HandThrowUsercmds[static_cast<size_t>(command) % kHandThrowUsercmdCount];
    if (entry.command != command)
        return false;
    out = entry;
    return true;
}

void VR::NoteHandThrowServerSpawn(const Vector& origin, const Vector& velocity, const Vector& body, int command)
{
    {
        std::lock_guard<std::mutex> lock(m_HandThrowMutex);
        m_HandThrowServerSpawnOrigin = origin;
        m_HandThrowServerSpawnVelocity = velocity;
        m_HandThrowServerSpawnBody = body;
        m_HandThrowServerSpawnCommand = command;
    }
    m_HandThrowServerSpawnMs.store(GetTickCount64(), std::memory_order_release);
    m_HandThrowServerSpawnCount.fetch_add(1u, std::memory_order_acq_rel);
}

// The grenade has left the hand: do not keep drawing it there while the
// server's empty-hand switch is on its way (one round trip).
bool VR::HandThrowHidesViewmodel(const std::string& modelName) const
{
    const uint64_t until = m_HandThrowHideUntilMs.load(std::memory_order_acquire);
    if (until == 0u || GetTickCount64() > until)
        return false;
    return modelName.find("v_pipebomb") != std::string::npos || modelName.find("v_molotov") != std::string::npos ||
        modelName.find("v_bile_flask") != std::string::npos;
}

// GrenadeThrowMode=arc on the dedicated plugin: opt out of its throw velocity
// paths (ThrowVelocity: the native projectile re-aimed from the hand's motion;
// HandThrow), so the grenade keeps L4D2's own throw along the shown arc. The
// plugin answers with the reduced feature ack. Physics mode asks for all again.
void VR::SendGrenadeThrowFeatureRequest()
{
    if (!m_Game || !Hooks::s_ServerUnderstandsVR || m_ServerFeatureMask.load(std::memory_order_acquire) == 0u)
    {
        if (!Hooks::s_ServerUnderstandsVR)
            m_GrenadeFeatureRequestSent = -1;
        return;
    }
    const int wanted = GrenadeArcMode() ? 1 : 0;
    if (wanted == m_GrenadeFeatureRequestSent)
        return;
    // Nothing to opt back into: never ask a server that has not opted us out.
    if (wanted == 0 && m_GrenadeFeatureRequestSent < 0)
    {
        m_GrenadeFeatureRequestSent = 0;
        return;
    }
    const std::uint32_t mask = wanted
        ? (l4d2vr_features::kKnownMask & ~(l4d2vr_features::ThrowVelocity | l4d2vr_features::HandThrow))
        : l4d2vr_features::kKnownMask;
    char command[64];
    std::snprintf(command, sizeof(command), "l4d2vr_feature_hello %x %u", mask, l4d2vr_features::kVersion);
    m_Game->ServerCmd(command, true);
    m_GrenadeFeatureRequestSent = wanted;
    Game::logMsg("[VR][Grenade] sent %s (GrenadeThrowMode=%s)", command, wanted ? "arc" : "physics");
}

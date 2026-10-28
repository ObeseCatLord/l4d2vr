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
    return m_IsVREnabled && m_ManualThrowEnabled && !m_MouseModeEnabled && !m_ForceNonVRServerMovement &&
        m_EncodeVRUsercmd && Hooks::s_ServerUnderstandsVR && ServerFeature(l4d2vr_features::HandThrow);
}

// The hand that holds (and throws) the active grenade. The weapon hand for
// now; ambidextrous item use wires the off hand in here.
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
        if (!m_HandThrowTracker[leftHand ? 0 : 1].Estimate(HandThrowNowSeconds(),
            static_cast<float>(m_ManualThrowVelocityWindowMs) / 1000.0f, m_ManualThrowPeakVelocityBlend, tracking))
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

void VR::NoteHandThrowServerSpawn(const Vector& origin, const Vector& velocity, int command)
{
    {
        std::lock_guard<std::mutex> lock(m_HandThrowMutex);
        m_HandThrowServerSpawnOrigin = origin;
        m_HandThrowServerSpawnVelocity = velocity;
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

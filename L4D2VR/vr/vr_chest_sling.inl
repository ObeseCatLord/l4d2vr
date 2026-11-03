// Chest sling for long guns (ChestSlingEnabled, vr_chest_sling.h). Included by
// vr.cpp. The slung gun stays the active weapon; only the client changes: the
// gun is drawn on the chest (through the holster item renderer), both hands
// are free, and firing, reloading and bashing are blocked until a hand takes
// it back by its handle or foregrip.

namespace
{
    Vector ChestSlingNormalized(const Vector& v, const Vector& fallback)
    {
        const float length = v.Length();
        return (std::isfinite(length) && length > 1.0e-4f) ? v * (1.0f / length) : fallback;
    }
}

// Where the slung gun hangs: in front of the belly, a little toward the
// dominant side, muzzle down and across toward the off-side knee, the sights
// facing out. The handle (pistol grip) is 12 cm behind the gun's centre along
// the barrel, the foregrip 16 cm in front of it.
bool VR::BuildChestSlingPose(const l4d2vr_holster::BodyFrame& frame, l4d2vr_holster::ItemPose& pose,
    Vector* handle, Vector* foregrip) const
{
    pose = {};
    if (!frame.valid)
        return false;
    const Vector up(0.0f, 0.0f, 1.0f);
    const Vector f = frame.forward;
    const Vector d = frame.dominant;
    Vector axisZ = ChestSlingNormalized(f * 0.20f + d * -0.42f + up * -0.88f, up * -1.0f); // barrel
    Vector axisY = ChestSlingNormalized(f * 0.85f + d * 0.35f + up * 0.30f, f);          // sights
    axisY = ChestSlingNormalized(axisY - axisZ * DotProduct(axisY, axisZ), f);
    Vector axisX{};
    CrossProduct(axisY, axisZ, axisX);
    axisX = ChestSlingNormalized(axisX, d);
    const Vector center = l4d2vr_holster::RootToWorld(frame, Vector(0.17f, 0.04f, 0.33f));
    const Vector centerUnits = l4d2vr_holster::ReferenceToCenterUnits(l4d2vr_holster::ItemShape::LongGun);
    pose.origin = center - (axisX * centerUnits.x + axisY * centerUnits.y + axisZ * centerUnits.z);
    pose.axisX = axisX;
    pose.axisY = axisY;
    pose.axisZ = axisZ;
    pose.valid = l4d2vr_interaction::Finite(pose.origin) && l4d2vr_interaction::Finite(pose.axisX);
    const float s = frame.unitsPerMeter;
    if (handle)
        *handle = center - axisZ * (0.12f * s);
    if (foregrip)
        *foregrip = center + axisZ * (0.16f * s);
    return pose.valid;
}

bool VR::GetChestSlingPoint(Vector& handle, Vector& foregrip) const
{
    l4d2vr_holster::BodyFrame frame{};
    l4d2vr_holster::ItemPose pose{};
    return GetHolsterBodyFrame(frame) && BuildChestSlingPose(frame, pose, &handle, &foregrip);
}

bool VR::ChestSlingCatchRelease(uintptr_t weaponTag, int weaponId)
{
    if (weaponTag == 0u)
        return false;
    Vector handle{}, foregrip{};
    l4d2vr_chest_sling::ReleaseInput in{};
    in.enabled = m_ChestSlingEnabled && m_IsVREnabled && !m_MouseModeEnabled;
    in.dropRequested = true;
    in.weaponId = weaponId;
    in.dropDistanceMeters = m_ChestSlingDropDistanceMeters;
    in.handToSlingMeters = GetChestSlingPoint(handle, foregrip)
        ? (m_RightControllerPosAbs - handle).Length() / (std::max)(1.0f, m_VRScale)
        : -1.0f;
    const l4d2vr_chest_sling::ReleaseAction action = l4d2vr_chest_sling::OnRelease(in);
    Game::logMsg("[VR][ChestSling] release weaponId=%d hand %.2f m from the sling (drop beyond %.2f): %s",
        weaponId, in.handToSlingMeters, in.dropDistanceMeters,
        action == l4d2vr_chest_sling::ReleaseAction::Sling ? "slung" : "dropped");
    if (action != l4d2vr_chest_sling::ReleaseAction::Sling)
        return false;
    m_ChestSlungWeapon.store(weaponTag, std::memory_order_release);
    m_ChestSlingHangCount.fetch_add(1u, std::memory_order_acq_rel);
    // The foregrip lets go with the gun.
    m_VrHandsTwoHandedGripActive = false;
    m_VrHandsTwoHandedGripWeaponId = 0;
    m_VrHandsTwoHandedGripWeaponTag = 0;
    TriggerPhysicalHandHapticPulse(IsGameplayHandLeftPhysical(false), 0.02f, 90.0f, 0.35f, 1);
    return true;
}

// Every frame from the holster block (ProcessInput): forget the sling when the
// gun is no longer the active weapon, and let either hand take it back.
void VR::UpdateChestSling(C_BasePlayer* localPlayer, bool ready, const l4d2vr_holster::BodyFrame* frame,
    std::uint32_t occupiedMask, bool itemHandGripDown, bool itemHandClaimed)
{
    static bool s_PrevItemGrip = false;
    static bool s_PrevSupportGrip = false;
    const uint32_t supportState = m_PistolOffhandGripState.load(std::memory_order_acquire);
    const bool supportGrip = (supportState & 3u) == 3u;
    const bool itemPress = itemHandGripDown && !s_PrevItemGrip;
    const bool supportPress = supportGrip && !s_PrevSupportGrip;
    s_PrevItemGrip = itemHandGripDown;
    s_PrevSupportGrip = supportGrip;

    const uintptr_t slung = m_ChestSlungWeapon.load(std::memory_order_acquire);
    if (slung == 0u)
        return;
    C_BaseCombatWeapon* const active = localPlayer ? localPlayer->GetActiveWeapon() : nullptr;
    const bool alive = ready && localPlayer != nullptr;
    if (!m_ChestSlingEnabled || !alive || reinterpret_cast<uintptr_t>(active) != slung)
    {
        m_ChestSlungWeapon.store(0u, std::memory_order_release);
        Game::logMsg("[VR][ChestSling] cleared (%s)", !m_ChestSlingEnabled ? "option off" : !alive ? "not ready" :
            "the active weapon changed");
        return;
    }
    // No bash or shove from a hand that is not holding the gun.
    const uint64_t nowMs = GetTickCount64();
    if (nowMs + 300u > m_GunBashGripBlockUntilMs.load(std::memory_order_acquire))
        m_GunBashGripBlockUntilMs.store(nowMs + 300u, std::memory_order_release);
    if (!frame || !frame->valid)
        return;

    l4d2vr_holster::ItemPose pose{};
    Vector handle{}, foregrip{};
    if (!BuildChestSlingPose(*frame, pose, &handle, &foregrip))
        return;
    const float s = (std::max)(1.0f, m_VRScale);
    l4d2vr_chest_sling::GrabInput grab{};
    grab.radiusMeters = m_ChestSlingGrabRadiusMeters;
    // [0] = the support (gameplay left) hand, [1] = the item (gameplay right) hand.
    grab.pressEdge[0] = supportPress;
    grab.pressEdge[1] = itemPress;
    grab.claimed[0] = MagazineInteractionOwnsGrip(true) || IsMagazineInteractionManualActive() ||
        l4d2vr_holster::FindZone(*frame, m_HolsterLayout, m_LeftControllerPosAbs, occupiedMask) !=
            l4d2vr_holster::Zone::None;
    grab.claimed[1] = itemHandClaimed;
    grab.toHandleMeters[0] = (m_LeftControllerPosAbs - handle).Length() / s;
    grab.toHandleMeters[1] = (m_RightControllerPosAbs - handle).Length() / s;
    // Like Pavlov (an 11.5 cm hand grab sphere against the item's whole body),
    // a squeeze anywhere along the gun from the handle to the foregrip takes it;
    // the handle wins when both are in reach. The hand snaps to the grip.
    auto toBody = [&](const Vector& hand)
    {
        const Vector axis = foregrip - handle;
        const float lengthSq = DotProduct(axis, axis);
        const float u = lengthSq > 1.0e-4f ? std::clamp(DotProduct(hand - handle, axis) / lengthSq, 0.0f, 1.0f) : 0.0f;
        return (hand - (handle + axis * u)).Length() / s;
    };
    grab.toForegripMeters[0] = toBody(m_LeftControllerPosAbs);
    grab.toForegripMeters[1] = toBody(m_RightControllerPosAbs);
    if (supportPress || itemPress)
    {
        Game::logMsg("[VR][ChestSling] squeeze: support %d (handle %.2f, foregrip %.2f, claimed %d) item %d (handle %.2f, foregrip %.2f, claimed %d) reach %.2f",
            supportPress ? 1 : 0, grab.toHandleMeters[0], grab.toForegripMeters[0], grab.claimed[0] ? 1 : 0,
            itemPress ? 1 : 0, grab.toHandleMeters[1], grab.toForegripMeters[1], grab.claimed[1] ? 1 : 0,
            grab.radiusMeters);
    }
    const int hand = l4d2vr_chest_sling::GrabHand(grab);
    if (hand < 0)
        return;
    m_ChestSlungWeapon.store(0u, std::memory_order_release);
    m_ChestSlingTakeCount.fetch_add(1u, std::memory_order_acq_rel);
    const bool physicalLeft = IsGameplayHandLeftPhysical(hand == 0);
    if (hand == 0)
    {
        // The other hand takes it: it becomes the trigger hand (and its grip owns
        // the gun, as after a hand-off; a toggle grip holds it).
        m_ItemHandAdoptHeld = true;
        RequestItemHand(physicalLeft, l4d2vr_item_hand::Reason::HandOff);
        if (!m_AmbidextrousItemHands)
        {
            // Without ambidextrous items only the item hand can hold it: the
            // support hand puts it back in that hand.
            m_ChestSlingTakeSerial.fetch_add(1u, std::memory_order_acq_rel);
        }
    }
    else
    {
        m_ChestSlingTakeSerial.fetch_add(1u, std::memory_order_acq_rel);
        m_GripToggleAdoptSerial.fetch_add(1u, std::memory_order_acq_rel);
    }
    TriggerPhysicalHandHapticPulse(physicalLeft, 0.03f, 120.0f, 0.6f, 2);
    Game::logMsg("[VR][ChestSling] taken by the %s hand (%s)", physicalLeft ? "left" : "right",
        hand == 0 ? "the free hand: it is now the trigger hand" : "the trigger hand");
}

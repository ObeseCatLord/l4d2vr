// Chest sling for long guns (ChestSlingEnabled, vr_chest_sling.h) and empty
// hands (HolsterEmptyHands). Included by vr.cpp. A stowed weapon stays the
// active weapon; only the client changes: it is drawn on the body (through the
// holster item renderer, at the sling or its own holster), both hands are
// free with empty-hand poses, and firing, reloading and bashing are blocked
// until a hand takes it back from where it hangs.

// The slung gun (Forefront's chest sling, l4d2vr_holster::SlingPose): grip on
// the dominant side of the belly, barrel down toward the off-hand hip.
bool VR::BuildChestSlingPose(const l4d2vr_holster::BodyFrame& frame, l4d2vr_holster::ItemPose& pose,
    Vector* handle, Vector* foregrip) const
{
    pose = l4d2vr_holster::SlingPose(frame, m_HolsterLayout);
    if (!pose.valid)
        return false;
    Vector h{}, f{};
    l4d2vr_holster::SlingHandle(frame, m_HolsterLayout, h, f);
    if (handle)
        *handle = h;
    if (foregrip)
        *foregrip = f;
    return true;
}

bool VR::GetChestSlingPoint(Vector& handle, Vector& foregrip) const
{
    l4d2vr_holster::BodyFrame frame{};
    l4d2vr_holster::ItemPose pose{};
    return GetHolsterBodyFrame(frame) && BuildChestSlingPose(frame, pose, &handle, &foregrip);
}

void VR::StowActiveWeapon(uintptr_t weaponTag, int slot, const char* why)
{
    if (weaponTag == 0u || slot < 0 || slot >= l4d2vr_holster::kZoneCount)
        return;
    m_StowedSlot.store(slot, std::memory_order_release);
    m_ChestSlungWeapon.store(weaponTag, std::memory_order_release);
    if (slot == 0)
        m_ChestSlingHangCount.fetch_add(1u, std::memory_order_acq_rel);
    m_HolsterStowCount.fetch_add(1u, std::memory_order_acq_rel);
    // The foregrip lets go with the gun.
    m_VrHandsTwoHandedGripActive = false;
    m_VrHandsTwoHandedGripWeaponId = 0;
    m_VrHandsTwoHandedGripWeaponTag = 0;
    TriggerPhysicalHandHapticPulse(IsGameplayHandLeftPhysical(false), 0.02f, 90.0f, 0.35f, 1);
    Game::logMsg("[VR][EmptyHands] slot%d put away (%s): both hands empty", slot + 1, why ? why : "");
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
    StowActiveWeapon(weaponTag, 0, "let go of the long gun: on the chest sling");
    return true;
}

bool VR::HolsterCatchRelease(uintptr_t weaponTag, int weaponId)
{
    if (weaponTag == 0u)
        return false;
    l4d2vr_holster::BodyFrame frame{};
    float handToHolster = -1.0f;
    if (GetHolsterBodyFrame(frame))
    {
        Vector top{}, bottom{};
        l4d2vr_holster::HipSegment(frame, m_HolsterLayout, l4d2vr_holster::Zone::Secondary, top, bottom);
        handToHolster = l4d2vr_holster::DistanceToSegment(m_RightControllerPosAbs, top, bottom) /
            (std::max)(1.0f, m_VRScale);
    }
    l4d2vr_chest_sling::ReleaseInput in{};
    in.enabled = m_HolsterEmptyHands && m_BodyGripInventoryEnabled && m_HolsterPavlovLayoutEnabled &&
        m_IsVREnabled && !m_MouseModeEnabled;
    in.dropRequested = true;
    in.weaponId = weaponId;
    in.handToSlingMeters = handToHolster;
    in.dropDistanceMeters = m_ChestSlingDropDistanceMeters;
    Vector velocity{};
    if (EstimateHandThrowVelocity(false, velocity))
        in.handSpeedMetersPerSecond = velocity.Length() / (std::max)(1.0f, m_VRScale);
    const l4d2vr_chest_sling::ReleaseAction action = l4d2vr_chest_sling::OnSidearmRelease(in);
    if (action == l4d2vr_chest_sling::ReleaseAction::None)
        return false;
    Game::logMsg("[VR][EmptyHands] release weaponId=%d hand %.2f m from the hip holster (drop beyond %.2f) at %.1f m/s: %s",
        weaponId, handToHolster, in.dropDistanceMeters, in.handSpeedMetersPerSecond,
        action == l4d2vr_chest_sling::ReleaseAction::Sling ? "holstered" : "dropped");
    if (action != l4d2vr_chest_sling::ReleaseAction::Sling)
        return false;
    StowActiveWeapon(weaponTag, l4d2vr_holster::SlotIndex(l4d2vr_holster::Zone::Secondary),
        "let go of the sidearm: back in the hip holster");
    return true;
}

// Every frame from the holster block (ProcessInput): forget the stowed weapon
// when it is no longer the active weapon, and let either hand take it back.
// Long guns: by the sling's handle or anywhere along the gun; holstered items:
// in their holster's grab volume (the same one a draw uses).
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
    const int stowedSlot = m_StowedSlot.load(std::memory_order_acquire);
    const bool onSling = stowedSlot == 0;
    C_BaseCombatWeapon* const active = localPlayer ? localPlayer->GetActiveWeapon() : nullptr;
    const bool alive = ready && localPlayer != nullptr;
    const bool optionOn = onSling ? (m_ChestSlingEnabled || m_HolsterEmptyHands) : m_HolsterEmptyHands;
    if (!optionOn || !alive || reinterpret_cast<uintptr_t>(active) != slung)
    {
        m_ChestSlungWeapon.store(0u, std::memory_order_release);
        Game::logMsg("[VR][ChestSling] cleared slot%d (%s)", stowedSlot + 1, !optionOn ? "option off" : !alive ? "not ready" :
            "the active weapon changed");
        return;
    }
    // No bash or shove from a hand that is not holding the gun.
    const uint64_t nowMs = GetTickCount64();
    if (nowMs + 300u > m_GunBashGripBlockUntilMs.load(std::memory_order_acquire))
        m_GunBashGripBlockUntilMs.store(nowMs + 300u, std::memory_order_release);
    if (!frame || !frame->valid)
        return;

    const float s = (std::max)(1.0f, m_VRScale);
    const l4d2vr_holster::Zone stowedZone = static_cast<l4d2vr_holster::Zone>(stowedSlot);
    const std::uint32_t stowedBit = 1u << stowedSlot;
    l4d2vr_chest_sling::GrabInput grab{};
    // [0] = the support (gameplay left) hand, [1] = the item (gameplay right) hand.
    grab.pressEdge[0] = supportPress;
    grab.pressEdge[1] = itemPress;
    // Another holster under the hand takes the squeeze (a draw); the stowed
    // item's own holster does not count.
    grab.claimed[0] = MagazineInteractionOwnsGrip(true) || IsMagazineInteractionManualActive() ||
        l4d2vr_holster::FindZone(*frame, m_HolsterLayout, m_LeftControllerPosAbs, occupiedMask) !=
            l4d2vr_holster::Zone::None;
    grab.claimed[1] = itemHandClaimed;
    const char* what = "sling";
    if (onSling)
    {
        Vector handle{}, foregrip{};
        l4d2vr_holster::SlingHandle(*frame, m_HolsterLayout, handle, foregrip);
        grab.radiusMeters = m_ChestSlingGrabRadiusMeters;
        grab.toHandleMeters[0] = (m_LeftControllerPosAbs - handle).Length() / s;
        grab.toHandleMeters[1] = (m_RightControllerPosAbs - handle).Length() / s;
        // Like Forefront (the nearest grab handle on the gun), a squeeze anywhere
        // along the gun from the handle to the foregrip takes it; the handle
        // wins when both are in reach. The hand snaps to the grip.
        grab.toForegripMeters[0] = l4d2vr_holster::DistanceToSegment(m_LeftControllerPosAbs, handle, foregrip) / s;
        grab.toForegripMeters[1] = l4d2vr_holster::DistanceToSegment(m_RightControllerPosAbs, handle, foregrip) / s;
    }
    else
    {
        // A holstered item: the hand is in its holster's grab volume and that
        // holster is the nearest grab for it (the same rule as a draw).
        what = "holster";
        grab.radiusMeters = l4d2vr_holster::ZoneReachMeters(m_HolsterLayout, stowedZone);
        auto toHolster = [&](const Vector& hand)
        {
            float distance = -1.0f;
            const std::uint32_t mask = occupiedMask | stowedBit;
            if (l4d2vr_holster::FindZone(*frame, m_HolsterLayout, hand, mask, &distance) != stowedZone)
                return -1.0f;
            return distance;
        };
        grab.toHandleMeters[0] = toHolster(m_LeftControllerPosAbs);
        grab.toHandleMeters[1] = toHolster(m_RightControllerPosAbs);
    }
    if (supportPress || itemPress)
    {
        Game::logMsg("[VR][ChestSling] squeeze at the %s (slot%d): support %d (handle %.2f, body %.2f, claimed %d) item %d (handle %.2f, body %.2f, claimed %d) reach %.2f",
            what, stowedSlot + 1, supportPress ? 1 : 0, grab.toHandleMeters[0], grab.toForegripMeters[0], grab.claimed[0] ? 1 : 0,
            itemPress ? 1 : 0, grab.toHandleMeters[1], grab.toForegripMeters[1], grab.claimed[1] ? 1 : 0,
            grab.radiusMeters);
    }
    const int hand = l4d2vr_chest_sling::GrabHand(grab);
    if (hand < 0)
        return;
    m_ChestSlungWeapon.store(0u, std::memory_order_release);
    if (onSling)
        m_ChestSlingTakeCount.fetch_add(1u, std::memory_order_acq_rel);
    m_HolsterUnstowCount.fetch_add(1u, std::memory_order_acq_rel);
    const bool physicalLeft = IsGameplayHandLeftPhysical(hand == 0);
    if (hand == 0)
    {
        // The other hand takes it: it becomes the trigger hand (and its grip owns
        // the gun, as after a hand-off; a toggle grip holds it).
        m_ItemHandAdoptHeld = true;
        RequestItemHand(physicalLeft, l4d2vr_item_hand::Reason::HandOff);
        // The grip owns it again once the hands have traded (CreateMove), so
        // letting go stows it again. Without ambidextrous items the support
        // hand puts it back in the item hand.
        m_ChestSlingTakeSerial.fetch_add(1u, std::memory_order_acq_rel);
    }
    else
    {
        m_ChestSlingTakeSerial.fetch_add(1u, std::memory_order_acq_rel);
        m_GripToggleAdoptSerial.fetch_add(1u, std::memory_order_acq_rel);
    }
    TriggerPhysicalHandHapticPulse(physicalLeft, 0.03f, 120.0f, 0.6f, 2);
    Game::logMsg("[VR][ChestSling] slot%d taken from the %s by the %s hand (%s)", stowedSlot + 1, what,
        physicalLeft ? "left" : "right",
        hand == 0 ? "the free hand: it is now the trigger hand" : "the trigger hand");
}

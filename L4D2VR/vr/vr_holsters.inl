// Pavlov-style holsters: render snapshot of the carried items and their world
// placement. Input handling lives in ProcessInput (vr_process_input.inl).

namespace
{
    l4d2vr_holster::ItemShape HolsterShapeForWeaponId(int weaponId)
    {
        return l4d2vr_holster::ItemShapeForWeaponId(weaponId);
    }

    int HolsterReadWeaponId(void* weapon)
    {
        if (!weapon)
            return 0;
        __try
        {
            return static_cast<int>(reinterpret_cast<C_WeaponCSBase*>(weapon)->GetWeaponID());
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
    }

    int HolsterFindClientEntityIndex(Game* game, const void* entity)
    {
        if (!game || !game->m_ClientEntityList || !entity)
            return -1;
        __try
        {
            const int highest = (std::min)(game->m_ClientEntityList->GetHighestEntityIndex(), 4096);
            for (int i = 1; i <= highest; ++i)
            {
                if (game->m_ClientEntityList->GetClientEntity(i) == entity)
                    return i;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
        return -1;
    }

    void* HolsterReadRenderable(void* weapon)
    {
        if (!weapon)
            return nullptr;
        __try
        {
            return reinterpret_cast<C_BaseEntity*>(weapon)->GetClientRenderable();
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
    }
}

void VR::UpdateHolsterRenderSnapshot(C_BasePlayer* localPlayer, int activeSlot)
{
    HolsterRenderSnapshot snapshot{};
    if (localPlayer && m_HolsterItemsVisible && m_Game)
    {
        const int highlight = m_HolsterHighlightSlot.load(std::memory_order_acquire);
        for (int slot = 0; slot < l4d2vr_holster::kZoneCount; ++slot)
        {
            if (slot == activeSlot)
                continue;
            void* const weapon = localPlayer->Weapon_GetSlot(slot);
            if (!weapon)
            {
                m_HolsterEntityIndexCacheKey[slot] = nullptr;
                continue;
            }
            void* const renderable = HolsterReadRenderable(weapon);
            if (!renderable)
                continue;
            // Resolving an entity index scans the entity list, so do it only when
            // the slot's item changes.
            if (m_HolsterEntityIndexCacheKey[slot] != weapon)
            {
                m_HolsterEntityIndexCacheKey[slot] = weapon;
                m_HolsterEntityIndexCache[slot] = HolsterFindClientEntityIndex(m_Game, weapon);
            }
            HolsterRenderItem& item = snapshot.items[snapshot.count++];
            item.renderable = renderable;
            item.entityIndex = m_HolsterEntityIndexCache[slot];
            item.slot = slot;
            item.weaponId = HolsterReadWeaponId(weapon);
            item.shape = HolsterShapeForWeaponId(item.weaponId);
            item.highlighted = slot == highlight;
        }
    }

    std::lock_guard<std::mutex> lock(m_HolsterRenderMutex);
    snapshot.serial = m_HolsterRenderSnapshot.serial + 1u;
    m_HolsterRenderSnapshot = snapshot;
}

bool VR::GetHolsterRenderSnapshot(HolsterRenderSnapshot& out) const
{
    std::lock_guard<std::mutex> lock(m_HolsterRenderMutex);
    out = m_HolsterRenderSnapshot;
    return out.count > 0;
}

bool VR::BuildHolsterItemWorld(const HolsterRenderItem& item, l4d2vr_holster::ItemPose& pose) const
{
    pose = {};
    if (item.slot < 0 || item.slot >= l4d2vr_holster::kZoneCount)
        return false;
    l4d2vr_holster::BodyFrame frame{};
    if (!GetHolsterBodyFrame(frame))
        return false;
    pose = l4d2vr_holster::BuildItemPose(frame, m_HolsterLayout,
        static_cast<l4d2vr_holster::Zone>(item.slot), item.shape);
    if (m_SelfTestEnabled && m_SelfTestBackPreviewMeters > 0.0f &&
        l4d2vr_holster::IsBackZone(static_cast<l4d2vr_holster::Zone>(item.slot)))
        pose.origin += frame.forward * (m_SelfTestBackPreviewMeters * frame.unitsPerMeter);
    return pose.valid;
}

// One line per grip press: the hand in the body frame (metres from the level
// head: forward, toward the dominant side, up), the nearest zone, and the frame
// yaw against the head yaw, so real reaches can be compared with the layout.
void VR::LogHolsterGripPress(bool leftHand, const l4d2vr_holster::BodyFrame* frame, const Vector& hand, bool ready,
    int zone, int activeSlot) const
{
    if (!frame || !frame->valid)
    {
        Game::logMsg("[VR][Holster] grip pressed (%s): ready=%d no body frame", leftHand ? "left" : "right", ready ? 1 : 0);
        return;
    }
    const Vector local = l4d2vr_holster::ToLocal(*frame, hand);
    int nearest = -1;
    float nearestDistance = 1.0e9f, nearestMargin = 1.0e9f;
    for (int i = 0; i < l4d2vr_holster::kZoneCount; ++i)
    {
        const auto zoneId = static_cast<l4d2vr_holster::Zone>(i);
        float d = 0.0f;
        l4d2vr_holster::HandFitsZone(*frame, m_HolsterLayout, zoneId, hand, d);
        const float margin = d - l4d2vr_holster::ZoneReachMeters(m_HolsterLayout, zoneId);
        if (margin < nearestMargin)
        {
            nearestMargin = margin;
            nearestDistance = d;
            nearest = i;
        }
    }
    const float frameYaw = std::atan2(frame->forward.y, frame->forward.x) * 180.0f / l4d2vr_holster::kPi;
    const float pitch = std::asin(std::clamp(m_HmdForward.z, -1.0f, 1.0f)) * 180.0f / l4d2vr_holster::kPi;
    // nearest = the zone the hand came closest to reaching: distance from the
    // hand centre to its geometry against the reach that counts.
    Game::logMsg("[VR][Holster] grip pressed (%s): ready=%d zone=%d hand=(fwd %+.2f, dom %+.2f, up %+.2f) m from the head, nearest=slot%d %.2f m (reach %.2f) frameYaw=%.0f headYaw=%.0f (off %+.0f) pitch=%.0f active=slot%d",
        leftHand ? "left" : "right", ready ? 1 : 0, zone, local.x, local.y, local.z, nearest + 1, nearestDistance,
        nearest >= 0 ? l4d2vr_holster::ZoneReachMeters(m_HolsterLayout, static_cast<l4d2vr_holster::Zone>(nearest)) : 0.0f,
        frameYaw,
        m_BodyInventoryHeadYaw, l4d2vr_interaction::WrapYaw(m_BodyInventoryHeadYaw - frameYaw), pitch, activeSlot + 1);
}

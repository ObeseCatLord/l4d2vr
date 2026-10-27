// Pavlov-style holsters: render snapshot of the carried items and their world
// placement. Input handling lives in ProcessInput (vr_process_input.inl).

namespace
{
    l4d2vr_holster::ItemShape HolsterShapeForWeaponId(int weaponId)
    {
        using W = C_WeaponCSBase::WeaponID;
        switch (static_cast<W>(weaponId))
        {
        case W::PISTOL:
        case W::MAGNUM:
            return l4d2vr_holster::ItemShape::Pistol;
        case W::MELEE:
        case W::CHAINSAW:
            return l4d2vr_holster::ItemShape::Melee;
        case W::FIRST_AID_KIT:
        case W::DEFIBRILLATOR:
        case W::INCENDIARY_AMMO:
        case W::FRAG_AMMO:
        case W::MOLOTOV:
        case W::PIPE_BOMB:
        case W::VOMITJAR:
        case W::PAIN_PILLS:
        case W::ADRENALINE:
            return l4d2vr_holster::ItemShape::Equipment;
        default:
            return l4d2vr_holster::ItemShape::LongGun;
        }
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
    return pose.valid;
}

// Ejected magazines falling to the floor. Simulated on the main thread, drawn by
// the viewmodel pass with the weapon's clip subtree at each magazine's pose.

namespace
{
    bool DroppedMagazineGroundQuery(void* context, const Vector& from, const Vector& to, Vector& hit, Vector& normal)
    {
        Game* const game = static_cast<Game*>(context);
        if (!game || !game->m_EngineTrace)
            return false;
        __try
        {
            CGameTrace trace;
            Ray_t ray;
            CTraceFilterSkipNPCsAndPlayers filter(nullptr, 0);
            ray.Init(from, to);
            game->m_EngineTrace->TraceRay(ray, MASK_NPCWORLDSTATIC, &filter, &trace);
            if (trace.fraction >= 1.0f || trace.startsolid)
                return false;
            hit = trace.endpos;
            normal = trace.plane.normal;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }
}

void VR::SpawnDroppedMagazine(const std::string& modelName, int boneIndex, const VrHandMatrix4& world, bool leftHand)
{
    if (!m_MagazineDropEnabled || modelName.empty() || boneIndex < 0)
        return;
    const Vector origin(VrHandMath::Get(world, 0, 3), VrHandMath::Get(world, 1, 3), VrHandMath::Get(world, 2, 3));
    l4d2vr_dropped_mags::Params params{};
    params.lifetimeSeconds = m_MagazineDropLifetimeSeconds;
    const Vector velocity = m_DroppedMagazineHandVelocity[leftHand ? 0 : 1];
    const l4d2vr_dropped_mags::Mag mag = l4d2vr_dropped_mags::Spawn(origin, velocity, params);
    if (!mag.active)
        return;
    std::lock_guard<std::mutex> lock(m_DroppedMagazineMutex);
    const int slot = m_DroppedMagazinePool.Add(mag);
    DroppedMagazineInfo& info = m_DroppedMagazineInfo[slot];
    info.modelLower = modelName;
    for (char& c : info.modelLower)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    info.boneIndex = boneIndex;
    info.rotation = world;
    VrHandMath::Set(info.rotation, 0, 3, 0.0f);
    VrHandMath::Set(info.rotation, 1, 3, 0.0f);
    VrHandMath::Set(info.rotation, 2, 3, 0.0f);
}

void VR::UpdateDroppedMagazines()
{
    const auto now = std::chrono::steady_clock::now();
    const float dt = m_DroppedMagazineLastUpdate.time_since_epoch().count() == 0 ? 0.0f :
        std::chrono::duration<float>(now - m_DroppedMagazineLastUpdate).count();
    m_DroppedMagazineLastUpdate = now;

    // Hand velocity for throwing magazines, from frame-to-frame motion.
    const Vector hands[2] = { m_LeftControllerPosAbs, m_RightControllerPosAbs };
    for (int i = 0; i < 2; ++i)
    {
        if (m_DroppedMagazinePrevHandValid && dt > 0.0005f && dt < 0.1f)
        {
            const Vector instant = (hands[i] - m_DroppedMagazinePrevHand[i]) * (1.0f / dt);
            if (l4d2vr_interaction::Finite(instant))
                m_DroppedMagazineHandVelocity[i] = m_DroppedMagazineHandVelocity[i] * 0.6f + instant * 0.4f;
        }
        m_DroppedMagazinePrevHand[i] = hands[i];
    }
    m_DroppedMagazinePrevHandValid = true;

    l4d2vr_dropped_mags::Params params{};
    params.lifetimeSeconds = m_MagazineDropLifetimeSeconds;
    std::lock_guard<std::mutex> lock(m_DroppedMagazineMutex);
    for (auto& mag : m_DroppedMagazinePool.mags)
        l4d2vr_dropped_mags::Step(mag, dt, params, &DroppedMagazineGroundQuery, m_Game);
}

int VR::GetDroppedMagazineRenders(const std::string& lowerModel, DroppedMagazineRender* out, int capacity) const
{
    if (!out || capacity <= 0)
        return 0;
    int count = 0;
    std::lock_guard<std::mutex> lock(m_DroppedMagazineMutex);
    for (int i = 0; i < 4 && count < capacity; ++i)
    {
        const auto& mag = m_DroppedMagazinePool.mags[static_cast<std::size_t>(i)];
        const DroppedMagazineInfo& info = m_DroppedMagazineInfo[i];
        if (!mag.active || info.modelLower != lowerModel || info.boneIndex < 0)
            continue;
        DroppedMagazineRender& render = out[count++];
        render.boneIndex = info.boneIndex;
        render.world = info.rotation;
        VrHandMath::Set(render.world, 0, 3, mag.position.x);
        VrHandMath::Set(render.world, 1, 3, mag.position.y);
        VrHandMath::Set(render.world, 2, 3, mag.position.z);
    }
    return count;
}

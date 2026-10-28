// Native wrist HUD (WristHudStyle=native).
//
// The game's HUD is painted by VGUI into m_HUDTexture (hooks_misc.inl dVGui_Paint).
// After that paint, the HUD elements' screen rectangles are read from the VGUI
// panel tree (main thread) and the D3D thread copies scaled crops of the HUD
// texture into one render target per wrist. Left wrist: the local player's health
// icon, number and bar (large), then each teammate's bar and name (no portraits).
// Right wrist: the weapon / item panel. Fonts, bar graphics, colours and the
// incapacitated / black-and-white states are therefore exactly the game's.

namespace
{
    namespace nwh = l4d2vr_native_wrist_hud;

    using NativeHudVPanel = unsigned int;

    // vgui::IPanel (VGUI_Panel009) and IEngineVGui (VEngineVGui001) vtable slots,
    // checked against L4D2's vgui2.dll / engine.dll (slot 0 is the destructor).
    constexpr int kIPanelGetSize = 5;
    constexpr int kIPanelGetAbsPos = 10;
    constexpr int kIPanelIsVisible = 15;
    constexpr int kIPanelGetChildCount = 17;
    constexpr int kIPanelGetChild = 18;
    constexpr int kIPanelGetName = 36;
    constexpr int kEngineVguiGetPanel = 1;
    constexpr int kVguiPanelRoot = 0;

    template <typename R, typename... Args>
    R NativeHudVCall(void* object, int index, Args... args)
    {
        using Fn = R(__thiscall*)(void*, Args...);
        return reinterpret_cast<Fn>((*reinterpret_cast<void***>(object))[index])(object, args...);
    }

    void* NativeHudCreateInterface(const char* module, const char* name)
    {
        HMODULE mod = GetModuleHandleA(module);
        if (!mod)
            return nullptr;
        using CreateInterfaceFn = void* (__cdecl*)(const char*, int*);
        auto create = reinterpret_cast<CreateInterfaceFn>(GetProcAddress(mod, "CreateInterface"));
        if (!create)
            return nullptr;
        int rc = 0;
        return create(name, &rc);
    }

    struct NativeHudPanelTree
    {
        void* panel = nullptr;

        bool Visible(NativeHudVPanel p) const { return NativeHudVCall<bool>(panel, kIPanelIsVisible, p); }
        int ChildCount(NativeHudVPanel p) const { return NativeHudVCall<int>(panel, kIPanelGetChildCount, p); }
        NativeHudVPanel Child(NativeHudVPanel p, int i) const { return NativeHudVCall<NativeHudVPanel>(panel, kIPanelGetChild, p, i); }
        const char* Name(NativeHudVPanel p) const
        {
            const char* n = NativeHudVCall<const char*>(panel, kIPanelGetName, p);
            return n ? n : "";
        }
        nwh::Rect ScreenRect(NativeHudVPanel p) const
        {
            int x = 0, y = 0, w = 0, h = 0;
            NativeHudVCall<void, NativeHudVPanel, int*, int*>(panel, kIPanelGetAbsPos, p, &x, &y);
            NativeHudVCall<void, NativeHudVPanel, int*, int*>(panel, kIPanelGetSize, p, &w, &h);
            return { x, y, w, h };
        }
        NativeHudVPanel FindVisibleChild(NativeHudVPanel p, const char* name) const
        {
            const int n = ChildCount(p);
            for (int i = 0; i < n && i < 256; ++i)
            {
                const NativeHudVPanel c = Child(p, i);
                if (c && Visible(c) && std::strcmp(Name(c), name) == 0)
                    return c;
            }
            return 0;
        }
    };

    class NativeWristHudCopyFunctor final : public CFunctor
    {
    public:
        NativeWristHudCopyFunctor(VR* vr, l4d2vr_native_wrist_hud::Job job)
            : m_VR(vr), m_Job(std::move(job)) {}
        int AddRef() override { return static_cast<int>(m_Refs.fetch_add(1, std::memory_order_acq_rel) + 1); }
        int Release() override
        {
            const long refs = m_Refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
            if (!refs) delete this;
            return static_cast<int>(refs);
        }
        void operator()() override
        {
            if (m_VR) m_VR->RenderNativeWristHud(m_Job);
        }
    private:
        std::atomic<long> m_Refs{ 0 };
        VR* m_VR = nullptr;
        l4d2vr_native_wrist_hud::Job m_Job;
    };

    std::string NativeWristHudDumpPath(const char* name)
    {
        char exePath[MAX_PATH] = {};
        if (GetModuleFileNameA(nullptr, exePath, MAX_PATH) == 0)
            return name;
        std::string dir(exePath);
        const size_t slash = dir.find_last_of("\\/");
        dir = slash == std::string::npos ? std::string() : dir.substr(0, slash + 1);
        return dir + name;
    }

    // Writes a 32-bit top-down BMP (BGRA, alpha kept) of a render-target surface.
    bool NativeWristHudDumpSurface(IDirect3DDevice9* device, IDirect3DSurface9* surface, const char* fileName, int cropW, int cropH)
    {
        if (!device || !surface)
            return false;
        D3DSURFACE_DESC desc{};
        if (FAILED(surface->GetDesc(&desc)))
            return false;
        if (desc.Format != D3DFMT_A8R8G8B8 && desc.Format != D3DFMT_X8R8G8B8)
        {
            Game::logMsg("[VR][WristHUD] dump %s skipped: surface format %d", fileName, (int)desc.Format);
            return false;
        }
        IDirect3DSurface9* sys = nullptr;
        if (FAILED(device->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format, D3DPOOL_SYSTEMMEM, &sys, nullptr)) || !sys)
            return false;
        bool ok = false;
        if (SUCCEEDED(device->GetRenderTargetData(surface, sys)))
        {
            D3DLOCKED_RECT lr{};
            if (SUCCEEDED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY)) && lr.pBits)
            {
                const int w = cropW > 0 ? (std::min)(cropW, (int)desc.Width) : (int)desc.Width;
                const int h = cropH > 0 ? (std::min)(cropH, (int)desc.Height) : (int)desc.Height;
                const std::string path = NativeWristHudDumpPath(fileName);
                std::ofstream out(path, std::ios::binary | std::ios::trunc);
                if (out)
                {
                    const uint32_t imageBytes = (uint32_t)w * (uint32_t)h * 4u;
                    uint8_t header[54] = {};
                    auto put32 = [&](int at, uint32_t v) { std::memcpy(header + at, &v, 4); };
                    auto put16 = [&](int at, uint16_t v) { std::memcpy(header + at, &v, 2); };
                    header[0] = 'B'; header[1] = 'M';
                    put32(2, 54u + imageBytes);
                    put32(10, 54u);
                    put32(14, 40u);
                    put32(18, (uint32_t)w);
                    put32(22, (uint32_t)(-h)); // top-down
                    put16(26, 1);
                    put16(28, 32);
                    put32(34, imageBytes);
                    out.write(reinterpret_cast<const char*>(header), sizeof(header));
                    for (int y = 0; y < h; ++y)
                    {
                        const uint8_t* row = reinterpret_cast<const uint8_t*>(lr.pBits) + (size_t)y * (size_t)lr.Pitch;
                        if (desc.Format == D3DFMT_X8R8G8B8)
                        {
                            std::vector<uint8_t> opaque(row, row + (size_t)w * 4u);
                            for (int x = 0; x < w; ++x)
                                opaque[(size_t)x * 4u + 3u] = 255;
                            out.write(reinterpret_cast<const char*>(opaque.data()), (std::streamsize)opaque.size());
                        }
                        else
                        {
                            out.write(reinterpret_cast<const char*>(row), (std::streamsize)w * 4);
                        }
                    }
                    ok = out.good();
                    Game::logMsg("[VR][WristHUD] dumped %s (%dx%d) %s", fileName, w, h, ok ? "ok" : "write failed");
                }
                sys->UnlockRect();
            }
        }
        sys->Release();
        return ok;
    }
}

bool VR::WantsNativeWristHudCapture() const
{
    if (!m_WristHudNativeStyle || !m_HandHudWorldQuadEnabled || !g_D3DVR9)
        return false;
    if (m_NativeWristHudDumpRequested.load(std::memory_order_acquire))
        return true;
    return (m_LeftWristHudEnabled || m_RightAmmoHudEnabled) &&
        m_NativeWristHudWanted.load(std::memory_order_acquire);
}

// Main thread, right after VGUI painted the HUD into m_HUDTexture.
void VR::OnNativeHudPainted(IMatRenderContext* renderContext)
{
    if (!WantsNativeWristHudCapture() || !m_Game)
        return;

    if (!m_NativeHudPanelApi)
        m_NativeHudPanelApi = NativeHudCreateInterface("vgui2.dll", "VGUI_Panel009");
    if (!m_NativeHudEngineVgui)
        m_NativeHudEngineVgui = NativeHudCreateInterface("engine.dll", "VEngineVGui001");
    if (!m_NativeHudPanelApi || !m_NativeHudEngineVgui)
        return;

    int hudW = 0;
    int hudH = 0;
    {
        std::lock_guard<TextureStateMutex> lock(m_TextureMutex);
        if (!m_HUDTexture || m_CreatingTextureID != Texture_None)
            return;
        hudW = m_HUDTexture->GetMappingWidth();
        hudH = m_HUDTexture->GetMappingHeight();
    }
    if (hudW <= 0 || hudH <= 0)
        return;

    NativeHudPanelTree tree{ m_NativeHudPanelApi };
    const NativeHudVPanel root = NativeHudVCall<NativeHudVPanel, int>(m_NativeHudEngineVgui, kEngineVguiGetPanel, kVguiPanelRoot);
    if (!root)
        return;
    const nwh::Rect rootRect = tree.ScreenRect(root);
    const int screenW = rootRect.w > 0 ? rootRect.w : hudW;
    const int screenH = rootRect.h > 0 ? rootRect.h : hudH;
    // HUD element -> texture crop (through the squeeze of its root panel, see PanelMap)
    // plus its laid-out size, which the wrist restores.
    auto toTex = [&](NativeHudVPanel p, const nwh::PanelMap& map) -> nwh::Element
        {
            const nwh::Rect r = tree.ScreenRect(p);
            nwh::Element e;
            e.src = nwh::ScreenToTexture(map.Apply(r), screenW, screenH, hudW, hudH);
            e.w = (int)std::lround((double)r.w * hudW / screenW);
            e.h = (int)std::lround((double)r.h * hudH / screenH);
            return e;
        };

    const bool dump = m_NativeWristHudDumpRequested.exchange(false, std::memory_order_acq_rel);
    const uint64_t nowMs = GetTickCount64();
    const bool logLayout = dump || (m_HandHudDebugLog && nowMs - m_NativeWristHudLastLayoutLogMs >= 5000u);
    if (logLayout)
        m_NativeWristHudLastLayoutLogMs = nowMs;

    nwh::Element own{};
    nwh::Rect ownPanel{};
    nwh::Element weaponPanel{};
    std::vector<std::pair<int, nwh::Teammate>> mates;
    std::vector<nwh::Rect> mateHide;

    // Visible-only depth-first walk: hidden subtrees (menus, unused HUD variants) are skipped.
    // The client root panels can be narrower than the CBaseViewport laid out inside them;
    // VGUI then paints that HUD squeezed into the root's rectangle, so each subtree carries
    // the mapping from layout to painted coordinates.
    struct Node { NativeHudVPanel panel; int depth; nwh::Rect parentRect; nwh::PanelMap map; };
    std::vector<Node> stack;
    stack.reserve(256);
    stack.push_back({ root, 0, rootRect, nwh::PanelMap{} });
    int visited = 0;
    while (!stack.empty() && visited < 6000)
    {
        const Node node = stack.back();
        stack.pop_back();
        ++visited;
        const char* name = tree.Name(node.panel);
        const nwh::Rect nodeRect = tree.ScreenRect(node.panel);
        if (logLayout && node.depth <= 7 && *name)
        {
            Game::logMsg("[VR][WristHUD] panel %*s%s %d,%d %dx%d", node.depth * 2, "", name,
                nodeRect.x, nodeRect.y, nodeRect.w, nodeRect.h);
        }

        if (logLayout && (std::strcmp(name, "LocalPlayer") == 0 || std::strncmp(name, "TeamPlayer", 10) == 0))
        {
            const int n = tree.ChildCount(node.panel);
            for (int i = 0; i < n && i < 64; ++i)
            {
                const NativeHudVPanel c = tree.Child(node.panel, i);
                const nwh::Rect r = c ? tree.ScreenRect(c) : nwh::Rect{};
                Game::logMsg("[VR][WristHUD] panel %*s%s %d,%d %dx%d%s", node.depth * 2 + 2, "", c ? tree.Name(c) : "?",
                    r.x, r.y, r.w, r.h, c && tree.Visible(c) ? "" : " (hidden)");
            }
        }
        if (std::strcmp(name, "LocalPlayer") == 0 && ownPanel.Empty())
        {
            ownPanel = toTex(node.panel, node.map).src;
            for (const char* part : { "HealthIcon", "HealthNumber", "Health" })
                if (const NativeHudVPanel c = tree.FindVisibleChild(node.panel, part))
                    own = nwh::Union(own, toTex(c, node.map));
            continue;
        }
        if (std::strncmp(name, "TeamPlayer", 10) == 0)
        {
            // Bar row: the health bar plus the item icons above it (medkit, pills, grenade).
            // The portrait (Head) and the Status label above it are left out.
            nwh::Teammate mate{};
            if (const NativeHudVPanel c = tree.FindVisibleChild(node.panel, "Health"))
                mate.bar = toTex(c, node.map);
            if (!mate.bar.Empty())
            {
                for (const char* icon : { "IconForSlot_Grenade", "IconForSlot_First_Aid", "IconForSlot_Pills" })
                    if (const NativeHudVPanel c = tree.FindVisibleChild(node.panel, icon))
                        mate.bar = nwh::Union(mate.bar, toTex(c, node.map));
            }
            if (const NativeHudVPanel c = tree.FindVisibleChild(node.panel, "Name"))
                mate.name = toTex(c, node.map);
            if (!mate.bar.Empty() || !mate.name.Empty())
            {
                mates.push_back({ std::atoi(name + 10), mate });
                mateHide.push_back(toTex(node.panel, node.map).src);
            }
            continue;
        }
        if (std::strcmp(name, "HudWeaponSelection") == 0 && weaponPanel.Empty())
        {
            weaponPanel = toTex(node.panel, node.map);
            continue;
        }

        nwh::PanelMap childMap = node.map;
        if (std::strcmp(name, "CBaseViewport") == 0)
            childMap = nwh::PanelMap::Between(nodeRect, node.parentRect);
        const int n = tree.ChildCount(node.panel);
        for (int i = n - 1; i >= 0; --i)
        {
            const NativeHudVPanel c = tree.Child(node.panel, i);
            if (c && tree.Visible(c))
                stack.push_back({ c, node.depth + 1, nodeRect, childMap });
        }
    }

    std::stable_sort(mates.begin(), mates.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<nwh::Teammate> mateList;
    for (const auto& m : mates)
        mateList.push_back(m.second);

    nwh::Job job;
    job.epoch = m_SourceRenderQueueMarkerEpoch.load(std::memory_order_acquire);
    job.dump = dump;
    if (m_LeftWristHudEnabled || dump)
        job.left = nwh::ComposeLeft(own, m_LeftWristHudShowTeammates ? mateList : std::vector<nwh::Teammate>{}, nwh::kLeftTexW, nwh::kLeftTexH);
    if (m_RightAmmoHudEnabled || dump)
    {
        job.trimSrc = weaponPanel.src;
        float trim[4];
        {
            std::lock_guard<std::mutex> lock(m_NativeWristTrimMutex);
            std::memcpy(trim, m_NativeWristTrim, sizeof(trim));
        }
        job.right = nwh::ComposeRight(nwh::Trim(weaponPanel, trim[0], trim[1], trim[2], trim[3]),
            nwh::kRightTexW, nwh::kRightTexH);
    }
    if (m_WristHudNativeHideOnFloatingHud)
    {
        if (m_NativeWristHudShown[0].load(std::memory_order_acquire) && !job.left.Empty())
        {
            job.hide.push_back(ownPanel);
            if (m_LeftWristHudShowTeammates)
                job.hide.insert(job.hide.end(), mateHide.begin(), mateHide.end());
        }
        if (m_NativeWristHudShown[1].load(std::memory_order_acquire) && !job.right.Empty())
            job.hide.push_back(weaponPanel.src);
    }

    if (logLayout)
    {
        Game::logMsg("[VR][WristHUD] layout screen=%dx%d hud=%dx%d visited=%d own=%d,%d %dx%d (%dx%d) mates=%zu weapon=%d,%d %dx%d (%dx%d) left=%zu blits %dx%d right=%zu blits %dx%d hide=%zu",
            screenW, screenH, hudW, hudH, visited, own.src.x, own.src.y, own.src.w, own.src.h, own.W(), own.H(), mateList.size(),
            weaponPanel.src.x, weaponPanel.src.y, weaponPanel.src.w, weaponPanel.src.h, weaponPanel.W(), weaponPanel.H(),
            job.left.blits.size(), job.left.usedW, job.left.usedH, job.right.blits.size(), job.right.usedW, job.right.usedH,
            job.hide.size());
    }

    const bool queued = m_Game->GetMatQueueMode() != 0;
    if (!queued)
    {
        RenderNativeWristHud(job);
        return;
    }
    void* contextVtable = nullptr;
    void* getCallQueue = nullptr;
    ICallQueue* queue = GetSourceRenderContextCallQueue(renderContext, &contextVtable, &getCallQueue);
    void* queueVtable = nullptr;
    if (!IsSourceCallQueueUsable(queue, &queueVtable))
        return;
    queue->QueueFunctor(new NativeWristHudCopyFunctor(this, std::move(job)));
}

// D3D thread (the material worker in queued mode), after the HUD paint commands ran.
// Lock order: m_TextureMutex is only held to take references and create resources,
// never while holding the D3D device lock (Present holds m_TextureMutex while
// TransferSurface takes the device lock), and it is only tried, never waited for.
void VR::RenderNativeWristHud(const l4d2vr_native_wrist_hud::Job& job)
{
    if (m_SourceRenderQueueMarkerEpoch.load(std::memory_order_acquire) != job.epoch || !g_D3DVR9)
        return;

    struct Refs
    {
        IDirect3DSurface9* hud = nullptr;
        IDirect3DSurface9* wrist[2] = {};
        IDirect3DSurface9* trimRT = nullptr;
        IDirect3DSurface9* trimSys = nullptr;
        IDirect3DDevice9* device = nullptr;
        ~Refs()
        {
            for (IDirect3DSurface9* s : { hud, wrist[0], wrist[1], trimRT, trimSys })
                if (s) s->Release();
            if (device) device->Release();
        }
    } refs;

    const int texW[2] = { nwh::kLeftTexW, nwh::kRightTexW };
    const int texH[2] = { nwh::kLeftTexH, nwh::kRightTexH };
    const nwh::Composition* comps[2] = { &job.left, &job.right };
    const uint64_t nowMs = GetTickCount64();
    D3DSURFACE_DESC hudDesc{};
    nwh::Rect trimSrc{};
    bool startTrim = false;
    {
        // try_lock: the main thread can hold m_TextureMutex while it waits for this
        // material worker (texture recreation); skipping one copy is harmless.
        std::unique_lock<TextureStateMutex> textureLock(m_TextureMutex, std::try_to_lock);
        if (!textureLock.owns_lock())
            return;
        if (m_CreatingTextureID != Texture_None || !m_D9HUDSurface)
            return;
        if (FAILED(m_D9HUDSurface->GetDesc(&hudDesc)) || FAILED(m_D9HUDSurface->GetDevice(&refs.device)) || !refs.device)
            return;
        refs.hud = m_D9HUDSurface;
        refs.hud->AddRef();
        IDirect3DDevice9* device = refs.device;

        for (int side = 0; side < 2; ++side)
        {
            if (comps[side]->Empty())
                continue;
            IDirect3DTexture9*& tex = m_D9NativeWristHudTex[side];
            IDirect3DSurface9*& surf = m_D9NativeWristHudSurface[side];
            if (!tex)
            {
                if (FAILED(device->CreateTexture((UINT)texW[side], (UINT)texH[side], 1, D3DUSAGE_RENDERTARGET,
                    hudDesc.Format, D3DPOOL_DEFAULT, &tex, nullptr)) || !tex)
                {
                    tex = nullptr;
                    continue;
                }
                D3D9_TEXTURE_VR_DESC desc{};
                if (FAILED(tex->GetSurfaceLevel(0, &surf)) || !surf || FAILED(g_D3DVR9->GetVRDesc(surf, &desc)))
                {
                    if (surf) { surf->Release(); surf = nullptr; }
                    tex->Release();
                    tex = nullptr;
                    continue;
                }
                SharedTextureHolder& vk = m_VKNativeWristHud[side];
                std::memcpy(&vk.m_VulkanData, &desc, sizeof(vr::VRVulkanTextureData_t));
                vk.m_VRTexture.handle = &vk.m_VulkanData;
                vk.m_VRTexture.eColorSpace = vr::ColorSpace_Auto;
                vk.m_VRTexture.eType = vr::TextureType_Vulkan;
                Game::logMsg("[VR][WristHUD] created %s wrist texture %dx%d format=%d", side == 0 ? "left" : "right",
                    texW[side], texH[side], (int)hudDesc.Format);
            }
            refs.wrist[side] = surf;
            surf->AddRef();
        }

        // Weapon panel trim targets (see the readback below).
        trimSrc = nwh::Clip(job.trimSrc, (int)hudDesc.Width, (int)hudDesc.Height);
        startTrim = m_NativeWristTrimPendingFrames < 0 && !trimSrc.Empty() &&
            (job.dump || nowMs - m_NativeWristTrimLastMs >= 500u) && hudDesc.Format == D3DFMT_A8R8G8B8;
        if (startTrim && (m_NativeWristTrimW != trimSrc.w || m_NativeWristTrimH != trimSrc.h))
        {
            if (m_D9NativeWristTrimRT) { m_D9NativeWristTrimRT->Release(); m_D9NativeWristTrimRT = nullptr; }
            if (m_D9NativeWristTrimSys) { m_D9NativeWristTrimSys->Release(); m_D9NativeWristTrimSys = nullptr; }
            m_NativeWristTrimW = m_NativeWristTrimH = 0;
            if (SUCCEEDED(device->CreateRenderTarget((UINT)trimSrc.w, (UINT)trimSrc.h, hudDesc.Format, D3DMULTISAMPLE_NONE, 0, FALSE,
                    &m_D9NativeWristTrimRT, nullptr)) &&
                SUCCEEDED(device->CreateOffscreenPlainSurface((UINT)trimSrc.w, (UINT)trimSrc.h, hudDesc.Format, D3DPOOL_SYSTEMMEM,
                    &m_D9NativeWristTrimSys, nullptr)))
            {
                m_NativeWristTrimW = trimSrc.w;
                m_NativeWristTrimH = trimSrc.h;
            }
        }
        if (m_D9NativeWristTrimRT && m_D9NativeWristTrimSys)
        {
            refs.trimRT = m_D9NativeWristTrimRT;
            refs.trimSys = m_D9NativeWristTrimSys;
            refs.trimRT->AddRef();
            refs.trimSys->AddRef();
        }
    }

    struct DeviceLock
    {
        bool locked = false;
        explicit DeviceLock(bool required)
        { if (required && g_D3DVR9) locked = SUCCEEDED(g_D3DVR9->LockDevice()); }
        ~DeviceLock() { if (locked && g_D3DVR9) g_D3DVR9->UnlockDevice(); }
    } deviceLock(m_Game && m_Game->GetMatQueueMode() != 0);
    if (m_Game && m_Game->GetMatQueueMode() != 0 && !deviceLock.locked)
        return;

    IDirect3DDevice9* device = refs.device;
    IDirect3DSurface9* hud = refs.hud;
    for (int side = 0; side < 2; ++side)
    {
        const nwh::Composition& comp = *comps[side];
        IDirect3DSurface9* surf = refs.wrist[side];
        if (comp.Empty() || !surf)
            continue;
        device->ColorFill(surf, nullptr, D3DCOLOR_ARGB(0, 0, 0, 0));
        bool ok = true;
        for (const nwh::Blit& b : comp.blits)
        {
            const nwh::Rect src = nwh::Clip(b.src, (int)hudDesc.Width, (int)hudDesc.Height);
            if (src.Empty() || b.dst.Empty())
                continue;
            const RECT s{ src.x, src.y, src.Right(), src.Bottom() };
            const RECT d{ b.dst.x, b.dst.y, b.dst.Right(), b.dst.Bottom() };
            ok = SUCCEEDED(device->StretchRect(hud, &s, surf, &d, D3DTEXF_LINEAR)) && ok;
        }
        if (!ok)
            continue;
        m_NativeWristHudUsedSize[side].store(((uint32_t)comp.usedW << 16) | (uint32_t)comp.usedH, std::memory_order_release);
        m_NativeWristHudContentMs[side].store(nowMs, std::memory_order_release);
        m_NativeWristHudTexReady[side].store(true, std::memory_order_release);
    }

    // Self-test dumps: the first set is l4d2vr_selftest_wrist_*.bmp, later ones get _2, _3, ...
    const int dumpIndex = m_NativeWristHudDumpCount.load(std::memory_order_acquire);
    auto dumpName = [&](const char* part) -> std::string
        {
            std::string name = std::string("l4d2vr_selftest_wrist_") + part;
            if (dumpIndex > 0)
                name += "_" + std::to_string(dumpIndex + 1);
            return name + ".bmp";
        };
    if (job.dump)
    {
        NativeWristHudDumpSurface(device, hud, dumpName("hud").c_str(), 0, 0);
        if (refs.wrist[0] && !job.left.Empty())
            NativeWristHudDumpSurface(device, refs.wrist[0], dumpName("left").c_str(), job.left.usedW, job.left.usedH);
        if (refs.wrist[1] && !job.right.Empty())
            NativeWristHudDumpSurface(device, refs.wrist[1], dumpName("right").c_str(), job.right.usedW, job.right.usedH);
    }

    // Weapon panel trim: copy the panel 1:1 into a scratch target and read it back; the
    // result is scanned a few frames later (when the GPU is done) for its painted bounds.
    if (m_NativeWristTrimPendingFrames >= 0 && ++m_NativeWristTrimPendingFrames >= 3 && refs.trimSys)
    {
        D3DLOCKED_RECT lr{};
        if (SUCCEEDED(refs.trimSys->LockRect(&lr, nullptr, D3DLOCK_READONLY)) && lr.pBits)
        {
            float u0 = 0.0f, v0 = 0.0f, u1 = 1.0f, v1 = 1.0f;
            if (nwh::AlphaBounds(reinterpret_cast<const uint8_t*>(lr.pBits), m_NativeWristTrimW, m_NativeWristTrimH,
                lr.Pitch, 8, 2, u0, v0, u1, v1))
            {
                std::lock_guard<std::mutex> lock(m_NativeWristTrimMutex);
                m_NativeWristTrim[0] = u0; m_NativeWristTrim[1] = v0;
                m_NativeWristTrim[2] = u1; m_NativeWristTrim[3] = v1;
            }
            refs.trimSys->UnlockRect();
        }
        m_NativeWristTrimPendingFrames = -1;
    }
    if (startTrim && refs.trimRT && refs.trimSys && m_NativeWristTrimW == trimSrc.w && m_NativeWristTrimH == trimSrc.h)
    {
        const RECT s{ trimSrc.x, trimSrc.y, trimSrc.Right(), trimSrc.Bottom() };
        if (SUCCEEDED(device->StretchRect(hud, &s, refs.trimRT, nullptr, D3DTEXF_NONE)) &&
            SUCCEEDED(device->GetRenderTargetData(refs.trimRT, refs.trimSys)))
        {
            m_NativeWristTrimPendingFrames = 0;
            m_NativeWristTrimLastMs = nowMs;
        }
    }

    // The floating HUD shows the same texture: clear what the wrists already show.
    for (const nwh::Rect& r : job.hide)
    {
        const nwh::Rect c = nwh::Clip(r, (int)hudDesc.Width, (int)hudDesc.Height);
        if (c.Empty())
            continue;
        const RECT rc{ c.x, c.y, c.Right(), c.Bottom() };
        device->ColorFill(hud, &rc, D3DCOLOR_ARGB(0, 0, 0, 0));
    }

    if (job.dump)
    {
        if (!job.hide.empty())
            NativeWristHudDumpSurface(device, hud, dumpName("hud_floating").c_str(), 0, 0);
        m_NativeWristHudDumpCount.fetch_add(1, std::memory_order_acq_rel);
    }
}

// Called with m_TextureMutex held (texture teardown).
void VR::DestroyNativeWristHudTextures()
{
    for (int side = 0; side < 2; ++side)
    {
        m_NativeWristHudTexReady[side].store(false, std::memory_order_release);
        m_NativeWristHudContentMs[side].store(0, std::memory_order_release);
        if (m_D9NativeWristHudSurface[side])
        {
            m_D9NativeWristHudSurface[side]->Release();
            m_D9NativeWristHudSurface[side] = nullptr;
        }
        if (m_D9NativeWristHudTex[side])
        {
            m_D9NativeWristHudTex[side]->Release();
            m_D9NativeWristHudTex[side] = nullptr;
        }
        std::memset(&m_VKNativeWristHud[side], 0, sizeof(m_VKNativeWristHud[side]));
    }
    if (m_D9NativeWristTrimRT) { m_D9NativeWristTrimRT->Release(); m_D9NativeWristTrimRT = nullptr; }
    if (m_D9NativeWristTrimSys) { m_D9NativeWristTrimSys->Release(); m_D9NativeWristTrimSys = nullptr; }
    m_NativeWristTrimW = m_NativeWristTrimH = 0;
    m_NativeWristTrimPendingFrames = -1;
}

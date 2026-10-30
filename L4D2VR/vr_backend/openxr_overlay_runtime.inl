// OpenXR overlays (OpenXR port M4). Included by vr_backend_openxr.cpp inside its
// anonymous namespace, after openxr_input_runtime.inl.
//
// The mod keeps its IVROverlay calls; this class answers them with OpenXR
// composition layers:
//   * Every overlay keeps its OpenVR state (transform, width, bounds, flags,
//     sort order, curvature, alpha) and owns one XrSwapchain sized to its
//     content. SetOverlayTexture copies the Vulkan image into the swapchain at
//     once (the caller holds DXVK's queue lock, as for the eye Submit: the
//     image may change right after the call, exactly like SteamVR's copy).
//     SetOverlayRaw keeps the pixels and uploads them at the next frame end.
//   * At xrEndFrame the visible overlays become quad layers (cylinder layers
//     when curved and XR_KHR_composition_layer_cylinder is enabled) in the
//     base space, after the projection, sorted by sort order. The runtime's
//     maxLayerCount is a budget: kill indicators are dropped first.
//   * wineopenxr (Proton #10275) corrupts a frame whose projection layer is
//     followed by 2+ layers; a 4x4 transparent quad goes first then.
//   * SetOverlayAlpha uses XR_KHR_composition_layer_color_scale_bias on native
//     runtimes. Under Wine (which drops layer next chains) and without the
//     extension the copied pixels' alpha is scaled instead.
//   * Overlay mouse: SteamVR's laser rebuilt here. Each input sync ray-casts
//     both controller tips against the visible, interactive mouse overlays,
//     queues OpenVR mouse/scroll events (PollNextOverlayEvent), draws a beam
//     and a dot, and keeps the pointing hand's trigger away from the game
//     (SteamVR does the same). ComputeOverlayIntersection uses the same
//     surfaces.
//   * Keyboard: ShowKeyboardForOverlay opens an in-mod keyboard panel (one
//     more overlay, GDI-rendered) driven by the same laser; it sends
//     KeyboardCharInput/Done/Closed to the overlay it was opened for.
//   * Debug: TestCommand("dump=<prefix>") saves every submitted layer image as
//     <prefix>_<key>.bmp (after any flip/alpha, i.e. what the runtime gets) and
//     logs each layer's type, pose, size and rect next to the OpenVR transform.

namespace ovl = l4d2vr_openxr_overlay;
namespace opn = l4d2vr_overlay_pointer;

struct XrOverlayDevice
{
    XrSession session = XR_NULL_HANDLE;
    XrSpace base = XR_NULL_HANDLE;
    XrSpace view = XR_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queueFamily = 0;
    VkPhysicalDeviceMemoryProperties memory{};
    std::vector<int64_t> formats;
};

class XrOverlayRuntime final : public IVrOverlay, public XrPointerHook
{
public:
    static constexpr vr::VROverlayHandle_t kInternalBase = 0x7F000000ull;
    static constexpr size_t kMaxQueuedEvents = 128;
    static constexpr uint64_t kEventMaxAgeMs = 500;

    XrOverlayRuntime(const XrApi& xr, const VkApi& vk, XrInputRuntime& input) : m_Xr(xr), m_Vk(vk), m_Input(input) {}

    // After xrCreateInstance: what the runtime offers.
    void Configure(bool wine, bool cylinder, bool colorScaleBias, uint32_t maxLayers)
    {
        m_Wine = wine;
        m_Cylinder = cylinder;
        // wineopenxr's layer thunks drop next chains ("Unexpected next"): the
        // colour scale would be ignored, so fade the pixels instead.
        m_ColorBias = colorScaleBias && !wine;
        m_MaxLayers = maxLayers;
        const std::string mode = EnvValue("L4D2VR_XR_LAYER_WORKAROUND");
        m_Workaround = !wine || mode == "off" ? ovl::Workaround::Off : mode == "always" ? ovl::Workaround::Always : ovl::Workaround::Auto;
        Log("overlays: %u layers max, cylinder %d, colour scale %d (offered %d), wine layer workaround %s",
            maxLayers, cylinder ? 1 : 0, m_ColorBias ? 1 : 0, colorScaleBias ? 1 : 0,
            m_Workaround == ovl::Workaround::Off ? "off" : m_Workaround == ovl::Workaround::Always ? "always" : "auto");
    }

    // Session and Vulkan objects (DXVK's device). Called once.
    void SessionCreated(const XrOverlayDevice& device)
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_Device = device;
        VkCommandPoolCreateInfo pci{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pci.queueFamilyIndex = device.queueFamily;
        if (m_Vk.vkCreateCommandPool(device.device, &pci, nullptr, &m_Pool) != VK_SUCCESS)
            return;
        VkCommandBufferAllocateInfo cai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        cai.commandPool = m_Pool;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        if (m_Vk.vkAllocateCommandBuffers(device.device, &cai, &m_Cmd) != VK_SUCCESS)
            return;
        VkFenceCreateInfo fci{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        if (m_Vk.vkCreateFence(device.device, &fci, nullptr, &m_Fence) != VK_SUCCESS)
            return;
        m_Ready = true;
        CreateInternalLocked();
    }

    // The HMD pose in the base space (keyboard placement, beam facing).
    std::function<bool(vr::HmdMatrix34_t&)> hmdPose;

    // ---- IVROverlay -------------------------------------------------------------
    vr::EVROverlayError FindOverlay(const char* key, vr::VROverlayHandle_t* handle) override
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        if (!key || !handle) return vr::VROverlayError_InvalidParameter;
        for (const auto& it : m_Overlays)
            if (it.second.key == key && !it.second.internal)
            {
                *handle = it.first;
                return vr::VROverlayError_None;
            }
        *handle = vr::k_ulOverlayHandleInvalid;
        return vr::VROverlayError_UnknownOverlay;
    }
    vr::EVROverlayError CreateOverlay(const char* key, const char* name, vr::VROverlayHandle_t* handle) override
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        if (!key || !handle) return vr::VROverlayError_InvalidParameter;
        for (const auto& it : m_Overlays)
            if (it.second.key == key)
                return vr::VROverlayError_KeyInUse;
        const vr::VROverlayHandle_t h = ++m_NextHandle;
        Overlay& o = m_Overlays[h];
        o.key = key;
        o.name = name ? name : "";
        o.serial = ++m_Serial;
        o.priority = ovl::PriorityForKey(o.key);
        *handle = h;
        return vr::VROverlayError_None;
    }
    vr::EVROverlayError DestroyOverlay(vr::VROverlayHandle_t h) override
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        auto it = m_Overlays.find(h);
        if (it == m_Overlays.end() || it->second.internal)
            return vr::VROverlayError_InvalidHandle;
        // Destroyed at the next frame end, under DXVK's queue lock.
        if (it->second.sc.handle != XR_NULL_HANDLE)
            m_Graveyard.push_back(it->second.sc);
        m_Overlays.erase(it);
        return vr::VROverlayError_None;
    }
    vr::EVROverlayError SetOverlayFlag(vr::VROverlayHandle_t h, vr::VROverlayFlags flag, bool on) override
    {
        return With(h, [&](Overlay& o) { if (on) o.flags |= uint64_t(flag); else o.flags &= ~uint64_t(flag); });
    }
    vr::EVROverlayError SetOverlayAlpha(vr::VROverlayHandle_t h, float a) override
    {
        return With(h, [&](Overlay& o) { o.alpha = std::isfinite(a) ? std::clamp(a, 0.0f, 1.0f) : 1.0f; });
    }
    vr::EVROverlayError SetOverlayTexelAspect(vr::VROverlayHandle_t h, float f) override { return With(h, [&](Overlay& o) { o.texelAspect = f; }); }
    vr::EVROverlayError SetOverlaySortOrder(vr::VROverlayHandle_t h, uint32_t u) override { return With(h, [&](Overlay& o) { o.sortOrder = u; }); }
    vr::EVROverlayError SetOverlayWidthInMeters(vr::VROverlayHandle_t h, float f) override
    {
        if (!std::isfinite(f) || f <= 0.0f)
            return vr::VROverlayError_InvalidParameter;
        return With(h, [&](Overlay& o) { o.width = f; });
    }
    vr::EVROverlayError GetOverlayWidthInMeters(vr::VROverlayHandle_t h, float* pf) override { return With(h, [&](Overlay& o) { if (pf) *pf = o.width; }); }
    vr::EVROverlayError SetOverlayCurvature(vr::VROverlayHandle_t h, float f) override
    {
        return With(h, [&](Overlay& o) { o.curvature = std::isfinite(f) ? std::clamp(f, 0.0f, 1.0f) : 0.0f; });
    }
    vr::EVROverlayError GetOverlayCurvature(vr::VROverlayHandle_t h, float* pf) override { return With(h, [&](Overlay& o) { if (pf) *pf = o.curvature; }); }
    vr::EVROverlayError SetOverlayTextureBounds(vr::VROverlayHandle_t h, const vr::VRTextureBounds_t* b) override
    {
        return With(h, [&](Overlay& o) { if (b) o.bounds = { b->uMin, b->vMin, b->uMax, b->vMax }; });
    }
    vr::EVROverlayError GetOverlayTransformType(vr::VROverlayHandle_t h, vr::VROverlayTransformType* pe) override
    {
        return With(h, [&](Overlay&) { if (pe) *pe = vr::VROverlayTransform_Absolute; });
    }
    vr::EVROverlayError SetOverlayTransformAbsolute(vr::VROverlayHandle_t h, vr::ETrackingUniverseOrigin origin,
        const vr::HmdMatrix34_t* m) override
    {
        if (!m)
            return vr::VROverlayError_InvalidParameter;
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 4; ++c)
                if (!std::isfinite(m->m[r][c]))
                    return vr::VROverlayError_InvalidParameter;
        return With(h, [&](Overlay& o) { o.origin = origin; o.transform = *m; o.placed = true; });
    }
    vr::EVROverlayError GetOverlayTransformAbsolute(vr::VROverlayHandle_t h, vr::ETrackingUniverseOrigin* origin, vr::HmdMatrix34_t* m) override
    {
        return With(h, [&](Overlay& o) { if (origin) *origin = o.origin; if (m) *m = o.transform; });
    }
    vr::EVROverlayError GetOverlayTransformTrackedDeviceRelative(vr::VROverlayHandle_t, vr::TrackedDeviceIndex_t* device, vr::HmdMatrix34_t*) override
    {
        // The mod places device-relative overlays as absolute ones (vr.cpp).
        if (device) *device = vr::k_unTrackedDeviceIndexInvalid;
        return vr::VROverlayError_RequestFailed;
    }
    vr::EVROverlayError ShowOverlay(vr::VROverlayHandle_t h) override { return With(h, [&](Overlay& o) { o.visible = true; }); }
    vr::EVROverlayError HideOverlay(vr::VROverlayHandle_t h) override { return With(h, [&](Overlay& o) { o.visible = false; }); }
    bool IsOverlayVisible(vr::VROverlayHandle_t h) override
    {
        bool visible = false;
        With(h, [&](Overlay& o) { visible = o.visible; });
        return visible;
    }
    bool PollNextOverlayEvent(vr::VROverlayHandle_t h, vr::VREvent_t* event, uint32_t size) override
    {
        if (!event || size < sizeof(vr::VREvent_t))
            return false;
        std::lock_guard<std::mutex> lock(m_Mutex);
        auto it = m_Overlays.find(h);
        if (it == m_Overlays.end())
            return false;
        // Events are live input: ones nobody polled for half a second (the mod
        // polls the menu/HUD only while it sees the laser on them) are dropped,
        // so an old click never lands later.
        std::deque<QueuedEvent>& q = it->second.events;
        const uint64_t now = GetTickCount64();
        while (!q.empty() && now - q.front().ms > kEventMaxAgeMs)
        {
            q.pop_front();
            ++m_EventsExpired;
        }
        if (q.empty())
            return false;
        *event = q.front().event;
        event->eventAgeSeconds = static_cast<float>(now - q.front().ms) / 1000.0f;
        q.pop_front();
        ++m_EventsDelivered;
        return true;
    }
    vr::EVROverlayError SetOverlayInputMethod(vr::VROverlayHandle_t h, vr::VROverlayInputMethod method) override
    {
        return With(h, [&](Overlay& o) { o.inputMethod = method; });
    }
    vr::EVROverlayError SetOverlayMouseScale(vr::VROverlayHandle_t h, const vr::HmdVector2_t* scale) override
    {
        return With(h, [&](Overlay& o) { if (scale && scale->v[0] > 0.0f && scale->v[1] > 0.0f) o.mouseScale = *scale; });
    }
    bool ComputeOverlayIntersection(vr::VROverlayHandle_t h, const vr::VROverlayIntersectionParams_t* params,
        vr::VROverlayIntersectionResults_t* results) override
    {
        if (!params)
            return false;
        std::lock_guard<std::mutex> lock(m_Mutex);
        auto it = m_Overlays.find(h);
        if (it == m_Overlays.end() || !it->second.visible)
            return false;
        ovl::Geometry g;
        if (!GeometryLocked(it->second, g))
            return false;
        opn::Ray ray;
        ray.origin = { params->vSource.v[0], params->vSource.v[1], params->vSource.v[2] };
        if (!opn::Normalize(opn::Vec3{ params->vDirection.v[0], params->vDirection.v[1], params->vDirection.v[2] }, ray.direction))
            return false;
        opn::Hit hit;
        if (!opn::Intersect(g.panel, ray, ovl::kPointerMaxDistance, hit))
            return false;
        if (results)
        {
            results->vPoint = { { hit.point.x, hit.point.y, hit.point.z } };
            results->vNormal = { { hit.normal.x, hit.normal.y, hit.normal.z } };
            results->vUVs = { { hit.u, 1.0f - hit.v } };
            results->fDistance = hit.distance;
        }
        return true;
    }
    vr::EVROverlayError SetOverlayTexture(vr::VROverlayHandle_t h, const vr::Texture_t* texture) override
    {
        if (!texture || !texture->handle)
            return vr::VROverlayError_InvalidParameter;
        if (texture->eType != vr::TextureType_Vulkan)
            return vr::VROverlayError_InvalidTexture;
        const auto* data = static_cast<const vr::VRVulkanTextureData_t*>(texture->handle);
        if (!data->m_nImage || data->m_nWidth == 0 || data->m_nHeight == 0 || data->m_nSampleCount > 1)
            return vr::VROverlayError_InvalidTexture;
        std::unique_lock<std::mutex> lock(m_Mutex, std::defer_lock);
        {
            Crumb crumb("SetOverlayTexture waiting for the overlay lock");
            lock.lock();
        }
        auto it = m_Overlays.find(h);
        if (it == m_Overlays.end())
            return vr::VROverlayError_InvalidHandle;
        Overlay& o = it->second;
        o.kind = Content::Vulkan;
        o.source = (VkImage)data->m_nImage;
        o.sourceWidth = data->m_nWidth;
        o.sourceHeight = data->m_nHeight;
        o.sourceFormat = static_cast<VkFormat>(data->m_nFormat);
        o.raw.clear();
        ++m_TextureCalls;
        // Copy now (queue lock held by the caller): the image changes after the call.
        if (!m_Ready)
            return vr::VROverlayError_None;
        if (!CopyVulkanLocked(o))
            return vr::VROverlayError_RequestFailed;
        return vr::VROverlayError_None;
    }
    vr::EVROverlayError SetOverlayRaw(vr::VROverlayHandle_t h, void* buffer, uint32_t width, uint32_t height, uint32_t bpp) override
    {
        if (!buffer || width == 0 || height == 0 || (bpp != 1 && bpp != 3 && bpp != 4) || width > 8192 || height > 8192)
            return vr::VROverlayError_InvalidParameter;
        std::vector<uint8_t> rgba(static_cast<size_t>(width) * height * 4u);
        const uint8_t* src = static_cast<const uint8_t*>(buffer);
        for (size_t i = 0, n = static_cast<size_t>(width) * height; i < n; ++i)
        {
            uint8_t* d = &rgba[i * 4];
            if (bpp == 4) { d[0] = src[i * 4]; d[1] = src[i * 4 + 1]; d[2] = src[i * 4 + 2]; d[3] = src[i * 4 + 3]; }
            else if (bpp == 3) { d[0] = src[i * 3]; d[1] = src[i * 3 + 1]; d[2] = src[i * 3 + 2]; d[3] = 255; }
            else { d[0] = d[1] = d[2] = src[i]; d[3] = 255; }
        }
        std::lock_guard<std::mutex> lock(m_Mutex);
        auto it = m_Overlays.find(h);
        if (it == m_Overlays.end())
            return vr::VROverlayError_InvalidHandle;
        Overlay& o = it->second;
        o.kind = Content::Raw;
        o.raw.swap(rgba);
        o.rawWidth = width;
        o.rawHeight = height;
        o.rawDirty = true;
        o.source = VK_NULL_HANDLE;
        ++m_RawCalls;
        return vr::VROverlayError_None;
    }
    vr::EVROverlayError ShowKeyboardForOverlay(vr::VROverlayHandle_t h, vr::EGamepadTextInputMode, vr::EGamepadTextInputLineMode lineMode,
        uint32_t, const char* description, uint32_t charMax, const char* existing, uint64_t userValue) override
    {
        vr::HmdMatrix34_t hmd{};
        const bool haveHmd = hmdPose && hmdPose(hmd);
        std::lock_guard<std::mutex> lock(m_Mutex);
        if (h != vr::k_ulOverlayHandleInvalid && !m_Overlays.count(h))
            return vr::VROverlayError_InvalidHandle;
        if (!m_Ready || m_KeyboardHandle == 0)
            return vr::VROverlayError_RequestFailed;
        m_Keyboard.Open(description ? description : "", existing ? existing : "", charMax,
            lineMode == vr::k_EGamepadTextInputLineModeMultipleLines, userValue);
        m_KeyboardTarget = h;
        m_KeyboardHover = -1;
        Overlay& kb = m_Overlays[m_KeyboardHandle];
        kb.transform = KeyboardPlacement(haveHmd ? &hmd : nullptr);
        kb.visible = true;
        RenderKeyboardLocked();
        Log("keyboard opened for overlay %llu ('%s', %u chars max, text '%s')", static_cast<unsigned long long>(h),
            description ? description : "", charMax, existing ? existing : "");
        return vr::VROverlayError_None;
    }
    uint32_t GetKeyboardText(char* text, uint32_t cch) override
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        const std::string& s = m_Keyboard.Text();
        if (!text || cch == 0)
            return static_cast<uint32_t>(s.size() + 1);
        const size_t n = (std::min)(s.size(), static_cast<size_t>(cch - 1));
        std::memcpy(text, s.data(), n);
        text[n] = '\0';
        return static_cast<uint32_t>(n + 1);
    }
    void HideKeyboard() override
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        CloseKeyboardLocked("hidden by the mod");
    }

    // ---- Frame end (DXVK queue lock and the runtime's frame mutex held) ---------
    // Uploads pending content and returns the full layer list: [dummy]
    // [projection] overlays. Pointers stay valid until the next call.
    const std::vector<const XrCompositionLayerBaseHeader*>& BuildLayers(const XrCompositionLayerBaseHeader* projection)
    {
        std::unique_lock<std::mutex> lock(m_Mutex, std::defer_lock);
        {
            Crumb crumb("frame end waiting for the overlay lock");
            lock.lock();
        }
        m_Layers.clear();
        m_Quads.clear();
        m_Cylinders.clear();
        m_ColorScales.clear();
        m_LayerInfo.clear();
        if (!m_Ready)
        {
            if (projection)
                m_Layers.push_back(projection);
            return m_Layers;
        }
        for (Swapchain& dead : m_Graveyard)
            DestroySwapchainLocked(dead);
        m_Graveyard.clear();
        if (m_DumpEveryLeft && m_DumpPrefix.empty() && GetTickCount64() >= m_DumpEveryNextMs)
        {
            --m_DumpEveryLeft;
            m_DumpPrefix = m_DumpEveryPrefix + "_" + std::to_string(++m_DumpEveryIndex);
            m_DumpEveryNextMs = GetTickCount64() + m_DumpEveryMs;
            for (auto& it : m_Overlays)
                it.second.dumped = false;
        }

        // Candidates: visible overlays with content and a placement.
        struct Pending { vr::VROverlayHandle_t handle; ovl::Geometry g; };
        std::vector<Pending> pending;
        std::vector<ovl::Candidate> candidates;
        for (auto& it : m_Overlays)
        {
            Overlay& o = it.second;
            if (!o.visible || o.kind == Content::None)
                continue;
            if (o.kind == Content::Raw && o.rawDirty)
                UploadRawLocked(o);
            else if (o.sc.handle != XR_NULL_HANDLE && !m_ColorBias && std::fabs(o.appliedAlpha - o.alpha) > 0.002f)
                RefreshAlphaLocked(o);  // faded pixels: re-upload at the new alpha (also back to 1)
            if (!o.sc.released || !o.placed)
                continue;
            ovl::Geometry g;
            if (!GeometryLocked(o, g))
                continue;
            if (!m_ColorBias && o.alpha < 0.004f)
                continue;  // fully faded
            ovl::Candidate c;
            c.id = it.first;
            c.sortOrder = o.sortOrder;
            c.serial = o.serial;
            c.priority = o.internal ? ovl::Priority::High : o.priority;
            candidates.push_back(c);
            pending.push_back(Pending{ it.first, g });
        }
        const uint32_t limit = m_MaxLayersOverride ? m_MaxLayersOverride : m_MaxLayers;
        ovl::Plan plan = ovl::PlanLayers(candidates, limit, projection != nullptr, m_Workaround);
        m_Quads.reserve(plan.order.size() + 1);
        m_Cylinders.reserve(plan.order.size());
        m_ColorScales.reserve(plan.order.size());
        bool dummy = false;
        if (plan.dummyFirst)
        {
            dummy = EnsureDummyLocked();
            if (!dummy)
            {
                // Without the dummy, wineopenxr only survives projection + 1 layer.
                while (plan.order.size() > 1)
                {
                    plan.dropped.push_back(plan.order.front());
                    plan.order.erase(plan.order.begin());
                }
            }
        }
        if (dummy)
        {
            XrCompositionLayerQuad q{ XR_TYPE_COMPOSITION_LAYER_QUAD };
            q.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT | XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
            q.space = m_Device.view != XR_NULL_HANDLE ? m_Device.view : m_Device.base;
            q.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            q.subImage.swapchain = m_Dummy.handle;
            q.subImage.imageRect = { { 0, 0 }, { static_cast<int32_t>(m_Dummy.width), static_cast<int32_t>(m_Dummy.height) } };
            q.pose.orientation.w = 1.0f;
            q.pose.position.z = -1.0f;
            q.size = { 0.001f, 0.001f };
            m_Quads.push_back(q);
            m_LayerInfo.push_back(LayerInfo{ 0, 'D', "(wine dummy)" });
        }
        // The left eye this frame is seen from (dumps: tools/composite.py renders the layers into it).
        if (projection)
        {
            const auto* proj = reinterpret_cast<const XrCompositionLayerProjection*>(projection);
            if (proj->viewCount > 0 && proj->views)
            {
                m_LastEyePose = proj->views[0].pose;
                m_LastEyeFov = proj->views[0].fov;
                m_HaveEye = true;
            }
        }
        {
            char eye[256] = "";
            if (m_HaveEye)
                std::snprintf(eye, sizeof(eye), " left eye pos (%.4f %.4f %.4f) quat (%.5f %.5f %.5f %.5f) fov (%.4f %.4f %.4f %.4f)",
                    m_LastEyePose.position.x, m_LastEyePose.position.y, m_LastEyePose.position.z, m_LastEyePose.orientation.x,
                    m_LastEyePose.orientation.y, m_LastEyePose.orientation.z, m_LastEyePose.orientation.w, m_LastEyeFov.angleLeft,
                    m_LastEyeFov.angleRight, m_LastEyeFov.angleUp, m_LastEyeFov.angleDown);
            m_LayerInfo.push_back(LayerInfo{ 0, 'P', std::string(projection ? "(projection)" : "(no projection, last)") + eye });
        }
        struct Slot { char type; size_t index; };
        std::vector<Slot> slots;
        for (size_t idx : plan.order)
        {
            const Pending& p = pending[idx];
            Overlay& o = m_Overlays[p.handle];
            const ovl::Geometry& g = p.g;
            XrSwapchainSubImage sub{};
            sub.swapchain = o.sc.handle;
            sub.imageRect = { { o.rect.x, o.rect.y }, { o.rect.width, o.rect.height } };
            sub.imageArrayIndex = 0;
            const bool ignoreAlpha = (o.flags & vr::VROverlayFlags_IgnoreTextureAlpha) != 0;
            XrCompositionLayerFlags flags = 0;
            if (!ignoreAlpha || (o.alpha < 0.999f))
                flags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT | XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
            const void* next = nullptr;
            if (m_ColorBias && o.alpha < 0.999f)
            {
                XrCompositionLayerColorScaleBiasKHR cs{ XR_TYPE_COMPOSITION_LAYER_COLOR_SCALE_BIAS_KHR };
                cs.colorScale = { 1.0f, 1.0f, 1.0f, ignoreAlpha ? 0.0f : o.alpha };
                cs.colorBias = { 0.0f, 0.0f, 0.0f, ignoreAlpha ? o.alpha : 0.0f };
                m_ColorScales.push_back(cs);
                next = &m_ColorScales.back();
            }
            if (g.cylinder)
            {
                XrCompositionLayerCylinderKHR c{ XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR };
                c.next = next;
                c.layerFlags = flags;
                c.space = m_Device.base;
                c.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                c.subImage = sub;
                c.pose = ToXrPose(g.pose);
                c.radius = g.radius;
                c.centralAngle = g.centralAngle;
                c.aspectRatio = g.aspectRatio;
                m_Cylinders.push_back(c);
                slots.push_back(Slot{ 'C', m_Cylinders.size() - 1 });
            }
            else
            {
                XrCompositionLayerQuad q{ XR_TYPE_COMPOSITION_LAYER_QUAD };
                q.next = next;
                q.layerFlags = flags;
                q.space = m_Device.base;
                q.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                q.subImage = sub;
                q.pose = ToXrPose(g.pose);
                q.size = { g.width, g.height };
                m_Quads.push_back(q);
                slots.push_back(Slot{ 'Q', m_Quads.size() - 1 });
            }
            m_LayerInfo.push_back(LayerInfo{ p.handle, g.cylinder ? 'C' : 'Q', o.key, g, flags, o.alpha });
        }
        // Assemble: dummy, projection, overlays (the vectors no longer grow).
        if (dummy)
            m_Layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&m_Quads[0]));
        if (projection)
            m_Layers.push_back(projection);
        for (const Slot& slot : slots)
        {
            if (slot.type == 'C')
                m_Layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&m_Cylinders[slot.index]));
            else
                m_Layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&m_Quads[slot.index]));
        }

        // Budget log (on change) and counters.
        m_LastOverlayLayers = static_cast<uint32_t>(plan.order.size());
        m_LastDropped = static_cast<uint32_t>(plan.dropped.size());
        if (!plan.dropped.empty())
        {
            m_DroppedTotal += plan.dropped.size();
            if (plan.dropped.size() != m_LoggedDropped)
            {
                std::string names;
                for (size_t idx : plan.dropped)
                    names += m_Overlays[pending[idx].handle].key + " ";
                Log("layer budget %u (projection %d, wine dummy %d): %zu overlay(s) not shown: %s", limit,
                    projection ? 1 : 0, dummy ? 1 : 0, plan.dropped.size(), names.c_str());
            }
        }
        m_LoggedDropped = plan.dropped.size();
        if (dummy)
            ++m_DummyFrames;
        if (!m_DumpPrefix.empty())
            WriteLayerDumpLocked();
        return m_Layers;
    }

    void FrameEnded(XrResult r)
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        ++m_Frames;
        if (m_LastOverlayLayers)
            ++m_FramesWithOverlays;
        m_MaxOverlayLayers = (std::max)(m_MaxOverlayLayers, m_LastOverlayLayers);
        if (XR_FAILED(r) && m_LastOverlayLayers)
            ++m_FailedOverlayFrames;
    }

    // ---- Overlay mouse (game thread, from XrInputRuntime::Sync) ----------------
    void PointerStep(XrTime now, const XrPointerInputs& in, bool engaged[2]) override
    {
        engaged[0] = engaged[1] = false;
        if (!m_Ready || now <= 0)
            return;
        // Hand rays first: LocateHand may ask AimTarget (this mutex).
        ovl::HandInput hands[2];
        for (int h = 0; h < 2; ++h)
        {
            vr::TrackedDevicePose_t pose{};
            vr::HmdMatrix34_t tip{};
            if (!in.active[h] || !m_Input.LocateHand(h, m_Device.base, now, pose) || !pose.bPoseIsValid || !m_Input.TipLocal(h, tip))
                continue;
            const opn::Mat34 world = opn::Multiply(ToMat(pose.mDeviceToAbsoluteTracking), ToMat(tip));
            hands[h].valid = opn::RayFromPose(world, hands[h].ray);
            hands[h].trigger = in.trigger[h];
            hands[h].stickY = in.stickY[h];
        }
        vr::HmdMatrix34_t hmd{};
        bool haveHmd = false;
        {
            Crumb crumb("laser reading the HMD pose");
            haveHmd = hmdPose && hmdPose(hmd);
        }
        const uint64_t nowMs = GetTickCount64();

        std::unique_lock<std::mutex> lock(m_Mutex, std::defer_lock);
        {
            Crumb crumb("laser waiting for the overlay lock");
            lock.lock();
        }
        std::vector<ovl::Target> targets;
        for (auto& it : m_Overlays)
        {
            Overlay& o = it.second;
            const bool interactive = o.visible && o.inputMethod == vr::VROverlayInputMethod_Mouse &&
                (o.internal || (o.flags & vr::VROverlayFlags_MakeOverlaysInteractiveIfVisible) != 0) && o.sc.released && o.placed;
            if (!interactive)
                continue;
            ovl::Geometry g;
            if (!GeometryLocked(o, g))
                continue;
            ovl::Target t;
            t.id = it.first;
            t.panel = g.panel;
            t.sortOrder = o.sortOrder;
            t.mouseScaleX = o.mouseScale.v[0];
            t.mouseScaleY = o.mouseScale.v[1];
            t.scrollEvents = (o.flags & vr::VROverlayFlags_SendVRDiscreteScrollEvents) != 0;
            targets.push_back(t);
        }
        m_EventScratch.clear();
        const ovl::PointerOutput out = m_Pointer.Step(targets, hands, nowMs, m_EventScratch);
        for (const ovl::Event& e : m_EventScratch)
            DeliverLocked(e);
        engaged[0] = out.engaged[0];
        engaged[1] = out.engaged[1];

        // Keyboard hover highlight.
        int hover = -1;
        if (out.active >= 0 && out.hands[out.active].hit && targets[out.hands[out.active].target].id == m_KeyboardHandle)
        {
            const opn::Hit& hit = out.hands[out.active].where;
            hover = ovl::KeyAt(static_cast<int>(hit.u * ovl::kKeyboardWidth), static_cast<int>(hit.v * ovl::kKeyboardHeight));
        }
        if (m_Keyboard.Active() && hover != m_KeyboardHover)
        {
            m_KeyboardHover = hover;
            RenderKeyboardLocked();
        }

        // Laser beam and dot for the pointing hand.
        Overlay* dot = m_DotHandle ? &m_Overlays[m_DotHandle] : nullptr;
        Overlay* beam = m_BeamHandle ? &m_Overlays[m_BeamHandle] : nullptr;
        bool shown = false;
        if (dot && beam && out.active >= 0 && out.hands[out.active].hit)
        {
            const ovl::HandHit& hh = out.hands[out.active];
            const ovl::Target& t = targets[hh.target];
            opn::Mat34 dotM;
            if (opn::MarkerTransform(t.panel, hh.where, 0.004f, dotM))
            {
                dot->transform = ToHmd(dotM);
                dot->width = std::clamp(0.012f * hh.where.distance, 0.012f, 0.05f) * (out.pressed ? 0.7f : 1.0f);
                dot->placed = dot->visible = true;
                shown = true;
            }
            const opn::Vec3 start = hands[out.active].ray.origin;
            const opn::Vec3 viewer = haveHmd ? opn::Vec3{ hmd.m[0][3], hmd.m[1][3], hmd.m[2][3] } : start;
            opn::Vec3 dir;
            opn::Mat34 beamM;
            float length = 0.0f;
            if (opn::Normalize(opn::Sub(hh.where.point, start), dir) && hh.where.distance > 0.05f &&
                opn::BeamTransform(start, opn::Sub(hh.where.point, opn::Scale(dir, 0.01f)), viewer, beamM, length) && length > 0.01f)
            {
                beam->transform = ToHmd(beamM);
                beam->width = length;
                beam->placed = beam->visible = true;
            }
            else
                beam->visible = false;
        }
        if (!shown)
        {
            if (dot) dot->visible = false;
            if (beam) beam->visible = false;
        }
        m_PointerActive = out.active;
        m_PointerHover = (out.active >= 0 && out.hands[out.active].hit) ? targets[out.hands[out.active].target].id : 0;
        if (out.active >= 0 && out.hands[out.active].hit)
        {
            m_PointerU = out.hands[out.active].where.u;
            m_PointerV = out.hands[out.active].where.v;
        }
        // Log hover changes (the config panel's two alternating copies count as one).
        auto family = [&](vr::VROverlayHandle_t h) {
            auto it = m_Overlays.find(h);
            if (it == m_Overlays.end())
                return std::string();
            const std::string& k = it->second.key;
            const size_t dot = k.rfind('.');
            return (dot != std::string::npos && (k.compare(dot, std::string::npos, ".front") == 0 || k.compare(dot, std::string::npos, ".back") == 0))
                ? k.substr(0, dot) : k;
        };
        const std::string hoverFamily = family(m_PointerHover);
        if (hoverFamily != m_LoggedHover)
        {
            m_LoggedHover = hoverFamily;
            Log("laser %s %s", hoverFamily.empty() ? "off" : "on", hoverFamily.c_str());
        }
    }

    // Self-test aim override: the world point a hand must point at.
    bool AimTarget(int hand, opn::Vec3& target) override
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        if (hand < 0 || hand > 1 || !m_Aim[hand].active)
            return false;
        vr::VROverlayHandle_t h = 0;
        const std::string key = ResolveAlias(m_Aim[hand].alias);
        for (const auto& it : m_Overlays)
            if (_stricmp(it.second.key.c_str(), key.c_str()) == 0)
                h = it.first;
        auto it = m_Overlays.find(h);
        if (it == m_Overlays.end())
            return false;
        ovl::Geometry g;
        if (!GeometryLocked(it->second, g))
            return false;
        target = PanelPoint(g.panel, m_Aim[hand].u, m_Aim[hand].v);
        return true;
    }

    // ---- Tests ---------------------------------------------------------------------
    // dump=<prefix> | aim=<left|right>:<key|alias>:<u>:<v> | aim=<hand>:off |
    // keyuv=<char|Done|Cancel|Shift|Bksp|Clear> | maxlayers=<n> | state
    bool TestCommand(const std::string& spec, std::string& result)
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        result.clear();
        if (spec.rfind("dump=", 0) == 0)
        {
            m_Shadowing = true;
            m_DumpPrefix = spec.substr(5);
            for (auto& it : m_Overlays)
                it.second.dumped = false;
            result = "dump requested";
            return true;
        }
        if (spec.rfind("dumpevery=", 0) == 0)
        {
            // dumpevery=<ms>:<count>:<prefix>: from the frame loop, so it keeps
            // going while the game thread is stuck in a map load.
            const std::string rest = spec.substr(10);
            const size_t a = rest.find(':'), b = a == std::string::npos ? a : rest.find(':', a + 1);
            if (b == std::string::npos)
            {
                result = "expected dumpevery=<ms>:<count>:<prefix>";
                return false;
            }
            m_Shadowing = true;
            m_DumpEveryMs = static_cast<uint32_t>(std::strtoul(rest.c_str(), nullptr, 10));
            m_DumpEveryLeft = static_cast<uint32_t>(std::strtoul(rest.c_str() + a + 1, nullptr, 10));
            m_DumpEveryPrefix = rest.substr(b + 1);
            m_DumpEveryIndex = 0;
            m_DumpEveryNextMs = GetTickCount64();
            result = "periodic dump armed";
            return true;
        }
        if (spec.rfind("maxlayers=", 0) == 0)
        {
            m_MaxLayersOverride = static_cast<uint32_t>(std::strtoul(spec.c_str() + 10, nullptr, 10));
            result = "layer budget " + std::to_string(m_MaxLayersOverride ? m_MaxLayersOverride : m_MaxLayers);
            Log("layer budget override %u", m_MaxLayersOverride);
            return true;
        }
        if (spec.rfind("aim=", 0) == 0)
        {
            std::vector<std::string> parts;
            size_t start = 4;
            for (;;)
            {
                const size_t colon = spec.find(':', start);
                parts.push_back(spec.substr(start, colon == std::string::npos ? std::string::npos : colon - start));
                if (colon == std::string::npos) break;
                start = colon + 1;
            }
            const int hand = parts[0] == "left" ? 0 : parts[0] == "right" ? 1 : -1;
            if (hand < 0)
            {
                result = "hand must be left or right";
                return false;
            }
            if (parts.size() >= 2 && parts[1] == "off")
            {
                m_Aim[hand].active = false;
                result = "aim off";
                return true;
            }
            if (parts.size() < 4)
            {
                result = "expected aim=<hand>:<key>:<u>:<v>";
                return false;
            }
            m_Aim[hand].alias = parts[1];
            m_Aim[hand].key = ResolveAlias(parts[1]);
            m_Aim[hand].u = std::strtof(parts[2].c_str(), nullptr);
            m_Aim[hand].v = std::strtof(parts[3].c_str(), nullptr);
            m_Aim[hand].active = true;
            result = "aiming " + parts[0] + " at " + m_Aim[hand].key;
            return true;
        }
        if (spec.rfind("keyuv=", 0) == 0)
        {
            const std::string k = spec.substr(6);
            ovl::KeyKind kind = ovl::KeyKind::Char;
            if (k == "Done") kind = ovl::KeyKind::Done;
            else if (k == "Cancel") kind = ovl::KeyKind::Cancel;
            else if (k == "Shift") kind = ovl::KeyKind::Shift;
            else if (k == "Bksp") kind = ovl::KeyKind::Backspace;
            else if (k == "Clear") kind = ovl::KeyKind::Clear;
            else if (k.size() != 1) { result = "unknown key"; return false; }
            bool shift = false;
            float u = 0.0f, v = 0.0f;
            if (!ovl::KeyCentreUv(kind == ovl::KeyKind::Char ? k[0] : 0, kind, shift, u, v))
            {
                result = "no such key";
                return false;
            }
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%.4f %.4f %d", u, v, shift ? 1 : 0);
            result = buf;
            return true;
        }
        if (spec == "state")
        {
            char buf[512];
            auto it = m_Overlays.find(m_PointerHover);
            std::snprintf(buf, sizeof(buf), "keyboard=%d text='%s' target=%llu laser=%s hand=%d uv=%.3f,%.3f layers=%u dropped=%u events=%llu",
                m_Keyboard.Active() ? 1 : 0, m_Keyboard.Text().c_str(), static_cast<unsigned long long>(m_KeyboardTarget),
                it != m_Overlays.end() ? it->second.key.c_str() : "-", m_PointerActive, m_PointerU, m_PointerV,
                m_LastOverlayLayers, m_LastDropped, static_cast<unsigned long long>(m_EventsDelivered));
            result = buf;
            return true;
        }
        result = "unknown overlay test command";
        return false;
    }

    std::string StatsLine()
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        uint32_t visible = 0, withContent = 0;
        for (const auto& it : m_Overlays)
        {
            if (it.second.internal) continue;
            visible += it.second.visible ? 1 : 0;
            withContent += it.second.sc.released ? 1 : 0;
        }
        char buf[480];
        std::snprintf(buf, sizeof(buf),
            "overlays=%zu visible=%u content=%u layers=%u (max %u) dropped=%u (total %llu) frames=%llu withOverlays=%llu failed=%llu dummyFrames=%llu copies=%llu uploads=%llu texCalls=%llu rawCalls=%llu events=%llu expired=%llu",
            m_Overlays.size(), visible, withContent, m_LastOverlayLayers, m_MaxOverlayLayers, m_LastDropped,
            static_cast<unsigned long long>(m_DroppedTotal), static_cast<unsigned long long>(m_Frames),
            static_cast<unsigned long long>(m_FramesWithOverlays), static_cast<unsigned long long>(m_FailedOverlayFrames),
            static_cast<unsigned long long>(m_DummyFrames), static_cast<unsigned long long>(m_Copies),
            static_cast<unsigned long long>(m_Uploads), static_cast<unsigned long long>(m_TextureCalls),
            static_cast<unsigned long long>(m_RawCalls), static_cast<unsigned long long>(m_EventsDelivered),
            static_cast<unsigned long long>(m_EventsExpired));
        return buf;
    }

private:
    enum class Content : uint8_t { None, Vulkan, Raw };

    struct Swapchain
    {
        XrSwapchain handle = XR_NULL_HANDLE;
        uint32_t width = 0, height = 0;
        int64_t format = 0;
        VkFormat sourceFormat = VK_FORMAT_UNDEFINED;
        std::vector<VkImage> images;
        bool released = false;
    };

    struct QueuedEvent
    {
        vr::VREvent_t event;
        uint64_t ms = 0;
    };

    struct Overlay
    {
        std::string key, name;
        bool internal = false;
        bool visible = false;
        bool placed = false;
        float alpha = 1.0f, texelAspect = 1.0f, width = 1.0f, curvature = 0.0f;
        uint32_t sortOrder = 0;
        uint64_t flags = 0;
        uint64_t serial = 0;
        ovl::Priority priority = ovl::Priority::High;
        vr::ETrackingUniverseOrigin origin = vr::TrackingUniverseStanding;
        vr::HmdMatrix34_t transform{ { { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 } } };
        ovl::Bounds bounds;
        vr::VROverlayInputMethod inputMethod = vr::VROverlayInputMethod_None;
        vr::HmdVector2_t mouseScale{ { 1.0f, 1.0f } };
        std::deque<QueuedEvent> events;

        Content kind = Content::None;
        VkImage source = VK_NULL_HANDLE;
        uint32_t sourceWidth = 0, sourceHeight = 0;
        VkFormat sourceFormat = VK_FORMAT_UNDEFINED;
        std::vector<uint8_t> raw;  // RGBA, top row first
        uint32_t rawWidth = 0, rawHeight = 0;
        bool rawDirty = false;

        Swapchain sc;
        xrm::Rect rect;            // the layer's image rect (after a mirror copy)
        float appliedAlpha = 1.0f; // alpha baked into the pixels (CPU alpha path)
        uint64_t lastCopyMs = 0;
        std::vector<uint8_t> cache;  // Vulkan content read back for the CPU alpha path (swapchain byte order)
        std::vector<uint8_t> shadow; // test mode: the pixels the swapchain last received
        uint32_t shadowWidth = 0, shadowHeight = 0;
        bool shadowBgra = false;
        uint64_t shadowMs = 0;
        uint32_t cacheWidth = 0, cacheHeight = 0;
        bool dumped = false;
    };

    struct LayerInfo
    {
        vr::VROverlayHandle_t handle = 0;
        char type = 'Q';
        std::string key;
        ovl::Geometry g;
        XrCompositionLayerFlags flags = 0;
        float alpha = 1.0f;
    };

    struct Aim
    {
        bool active = false;
        std::string alias;  // re-resolved each time (the config panel's two copies swap)
        std::string key;
        float u = 0.5f, v = 0.5f;
    };

    static void Log(const char* fmt, ...)
    {
        char buf[1024];
        va_list args;
        va_start(args, fmt);
        std::vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);
        Game::logMsg("[VR][OpenXR][Overlay] %s", buf);
    }

    static opn::Mat34 ToMat(const vr::HmdMatrix34_t& m)
    {
        opn::Mat34 out;
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 4; ++c)
                out.m[r][c] = m.m[r][c];
        return out;
    }
    static vr::HmdMatrix34_t ToHmd(const opn::Mat34& m)
    {
        vr::HmdMatrix34_t out{};
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 4; ++c)
                out.m[r][c] = m.m[r][c];
        return out;
    }
    static opn::Vec3 PanelPoint(const opn::Panel& p, float u, float v)
    {
        if (p.curvature >= ovl::kMinCurvature)
        {
            const float angle = 2.0f * ovl::kPi * p.curvature;
            const float radius = p.width / angle;
            const float theta = (u - 0.5f) * angle;
            const opn::Vec3 axis = opn::Add(p.center, opn::Scale(p.normal, radius));
            const opn::Vec3 onArc = opn::Add(opn::Scale(p.right, radius * std::sin(theta)), opn::Scale(p.normal, -radius * std::cos(theta)));
            return opn::Add(opn::Add(axis, onArc), opn::Scale(p.up, (0.5f - v) * p.height));
        }
        return opn::Add(opn::Add(p.center, opn::Scale(p.right, (u - 0.5f) * p.width)), opn::Scale(p.up, (0.5f - v) * p.height));
    }

    std::string ResolveAlias(const std::string& alias) const
    {
        if (alias == "menu") return "MenuOverlayKey";
        if (alias == "hud") return "HUDOverlayTopKey";
        if (alias == "configbutton") return "l4d2vr.config.menu_button";
        if (alias == "keyboard") return "__l4d2vr.keyboard";
        if (alias == "config")
        {
            // The config panel's two copies swap after each upload: the
            // visible mouse-input one.
            for (const auto& it : m_Overlays)
                if (it.second.key.rfind("l4d2vr.config.overlay", 0) == 0 && it.second.visible &&
                    it.second.inputMethod == vr::VROverlayInputMethod_Mouse)
                    return it.second.key;
            return "l4d2vr.config.overlay.front";
        }
        return alias;
    }

    template<class F>
    vr::EVROverlayError With(vr::VROverlayHandle_t handle, F&& f)
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        auto it = m_Overlays.find(handle);
        if (it == m_Overlays.end())
            return vr::VROverlayError_InvalidHandle;
        f(it->second);
        return vr::VROverlayError_None;
    }

    bool GeometryLocked(const Overlay& o, ovl::Geometry& g) const
    {
        ovl::Shape s;
        s.transform = ToMat(o.transform);
        s.widthMeters = o.width;
        s.texelAspect = o.texelAspect;
        s.curvature = o.curvature;
        s.texWidth = o.sc.width;
        s.texHeight = o.sc.height;
        s.bounds = o.bounds;
        return ovl::ComputeGeometry(s, m_Cylinder, g);
    }

    bool NeedsCpuAlpha(const Overlay& o) const
    {
        return !m_ColorBias && o.alpha < 0.999f;
    }

    // ---- Events ---------------------------------------------------------------------
    void PushEventLocked(vr::VROverlayHandle_t handle, const vr::VREvent_t& e)
    {
        auto it = m_Overlays.find(handle);
        if (it == m_Overlays.end())
            return;
        std::deque<QueuedEvent>& q = it->second.events;
        if (q.size() >= kMaxQueuedEvents)
            q.pop_front();  // nobody polls this overlay (the mod polls hovered panels only)
        q.push_back(QueuedEvent{ e, GetTickCount64() });
    }

    void DeliverLocked(const ovl::Event& e)
    {
        vr::VREvent_t ev{};
        ev.trackedDeviceIndex = e.hand == 0 ? XrInputRuntime::kDeviceLeft : e.hand == 1 ? XrInputRuntime::kDeviceRight : vr::k_unTrackedDeviceIndexInvalid;
        if (e.overlay == m_KeyboardHandle)
        {
            if (e.type == ovl::EventType::Down)
                KeyboardClickLocked(e.x, e.y);
            return;
        }
        switch (e.type)
        {
        case ovl::EventType::Move: ev.eventType = vr::VREvent_MouseMove; break;
        case ovl::EventType::Down: ev.eventType = vr::VREvent_MouseButtonDown; break;
        case ovl::EventType::Up: ev.eventType = vr::VREvent_MouseButtonUp; break;
        case ovl::EventType::Scroll: ev.eventType = vr::VREvent_ScrollDiscrete; break;
        }
        if (e.type == ovl::EventType::Scroll)
        {
            ev.data.scroll.xdelta = 0.0f;
            ev.data.scroll.ydelta = e.scrollY;
            ev.data.scroll.viewportscale = 1.0f;
        }
        else
        {
            ev.data.mouse.x = e.x;
            ev.data.mouse.y = e.y;
            ev.data.mouse.button = vr::VRMouseButton_Left;
        }
        PushEventLocked(e.overlay, ev);
        if (e.type != ovl::EventType::Move && m_LoggedClicks < 400)
        {
            ++m_LoggedClicks;
            auto it = m_Overlays.find(e.overlay);
            Log("mouse %s on %s at (%.0f, %.0f) %s hand", e.type == ovl::EventType::Down ? "down" : e.type == ovl::EventType::Up ? "up" : "scroll",
                it != m_Overlays.end() ? it->second.key.c_str() : "?", e.x, e.y, e.hand == 0 ? "left" : "right");
        }
    }

    // ---- Keyboard -------------------------------------------------------------------
    vr::HmdMatrix34_t KeyboardPlacement(const vr::HmdMatrix34_t* hmd) const
    {
        // In front of the head, below eye height, tilted up towards the eyes.
        opn::Vec3 pos{ 0.0f, 1.2f, -0.7f };
        float yaw = 0.0f;
        if (hmd)
        {
            yaw = std::atan2(hmd->m[0][2], hmd->m[2][2]);
            const opn::Vec3 fwd{ -std::sin(yaw), 0.0f, -std::cos(yaw) };
            pos = opn::Add(opn::Vec3{ hmd->m[0][3], hmd->m[1][3] - 0.38f, hmd->m[2][3] }, opn::Scale(fwd, 0.62f));
        }
        const float tilt = 0.55f;  // ~32 degrees: the face turns up towards the eyes
        const float cy = std::cos(yaw), sy = std::sin(yaw), ct = std::cos(tilt), st = std::sin(tilt);
        // Columns: right (cy, 0, -sy), up tilted back, normal tilted up.
        vr::HmdMatrix34_t m{};
        m.m[0][0] = cy;  m.m[0][1] = -sy * st; m.m[0][2] = sy * ct; m.m[0][3] = pos.x;
        m.m[1][0] = 0;   m.m[1][1] = ct;       m.m[1][2] = st;      m.m[1][3] = pos.y;
        m.m[2][0] = -sy; m.m[2][1] = -cy * st; m.m[2][2] = cy * ct; m.m[2][3] = pos.z;
        return m;
    }

    void KeyboardClickLocked(float x, float y)
    {
        if (!m_Keyboard.Active())
            return;
        const int px = static_cast<int>(x);
        const int py = ovl::kKeyboardHeight - static_cast<int>(y);
        const int key = ovl::KeyAt(px, py);
        char typed = 0;
        const ovl::Keyboard::Result r = m_Keyboard.Press(key, typed);
        vr::VREvent_t ev{};
        ev.data.keyboard.uUserValue = m_Keyboard.UserValue();
        ev.data.keyboard.overlayHandle = m_KeyboardTarget;
        if (r == ovl::Keyboard::Result::Typed)
        {
            ev.eventType = vr::VREvent_KeyboardCharInput;
            ev.data.keyboard.cNewInput[0] = typed;
            PushEventLocked(m_KeyboardTarget, ev);
        }
        else if (r == ovl::Keyboard::Result::Done || r == ovl::Keyboard::Result::Cancelled)
        {
            ev.eventType = r == ovl::Keyboard::Result::Done ? vr::VREvent_KeyboardDone : vr::VREvent_KeyboardClosed;
            PushEventLocked(m_KeyboardTarget, ev);
            Log("keyboard %s: '%s'", r == ovl::Keyboard::Result::Done ? "done" : "cancelled", m_Keyboard.Text().c_str());
            m_Overlays[m_KeyboardHandle].visible = false;
            return;
        }
        if (key >= 0 && m_LoggedKeys < 400)
        {
            ++m_LoggedKeys;
            Log("keyboard key '%s' -> '%s'", ovl::KeyboardLayout()[static_cast<size_t>(key)].kind == ovl::KeyKind::Char
                ? std::string(1, ovl::KeyboardLayout()[static_cast<size_t>(key)].lower).c_str() : ovl::KeyboardLayout()[static_cast<size_t>(key)].label,
                m_Keyboard.Text().c_str());
        }
        RenderKeyboardLocked();
    }

    void CloseKeyboardLocked(const char* why)
    {
        if (!m_Keyboard.Active())
            return;
        m_Keyboard.Close();
        if (m_KeyboardHandle)
            m_Overlays[m_KeyboardHandle].visible = false;
        Log("keyboard closed (%s)", why);
    }

    // GDI raster of the keyboard into the internal overlay's raw pixels.
    void RenderKeyboardLocked()
    {
        if (!m_KeyboardHandle)
            return;
        const int W = ovl::kKeyboardWidth, H = ovl::kKeyboardHeight;
        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = W;
        bmi.bmiHeader.biHeight = -H;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        HDC dc = CreateCompatibleDC(nullptr);
        HBITMAP bmp = dc ? CreateDIBSection(dc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
        if (!dc || !bmp || !bits)
        {
            if (bmp) DeleteObject(bmp);
            if (dc) DeleteDC(dc);
            return;
        }
        HGDIOBJ oldBmp = SelectObject(dc, bmp);
        SetBkMode(dc, TRANSPARENT);
        auto fill = [&](int x, int y, int w, int h, COLORREF c) {
            RECT rc{ x, y, x + w, y + h };
            HBRUSH b = CreateSolidBrush(c);
            FillRect(dc, &rc, b);
            DeleteObject(b);
        };
        auto font = [&](int px, int weight) {
            return CreateFontW(-px, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
        };
        auto text = [&](const std::string& s, int x, int y, int w, int h, HFONT f, COLORREF c, UINT format) {
            const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
            std::wstring ws(n > 0 ? static_cast<size_t>(n) : 1u, L'\0');
            if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &ws[0], n);
            HGDIOBJ old = SelectObject(dc, f);
            SetTextColor(dc, c);
            RECT rc{ x, y, x + w, y + h };
            DrawTextW(dc, ws.c_str(), -1, &rc, format | DT_NOPREFIX);
            SelectObject(dc, old);
        };
        HFONT big = font(34, FW_NORMAL), keyFont = font(30, FW_BOLD), smallFont = font(20, FW_NORMAL);
        fill(0, 0, W, H, RGB(24, 28, 38));
        fill(0, 0, W, 4, RGB(136, 162, 205));
        fill(0, H - 4, W, 4, RGB(136, 162, 205));
        // Description and the text field.
        text(m_Keyboard.Description(), 24, ovl::kKeyboardTextTop, W - 48, 28, smallFont, RGB(170, 186, 214), DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
        fill(20, ovl::kKeyboardTextTop + 32, W - 40, ovl::kKeyboardTextHeight - 40, RGB(12, 14, 20));
        text(m_Keyboard.Text() + "_", 32, ovl::kKeyboardTextTop + 38, W - 64, ovl::kKeyboardTextHeight - 52, big, RGB(255, 236, 170),
            DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        const std::vector<ovl::Key>& keys = ovl::KeyboardLayout();
        for (size_t i = 0; i < keys.size(); ++i)
        {
            const ovl::Key& k = keys[i];
            const bool hover = static_cast<int>(i) == m_KeyboardHover;
            const bool special = k.kind != ovl::KeyKind::Char && k.kind != ovl::KeyKind::Space;
            COLORREF bg = hover ? RGB(70, 120, 200) : special ? RGB(52, 60, 78) : RGB(40, 46, 60);
            if (k.kind == ovl::KeyKind::Shift && m_Keyboard.Shift())
                bg = RGB(160, 120, 40);
            if (k.kind == ovl::KeyKind::Done && !hover)
                bg = RGB(40, 110, 70);
            fill(k.x, k.y, k.w, k.h, bg);
            std::string label = k.label;
            if (k.kind == ovl::KeyKind::Char)
                label = std::string(1, m_Keyboard.Shift() ? k.upper : k.lower);
            text(label, k.x, k.y, k.w, k.h, k.kind == ovl::KeyKind::Char ? keyFont : smallFont, RGB(236, 240, 248),
                DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        GdiFlush();
        Overlay& o = m_Overlays[m_KeyboardHandle];
        o.raw.resize(static_cast<size_t>(W) * H * 4u);
        const uint8_t* src = static_cast<const uint8_t*>(bits);
        for (size_t i = 0, n = static_cast<size_t>(W) * H; i < n; ++i)
        {
            o.raw[i * 4 + 0] = src[i * 4 + 2];
            o.raw[i * 4 + 1] = src[i * 4 + 1];
            o.raw[i * 4 + 2] = src[i * 4 + 0];
            o.raw[i * 4 + 3] = 245;
        }
        o.rawWidth = W;
        o.rawHeight = H;
        o.rawDirty = true;
        o.kind = Content::Raw;
        SelectObject(dc, oldBmp);
        DeleteObject(big);
        DeleteObject(keyFont);
        DeleteObject(smallFont);
        DeleteObject(bmp);
        DeleteDC(dc);
    }

    // ---- Internal overlays (laser, keyboard) -------------------------------------
    vr::VROverlayHandle_t AddInternalLocked(const char* key, uint32_t sort)
    {
        const vr::VROverlayHandle_t h = kInternalBase + (++m_NextInternal);
        Overlay& o = m_Overlays[h];
        o.key = key;
        o.name = key;
        o.internal = true;
        o.sortOrder = sort;
        o.serial = ++m_Serial;
        o.priority = ovl::Priority::High;
        return h;
    }

    void CreateInternalLocked()
    {
        // Dot: white core, blue ring, soft edge (as the mod's own pointer).
        m_DotHandle = AddInternalLocked("__l4d2vr.laser.dot", 0xFFFFFFF0u);
        {
            Overlay& o = m_Overlays[m_DotHandle];
            const int n = 64;
            o.raw.assign(static_cast<size_t>(n) * n * 4u, 0);
            for (int y = 0; y < n; ++y)
                for (int x = 0; x < n; ++x)
                {
                    const float dx = (x + 0.5f - n * 0.5f) / (n * 0.5f), dy = (y + 0.5f - n * 0.5f) / (n * 0.5f);
                    const float r = std::sqrt(dx * dx + dy * dy);
                    uint8_t* p = &o.raw[(static_cast<size_t>(y) * n + x) * 4u];
                    if (r < 0.45f) { p[0] = p[1] = p[2] = 255; p[3] = 255; }
                    else if (r < 0.75f) { p[0] = 38; p[1] = 166; p[2] = 255; p[3] = 255; }
                    else if (r < 1.0f) { p[3] = static_cast<uint8_t>(190.0f * (1.0f - (r - 0.75f) / 0.25f)); }
                }
            o.rawWidth = o.rawHeight = n;
            o.rawDirty = true;
            o.kind = Content::Raw;
        }
        // Beam: thin light-blue strip, fainter at the controller.
        m_BeamHandle = AddInternalLocked("__l4d2vr.laser.beam", 0xFFFFFFE0u);
        {
            Overlay& o = m_Overlays[m_BeamHandle];
            const int w = 512, h = 4;
            o.raw.assign(static_cast<size_t>(w) * h * 4u, 0);
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                    const float edge = (y == 0 || y == h - 1) ? 0.35f : 1.0f;
                    uint8_t* p = &o.raw[(static_cast<size_t>(y) * w + x) * 4u];
                    p[0] = 140; p[1] = 210; p[2] = 255;
                    p[3] = static_cast<uint8_t>(255.0f * edge * (0.2f + 0.6f * (x + 0.5f) / w));
                }
            o.rawWidth = w;
            o.rawHeight = h;
            o.rawDirty = true;
            o.kind = Content::Raw;
        }
        m_KeyboardHandle = AddInternalLocked("__l4d2vr.keyboard", 0xFFFFFF00u);
        {
            Overlay& o = m_Overlays[m_KeyboardHandle];
            o.width = 0.78f;
            o.inputMethod = vr::VROverlayInputMethod_Mouse;
            o.mouseScale = { { static_cast<float>(ovl::kKeyboardWidth), static_cast<float>(ovl::kKeyboardHeight) } };
            o.placed = true;
            RenderKeyboardLocked();
        }
    }

    bool EnsureDummyLocked()
    {
        if (m_Dummy.handle != XR_NULL_HANDLE && m_Dummy.released)
            return true;
        if (!EnsureSwapchain(m_Dummy, 4, 4, VK_FORMAT_R8G8B8A8_UNORM))
            return false;
        std::vector<uint8_t> zero(4 * 4 * 4, 0);
        return UploadPixelsLocked(m_Dummy, zero.data(), 4, 4, nullptr);
    }

    // ---- Swapchains and copies (queue lock held) ------------------------------------
    int64_t PickFormat(VkFormat source) const
    {
        auto offered = [&](VkFormat f) {
            return std::find(m_Device.formats.begin(), m_Device.formats.end(), static_cast<int64_t>(f)) != m_Device.formats.end();
        };
        // Same bits, sRGB view: the game's UNORM images and the mod's raw
        // pixels hold gamma-encoded colour, shown as SteamVR shows them.
        VkFormat preferred[2] = { VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED };
        switch (source)
        {
        case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_B8G8R8A8_SRGB:
            preferred[0] = VK_FORMAT_B8G8R8A8_SRGB; preferred[1] = VK_FORMAT_B8G8R8A8_UNORM; break;
        case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SRGB:
            preferred[0] = VK_FORMAT_R8G8B8A8_SRGB; preferred[1] = VK_FORMAT_R8G8B8A8_UNORM; break;
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
            preferred[0] = VK_FORMAT_A2B10G10R10_UNORM_PACK32; break;
        case VK_FORMAT_R16G16B16A16_SFLOAT:
            preferred[0] = VK_FORMAT_R16G16B16A16_SFLOAT; break;
        default:
            break;
        }
        for (VkFormat f : preferred)
            if (f != VK_FORMAT_UNDEFINED && offered(f))
                return f;
        return 0;
    }

    static bool FourBytes(int64_t f)
    {
        return f == VK_FORMAT_B8G8R8A8_SRGB || f == VK_FORMAT_B8G8R8A8_UNORM || f == VK_FORMAT_R8G8B8A8_SRGB || f == VK_FORMAT_R8G8B8A8_UNORM;
    }
    static bool Bgra(int64_t f) { return f == VK_FORMAT_B8G8R8A8_SRGB || f == VK_FORMAT_B8G8R8A8_UNORM; }

    void DestroySwapchainLocked(Swapchain& sc)
    {
        if (sc.handle != XR_NULL_HANDLE)
            m_Xr.xrDestroySwapchain(sc.handle);
        sc = Swapchain();
    }

    bool EnsureSwapchain(Swapchain& sc, uint32_t width, uint32_t height, VkFormat source)
    {
        if (sc.handle != XR_NULL_HANDLE && sc.width == width && sc.height == height && sc.sourceFormat == source)
            return true;
        int64_t format = PickFormat(source);
        if (!format && source == VK_FORMAT_R8G8B8A8_UNORM)
            format = PickFormat(VK_FORMAT_B8G8R8A8_UNORM);  // raw pixels swizzled on upload
        if (!format)
        {
            if (!m_LoggedNoFormat)
            {
                m_LoggedNoFormat = true;
                Log("no swapchain format for overlay content format %d", static_cast<int>(source));
            }
            return false;
        }
        DestroySwapchainLocked(sc);
        XrSwapchainCreateInfo ci{ XR_TYPE_SWAPCHAIN_CREATE_INFO };
        ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT |
            XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        ci.format = format;
        ci.sampleCount = 1;
        ci.width = width;
        ci.height = height;
        ci.faceCount = 1;
        ci.arraySize = 1;
        ci.mipCount = 1;
        XrResult r = XR_SUCCESS;
        {
            Crumb crumb("overlay xrCreateSwapchain");
            r = m_Xr.xrCreateSwapchain(m_Device.session, &ci, &sc.handle);
        }
        if (XR_FAILED(r))
        {
            sc.handle = XR_NULL_HANDLE;
            Log("xrCreateSwapchain(%ux%u format %lld) failed: %d", width, height, static_cast<long long>(format), static_cast<int>(r));
            return false;
        }
        uint32_t n = 0;
        m_Xr.xrEnumerateSwapchainImages(sc.handle, 0, &n, nullptr);
        std::vector<XrSwapchainImageVulkanKHR> images(n, { XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR });
        r = m_Xr.xrEnumerateSwapchainImages(sc.handle, n, &n, reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data()));
        if (XR_FAILED(r) || n == 0)
        {
            DestroySwapchainLocked(sc);
            return false;
        }
        for (const auto& image : images)
            sc.images.push_back(image.image);
        sc.width = width;
        sc.height = height;
        sc.format = format;
        sc.sourceFormat = source;
        ++m_Swapchains;
        return true;
    }

    bool AcquireLocked(Swapchain& sc, uint32_t& index)
    {
        XrSwapchainImageAcquireInfo ai{ XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
        XrResult r = XR_SUCCESS;
        {
            Crumb crumb("overlay xrAcquireSwapchainImage");
            r = m_Xr.xrAcquireSwapchainImage(sc.handle, &ai, &index);
        }
        if (XR_FAILED(r))
        {
            if (m_AcquireErrors++ < 10)
                Log("xrAcquireSwapchainImage failed: %d", static_cast<int>(r));
            return false;
        }
        XrSwapchainImageWaitInfo wi{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
        wi.timeout = 100000000;  // 100 ms
        {
            Crumb crumb("overlay xrWaitSwapchainImage");
            r = m_Xr.xrWaitSwapchainImage(sc.handle, &wi);
        }
        if (r != XR_SUCCESS)
        {
            if (m_AcquireErrors++ < 10)
                Log("xrWaitSwapchainImage returned %d", static_cast<int>(r));
            if (r != XR_TIMEOUT_EXPIRED)
            {
                XrSwapchainImageReleaseInfo ri{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
                m_Xr.xrReleaseSwapchainImage(sc.handle, &ri);
            }
            return false;
        }
        return index < sc.images.size();
    }

    bool ReleaseLocked(Swapchain& sc)
    {
        XrSwapchainImageReleaseInfo ri{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
        XrResult r = XR_SUCCESS;
        {
            Crumb crumb("overlay xrReleaseSwapchainImage");
            r = m_Xr.xrReleaseSwapchainImage(sc.handle, &ri);
        }
        if (XR_FAILED(r))
            return false;
        sc.released = true;
        return true;
    }

    void Barrier(VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags srcAccess, VkAccessFlags dstAccess,
        VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage)
    {
        VkImageMemoryBarrier b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        b.srcAccessMask = srcAccess;
        b.dstAccessMask = dstAccess;
        b.oldLayout = from;
        b.newLayout = to;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image;
        b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        m_Vk.vkCmdPipelineBarrier(m_Cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
    }

    void BeginCmd()
    {
        VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        m_Vk.vkResetCommandBuffer(m_Cmd, 0);
        m_Vk.vkBeginCommandBuffer(m_Cmd, &bi);
    }

    bool SubmitAndWait()
    {
        m_Vk.vkEndCommandBuffer(m_Cmd);
        VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
        si.commandBufferCount = 1;
        si.pCommandBuffers = &m_Cmd;
        m_Vk.vkResetFences(m_Device.device, 1, &m_Fence);
        const VkResult r = m_Vk.vkQueueSubmit(m_Device.queue, 1, &si, m_Fence);
        if (r != VK_SUCCESS)
        {
            if (m_CopyErrors++ < 10)
                Log("overlay copy vkQueueSubmit failed: %d", static_cast<int>(r));
            return false;
        }
        Crumb crumb("overlay copy fence");
        return m_Vk.vkWaitForFences(m_Device.device, 1, &m_Fence, VK_TRUE, 1000000000ull) == VK_SUCCESS;
    }

    bool EnsureStaging(VkDeviceSize size)
    {
        if (m_Staging != VK_NULL_HANDLE && m_StagingSize >= size)
            return true;
        if (m_Staging != VK_NULL_HANDLE)
        {
            m_Vk.vkDestroyBuffer(m_Device.device, m_Staging, nullptr);
            m_Vk.vkFreeMemory(m_Device.device, m_StagingMemory, nullptr);
            m_Staging = VK_NULL_HANDLE;
            m_StagingMemory = VK_NULL_HANDLE;
            m_StagingSize = 0;
        }
        VkBufferCreateInfo ci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        ci.size = size;
        ci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        if (m_Vk.vkCreateBuffer(m_Device.device, &ci, nullptr, &m_Staging) != VK_SUCCESS)
            return false;
        VkMemoryRequirements req{};
        m_Vk.vkGetBufferMemoryRequirements(m_Device.device, m_Staging, &req);
        uint32_t type = UINT32_MAX;
        const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        for (uint32_t i = 0; i < m_Device.memory.memoryTypeCount; ++i)
            if ((req.memoryTypeBits & (1u << i)) && (m_Device.memory.memoryTypes[i].propertyFlags & want) == want)
            {
                type = i;
                break;
            }
        VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = type;
        if (type == UINT32_MAX || m_Vk.vkAllocateMemory(m_Device.device, &mai, nullptr, &m_StagingMemory) != VK_SUCCESS)
        {
            m_Vk.vkDestroyBuffer(m_Device.device, m_Staging, nullptr);
            m_Staging = VK_NULL_HANDLE;
            return false;
        }
        m_Vk.vkBindBufferMemory(m_Device.device, m_Staging, m_StagingMemory, 0);
        m_StagingSize = size;
        return true;
    }

    // The layer rect after a mirrored copy (the copy mirrors the whole image).
    static xrm::Rect MirroredRect(const ovl::Region& r, uint32_t w, uint32_t h)
    {
        xrm::Rect out{ r.x, r.y, r.width, r.height };
        if (r.flipU) out.x = static_cast<int32_t>(w) - r.x - r.width;
        if (r.flipV) out.y = static_cast<int32_t>(h) - r.y - r.height;
        out.x = (std::max)(0, out.x);
        out.y = (std::max)(0, out.y);
        return out;
    }

    // Vulkan content: GPU copy (mirrored by regions), or through the staging
    // buffer when the alpha must be baked in or a dump is due.
    bool CopyVulkanLocked(Overlay& o)
    {
        if (!o.source || !EnsureSwapchain(o.sc, o.sourceWidth, o.sourceHeight, o.sourceFormat))
            return false;
        const ovl::Region region = ovl::RegionFromBounds(o.bounds, o.sc.width, o.sc.height);
        const bool dump = !m_DumpPrefix.empty() && !o.dumped && FourBytes(o.sc.format);
        const bool cpu = NeedsCpuAlpha(o) && FourBytes(o.sc.format);
        uint32_t index = 0;
        if (!AcquireLocked(o.sc, index))
            return false;
        const VkImage target = o.sc.images[index];
        const uint32_t w = o.sc.width, h = o.sc.height;
        bool ok = true;
        if (cpu)
        {
            const VkDeviceSize size = static_cast<VkDeviceSize>(w) * h * 4u;
            ok = EnsureStaging(size);
            if (ok)
            {
                BeginCmd();
                VkMemoryBarrier mb{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
                mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
                mb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                m_Vk.vkCmdPipelineBarrier(m_Cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
                VkBufferImageCopy bic{};
                bic.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
                bic.imageExtent = { w, h, 1 };
                m_Vk.vkCmdCopyImageToBuffer(m_Cmd, o.source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m_Staging, 1, &bic);
                ok = SubmitAndWait();
            }
            if (ok)
            {
                void* mapped = nullptr;
                ok = m_Vk.vkMapMemory(m_Device.device, m_StagingMemory, 0, VK_WHOLE_SIZE, 0, &mapped) == VK_SUCCESS && mapped;
                if (ok)
                {
                    uint8_t* px = static_cast<uint8_t*>(mapped);
                    // Keep the unfaded pixels: a later fade re-uploads them
                    // without reading the (maybe reused) source image back.
                    if (NeedsCpuAlpha(o))
                    {
                        o.cache.assign(px, px + static_cast<size_t>(w) * h * 4u);
                        o.cacheWidth = w;
                        o.cacheHeight = h;
                    }
                    else
                        o.cache.clear();
                    ovl::FlipPixels(px, w, h, region.flipU, region.flipV);
                    if (NeedsCpuAlpha(o))
                        ovl::ApplyAlpha(px, static_cast<size_t>(w) * h, o.alpha, (o.flags & vr::VROverlayFlags_IgnoreTextureAlpha) != 0);
                    SetShadow(o, px, w, h);
                    if (dump)
                        WriteImageDumpLocked(o, px, w, h, Bgra(o.sc.format));
                    m_Vk.vkUnmapMemory(m_Device.device, m_StagingMemory);
                }
            }
            if (ok)
            {
                BeginCmd();
                Barrier(target, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
                VkBufferImageCopy bic{};
                bic.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
                bic.imageExtent = { w, h, 1 };
                m_Vk.vkCmdCopyBufferToImage(m_Cmd, m_Staging, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bic);
                Barrier(target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
                ok = SubmitAndWait();
            }
            o.appliedAlpha = NeedsCpuAlpha(o) ? o.alpha : 1.0f;
        }
        else
        {
            o.cache.clear();
            BeginCmd();
            VkMemoryBarrier mb{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
            mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
            mb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            m_Vk.vkCmdPipelineBarrier(m_Cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
            Barrier(target, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            std::vector<VkImageCopy> regions;
            if (region.flipU || region.flipV)
            {
                // Mirror with one-pixel-wide (or -tall) copies: bit exact, no format conversion.
                if (region.flipU && !region.flipV)
                {
                    regions.resize(w);
                    for (uint32_t x = 0; x < w; ++x)
                    {
                        VkImageCopy& c = regions[x];
                        c.srcSubresource = c.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
                        c.srcOffset = { static_cast<int32_t>(x), 0, 0 };
                        c.dstOffset = { static_cast<int32_t>(w - 1 - x), 0, 0 };
                        c.extent = { 1, h, 1 };
                    }
                }
                else if (region.flipV && !region.flipU)
                {
                    regions.resize(h);
                    for (uint32_t y = 0; y < h; ++y)
                    {
                        VkImageCopy& c = regions[y];
                        c.srcSubresource = c.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
                        c.srcOffset = { 0, static_cast<int32_t>(y), 0 };
                        c.dstOffset = { 0, static_cast<int32_t>(h - 1 - y), 0 };
                        c.extent = { w, 1, 1 };
                    }
                }
                else
                {
                    // Both axes: a half turn, one pixel at a time is too many
                    // regions; rows mirrored now, columns are not (rare).
                    regions.resize(h);
                    for (uint32_t y = 0; y < h; ++y)
                    {
                        VkImageCopy& c = regions[y];
                        c.srcSubresource = c.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
                        c.srcOffset = { 0, static_cast<int32_t>(y), 0 };
                        c.dstOffset = { 0, static_cast<int32_t>(h - 1 - y), 0 };
                        c.extent = { w, 1, 1 };
                    }
                    if (!m_LoggedDoubleFlip)
                    {
                        m_LoggedDoubleFlip = true;
                        Log("overlay '%s' flips both axes; only the vertical flip is applied on the GPU path", o.key.c_str());
                    }
                }
            }
            else
            {
                regions.resize(1);
                VkImageCopy& c = regions[0];
                c.srcSubresource = c.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
                c.extent = { w, h, 1 };
            }
            m_Vk.vkCmdCopyImage(m_Cmd, o.source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                static_cast<uint32_t>(regions.size()), regions.data());
            // Dumps (and, once a dump was asked for, every copy): read the
            // swapchain image back after the copy, exactly what the runtime gets.
            const bool readback = (dump || m_Shadowing) && EnsureStaging(static_cast<VkDeviceSize>(w) * h * 4u);
            if (readback)
            {
                Barrier(target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
                VkBufferImageCopy bic{};
                bic.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
                bic.imageExtent = { w, h, 1 };
                m_Vk.vkCmdCopyImageToBuffer(m_Cmd, target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m_Staging, 1, &bic);
                Barrier(target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT,
                    VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
            }
            else
            {
                Barrier(target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
            }
            ok = SubmitAndWait();
            if (ok && readback)
            {
                void* mapped = nullptr;
                if (m_Vk.vkMapMemory(m_Device.device, m_StagingMemory, 0, VK_WHOLE_SIZE, 0, &mapped) == VK_SUCCESS && mapped)
                {
                    const uint8_t* px = static_cast<const uint8_t*>(mapped);
                    SetShadow(o, px, w, h);
                    if (dump)
                        WriteImageDumpLocked(o, px, w, h, Bgra(o.sc.format));
                    m_Vk.vkUnmapMemory(m_Device.device, m_StagingMemory);
                }
            }
            o.appliedAlpha = 1.0f;
        }
        if (!ReleaseLocked(o.sc))
            ok = false;
        o.rect = MirroredRect(region, w, h);
        o.lastCopyMs = GetTickCount64();
        ++m_Copies;
        if (!ok && m_CopyErrors++ < 10)
            Log("overlay '%s' copy failed", o.key.c_str());
        return ok;
    }

    // RGBA pixels into a swapchain (staging buffer). `owner` gets the dump.
    bool UploadPixelsLocked(Swapchain& sc, const uint8_t* rgba, uint32_t w, uint32_t h, Overlay* owner, bool swizzle = true)
    {
        const VkDeviceSize size = static_cast<VkDeviceSize>(w) * h * 4u;
        if (!FourBytes(sc.format) || !EnsureStaging(size))
            return false;
        uint32_t index = 0;
        if (!AcquireLocked(sc, index))
            return false;
        void* mapped = nullptr;
        bool ok = m_Vk.vkMapMemory(m_Device.device, m_StagingMemory, 0, VK_WHOLE_SIZE, 0, &mapped) == VK_SUCCESS && mapped;
        if (ok)
        {
            uint8_t* px = static_cast<uint8_t*>(mapped);
            std::memcpy(px, rgba, static_cast<size_t>(size));
            if (owner)
            {
                const ovl::Region region = ovl::RegionFromBounds(owner->bounds, w, h);
                ovl::FlipPixels(px, w, h, region.flipU, region.flipV);
                owner->rect = MirroredRect(region, w, h);
                if (NeedsCpuAlpha(*owner))
                    ovl::ApplyAlpha(px, static_cast<size_t>(w) * h, owner->alpha, (owner->flags & vr::VROverlayFlags_IgnoreTextureAlpha) != 0);
                owner->appliedAlpha = NeedsCpuAlpha(*owner) ? owner->alpha : 1.0f;
            }
            if (swizzle && Bgra(sc.format))
                for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i)
                    std::swap(px[i * 4], px[i * 4 + 2]);
            if (owner)
            {
                SetShadow(*owner, px, w, h);
                owner->lastCopyMs = GetTickCount64();
            }
            if (owner && !m_DumpPrefix.empty() && !owner->dumped)
                WriteImageDumpLocked(*owner, px, w, h, Bgra(sc.format));
            m_Vk.vkUnmapMemory(m_Device.device, m_StagingMemory);
        }
        if (ok)
        {
            const VkImage target = sc.images[index];
            BeginCmd();
            Barrier(target, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkBufferImageCopy bic{};
            bic.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            bic.imageExtent = { w, h, 1 };
            m_Vk.vkCmdCopyBufferToImage(m_Cmd, m_Staging, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bic);
            Barrier(target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
            ok = SubmitAndWait();
        }
        if (!ReleaseLocked(sc))
            ok = false;
        ++m_Uploads;
        return ok;
    }

    void UploadRawLocked(Overlay& o)
    {
        if (o.raw.empty() || !EnsureSwapchain(o.sc, o.rawWidth, o.rawHeight, VK_FORMAT_R8G8B8A8_UNORM))
            return;
        if (UploadPixelsLocked(o.sc, o.raw.data(), o.rawWidth, o.rawHeight, &o))
            o.rawDirty = false;
    }

    // The alpha changed without new content (kill indicator fades).
    void RefreshAlphaLocked(Overlay& o)
    {
        if (o.kind == Content::Raw)
            UploadRawLocked(o);
        else if (o.kind == Content::Vulkan && !o.cache.empty() && o.cacheWidth == o.sc.width && o.cacheHeight == o.sc.height)
            UploadPixelsLocked(o.sc, o.cache.data(), o.cacheWidth, o.cacheHeight, &o, false);
        else if (o.kind == Content::Vulkan)
            CopyVulkanLocked(o);
    }

    // ---- Dumps --------------------------------------------------------------------
    // Test mode (after the first dump request): the last pixels each overlay's
    // swapchain received, so a dump never has to read a source image again
    // (the game may have moved on, e.g. its back buffer during a map load).
    void SetShadow(Overlay& o, const uint8_t* px, uint32_t w, uint32_t h)
    {
        if (!m_Shadowing)
            return;
        o.shadow.assign(px, px + static_cast<size_t>(w) * h * 4u);
        o.shadowWidth = w;
        o.shadowHeight = h;
        o.shadowBgra = Bgra(o.sc.format);
        o.shadowMs = GetTickCount64();
    }

    void WriteImageDumpLocked(Overlay& o, const uint8_t* px, uint32_t w, uint32_t h, bool bgra)
    {
        std::string safe = o.key;
        for (char& c : safe)
            if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_')
                c = '_';
        const std::string path = m_DumpPrefix + "_" + safe + ".bmp";
        FILE* f = nullptr;
        if (fopen_s(&f, path.c_str(), "wb") != 0 || !f)
            return;
        std::vector<uint8_t> out(static_cast<size_t>(w) * h * 4u);
        uint64_t sum = 0, alphaSum = 0;
        for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i)
        {
            const uint8_t* s = px + i * 4;
            uint8_t* d = &out[i * 4];
            d[0] = bgra ? s[0] : s[2];
            d[1] = s[1];
            d[2] = bgra ? s[2] : s[0];
            d[3] = s[3];
            sum += d[0] + d[1] + d[2];
            alphaSum += d[3];
        }
        const uint32_t dataSize = static_cast<uint32_t>(out.size());
        uint8_t header[54] = { 'B', 'M' };
        auto put32 = [&](int offset, uint32_t v) { std::memcpy(header + offset, &v, 4); };
        auto put16 = [&](int offset, uint16_t v) { std::memcpy(header + offset, &v, 2); };
        put32(2, 54 + dataSize); put32(10, 54); put32(14, 40); put32(18, w);
        put32(22, static_cast<uint32_t>(-static_cast<int32_t>(h))); put16(26, 1); put16(28, 32); put32(34, dataSize);
        fwrite(header, 1, sizeof(header), f);
        fwrite(out.data(), 1, out.size(), f);
        fclose(f);
        o.dumped = true;
        Log("dump %s: %ux%u mean rgb %.1f alpha %.1f -> %s", o.key.c_str(), w, h, static_cast<double>(sum) / (3.0 * w * h),
            static_cast<double>(alphaSum) / (static_cast<double>(w) * h), path.c_str());
    }

    void WriteLayerDumpLocked()
    {
        // Overlays whose content did not change since the request: copy them
        // again now so the dump holds the image the runtime shows.
        bool missing = false;
        for (const LayerInfo& li : m_LayerInfo)
        {
            if (li.type != 'Q' && li.type != 'C')
                continue;
            Overlay& o = m_Overlays[li.handle];
            if (o.dumped)
                continue;
            if (!o.shadow.empty() && o.shadowWidth == o.sc.width && o.shadowHeight == o.sc.height)
            {
                WriteImageDumpLocked(o, o.shadow.data(), o.shadowWidth, o.shadowHeight, o.shadowBgra);
                continue;
            }
            if (o.kind == Content::Raw && !o.raw.empty())
            {
                std::vector<uint8_t> px = o.raw;
                const ovl::Region region = ovl::RegionFromBounds(o.bounds, o.rawWidth, o.rawHeight);
                ovl::FlipPixels(px.data(), o.rawWidth, o.rawHeight, region.flipU, region.flipV);
                if (NeedsCpuAlpha(o))
                    ovl::ApplyAlpha(px.data(), px.size() / 4, o.alpha, (o.flags & vr::VROverlayFlags_IgnoreTextureAlpha) != 0);
                WriteImageDumpLocked(o, px.data(), o.rawWidth, o.rawHeight, false);
            }
            else if (o.kind == Content::Vulkan && o.priority == ovl::Priority::Low)
                missing |= !CopyVulkanLocked(o) || !o.dumped;  // kill indicators: bound once, persistent textures
            else
                missing = true;
        }
        std::string text;
        char line[1024];
        std::snprintf(line, sizeof(line), "layers submitted: %zu (overlays %u, dropped %u, budget %u)\n", m_Layers.size(), m_LastOverlayLayers,
            m_LastDropped, m_MaxLayersOverride ? m_MaxLayersOverride : m_MaxLayers);
        text += line;
        for (size_t i = 0; i < m_LayerInfo.size(); ++i)
        {
            const LayerInfo& li = m_LayerInfo[i];
            if (li.type == 'D' || li.type == 'P')
            {
                std::snprintf(line, sizeof(line), "[%zu] %s\n", li.type == 'P' && li.key.rfind("(no", 0) == 0 ? 99 : i, li.key.c_str());
                text += line;
                continue;
            }
            const Overlay& o = m_Overlays[li.handle];
            const ovl::Geometry& g = li.g;
            const vr::HmdMatrix34_t& m = o.transform;
            std::snprintf(line, sizeof(line),
                "[%zu] %s %s: pos (%.3f %.3f %.3f) quat (%.4f %.4f %.4f %.4f) size %.3fx%.3f m%s radius %.3f angle %.1f deg aspect %.3f | "
                "rect %d,%d %dx%d of %ux%u fmt %lld | sort 0x%08X alpha %.2f blend %d updated %.2f s ago | OpenVR width %.3f curvature %.2f "
                "m=[%.3f %.3f %.3f %.3f; %.3f %.3f %.3f %.3f; %.3f %.3f %.3f %.3f]\n",
                i, li.type == 'C' ? "cylinder" : "quad", o.key.c_str(), g.pose.position.x, g.pose.position.y, g.pose.position.z,
                g.pose.orientation.x, g.pose.orientation.y, g.pose.orientation.z, g.pose.orientation.w, g.width, g.height,
                li.type == 'C' ? " (arc)" : "", g.radius, g.centralAngle * 57.29578f, g.aspectRatio, o.rect.x, o.rect.y, o.rect.width,
                o.rect.height, o.sc.width, o.sc.height, static_cast<long long>(o.sc.format), o.sortOrder, li.alpha,
                (li.flags & XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT) ? 1 : 0,
                o.lastCopyMs ? static_cast<double>(GetTickCount64() - o.lastCopyMs) / 1000.0 : -1.0, o.width, o.curvature,
                m.m[0][0], m.m[0][1], m.m[0][2], m.m[0][3], m.m[1][0], m.m[1][1], m.m[1][2], m.m[1][3], m.m[2][0], m.m[2][1], m.m[2][2], m.m[2][3]);
            text += line;
        }
        const std::string path = m_DumpPrefix + "_layers.txt";
        FILE* f = nullptr;
        if (fopen_s(&f, path.c_str(), "wb") == 0 && f)
        {
            fwrite(text.data(), 1, text.size(), f);
            fclose(f);
        }
        // One log line per layer as well (vrmod_log.txt is always collected).
        size_t start = 0;
        while (start < text.size())
        {
            const size_t end = text.find('\n', start);
            Log("layer dump %s", text.substr(start, end - start).c_str());
            if (end == std::string::npos) break;
            start = end + 1;
        }
        if (missing)
            Log("layer dump: some overlay images could not be read back");
        m_DumpPrefix.clear();
    }

    // ---- State ------------------------------------------------------------------
    const XrApi& m_Xr;
    const VkApi& m_Vk;
    XrInputRuntime& m_Input;
    std::mutex m_Mutex;
    XrOverlayDevice m_Device;
    bool m_Ready = false;
    bool m_Wine = false, m_Cylinder = false, m_ColorBias = false;
    ovl::Workaround m_Workaround = ovl::Workaround::Off;
    uint32_t m_MaxLayers = 0, m_MaxLayersOverride = 0;
    VkCommandPool m_Pool = VK_NULL_HANDLE;
    VkCommandBuffer m_Cmd = VK_NULL_HANDLE;
    VkFence m_Fence = VK_NULL_HANDLE;
    VkBuffer m_Staging = VK_NULL_HANDLE;
    VkDeviceMemory m_StagingMemory = VK_NULL_HANDLE;
    VkDeviceSize m_StagingSize = 0;

    std::map<vr::VROverlayHandle_t, Overlay> m_Overlays;
    vr::VROverlayHandle_t m_NextHandle = 0;
    vr::VROverlayHandle_t m_NextInternal = 0;
    uint64_t m_Serial = 0;
    Swapchain m_Dummy;
    std::vector<Swapchain> m_Graveyard;

    // Layer storage for the frame being ended.
    std::vector<const XrCompositionLayerBaseHeader*> m_Layers;
    std::vector<XrCompositionLayerQuad> m_Quads;
    std::vector<XrCompositionLayerCylinderKHR> m_Cylinders;
    std::vector<XrCompositionLayerColorScaleBiasKHR> m_ColorScales;
    std::vector<LayerInfo> m_LayerInfo;

    // Pointer, laser, keyboard.
    ovl::Pointer m_Pointer;
    std::vector<ovl::Event> m_EventScratch;
    vr::VROverlayHandle_t m_DotHandle = 0, m_BeamHandle = 0, m_KeyboardHandle = 0;
    ovl::Keyboard m_Keyboard;
    vr::VROverlayHandle_t m_KeyboardTarget = 0;
    int m_KeyboardHover = -1;
    int m_PointerActive = -1;
    vr::VROverlayHandle_t m_PointerHover = 0;
    std::string m_LoggedHover;
    float m_PointerU = 0.0f, m_PointerV = 0.0f;
    Aim m_Aim[2];

    std::string m_DumpPrefix;
    bool m_Shadowing = false;
    XrPosef m_LastEyePose{};
    XrFovf m_LastEyeFov{};
    bool m_HaveEye = false;
    std::string m_DumpEveryPrefix;
    uint32_t m_DumpEveryMs = 0, m_DumpEveryLeft = 0, m_DumpEveryIndex = 0;
    uint64_t m_DumpEveryNextMs = 0;
    uint64_t m_Frames = 0, m_FramesWithOverlays = 0, m_FailedOverlayFrames = 0, m_DummyFrames = 0;
    uint64_t m_Copies = 0, m_Uploads = 0, m_TextureCalls = 0, m_RawCalls = 0, m_EventsDelivered = 0, m_EventsExpired = 0, m_DroppedTotal = 0;
    uint64_t m_Swapchains = 0;
    uint32_t m_LastOverlayLayers = 0, m_MaxOverlayLayers = 0, m_LastDropped = 0;
    size_t m_LoggedDropped = 0;
    uint32_t m_AcquireErrors = 0, m_CopyErrors = 0, m_LoggedClicks = 0, m_LoggedKeys = 0;
    bool m_LoggedNoFormat = false, m_LoggedDoubleFlip = false;
};

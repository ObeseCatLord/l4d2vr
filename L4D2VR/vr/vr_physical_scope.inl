namespace
{
    Vector ScopeViewmodelPointToScene(const CViewSetup& view, const Vector& point)
    {
        Vector forward{}, right{}, up{};
        QAngle::AngleVectors(QAngle(view.angles.x, view.angles.y, view.angles.z), &forward, &right, &up);
        const float sceneFov = std::clamp(view.fov, 1.0f, 179.0f);
        const float vmFov = std::clamp(view.fovViewmodel, 1.0f, 179.0f);
        const float sceneAspect = view.m_flAspectRatio > 0.001f ? view.m_flAspectRatio :
            static_cast<float>(view.width) / (std::max)(1, view.height);
        const float vmAspect = static_cast<float>(view.width) / (std::max)(1, view.height);
        constexpr float radians = 3.14159265358979323846f / 360.0f;
        const float xRatio = std::tan(sceneFov * radians) / std::tan(vmFov * radians);
        const float yRatio = xRatio * vmAspect / sceneAspect;
        const Vector delta = point - view.origin;
        return view.origin + forward * DotProduct(delta, forward) +
            right * (DotProduct(delta, right) * xRatio) + up * (DotProduct(delta, up) * yRatio);
    }
    D3DMATRIX ScopeD3DMatrix(const VrHandMatrix4& matrix)
    {
        D3DMATRIX output{};
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c) output.m[r][c] = VrHandMath::Get(matrix, c, r);
        return output;
    }
}

void VR::PublishPhysicalScopeLens(const PhysicalScopeLensSnapshot& sample)
{
    const int eye = m_VrHandsActiveEyeIndex;
    const CViewSetup* view = m_VrHandsActiveEyeView;
    // Why a captured lens was not published ([VR][Scope], once per reason).
    auto refuse = [&](const char* why)
    {
        static std::mutex s_mutex;
        static std::unordered_set<std::string> s_logged;
        std::lock_guard<std::mutex> lock(s_mutex);
        if (s_logged.insert(why).second)
            Game::logMsg("[VR][Scope] lens not published: %s (weapon %d, lens weapon %d, eye %d)", why,
                m_PhysicalScopeWeaponId.load(std::memory_order_acquire), sample.weaponId, eye);
    };
    if (!IsPhysicalScopeMode() || !m_ScopeEnabled) return refuse("scopes off");
    if (!HasFreshPhysicalScopeTracking()) return refuse("no fresh tracking");
    if (!view || eye < 0 || eye > 1) return refuse("no eye view (outside the VR hands eye render)");
    if (!m_FirstPersonControlReady.load(std::memory_order_acquire) ||
        m_RenderPlayerIncap.load(std::memory_order_acquire) || m_RenderTpObserver.load(std::memory_order_acquire) ||
        m_RenderPlayerControlledBySI.load(std::memory_order_acquire) ||
        m_ManualInventoryEmptyHandsActive.load(std::memory_order_acquire)) return refuse("player state");
    if (sample.weaponId != m_PhysicalScopeWeaponId.load(std::memory_order_acquire)) return refuse("lens of another weapon");
    if (!l4d2vr_optics::Finite(sample.center) || !l4d2vr_optics::Finite(sample.forward) ||
        !l4d2vr_optics::Finite(sample.right) || !l4d2vr_optics::Finite(sample.up) ||
        !std::isfinite(sample.radius) || sample.radius <= 0.01f || sample.radius > 8.0f) return refuse("lens geometry");
    if (view->width <= 0 || view->height <= 0 || !std::isfinite(view->fov) ||
        view->fov < 1.0f || view->fov >= 179.0f || !std::isfinite(view->fovViewmodel) ||
        view->fovViewmodel < 1.0f || view->fovViewmodel >= 179.0f) return refuse("eye view fov");
    PhysicalScopeLensSnapshot lens = sample;
    lens.sceneCenter = ScopeViewmodelPointToScene(*view, lens.center);
    lens.sceneForward = ScopeViewmodelPointToScene(*view, lens.center + lens.forward) - lens.sceneCenter;
    lens.sceneUp = ScopeViewmodelPointToScene(*view, lens.center + lens.up) - lens.sceneCenter;
    const Vector sceneEdge = ScopeViewmodelPointToScene(*view, lens.center + lens.right * lens.radius);
    lens.sceneRadius = (sceneEdge - lens.sceneCenter).Length();
    if (!l4d2vr_optics::Finite(lens.sceneCenter) || !l4d2vr_optics::Normalize(lens.sceneForward) ||
        !l4d2vr_optics::Normalize(lens.sceneUp)) return refuse("scene mapping");
    lens.magnification = m_PhysicalScopeMagnification.load(std::memory_order_acquire);
    lens.trackingPose.origin = GetRightControllerAbsPos();
    QAngle::AngleVectors(GetRightControllerAbsAngle(),&lens.trackingPose.forward,&lens.trackingPose.right,&lens.trackingPose.up);
    if (!l4d2vr_optics::Valid(lens.trackingPose)) return refuse("controller pose");
    lens.publishedAt = GetTickCount64();
    std::lock_guard<std::mutex> lock(m_PhysicalScopeLensMutex);
    if (!m_PhysicalScopeWeapons.Current().Valid() || m_PhysicalScopeWeapons.Current().id != lens.weaponId)
        return refuse("weapon identity (owner/handle) not tracked");
    lens.weapon = m_PhysicalScopeWeapons.Current();
    m_PhysicalScopeLenses[eye] = lens;
}

bool VR::ReadPhysicalScopeLens(int eye, PhysicalScopeLensSnapshot& lens)
{
    if (eye < 0 || eye > 1) return false;
    std::lock_guard<std::mutex> lock(m_PhysicalScopeLensMutex);
    lens = m_PhysicalScopeLenses[eye];
    const uint64_t now = GetTickCount64();
    return l4d2vr_optics::Fresh(now,lens.publishedAt) &&
        lens.weapon.Valid() && lens.weapon == m_PhysicalScopeWeapons.Current();
}

bool VR::HasFreshPhysicalScopeTracking() const
{
    VRWorldPoseTrackingSnapshot pose{};
    if (!ReadWorldPoseTrackingSnapshot(pose) || !pose.hmdValid ||
        !l4d2vr_optics::Fresh(GetTickCount64(), pose.capturedAtMs)) return false;
    const bool left = IsGameplayHandLeftPhysical(false);
    const auto finiteAngles = [](const QAngle& angles) {
        return std::isfinite(angles.x) && std::isfinite(angles.y) && std::isfinite(angles.z);
    };
    return (left ? pose.leftHandValid : pose.rightHandValid) &&
        l4d2vr_optics::Finite(pose.hmdPosition) && finiteAngles(pose.hmdAngles) &&
        l4d2vr_optics::Finite(left ? pose.leftHandPosition : pose.rightHandPosition) &&
        finiteAngles(left ? pose.leftHandAngles : pose.rightHandAngles);
}

bool VR::ReadPhysicalScopeRender(PhysicalScopeRenderSnapshot& snapshot)
{
    if (!HasFreshPhysicalScopeTracking()) return false;
    std::lock_guard<std::mutex> lock(m_PhysicalScopeLensMutex);
    snapshot = m_PhysicalScopeRender;
    return snapshot.weapon.Valid() && snapshot.weapon == m_PhysicalScopeWeapons.Current() &&
        l4d2vr_optics::Finite(snapshot.origin) &&
        std::isfinite(snapshot.angles.x) && std::isfinite(snapshot.angles.y) && std::isfinite(snapshot.angles.z) &&
        l4d2vr_optics::ValidRenderFov(snapshot.fov) &&
        std::isfinite(snapshot.magnification) && snapshot.magnification >= 1.0f && snapshot.magnification <= 16.0f &&
        l4d2vr_optics::Fresh(GetTickCount64(), snapshot.publishedAt);
}

void VR::PublishPhysicalScopeImage(const PhysicalScopeRenderSnapshot& snapshot, uint32_t renderEpoch)
{
    if (m_SourceRenderQueueMarkerEpoch.load(std::memory_order_acquire) != renderEpoch) return;
    std::lock_guard<std::mutex> lock(m_PhysicalScopeLensMutex);
    const uint64_t now = GetTickCount64();
    if (!snapshot.weapon.Valid())
    {
        // Legacy scopes render into the same surface. Their completion must
        // invalidate physical stamps rather than leave the previous label live.
        m_PhysicalScopeImage = {}; m_PhysicalScopeProcessedImage = {};
        return;
    }
    if (!(snapshot.weapon == m_PhysicalScopeWeapons.Current()) ||
        !l4d2vr_optics::Fresh(now, snapshot.publishedAt)) return;
    m_PhysicalScopeImage = {snapshot.weapon, snapshot.magnification, now, renderEpoch};
}

l4d2vr_optics::ImageStamp VR::ReadPhysicalScopeImage(bool processed)
{
    std::lock_guard<std::mutex> lock(m_PhysicalScopeLensMutex);
    return processed ? m_PhysicalScopeProcessedImage : m_PhysicalScopeImage;
}

void VR::PublishPhysicalScopeProcessedImage(const l4d2vr_optics::ImageStamp& source)
{
    std::lock_guard<std::mutex> lock(m_PhysicalScopeLensMutex);
    if (source.weapon.Valid() && source.weapon == m_PhysicalScopeWeapons.Current() &&
        source.weapon == m_PhysicalScopeImage.weapon && source.completedAt == m_PhysicalScopeImage.completedAt &&
        source.magnification == m_PhysicalScopeImage.magnification &&
        source.renderEpoch == m_SourceRenderQueueMarkerEpoch.load(std::memory_order_acquire))
        m_PhysicalScopeProcessedImage = source;
    else m_PhysicalScopeProcessedImage = {};
}

// Stages of the picture-in-picture scope, for the [VR][Scope] log.
namespace
{
    enum PhysicalScopeStage : int
    {
        kScopeStageOff = 0,          // scopes off, mouse mode or third person
        kScopeStageNoOptic = 1,      // the held gun has no optic
        kScopeStagePlayerState = 2,  // not in control (incapped, pinned, empty hands)
        kScopeStageTracking = 3,     // no fresh headset/hand pose
        kScopeStageNoLens = 4,       // the viewmodel draw published no lens (model checksum, stale)
        kScopeStageRebase = 5,
        kScopeStageNotLooking = 6,   // no eye behind the lens within the eyebox
        kScopeStageFov = 7,
        kScopeStageActive = 8,       // the scope camera renders this frame
    };
    const char* PhysicalScopeStageName(int stage)
    {
        switch (stage)
        {
        case kScopeStageOff: return "off";
        case kScopeStageNoOptic: return "no-optic";
        case kScopeStagePlayerState: return "player-state";
        case kScopeStageTracking: return "no-tracking";
        case kScopeStageNoLens: return "no-lens";
        case kScopeStageRebase: return "rebase";
        case kScopeStageNotLooking: return "not-looking-through";
        case kScopeStageFov: return "bad-fov";
        case kScopeStageActive: return "active";
        default: return "?";
        }
    }
    // What an eye's lens draw showed (m_PhysicalScopeDiagDraw).
    const char* PhysicalScopeDrawName(int code)
    {
        switch (code)
        {
        case 0: return "none";
        case 1: return "black(eye-out-of-eyebox)";
        case 2: return "black(no-texture)";
        case 3: return "black(stale-image)";
        case 4: return "image";
        default: return "?";
        }
    }
}

void VR::LogPhysicalScopeState(const char* label, bool force)
{
    const int stage = m_PhysicalScopeDiagStage.load(std::memory_order_acquire);
    const int draw = m_PhysicalScopeDiagDraw[0].load(std::memory_order_acquire) |
        (m_PhysicalScopeDiagDraw[1].load(std::memory_order_acquire) << 4);
    const uint64_t now = GetTickCount64();
    const int weaponId = m_PhysicalScopeWeaponId.load(std::memory_order_acquire);
    if (!force)
    {
        // Only while an optic is held, on a change, at most twice a second.
        if (!l4d2vr_optics::HasOptic(weaponId) && m_PhysicalScopeDiagLoggedStage <= kScopeStageNoOptic)
            return;
        if (stage == m_PhysicalScopeDiagLoggedStage && draw == m_PhysicalScopeDiagLoggedDraw)
            return;
        if (now - m_PhysicalScopeDiagLoggedAt < 500u)
            return;
    }
    m_PhysicalScopeDiagLoggedStage = stage;
    m_PhysicalScopeDiagLoggedDraw = draw;
    m_PhysicalScopeDiagLoggedAt = now;
    const l4d2vr_optics::ImageStamp image = ReadPhysicalScopeImage();
    PhysicalScopeLensSnapshot lens{};
    uint64_t lensAt[2] = { 0u, 0u };
    {
        std::lock_guard<std::mutex> lock(m_PhysicalScopeLensMutex);
        lens = m_PhysicalScopeLenses[0].publishedAt ? m_PhysicalScopeLenses[0] : m_PhysicalScopeLenses[1];
        lensAt[0] = m_PhysicalScopeLenses[0].publishedAt;
        lensAt[1] = m_PhysicalScopeLenses[1].publishedAt;
    }
    const uint32_t epoch = m_SourceRenderQueueMarkerEpoch.load(std::memory_order_acquire);
    Game::logMsg("[VR][Scope] %s: weapon=%d stage=%s lens L/R age %lld/%lld ms radius %.2f (scene %.2f) mag %.2f | "
        "eye L alpha %.2f relief %.1f cm off-axis %.1f deg, R alpha %.2f relief %.1f cm off-axis %.1f deg | "
        "render fov %.2f active=%d texture=%d surface=%d | image age %lld ms epoch %u/%u mag %.2f | draw L=%s R=%s",
        label ? label : "state", weaponId, PhysicalScopeStageName(stage),
        lensAt[0] ? static_cast<long long>(now - lensAt[0]) : -1LL, lensAt[1] ? static_cast<long long>(now - lensAt[1]) : -1LL,
        lens.radius, lens.sceneRadius, m_PhysicalScopeMagnification.load(std::memory_order_acquire),
        m_PhysicalScopeDiagAlpha[0], m_PhysicalScopeDiagRelief[0] / std::max(0.001f, m_VRScale) * 100.0f, m_PhysicalScopeDiagOffAxisDeg[0],
        m_PhysicalScopeDiagAlpha[1], m_PhysicalScopeDiagRelief[1] / std::max(0.001f, m_VRScale) * 100.0f, m_PhysicalScopeDiagOffAxisDeg[1],
        m_PhysicalScopeRenderFov, m_ScopeActive ? 1 : 0, m_ScopeTexture ? 1 : 0, m_D9ScopeSurface ? 1 : 0,
        image.completedAt ? static_cast<long long>(now - image.completedAt) : -1LL, image.renderEpoch, epoch, image.magnification,
        PhysicalScopeDrawName(draw & 15), PhysicalScopeDrawName(draw >> 4));
}

void VR::UpdatePhysicalScope()
{
    m_ScopeActive = false;
    struct StageLog
    {
        VR* vr; int stage = kScopeStageOff;
        ~StageLog() { vr->m_PhysicalScopeDiagStage.store(stage, std::memory_order_release); vr->LogPhysicalScopeState("change", false); }
    } stageLog{ this };
    const float magnification = l4d2vr_optics::Magnification(m_ScopeFov, m_ScopeFovMax, m_ScopeBaseMagnification);
    m_PhysicalScopeMagnification.store(magnification, std::memory_order_release);
    if (!m_ScopeEnabled || !IsPhysicalScopeMode()) return;
    stageLog.stage = kScopeStageNoOptic;
    if (!ScopeWeaponEligible()) return;
    stageLog.stage = kScopeStagePlayerState;
    if (!m_FirstPersonControlReady.load(std::memory_order_acquire) ||
        m_RenderPlayerIncap.load(std::memory_order_acquire) || m_RenderPlayerControlledBySI.load(std::memory_order_acquire) ||
        m_ManualInventoryEmptyHandsActive.load(std::memory_order_acquire)) return;
    stageLog.stage = kScopeStageTracking;
    if (!HasFreshPhysicalScopeTracking()) return;
    stageLog.stage = kScopeStageNoLens;
    PhysicalScopeLensSnapshot lens{};
    if (!ReadPhysicalScopeLens(0, lens) && !ReadPhysicalScopeLens(1, lens)) return;
    stageLog.stage = kScopeStageRebase;
    l4d2vr_optics::Pose current{};
    current.origin = GetRightControllerAbsPos();
    QAngle::AngleVectors(GetRightControllerAbsAngle(),&current.forward,&current.right,&current.up);
    Vector center(0,0,0), forward(0,0,0), up(0,0,0);
    if (!l4d2vr_optics::RebasePoint(lens.sceneCenter,lens.trackingPose,current,center) ||
        !l4d2vr_optics::RebaseVector(lens.sceneForward,lens.trackingPose,current,forward) ||
        !l4d2vr_optics::RebaseVector(lens.sceneUp,lens.trackingPose,current,up)) return;
    lens.sceneCenter = center; lens.sceneForward = forward; lens.sceneUp = up;
    auto left = l4d2vr_optics::EvaluateEye(GetViewOriginLeft(), lens.sceneCenter, lens.sceneForward, lens.sceneRadius, m_VRScale, magnification, m_ScopeEyeboxDeg);
    auto right = l4d2vr_optics::EvaluateEye(GetViewOriginRight(), lens.sceneCenter, lens.sceneForward, lens.sceneRadius, m_VRScale, magnification, m_ScopeEyeboxDeg);
    m_PhysicalScopeDiagAlpha[0] = left.alpha; m_PhysicalScopeDiagRelief[0] = left.relief; m_PhysicalScopeDiagOffAxisDeg[0] = left.offAxisDeg;
    m_PhysicalScopeDiagAlpha[1] = right.alpha; m_PhysicalScopeDiagRelief[1] = right.relief; m_PhysicalScopeDiagOffAxisDeg[1] = right.offAxisDeg;
    const auto eye = left.alpha >= right.alpha ? left : right;
    stageLog.stage = kScopeStageNotLooking;
    if (eye.alpha <= 0.0f) return;
    stageLog.stage = kScopeStageFov;
    m_PhysicalScopeRenderFov = l4d2vr_optics::RenderFov(lens.sceneRadius, eye.relief, magnification);
    if (m_PhysicalScopeRenderFov <= 0.0f) return;
    m_ScopeCameraPosAbs = lens.sceneCenter;
    QAngle::VectorAngles(lens.sceneForward, lens.sceneUp, m_ScopeCameraAngAbs);
    {
        std::lock_guard<std::mutex> lock(m_PhysicalScopeLensMutex);
        if (!(lens.weapon == m_PhysicalScopeWeapons.Current())) return;
        m_PhysicalScopeRender = {lens.weapon, m_ScopeCameraPosAbs, m_ScopeCameraAngAbs,
            m_PhysicalScopeRenderFov, magnification, GetTickCount64()};
    }
    m_ScopeActive = true;
    stageLog.stage = kScopeStageActive;
}

bool VR::ReadNativeThrowArc(NativeThrowArcSnapshot& out) const
{
    std::lock_guard<std::mutex> lock(m_NativeThrowArcMutex);
    out = m_NativeThrowArcSnapshot;
    const uint64_t now = GetTickCount64();
    return out.valid && out.publishedAt != 0u && now >= out.publishedAt && now - out.publishedAt <= 150u &&
        GrenadeArcMode();
}

// The grenade arc into an eye (after the viewmodel, like the scope lens): a
// camera-facing ribbon through the arc's points, drawn over the scene so it
// stays readable, fading toward the landing point, with a ring where it lands.
bool VR::DrawNativeThrowArcForEye(const CViewSetup& view, int eyeIndex, const NativeThrowArcSnapshot& arc)
{
    if (!arc.valid || !g_D3DVR9 || eyeIndex < 0 || eyeIndex > 1 || view.width <= 0 || view.height <= 0 ||
        !std::isfinite(view.fov) || view.fov < 1.0f || view.fov >= 179.0f ||
        !l4d2vr_optics::Finite(view.origin) || !l4d2vr_optics::Finite(view.angles))
        return false;
    struct DeviceLock
    {
        bool locked = false;
        explicit DeviceLock(bool required)
        { if (required && g_D3DVR9) locked = SUCCEEDED(g_D3DVR9->LockDevice()); }
        ~DeviceLock() { if (locked && g_D3DVR9) g_D3DVR9->UnlockDevice(); }
    } deviceLock(m_Game && m_Game->GetMatQueueMode() != 0);
    if (m_Game && m_Game->GetMatQueueMode() != 0 && !deviceLock.locked) return false;
    // 2.0.11: on the material worker (queued) only try: the main thread can hold
    // m_TextureMutex while it waits for this worker (texture recreation);
    // skipping one draw is harmless.
    std::unique_lock<TextureStateMutex> textureLock(m_TextureMutex, std::defer_lock);
    if (deviceLock.locked) { if (!textureLock.try_lock()) return false; }
    else textureLock.lock();
    IDirect3DSurface9* surface = eyeIndex == 0 ? m_D9LeftEyeSurface : m_D9RightEyeSurface;
    if (m_CreatingTextureID != Texture_None || !surface) return false;
    IDirect3DDevice9* device = nullptr;
    if (FAILED(surface->GetDevice(&device)) || !device) return false;
    struct Restore
    {
        IDirect3DDevice9* device;
        IDirect3DStateBlock9* state = nullptr;
        IDirect3DSurface9* target = nullptr;
        D3DVIEWPORT9 viewport{};
        bool haveViewport = false;
        bool captured = false;
        ~Restore()
        {
            if (target) device->SetRenderTarget(0, target);
            if (state && captured) state->Apply();
            if (haveViewport) device->SetViewport(&viewport);
            if (state) state->Release();
            if (target) target->Release();
            device->Release();
        }
    } restore{device};
    if (FAILED(device->CreateStateBlock(D3DSBT_ALL, &restore.state)) || !restore.state ||
        !(restore.captured = SUCCEEDED(restore.state->Capture())) || FAILED(device->GetRenderTarget(0, &restore.target)))
        return false;
    restore.haveViewport = SUCCEEDED(device->GetViewport(&restore.viewport));
    if (!restore.haveViewport || FAILED(device->SetRenderTarget(0, surface))) return false;
    D3DVIEWPORT9 viewport{0,0,static_cast<DWORD>(view.width),static_cast<DWORD>(view.height),0.0f,1.0f};
    if (FAILED(device->SetViewport(&viewport))) return false;
    const float zNear = std::isfinite(view.zNear) && view.zNear > 0.1f ? view.zNear : 3.0f;
    const float zFar = std::isfinite(view.zFar) && view.zFar > zNear ? view.zFar : 30000.0f;
    const D3DMATRIX world = ScopeD3DMatrix(VrHandMath::Identity());
    const D3DMATRIX camera = ScopeD3DMatrix(VrHandMath::BuildSourceView(view.origin, view.angles));
    const D3DMATRIX projection = ScopeD3DMatrix(VrHandMath::BuildPerspective(view.fov,
        static_cast<float>(view.width) / view.height, zNear, zFar));
    struct Vertex { Vector position; D3DCOLOR color; };
    std::vector<Vertex> mesh;
    const size_t count = arc.points.size();
    mesh.reserve(count * 6 + 64 * 6);
    const float halfWidth = 0.006f * std::max(1.0f, m_VRScale);   // a 1.2 cm ribbon
    auto side = [&](const Vector& a, const Vector& b) -> Vector
    {
        Vector along = b - a;
        Vector toEye = view.origin - a;
        Vector s = CrossProduct(along, toEye);
        const float length = s.Length();
        return length > 1e-4f ? s * (halfWidth / length) : Vector(0.0f, 0.0f, halfWidth);
    };
    for (size_t i = 0; i + 1 < count; ++i)
    {
        const Vector& a = arc.points[i];
        const Vector& b = arc.points[i + 1];
        if (!l4d2vr_optics::Finite(a) || !l4d2vr_optics::Finite(b) || (b - a).Length() < 0.01f) continue;
        const Vector s = side(a, b);
        const float u0 = static_cast<float>(i) / static_cast<float>(count - 1);
        const float u1 = static_cast<float>(i + 1) / static_cast<float>(count - 1);
        const auto alpha = [](float u) { return static_cast<unsigned>(std::clamp(230.0f - 90.0f * u, 0.0f, 255.0f)); };
        const D3DCOLOR c0 = D3DCOLOR_ARGB(alpha(u0), 255, 225, 120), c1 = D3DCOLOR_ARGB(alpha(u1), 255, 225, 120);
        mesh.push_back({a - s, c0}); mesh.push_back({a + s, c0}); mesh.push_back({b + s, c1});
        mesh.push_back({a - s, c0}); mesh.push_back({b + s, c1}); mesh.push_back({b - s, c1});
    }
    // The landing ring, flat on the ground.
    {
        const Vector& end = arc.points[count - 1];
        const float r0 = 0.14f * std::max(1.0f, m_VRScale), r1 = r0 + 2.0f * halfWidth;
        const D3DCOLOR ring = D3DCOLOR_ARGB(200, 255, 140, 60);
        constexpr int kRing = 32;
        for (int i = 0; i < kRing && l4d2vr_optics::Finite(end); ++i)
        {
            const float a0 = i * (6.2831853f / kRing), a1 = (i + 1) * (6.2831853f / kRing);
            const Vector p00 = end + Vector(std::cos(a0) * r0, std::sin(a0) * r0, 1.0f);
            const Vector p01 = end + Vector(std::cos(a1) * r0, std::sin(a1) * r0, 1.0f);
            const Vector p10 = end + Vector(std::cos(a0) * r1, std::sin(a0) * r1, 1.0f);
            const Vector p11 = end + Vector(std::cos(a1) * r1, std::sin(a1) * r1, 1.0f);
            mesh.push_back({p00, ring}); mesh.push_back({p10, ring}); mesh.push_back({p11, ring});
            mesh.push_back({p00, ring}); mesh.push_back({p11, ring}); mesh.push_back({p01, ring});
        }
    }
    if (mesh.empty()) return false;
    bool ok = SUCCEEDED(device->SetVertexShader(nullptr)) && SUCCEEDED(device->SetPixelShader(nullptr)) &&
        SUCCEEDED(device->SetFVF(D3DFVF_XYZ | D3DFVF_DIFFUSE)) &&
        SUCCEEDED(device->SetTransform(D3DTS_WORLD,&world)) && SUCCEEDED(device->SetTransform(D3DTS_VIEW,&camera)) &&
        SUCCEEDED(device->SetTransform(D3DTS_PROJECTION,&projection));
    ok = ok && SUCCEEDED(device->SetRenderState(D3DRS_ZENABLE,FALSE)) &&
        SUCCEEDED(device->SetRenderState(D3DRS_ZWRITEENABLE,FALSE)) &&
        SUCCEEDED(device->SetRenderState(D3DRS_CLIPPING,TRUE)) &&
        SUCCEEDED(device->SetRenderState(D3DRS_SCISSORTESTENABLE,FALSE)) &&
        SUCCEEDED(device->SetRenderState(D3DRS_CLIPPLANEENABLE,0u)) &&
        SUCCEEDED(device->SetRenderState(D3DRS_FILLMODE,D3DFILL_SOLID)) &&
        SUCCEEDED(device->SetRenderState(D3DRS_VERTEXBLEND,D3DVBF_DISABLE)) &&
        SUCCEEDED(device->SetRenderState(D3DRS_INDEXEDVERTEXBLENDENABLE,FALSE)) &&
        SUCCEEDED(device->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE)) &&
        SUCCEEDED(device->SetRenderState(D3DRS_LIGHTING,FALSE)) &&
        SUCCEEDED(device->SetRenderState(D3DRS_FOGENABLE,FALSE)) &&
        SUCCEEDED(device->SetRenderState(D3DRS_STENCILENABLE,FALSE)) &&
        SUCCEEDED(device->SetRenderState(D3DRS_ALPHATESTENABLE,FALSE)) &&
        SUCCEEDED(device->SetRenderState(D3DRS_ALPHABLENDENABLE,TRUE)) &&
        SUCCEEDED(device->SetRenderState(D3DRS_BLENDOP,D3DBLENDOP_ADD)) &&
        SUCCEEDED(device->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE,FALSE)) &&
        SUCCEEDED(device->SetRenderState(D3DRS_SRCBLEND,D3DBLEND_SRCALPHA)) &&
        SUCCEEDED(device->SetRenderState(D3DRS_DESTBLEND,D3DBLEND_INVSRCALPHA)) &&
        SUCCEEDED(device->SetRenderState(D3DRS_COLORWRITEENABLE,15u)) &&
        SUCCEEDED(device->SetTexture(0,nullptr)) &&
        SUCCEEDED(device->SetTextureStageState(0,D3DTSS_TEXTURETRANSFORMFLAGS,D3DTTFF_DISABLE)) &&
        SUCCEEDED(device->SetTextureStageState(0,D3DTSS_COLOROP,D3DTOP_SELECTARG1)) &&
        SUCCEEDED(device->SetTextureStageState(0,D3DTSS_COLORARG1,D3DTA_DIFFUSE)) &&
        SUCCEEDED(device->SetTextureStageState(0,D3DTSS_ALPHAOP,D3DTOP_SELECTARG1)) &&
        SUCCEEDED(device->SetTextureStageState(0,D3DTSS_ALPHAARG1,D3DTA_DIFFUSE)) &&
        SUCCEEDED(device->SetTextureStageState(1,D3DTSS_COLOROP,D3DTOP_DISABLE)) &&
        SUCCEEDED(device->SetTextureStageState(1,D3DTSS_ALPHAOP,D3DTOP_DISABLE));
    if (ok) ok = SUCCEEDED(device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, static_cast<unsigned>(mesh.size() / 3),
        mesh.data(), sizeof(Vertex)));
    if (ok) m_NativeThrowArcEyeDraws.fetch_add(1u, std::memory_order_relaxed);
    return ok;
}

bool VR::DrawPhysicalScopeLensForEye(const CViewSetup& view, int eyeIndex, const PhysicalScopeLensSnapshot& lens)
{
    if (!IsPhysicalScopeMode() || !m_ScopeEnabled || !HasFreshPhysicalScopeTracking() || !g_D3DVR9 || eyeIndex < 0 || eyeIndex > 1 ||
        !l4d2vr_optics::Fresh(GetTickCount64(),lens.publishedAt) ||
        lens.weaponId != m_PhysicalScopeWeaponId.load(std::memory_order_acquire) ||
        !l4d2vr_optics::Finite(lens.center) || !l4d2vr_optics::Finite(lens.right) || !l4d2vr_optics::Finite(lens.up) ||
        !std::isfinite(lens.radius) || lens.radius <= 0.0f ||
        view.width <= 0 || view.height <= 0 || !std::isfinite(view.fovViewmodel) ||
        view.fovViewmodel < 1.0f || view.fovViewmodel >= 179.0f ||
        !l4d2vr_optics::Finite(view.origin) || !l4d2vr_optics::Finite(view.angles) ||
        !l4d2vr_optics::ValidClipPlanes(view.zNearViewmodel, view.zFarViewmodel)) return false;
    l4d2vr_optics::ImageStamp scopeImage{};
    l4d2vr_optics::ImageStamp processedImage{};
    {
        std::lock_guard<std::mutex> lock(m_PhysicalScopeLensMutex);
        if (!lens.weapon.Valid() || !(lens.weapon == m_PhysicalScopeWeapons.Current())) return false;
        scopeImage = m_PhysicalScopeImage;
        processedImage = m_PhysicalScopeProcessedImage;
    }
    struct DeviceLock
    {
        bool locked = false;
        explicit DeviceLock(bool required)
        { if (required && g_D3DVR9) locked = SUCCEEDED(g_D3DVR9->LockDevice()); }
        ~DeviceLock() { if (locked && g_D3DVR9) g_D3DVR9->UnlockDevice(); }
    } deviceLock(m_Game && m_Game->GetMatQueueMode() != 0);
    if (m_Game && m_Game->GetMatQueueMode() != 0 && !deviceLock.locked) return false;
    // 2.0.11: on the material worker (queued) only try: the main thread can hold
    // m_TextureMutex while it waits for this worker (texture recreation);
    // skipping one draw is harmless.
    std::unique_lock<TextureStateMutex> textureLock(m_TextureMutex, std::defer_lock);
    if (deviceLock.locked) { if (!textureLock.try_lock()) return false; }
    else textureLock.lock();
    IDirect3DSurface9* surface = eyeIndex == 0 ? m_D9LeftEyeSurface : m_D9RightEyeSurface;
    if (m_CreatingTextureID != Texture_None || !surface) return false;
    IDirect3DDevice9* device = nullptr;
    if (FAILED(surface->GetDevice(&device)) || !device) return false;
    struct Restore
    {
        IDirect3DDevice9* device;
        IDirect3DStateBlock9* state = nullptr;
        IDirect3DSurface9* target = nullptr;
        IDirect3DSurface9* depth = nullptr;
        D3DVIEWPORT9 viewport{};
        bool haveViewport = false;
        bool captured = false;
        bool restored = false;
        bool RestoreState()
        {
            if (restored) return true;
            restored = true;
            bool ok = true;
            // SetRenderTarget resets viewport and scissor state. Restore it
            // before applying the captured pipeline, including its scissor rect.
            if (target) ok = SUCCEEDED(device->SetRenderTarget(0, target)) && ok;
            if (state && captured) ok = SUCCEEDED(state->Apply()) && ok;
            if (haveViewport) ok = SUCCEEDED(device->SetViewport(&viewport)) && ok;
            return ok;
        }
        ~Restore()
        {
            (void)RestoreState();
            if (state) state->Release();
            if (target) target->Release();
            if (depth) depth->Release();
            device->Release();
        }
    } restore{device};
    if (FAILED(device->CreateStateBlock(D3DSBT_ALL, &restore.state)) || !restore.state ||
        !(restore.captured = SUCCEEDED(restore.state->Capture())) || FAILED(device->GetRenderTarget(0, &restore.target)) ||
        FAILED(device->GetDepthStencilSurface(&restore.depth)) || !restore.depth) return false;
    restore.haveViewport = SUCCEEDED(device->GetViewport(&restore.viewport));
    if (!restore.haveViewport || FAILED(device->SetRenderTarget(0, surface))) return false;
    D3DVIEWPORT9 viewport{0,0,static_cast<DWORD>(view.width),static_cast<DWORD>(view.height),0.0f,1.0f};
    if (FAILED(device->SetViewport(&viewport))) return false;
    const D3DMATRIX world = ScopeD3DMatrix(VrHandMath::Identity());
    const D3DMATRIX camera = ScopeD3DMatrix(VrHandMath::BuildSourceView(view.origin, view.angles));
    const D3DMATRIX projection = ScopeD3DMatrix(VrHandMath::BuildPerspective(view.fovViewmodel,
        static_cast<float>(view.width) / view.height, view.zNearViewmodel, view.zFarViewmodel));
    struct Vertex { Vector position; D3DCOLOR color; float u, v; };
    std::array<Vertex,66> vertices{};
    vertices[0] = {lens.center,D3DCOLOR_ARGB(255,255,255,255),0.5f,0.5f};
    for (size_t i = 1; i < vertices.size(); ++i)
    {
        const float angle = static_cast<float>(i - 1) * (2.0f * 3.14159265358979323846f / 64.0f);
        const float x = std::cos(angle), y = std::sin(angle);
        vertices[i] = {lens.center + lens.right * (x * lens.radius) + lens.up * (y * lens.radius),
            D3DCOLOR_ARGB(255,255,255,255),0.5f + x * 0.49f,0.5f - y * 0.49f};
    }
    bool ok = SUCCEEDED(device->SetVertexShader(nullptr)) && SUCCEEDED(device->SetPixelShader(nullptr)) &&
        SUCCEEDED(device->SetFVF(D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1)) &&
        SUCCEEDED(device->SetTransform(D3DTS_WORLD,&world)) && SUCCEEDED(device->SetTransform(D3DTS_VIEW,&camera)) &&
        SUCCEEDED(device->SetTransform(D3DTS_PROJECTION,&projection));
    // Establish the fixed-function state instead of inheriting native material
    // blending, generated UVs, wireframe fill or a prior scissor test.
    ok = ok && SUCCEEDED(device->SetRenderState(D3DRS_ZENABLE,TRUE)) &&
        SUCCEEDED(device->SetRenderState(D3DRS_ZWRITEENABLE,FALSE)) &&
        SUCCEEDED(device->SetRenderState(D3DRS_ZFUNC,D3DCMP_LESSEQUAL)) &&
        SUCCEEDED(device->SetRenderState(D3DRS_CLIPPING,m_VRNearClipSelfBody ? FALSE : TRUE)) &&
        SUCCEEDED(device->SetRenderState(D3DRS_SCISSORTESTENABLE,FALSE)) &&
        SUCCEEDED(device->SetRenderState(D3DRS_CLIPPLANEENABLE,0u)) &&
        SUCCEEDED(device->SetRenderState(D3DRS_FILLMODE,D3DFILL_SOLID)) &&
        SUCCEEDED(device->SetRenderState(D3DRS_VERTEXBLEND,D3DVBF_DISABLE)) &&
        SUCCEEDED(device->SetRenderState(D3DRS_INDEXEDVERTEXBLENDENABLE,FALSE)) &&
        SUCCEEDED(device->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE)) &&
        SUCCEEDED(device->SetRenderState(D3DRS_LIGHTING,FALSE)) &&
        SUCCEEDED(device->SetRenderState(D3DRS_FOGENABLE,FALSE)) &&
        SUCCEEDED(device->SetRenderState(D3DRS_STENCILENABLE,FALSE)) &&
        SUCCEEDED(device->SetRenderState(D3DRS_ALPHATESTENABLE,FALSE)) &&
        SUCCEEDED(device->SetRenderState(D3DRS_ALPHABLENDENABLE,FALSE)) &&
        SUCCEEDED(device->SetRenderState(D3DRS_COLORWRITEENABLE,15u)) &&
        SUCCEEDED(device->SetRenderState(D3DRS_WRAP0,0u)) &&
        SUCCEEDED(device->SetTexture(0,nullptr)) &&
        SUCCEEDED(device->SetTextureStageState(0,D3DTSS_TEXCOORDINDEX,0u)) &&
        SUCCEEDED(device->SetTextureStageState(0,D3DTSS_TEXTURETRANSFORMFLAGS,D3DTTFF_DISABLE)) &&
        SUCCEEDED(device->SetTextureStageState(0,D3DTSS_COLOROP,D3DTOP_SELECTARG1)) &&
        SUCCEEDED(device->SetTextureStageState(0,D3DTSS_COLORARG1,D3DTA_TFACTOR)) &&
        SUCCEEDED(device->SetTextureStageState(0,D3DTSS_ALPHAOP,D3DTOP_SELECTARG1)) &&
        SUCCEEDED(device->SetTextureStageState(0,D3DTSS_ALPHAARG1,D3DTA_DIFFUSE)) &&
        SUCCEEDED(device->SetRenderState(D3DRS_TEXTUREFACTOR,D3DCOLOR_ARGB(255,0,0,0))) &&
        SUCCEEDED(device->SetTextureStageState(1,D3DTSS_COLOROP,D3DTOP_DISABLE)) &&
        SUCCEEDED(device->SetTextureStageState(1,D3DTSS_ALPHAOP,D3DTOP_DISABLE));
    if (ok) ok = SUCCEEDED(device->DrawPrimitiveUP(D3DPT_TRIANGLEFAN,64,vertices.data(),sizeof(Vertex)));
    const auto eye = l4d2vr_optics::EvaluateEye(view.origin, lens.sceneCenter, lens.sceneForward,
        lens.sceneRadius, m_VRScale, lens.magnification, m_ScopeEyeboxDeg);
    IDirect3DTexture9* rawTexture = nullptr;
    if (m_D9ScopeSurface)
        m_D9ScopeSurface->GetContainer(__uuidof(IDirect3DTexture9), reinterpret_cast<void**>(&rawTexture));
    IDirect3DTexture9* texture = rawTexture;
    if (!texture && m_ScopeLensOverlayReady.load(std::memory_order_acquire))
    {
        texture = m_D9ScopeLensTexture;
        scopeImage = processedImage;
    }
    const bool imageMatches = l4d2vr_optics::ImageMatches(scopeImage, lens.weapon, lens.magnification,
        m_SourceRenderQueueMarkerEpoch.load(std::memory_order_acquire), GetTickCount64());
    m_PhysicalScopeDiagDraw[eyeIndex].store(!ok ? 0 : eye.alpha <= 0.0f ? 1 : !texture ? 2 : !imageMatches ? 3 : 4,
        std::memory_order_release);
    if (ok && eye.alpha > 0.0f && texture && imageMatches)
    {
        const auto brightness = static_cast<unsigned char>(eye.alpha * 255.0f);
        for (auto& vertex : vertices) vertex.color = D3DCOLOR_ARGB(255,brightness,brightness,brightness);
        ok = SUCCEEDED(device->SetTexture(0,texture)) &&
            SUCCEEDED(device->SetTextureStageState(0,D3DTSS_COLOROP,D3DTOP_MODULATE)) &&
            SUCCEEDED(device->SetTextureStageState(0,D3DTSS_COLORARG1,D3DTA_TEXTURE)) &&
            SUCCEEDED(device->SetTextureStageState(0,D3DTSS_COLORARG2,D3DTA_DIFFUSE)) &&
            SUCCEEDED(device->SetSamplerState(0,D3DSAMP_MINFILTER,D3DTEXF_LINEAR)) &&
            SUCCEEDED(device->SetSamplerState(0,D3DSAMP_MAGFILTER,D3DTEXF_LINEAR)) &&
            SUCCEEDED(device->SetSamplerState(0,D3DSAMP_ADDRESSU,D3DTADDRESS_CLAMP)) &&
            SUCCEEDED(device->SetSamplerState(0,D3DSAMP_ADDRESSV,D3DTADDRESS_CLAMP));
        if (ok && m_ScopePictureInPictureLens)
        {
            // HL2VR-style lens: the image shifts with the eye's offset from the
            // optical axis (parallax) and closes in from the edge as the eye
            // leaves the eyebox. Rings of vertices carry the shifted UVs and the
            // vignette as vertex colour.
            l4d2vr_scope_lens::LensGeometry geometry{};
            geometry.center = lens.center;
            geometry.axis = lens.forward;
            geometry.up = lens.up;
            geometry.radius = lens.radius;
            geometry.eyeRelief = std::max(0.5f, DotProduct(lens.center - view.origin, lens.forward));
            l4d2vr_scope_lens::LensParams lensParams{};
            lensParams.parallaxK = m_ScopeParallaxStrength;
            lensParams.eyeboxDeg = m_ScopeEyeboxDeg;
            lensParams.vignetteExponent = m_ScopeVignetteExponent;
            const l4d2vr_scope_lens::LensState state = l4d2vr_scope_lens::EvaluateLens(geometry, view.origin, lensParams);
            Vector basisForward{}, basisRight{}, basisUp{};
            const float uSign = l4d2vr_scope_lens::Basis(geometry, basisForward, basisRight, basisUp) &&
                DotProduct(basisRight, lens.right) < 0.0f ? -1.0f : 1.0f;
            constexpr int kRings = 6;
            constexpr int kSegments = 48;
            std::vector<Vertex> mesh;
            mesh.reserve(static_cast<size_t>(kRings) * kSegments * 6);
            auto makeVertex = [&](float r, float angle) -> Vertex
            {
                const float u = r * std::cos(angle), v = r * std::sin(angle);
                float texU = 0.5f, texV = 0.5f;
                l4d2vr_scope_lens::SampleUV(state, u * uSign, v, texU, texV);
                const float transmission = 1.0f - l4d2vr_scope_lens::VignetteAlphaAt(state, u * uSign, v);
                const auto level = static_cast<unsigned char>(std::clamp(eye.alpha * transmission, 0.0f, 1.0f) * 255.0f);
                if (uSign < 0.0f) texU = 1.0f - texU;
                return Vertex{ lens.center + lens.right * (u * lens.radius) + lens.up * (v * lens.radius),
                    D3DCOLOR_ARGB(255, level, level, level), std::clamp(texU, 0.0f, 1.0f), std::clamp(texV, 0.0f, 1.0f) };
            };
            for (int ring = 0; ring < kRings; ++ring)
            {
                const float r0 = static_cast<float>(ring) / kRings, r1 = static_cast<float>(ring + 1) / kRings;
                for (int segment = 0; segment < kSegments; ++segment)
                {
                    const float a0 = segment * (2.0f * 3.14159265358979323846f / kSegments);
                    const float a1 = (segment + 1) * (2.0f * 3.14159265358979323846f / kSegments);
                    const Vertex p00 = makeVertex(r0, a0), p01 = makeVertex(r0, a1);
                    const Vertex p10 = makeVertex(r1, a0), p11 = makeVertex(r1, a1);
                    mesh.push_back(p00); mesh.push_back(p10); mesh.push_back(p11);
                    mesh.push_back(p00); mesh.push_back(p11); mesh.push_back(p01);
                }
            }
            ok = SUCCEEDED(device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, static_cast<unsigned>(mesh.size() / 3),
                mesh.data(), sizeof(Vertex)));
        }
        else if (ok)
        {
            ok = SUCCEEDED(device->DrawPrimitiveUP(D3DPT_TRIANGLEFAN,64,vertices.data(),sizeof(Vertex)));
        }
        if (ok && rawTexture && m_ScopeReticleAlpha > 0.0f)
        {
            const Vector center = lens.center - lens.forward * 0.005f;
            const float extent = lens.radius * 0.25f;
            const auto opacity = static_cast<unsigned char>(std::clamp(m_ScopeReticleAlpha,0.0f,1.0f) * 255.0f);
            const D3DCOLOR color = D3DCOLOR_ARGB(opacity,0,0,0);
            const Vertex reticle[] = {
                {center-lens.right*extent,color,0,0},{center+lens.right*extent,color,0,0},
                {center-lens.up*extent,color,0,0},{center+lens.up*extent,color,0,0}};
            ok = SUCCEEDED(device->SetTexture(0,nullptr)) &&
                SUCCEEDED(device->SetTextureStageState(0,D3DTSS_COLOROP,D3DTOP_SELECTARG1)) &&
                SUCCEEDED(device->SetTextureStageState(0,D3DTSS_COLORARG1,D3DTA_DIFFUSE)) &&
                SUCCEEDED(device->SetTextureStageState(0,D3DTSS_ALPHAOP,D3DTOP_SELECTARG1)) &&
                SUCCEEDED(device->SetTextureStageState(0,D3DTSS_ALPHAARG1,D3DTA_DIFFUSE)) &&
                SUCCEEDED(device->SetRenderState(D3DRS_ALPHABLENDENABLE,TRUE)) &&
                SUCCEEDED(device->SetRenderState(D3DRS_BLENDOP,D3DBLENDOP_ADD)) &&
                SUCCEEDED(device->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE,FALSE)) &&
                SUCCEEDED(device->SetRenderState(D3DRS_SRCBLEND,D3DBLEND_SRCALPHA)) &&
                SUCCEEDED(device->SetRenderState(D3DRS_DESTBLEND,D3DBLEND_INVSRCALPHA));
            if (ok) ok = SUCCEEDED(device->DrawPrimitiveUP(D3DPT_LINELIST,2,reticle,sizeof(Vertex)));
        }
    }
    if (rawTexture) rawTexture->Release();
    const bool restored = restore.RestoreState();
    return ok && restored;
}

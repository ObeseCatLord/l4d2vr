// OpenXR input (OpenXR port M3). Included by vr_backend_openxr.cpp inside its
// anonymous namespace (uses XrApi and the pose helpers there).
//
// The mod keeps its OpenVR IVRInput calls; this class answers them:
//   * Action sets "gameplay" (one FLOAT/VECTOR2F action per mod action, both
//     hand subaction paths) and "controller" (grip/aim poses, haptics, the
//     hand stick for the menu dpad and the finger-curl sources).
//   * Suggested bindings for every interaction profile in
//     openxr_input_profiles.h whose extension the runtime offers. A profile the
//     runtime rejects is retried binding by binding and suggested without the
//     rejected paths (logged).
//   * UpdateActionState = xrSyncActions + one read of every action per hand.
//     Click thresholds with hysteresis, the stick deadzone and the menu dpad are
//     applied here, once per sync, so bChanged means "since the last
//     UpdateActionState" exactly as in OpenVR.
//   * Restrict-to-device = the hand's subaction path. activeOrigin is a
//     per-hand origin handle; GetOriginTrackedDeviceInfo maps it to the hand's
//     source handle and pseudo device index (1 = left, 2 = right), so
//     l4d2vr_hand_input::ValidateOrigin works unchanged.
//   * GetActionBindingInfo reports the bindings this class suggested for the
//     hand's current profile in SteamVR's shapes, so the left-handed mirror in
//     vr_lifecycle_pose_hud.inl builds the same map as under SteamVR.
//   * Controller poses: the grip pose converted to the OpenVR raw pose with the
//     profile's measured offset (openxr_input_profiles.h); the aim pose is the
//     controller tip (SteamVR builds its OpenXR aim pose from that component).
//   * Skeletal: summary curls from trigger/squeeze/thumb inputs; bone data is
//     not available until M5 (XR_EXT_hand_tracking retarget).
//   * Fake action source (tests): replaces the per-hand action states.

namespace xi = l4d2vr_openxr_input;

// The overlay runtime's view of the controllers (OpenXR port M4): the laser
// pointer runs once per input sync, and the self-test can aim a hand.
struct XrPointerInputs
{
    bool active[2] = { false, false };
    float trigger[2] = { 0.0f, 0.0f };   // index trigger value 0..1
    float stickY[2] = { 0.0f, 0.0f };    // hand stick (discrete scroll)
};

class XrPointerHook
{
public:
    virtual ~XrPointerHook() = default;
    // Ray-casts the overlays and queues mouse events; engaged[h] = this hand's
    // trigger belongs to an overlay (kept from the game, as SteamVR does).
    virtual void PointerStep(XrTime now, const XrPointerInputs& in, bool engaged[2]) = 0;
    // Self-test aim override: the point a hand's tip ray must point at.
    virtual bool AimTarget(int hand, l4d2vr_overlay_pointer::Vec3& target) = 0;
};

class XrInputRuntime final : public IVrInput
{
public:
    static constexpr vr::VRActionHandle_t kActionBase = 0x1000;
    static constexpr vr::VRActionHandle_t kUnknownActionBase = 0x5000;
    static constexpr vr::VRActionSetHandle_t kSetMain = 0x2001;
    static constexpr vr::VRActionSetHandle_t kSetBase = 0x2002;
    static constexpr vr::VRInputValueHandle_t kSourceLeft = 0x3001;
    static constexpr vr::VRInputValueHandle_t kSourceRight = 0x3002;
    static constexpr vr::VRInputValueHandle_t kOriginLeft = 0x4001;
    static constexpr vr::VRInputValueHandle_t kOriginRight = 0x4002;
    static constexpr vr::TrackedDeviceIndex_t kDeviceLeft = 1;
    static constexpr vr::TrackedDeviceIndex_t kDeviceRight = 2;

    explicit XrInputRuntime(const XrApi& xr) : m_Xr(xr) {}

    // ---- Setup ------------------------------------------------------------------
    // Instance extensions this class wants enabled when the runtime offers them.
    static std::vector<const char*> WantedExtensions()
    {
        std::vector<const char*> out;
        for (const xi::Profile& p : xi::Profiles())
            if (p.extension && std::find_if(out.begin(), out.end(), [&](const char* e) { return std::strcmp(e, p.extension) == 0; }) == out.end())
                out.push_back(p.extension);
        return out;
    }

    // After xrCreateInstance/xrGetSystem: action sets, actions and suggested bindings.
    void CreateActions(XrInstance instance, const std::set<std::string>& enabledExtensions, const std::string& runtimeName)
    {
        m_Instance = instance;
        m_Family = xi::FamilyFromRuntimeName(runtimeName.c_str());
        m_Paths[0] = Path(xi::HandPath(xi::Hand::Left));
        m_Paths[1] = Path(xi::HandPath(xi::Hand::Right));

        auto makeSet = [&](const char* name, const char* label, uint32_t priority, XrActionSet& out) {
            XrActionSetCreateInfo ci{ XR_TYPE_ACTION_SET_CREATE_INFO };
            strncpy_s(ci.actionSetName, name, _TRUNCATE);
            strncpy_s(ci.localizedActionSetName, label, _TRUNCATE);
            ci.priority = priority;
            const XrResult r = m_Xr.xrCreateActionSet(m_Instance, &ci, &out);
            if (XR_FAILED(r))
            {
                out = XR_NULL_HANDLE;
                Log("xrCreateActionSet(%s) failed: %d", name, static_cast<int>(r));
            }
        };
        makeSet("gameplay", "Gameplay", 0, m_GameplaySet);
        makeSet("controller", "Controller", 0, m_ControllerSet);
        if (m_GameplaySet == XR_NULL_HANDLE || m_ControllerSet == XR_NULL_HANDLE)
            return;

        uint32_t created = 0;
        for (size_t i = 0; i < xi::kActionCount; ++i)
        {
            const xi::ActionDef& d = xi::Actions()[i];
            if (d.kind != xi::Kind::Button && d.kind != xi::Kind::Vector2)
                continue;
            m_Actions[i] = MakeAction(m_GameplaySet, d.xrName, d.label,
                d.kind == xi::Kind::Vector2 ? XR_ACTION_TYPE_VECTOR2F_INPUT : XR_ACTION_TYPE_FLOAT_INPUT);
            if (m_Actions[i] != XR_NULL_HANDLE)
                ++created;
        }
        m_GripPose = MakeAction(m_ControllerSet, "grip_pose", "Grip Pose", XR_ACTION_TYPE_POSE_INPUT);
        m_AimPose = MakeAction(m_ControllerSet, "aim_pose", "Aim Pose", XR_ACTION_TYPE_POSE_INPUT);
        m_Haptic = MakeAction(m_ControllerSet, "haptic", "Haptic", XR_ACTION_TYPE_VIBRATION_OUTPUT);
        m_HandStick = MakeAction(m_ControllerSet, "hand_stick", "Hand Stick (menu)", XR_ACTION_TYPE_VECTOR2F_INPUT);
        m_HandStickTouch = MakeAction(m_ControllerSet, "hand_stick_touch", "Hand Stick Touch", XR_ACTION_TYPE_FLOAT_INPUT);
        m_CurlTrigger = MakeAction(m_ControllerSet, "curl_trigger", "Index Finger", XR_ACTION_TYPE_FLOAT_INPUT);
        m_CurlTriggerTouch = MakeAction(m_ControllerSet, "curl_trigger_touch", "Index Finger Touch", XR_ACTION_TYPE_FLOAT_INPUT);
        m_CurlSqueeze = MakeAction(m_ControllerSet, "curl_squeeze", "Grip Fingers", XR_ACTION_TYPE_FLOAT_INPUT);
        m_CurlThumb = MakeAction(m_ControllerSet, "curl_thumb", "Thumb Touch", XR_ACTION_TYPE_FLOAT_INPUT);

        std::string summary;
        for (const xi::Profile& p : xi::Profiles())
        {
            if (p.extension && !enabledExtensions.count(p.extension))
            {
                summary += std::string(p.key) + ":no-ext ";
                continue;
            }
            const uint32_t accepted = SuggestProfile(p);
            summary += std::string(p.key) + ":" + std::to_string(accepted) + " ";
        }
        Log("actions created: %u gameplay + 9 controller; runtime family %s; suggested bindings %s",
            created, m_Family == xi::RuntimeFamily::SteamVR ? "SteamVR" : "other", summary.c_str());
        m_Ready = true;
    }

    // After xrCreateSession: action spaces and attach (once per session).
    void SessionCreated(XrSession session)
    {
        if (!m_Ready || session == XR_NULL_HANDLE)
            return;
        m_Session = session;
        for (int h = 0; h < 2; ++h)
        {
            XrActionSpaceCreateInfo ci{ XR_TYPE_ACTION_SPACE_CREATE_INFO };
            ci.subactionPath = m_Paths[h];
            ci.poseInActionSpace.orientation.w = 1.0f;
            ci.action = m_GripPose;
            if (XR_FAILED(m_Xr.xrCreateActionSpace(m_Session, &ci, &m_GripSpace[h])))
                m_GripSpace[h] = XR_NULL_HANDLE;
            ci.action = m_AimPose;
            if (XR_FAILED(m_Xr.xrCreateActionSpace(m_Session, &ci, &m_AimSpace[h])))
                m_AimSpace[h] = XR_NULL_HANDLE;
        }
        XrActionSet sets[2] = { m_GameplaySet, m_ControllerSet };
        XrSessionActionSetsAttachInfo ai{ XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
        ai.countActionSets = 2;
        ai.actionSets = sets;
        const XrResult r = m_Xr.xrAttachSessionActionSets(m_Session, &ai);
        m_Attached = XR_SUCCEEDED(r);
        Log("action sets attached: %d (grip spaces %d/%d, aim spaces %d/%d)", static_cast<int>(r),
            m_GripSpace[0] != XR_NULL_HANDLE, m_GripSpace[1] != XR_NULL_HANDLE,
            m_AimSpace[0] != XR_NULL_HANDLE, m_AimSpace[1] != XR_NULL_HANDLE);
    }

    void NotifyProfileChanged() { m_ProfileDirty.store(true, std::memory_order_release); }
    void SetPointerHook(XrPointerHook* hook) { m_PointerHook = hook; }

    // ---- Devices ----------------------------------------------------------------
    static int HandOfDevice(vr::TrackedDeviceIndex_t index)
    {
        return index == kDeviceLeft ? 0 : index == kDeviceRight ? 1 : -1;
    }

    bool Connected(int hand) const
    {
        if (hand < 0 || hand > 1)
            return false;
        return m_ProfileIndex[hand].load(std::memory_order_acquire) >= 0 || m_PoseSeen[hand].load(std::memory_order_acquire);
    }

    const xi::Profile* RuntimeProfile(int hand) const
    {
        const int index = (hand >= 0 && hand < 2) ? m_ProfileIndex[hand].load(std::memory_order_acquire) : -1;
        return index >= 0 ? &xi::Profiles()[static_cast<size_t>(index)] : nullptr;
    }

    // The profile whose bindings and offsets apply (a fake profile wins).
    const xi::Profile* EffectiveProfile(int hand) const
    {
        const int fake = m_FakeProfileIndex.load(std::memory_order_acquire);
        if (fake >= 0)
            return &xi::Profiles()[static_cast<size_t>(fake)];
        return RuntimeProfile(hand);
    }

    std::string Describe() const
    {
        std::string out;
        for (int h = 0; h < 2; ++h)
        {
            const xi::Profile* p = RuntimeProfile(h);
            out += h == 0 ? "left=" : " right=";
            out += p ? p->key : (Connected(h) ? "unknown" : "none");
        }
        const int fake = m_FakeProfileIndex.load(std::memory_order_acquire);
        if (m_FakeEnabled.load(std::memory_order_acquire))
            out += std::string(" fake=") + (fake >= 0 ? xi::Profiles()[static_cast<size_t>(fake)].key : "runtime");
        return out;
    }

    // Raw device pose (OpenVR convention) of a hand at `time`, and the tip.
    bool LocateHand(int hand, XrSpace base, XrTime time, vr::TrackedDevicePose_t& out)
    {
        out = {};
        out.eTrackingResult = vr::TrackingResult_Uninitialized;
        if (hand < 0 || hand > 1 || base == XR_NULL_HANDLE || m_GripSpace[hand] == XR_NULL_HANDLE || time <= 0)
            return false;
        XrSpaceVelocity velocity{ XR_TYPE_SPACE_VELOCITY };
        XrSpaceLocation location{ XR_TYPE_SPACE_LOCATION };
        location.next = &velocity;
        if (XR_FAILED(m_Xr.xrLocateSpace(m_GripSpace[hand], base, time, &location)))
            return false;
        const bool valid = (location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) &&
            (location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT);
        if (!valid)
        {
            out.bDeviceIsConnected = Connected(hand);
            out.bPoseIsValid = false;
            out.eTrackingResult = out.bDeviceIsConnected ? vr::TrackingResult_Running_OutOfRange : vr::TrackingResult_Uninitialized;
            return true;
        }
        m_PoseSeen[hand].store(true, std::memory_order_release);
        const xrm::Pose grip = ToPose(location.pose);
        const xi::GripInRaw offset = GripOffset(hand);
        const xrm::Pose raw = xi::RawFromGrip(grip, offset);
        const bool haveVelocity = (velocity.velocityFlags & (XR_SPACE_VELOCITY_LINEAR_VALID_BIT | XR_SPACE_VELOCITY_ANGULAR_VALID_BIT)) != 0;
        const xrm::Vec3 angular{ velocity.angularVelocity.x, velocity.angularVelocity.y, velocity.angularVelocity.z };
        const xrm::Vec3 linear = xi::RawLinearVelocity(
            xrm::Vec3{ velocity.linearVelocity.x, velocity.linearVelocity.y, velocity.linearVelocity.z }, angular, grip, raw);
        out = xrm::ToTrackedPose(raw, location.locationFlags, linear, angular, haveVelocity);

        // Tip = the aim pose in the raw frame (constant per controller).
        if (m_AimSpace[hand] != XR_NULL_HANDLE)
        {
            XrSpaceLocation aim{ XR_TYPE_SPACE_LOCATION };
            if (XR_SUCCEEDED(m_Xr.xrLocateSpace(m_AimSpace[hand], base, time, &aim)) &&
                (aim.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) && (aim.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT))
            {
                const xrm::Pose tip = xi::Compose(xi::Inverse(raw), ToPose(aim.pose));
                std::lock_guard<std::mutex> lock(m_TipMutex);
                m_Tip[hand] = xrm::ToMatrix(tip);
                m_TipValid[hand] = true;
            }
        }
        // Self-test: turn the controller about its tip so the laser points at a target.
        l4d2vr_overlay_pointer::Vec3 aimTarget;
        if (m_PointerHook && out.bPoseIsValid && m_PointerHook->AimTarget(hand, aimTarget))
        {
            vr::HmdMatrix34_t tip{};
            if (TipLocal(hand, tip))
            {
                l4d2vr_overlay_pointer::Mat34 rawM, tipM;
                for (int r = 0; r < 3; ++r)
                    for (int c = 0; c < 4; ++c)
                    {
                        rawM.m[r][c] = out.mDeviceToAbsoluteTracking.m[r][c];
                        tipM.m[r][c] = tip.m[r][c];
                    }
                const l4d2vr_overlay_pointer::Mat34 aimed = l4d2vr_openxr_overlay::AimPoseAt(rawM, tipM, aimTarget);
                for (int r = 0; r < 3; ++r)
                    for (int c = 0; c < 4; ++c)
                        out.mDeviceToAbsoluteTracking.m[r][c] = aimed.m[r][c];
                out.vAngularVelocity = {};
            }
        }
        if (!m_LoggedPose[hand].exchange(true))
            Log("%s controller first pose: grip (%.3f %.3f %.3f) raw (%.3f %.3f %.3f) offset %s (%s)", hand == 0 ? "left" : "right",
                grip.position.x, grip.position.y, grip.position.z, raw.position.x, raw.position.y, raw.position.z,
                offset.identity() ? "identity" : "applied", EffectiveProfile(hand) ? EffectiveProfile(hand)->gripNote : "no profile yet");
        return true;
    }

    bool TipLocal(int hand, vr::HmdMatrix34_t& out)
    {
        if (hand < 0 || hand > 1)
            return false;
        std::lock_guard<std::mutex> lock(m_TipMutex);
        if (!m_TipValid[hand])
            return false;
        out = m_Tip[hand];
        return true;
    }

    // ---- Haptics ----------------------------------------------------------------
    bool Vibrate(int hand, float seconds, float frequency, float amplitude)
    {
        if (hand < 0 || hand > 1 || !m_Attached || m_Haptic == XR_NULL_HANDLE || m_Session == XR_NULL_HANDLE)
            return false;
        XrHapticVibration vibration{ XR_TYPE_HAPTIC_VIBRATION };
        vibration.duration = seconds > 0.0f ? static_cast<XrDuration>(static_cast<double>(seconds) * 1e9) : XR_MIN_HAPTIC_DURATION;
        vibration.frequency = frequency > 0.0f ? frequency : XR_FREQUENCY_UNSPECIFIED;
        vibration.amplitude = std::clamp(amplitude, 0.0f, 1.0f);
        XrHapticActionInfo info{ XR_TYPE_HAPTIC_ACTION_INFO };
        info.action = m_Haptic;
        info.subactionPath = m_Paths[hand];
        const XrResult r = m_Xr.xrApplyHapticFeedback(m_Session, &info, reinterpret_cast<const XrHapticBaseHeader*>(&vibration));
        ++m_HapticCalls;
        if (XR_FAILED(r) && m_HapticErrors++ < 5)
            Log("xrApplyHapticFeedback(%s) failed: %d", hand == 0 ? "left" : "right", static_cast<int>(r));
        if (XR_SUCCEEDED(r) && !m_LoggedHaptic.exchange(true))
            Log("first haptic pulse: %s hand %.3f s %.0f Hz amplitude %.2f -> %d", hand == 0 ? "left" : "right",
                seconds, frequency, vibration.amplitude, static_cast<int>(r));
        return true;
    }

    // ---- Fake action source -----------------------------------------------------
    bool SetFake(const char* spec, std::string& error)
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        if (!spec || !m_Fake.Apply(spec, error))
        {
            if (!spec) error = "no spec";
            return false;
        }
        m_FakeEnabled.store(m_Fake.enabled, std::memory_order_release);
        const int index = m_Fake.profile ? static_cast<int>(m_Fake.profile - &xi::Profiles()[0]) : -1;
        m_FakeProfileIndex.store(m_Fake.enabled ? index : -1, std::memory_order_release);
        Log("fake input: %s (fake %s, profile %s, %zu values)", spec, m_Fake.enabled ? "on" : "off",
            m_Fake.profile ? m_Fake.profile->key : "runtime", m_Fake.values.size());
        return true;
    }

    // ---- IVrInput ---------------------------------------------------------------
    vr::EVRInputError SetActionManifestPath(const char* path) override
    {
        // The action table is compiled in; the SteamVR manifest is OpenVR-only.
        if (!m_LoggedManifest.exchange(true))
            Log("action manifest %s ignored (OpenXR actions are built in)", path ? path : "(null)");
        return vr::VRInputError_None;
    }

    vr::EVRInputError GetActionSetHandle(const char* name, vr::VRActionSetHandle_t* handle) override
    {
        if (!name || !handle) return vr::VRInputError_InvalidParam;
        if (xi::EqualsNoCase(name, "/actions/main")) *handle = kSetMain;
        else if (xi::EqualsNoCase(name, "/actions/base")) *handle = kSetBase;
        else *handle = 0x2100 + Intern(std::string("set:") + name);
        return vr::VRInputError_None;
    }

    vr::EVRInputError GetActionHandle(const char* name, vr::VRActionHandle_t* handle) override
    {
        if (!name || !handle) return vr::VRInputError_InvalidParam;
        const xi::Action a = xi::FindAction(name);
        if (a != xi::Action::Count)
            *handle = kActionBase + static_cast<vr::VRActionHandle_t>(a);
        else
        {
            // OpenVR hands out handles for names not in the manifest too.
            *handle = kUnknownActionBase + Intern(std::string("action:") + name);
            bool first = false;
            {
                std::lock_guard<std::mutex> lock(m_InternMutex);
                first = m_LoggedUnknown.insert(name).second;
            }
            if (first)
                Log("action %s is not in the OpenXR table (always inactive)", name);
        }
        return vr::VRInputError_None;
    }

    vr::EVRInputError GetInputSourceHandle(const char* path, vr::VRInputValueHandle_t* handle) override
    {
        if (!path || !handle) return vr::VRInputError_InvalidParam;
        if (xi::EqualsNoCase(path, "/user/hand/left")) *handle = kSourceLeft;
        else if (xi::EqualsNoCase(path, "/user/hand/right")) *handle = kSourceRight;
        else *handle = 0x3100 + Intern(std::string("source:") + path);
        return vr::VRInputError_None;
    }

    vr::EVRInputError UpdateActionState(vr::VRActiveActionSet_t*, uint32_t, uint32_t) override
    {
        Sync();
        return vr::VRInputError_None;
    }

    vr::EVRInputError GetDigitalActionData(vr::VRActionHandle_t action, vr::InputDigitalActionData_t* data, uint32_t size,
        vr::VRInputValueHandle_t restrict) override
    {
        if (!data || size < sizeof(*data)) return vr::VRInputError_WrongType;
        *data = {};
        const int index = ActionIndex(action);
        if (index < 0)
            return action >= kUnknownActionBase ? vr::VRInputError_None : vr::VRInputError_InvalidHandle;
        if (xi::Def(static_cast<xi::Action>(index)).kind == xi::Kind::Vector2)
            return vr::VRInputError_WrongType;
        const int only = RestrictHand(restrict);
        if (only == -2)
            return vr::VRInputError_InvalidDevice;
        std::lock_guard<std::mutex> lock(m_Mutex);
        const ActionCache& c = m_Cache[static_cast<size_t>(index)];
        if (only >= 0)
        {
            const HandState& h = c.hand[only];
            data->bActive = h.active;
            data->bState = h.active && h.down;
            data->bChanged = h.active && h.changed;
            data->activeOrigin = h.active ? Origin(only) : vr::k_ulInvalidInputValueHandle;
            data->fUpdateTime = UpdateTime(h);
            return vr::VRInputError_None;
        }
        data->bActive = c.hand[0].active || c.hand[1].active;
        data->bState = c.aggDown;
        data->bChanged = c.aggChanged;
        const int origin = c.originHand;
        data->activeOrigin = (data->bActive && origin >= 0) ? Origin(origin) : vr::k_ulInvalidInputValueHandle;
        data->fUpdateTime = origin >= 0 ? UpdateTime(c.hand[origin]) : 0.0f;
        return vr::VRInputError_None;
    }

    vr::EVRInputError GetAnalogActionData(vr::VRActionHandle_t action, vr::InputAnalogActionData_t* data, uint32_t size,
        vr::VRInputValueHandle_t restrict) override
    {
        if (!data || size < sizeof(*data)) return vr::VRInputError_WrongType;
        *data = {};
        const int index = ActionIndex(action);
        if (index < 0)
            return action >= kUnknownActionBase ? vr::VRInputError_None : vr::VRInputError_InvalidHandle;
        const int only = RestrictHand(restrict);
        if (only == -2)
            return vr::VRInputError_InvalidDevice;
        std::lock_guard<std::mutex> lock(m_Mutex);
        const ActionCache& c = m_Cache[static_cast<size_t>(index)];
        int hand = only;
        if (hand < 0)
        {
            // Unrestricted: the hand with the larger deflection (or the active one).
            const float m0 = c.hand[0].active ? c.hand[0].x * c.hand[0].x + c.hand[0].y * c.hand[0].y : -1.0f;
            const float m1 = c.hand[1].active ? c.hand[1].x * c.hand[1].x + c.hand[1].y * c.hand[1].y : -1.0f;
            hand = (m1 > m0) ? 1 : 0;
        }
        const HandState& h = c.hand[hand];
        data->bActive = h.active;
        if (!h.active)
            return vr::VRInputError_None;
        data->activeOrigin = Origin(hand);
        data->x = h.x;
        data->y = h.y;
        data->deltaX = h.x - h.prevX;
        data->deltaY = h.y - h.prevY;
        data->fUpdateTime = UpdateTime(h);
        return vr::VRInputError_None;
    }

    vr::EVRInputError GetSkeletalActionData(vr::VRActionHandle_t action, vr::InputSkeletalActionData_t* data, uint32_t size) override
    {
        if (!data || size < sizeof(*data)) return vr::VRInputError_WrongType;
        *data = {};
        const int hand = SkeletonHand(action);
        if (hand < 0)
            return vr::VRInputError_WrongType;
        std::lock_guard<std::mutex> lock(m_Mutex);
        data->bActive = m_Internal[hand].curlActive;
        data->activeOrigin = data->bActive ? Origin(hand) : vr::k_ulInvalidInputValueHandle;
        return vr::VRInputError_None;
    }

    // Bones need XR_EXT_hand_tracking (M5); the capability is reported off.
    vr::EVRInputError GetBoneCount(vr::VRActionHandle_t, uint32_t* count) override
    {
        if (count) *count = 0;
        return vr::VRInputError_NoData;
    }
    vr::EVRInputError GetBoneHierarchy(vr::VRActionHandle_t, vr::BoneIndex_t*, uint32_t) override { return vr::VRInputError_NoData; }
    vr::EVRInputError GetBoneName(vr::VRActionHandle_t, vr::BoneIndex_t, char* name, uint32_t size) override
    {
        if (name && size) name[0] = '\0';
        return vr::VRInputError_NoData;
    }
    vr::EVRInputError GetSkeletalReferenceTransforms(vr::VRActionHandle_t, vr::EVRSkeletalTransformSpace, vr::EVRSkeletalReferencePose,
        vr::VRBoneTransform_t*, uint32_t) override { return vr::VRInputError_NoData; }
    vr::EVRInputError GetSkeletalBoneData(vr::VRActionHandle_t, vr::EVRSkeletalTransformSpace, vr::EVRSkeletalMotionRange,
        vr::VRBoneTransform_t*, uint32_t) override { return vr::VRInputError_NoData; }

    vr::EVRInputError GetSkeletalSummaryData(vr::VRActionHandle_t action, vr::EVRSummaryType, vr::VRSkeletalSummaryData_t* summary) override
    {
        if (!summary) return vr::VRInputError_InvalidParam;
        *summary = {};
        const int hand = SkeletonHand(action);
        if (hand < 0)
            return vr::VRInputError_WrongType;
        std::lock_guard<std::mutex> lock(m_Mutex);
        const InternalState& s = m_Internal[hand];
        if (!s.curlActive)
            return vr::VRInputError_NoData;
        xi::CurlInputs in;
        in.trigger = s.curlTrigger;
        in.triggerTouched = s.curlTriggerTouch > 0.5f;
        in.squeeze = s.curlSqueeze;
        in.thumbTouched = s.curlThumb > 0.5f;
        float curls[5];
        xi::CurlsFromInputs(in, curls);
        for (int f = 0; f < 5; ++f)
            summary->flFingerCurl[f] = curls[f];
        for (int f = 0; f < 4; ++f)
            summary->flFingerSplay[f] = 0.2f;
        return vr::VRInputError_None;
    }

    vr::EVRInputError GetOriginTrackedDeviceInfo(vr::VRInputValueHandle_t origin, vr::InputOriginInfo_t* info, uint32_t size) override
    {
        if (!info || size < sizeof(*info)) return vr::VRInputError_InvalidParam;
        *info = {};
        info->trackedDeviceIndex = vr::k_unTrackedDeviceIndexInvalid;
        int hand = -1;
        if (origin == kOriginLeft || origin == kSourceLeft) hand = 0;
        else if (origin == kOriginRight || origin == kSourceRight) hand = 1;
        if (hand < 0)
            return vr::VRInputError_InvalidHandle;
        info->devicePath = hand == 0 ? kSourceLeft : kSourceRight;
        info->trackedDeviceIndex = hand == 0 ? kDeviceLeft : kDeviceRight;
        return vr::VRInputError_None;
    }

    vr::EVRInputError GetActionBindingInfo(vr::VRActionHandle_t action, vr::InputBindingInfo_t* out, uint32_t size, uint32_t count,
        uint32_t* returned) override
    {
        if (returned) *returned = 0;
        const int index = ActionIndex(action);
        if (index < 0)
            return vr::VRInputError_None;
        if (out && size < sizeof(vr::InputBindingInfo_t))
            return vr::VRInputError_BufferTooSmall;
        uint32_t n = 0;
        bool anyProfile = false;
        for (int h = 0; h < 2; ++h)
        {
            const xi::Profile* p = EffectiveProfile(h);
            if (!p)
                continue;
            anyProfile = true;
            for (const xi::Binding& b : p->bindings)
            {
                if (static_cast<int>(b.action) != index || static_cast<int>(b.hand) != h)
                    continue;
                if (out && n < count)
                {
                    const xi::BindingInfoText t = xi::DescribeBinding(b);
                    vr::InputBindingInfo_t& info = out[n];
                    info = {};
                    strncpy_s(info.rchDevicePathName, t.device.c_str(), _TRUNCATE);
                    strncpy_s(info.rchInputPathName, t.input.c_str(), _TRUNCATE);
                    strncpy_s(info.rchModeName, t.mode.c_str(), _TRUNCATE);
                    strncpy_s(info.rchSlotName, t.slot.c_str(), _TRUNCATE);
                    strncpy_s(info.rchInputSourceType, t.sourceType.c_str(), _TRUNCATE);
                }
                ++n;
            }
        }
        if (returned) *returned = (std::min)(n, count);
        return anyProfile ? vr::VRInputError_None : vr::VRInputError_NoData;
    }

    // ---- Stats --------------------------------------------------------------------
    std::string StatsLine()
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        char buffer[256];
        snprintf(buffer, sizeof(buffer), "input %s syncs=%llu syncErrors=%llu lastSync=%d down=%u haptics=%llu",
            Describe().c_str(), static_cast<unsigned long long>(m_Syncs), static_cast<unsigned long long>(m_SyncErrors),
            static_cast<int>(m_LastSync), CountDown(), static_cast<unsigned long long>(m_HapticCalls));
        return buffer;
    }

private:
    struct HandState
    {
        bool active = false;
        float value = 0.0f;        // Button: raw float before the threshold
        float x = 0.0f, y = 0.0f;  // Vector2: after the deadzone; Button: x = value
        float prevX = 0.0f, prevY = 0.0f;
        bool down = false;
        bool changed = false;
        bool buttonDown = false;   // threshold half of the state
        bool dpadDown = false;     // derived menu dpad half of the state
        XrTime lastChange = 0;
    };
    struct ActionCache
    {
        HandState hand[2];
        bool aggDown = false;
        bool aggChanged = false;
        int originHand = -1;
    };
    struct InternalState
    {
        bool stickActive = false;
        xi::Stick2 stick;
        float stickTouch = 0.0f;
        bool curlActive = false;
        float curlTrigger = 0.0f, curlTriggerTouch = 0.0f, curlSqueeze = 0.0f, curlThumb = 0.0f;
    };
    struct RawState { bool active = false; float x = 0.0f, y = 0.0f; XrTime changed = 0; };

    template<class... Args>
    static void Log(const char* fmt, Args... args)
    {
        std::string format = std::string("[VR][OpenXR][Input] ") + fmt;
        Game::logMsg(format.c_str(), args...);
    }

    XrPath Path(const std::string& text) const
    {
        XrPath path = XR_NULL_PATH;
        if (XR_FAILED(m_Xr.xrStringToPath(m_Instance, text.c_str(), &path)))
            return XR_NULL_PATH;
        return path;
    }

    std::string PathText(XrPath path) const
    {
        if (path == XR_NULL_PATH)
            return std::string();
        char buffer[XR_MAX_PATH_LENGTH] = {};
        uint32_t n = 0;
        if (XR_FAILED(m_Xr.xrPathToString(m_Instance, path, sizeof(buffer), &n, buffer)))
            return std::string();
        return buffer;
    }

    XrAction MakeAction(XrActionSet set, const char* name, const char* label, XrActionType type)
    {
        XrActionCreateInfo ci{ XR_TYPE_ACTION_CREATE_INFO };
        strncpy_s(ci.actionName, name, _TRUNCATE);
        strncpy_s(ci.localizedActionName, label, _TRUNCATE);
        ci.actionType = type;
        ci.countSubactionPaths = 2;
        ci.subactionPaths = m_Paths;
        XrAction action = XR_NULL_HANDLE;
        const XrResult r = m_Xr.xrCreateAction(set, &ci, &action);
        if (XR_FAILED(r))
        {
            Log("xrCreateAction(%s) failed: %d", name, static_cast<int>(r));
            return XR_NULL_HANDLE;
        }
        return action;
    }

    uint32_t SuggestProfile(const xi::Profile& p)
    {
        const XrPath profilePath = Path(p.path);
        if (profilePath == XR_NULL_PATH)
            return 0;
        struct Entry { XrAction action; std::string path; };
        std::vector<Entry> entries;
        auto add = [&](XrAction action, int hand, const char* input) {
            if (action == XR_NULL_HANDLE || !input || !*input)
                return;
            const std::string full = std::string(xi::HandPath(static_cast<xi::Hand>(hand))) + input;
            for (const Entry& e : entries)
                if (e.action == action && e.path == full)
                    return;
            entries.push_back({ action, full });
        };
        for (const xi::Binding& b : p.bindings)
            if (b.mode != xi::Mode::Dpad)
                add(m_Actions[static_cast<size_t>(b.action)], static_cast<int>(b.hand), b.input);
        for (int h = 0; h < 2; ++h)
        {
            add(m_GripPose, h, "/input/grip/pose");
            add(m_AimPose, h, "/input/aim/pose");
            add(m_Haptic, h, "/output/haptic");
            add(m_HandStick, h, p.mainAxis);
            add(m_HandStickTouch, h, p.mainAxisTouch);
            add(m_CurlTrigger, h, p.triggerCurl);
            add(m_CurlSqueeze, h, p.squeezeCurl);
            for (const std::string& t : p.thumbTouch[h])
                add(m_CurlThumb, h, t.c_str());
            const std::string triggerTouch = "/input/trigger/touch";
            for (const std::string& legal : p.legal[h])
                if (legal == triggerTouch)
                    add(m_CurlTriggerTouch, h, triggerTouch.c_str());
        }

        auto suggest = [&](const std::vector<Entry>& list) {
            std::vector<XrActionSuggestedBinding> bindings;
            for (const Entry& e : list)
            {
                const XrPath path = Path(e.path);
                if (path != XR_NULL_PATH)
                    bindings.push_back({ e.action, path });
            }
            XrInteractionProfileSuggestedBinding s{ XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
            s.interactionProfile = profilePath;
            s.countSuggestedBindings = static_cast<uint32_t>(bindings.size());
            s.suggestedBindings = bindings.data();
            return bindings.empty() ? XR_ERROR_VALIDATION_FAILURE : m_Xr.xrSuggestInteractionProfileBindings(m_Instance, &s);
        };
        const XrResult first = suggest(entries);
        if (XR_SUCCEEDED(first))
            return static_cast<uint32_t>(entries.size());
        // Find the paths this runtime rejects, then suggest the rest.
        std::vector<Entry> kept;
        std::string rejected;
        for (const Entry& e : entries)
        {
            if (XR_SUCCEEDED(suggest({ e })))
                kept.push_back(e);
            else
                rejected += e.path + " ";
        }
        const XrResult r = kept.empty() ? XR_ERROR_PATH_UNSUPPORTED : suggest(kept);
        Log("profile %s: suggestion rejected (%d); retried without: %s-> %d", p.key, static_cast<int>(first), rejected.c_str(), static_cast<int>(r));
        return XR_SUCCEEDED(r) ? static_cast<uint32_t>(kept.size()) : 0;
    }

    // Whether the action is bound to this hand's index trigger (the laser's click).
    static bool TriggerBound(const xi::Profile* p, xi::Action action, int hand)
    {
        if (!p)
            return false;
        for (const xi::Binding& b : p->bindings)
        {
            if (b.action != action || static_cast<int>(b.hand) != hand || !b.input)
                continue;
            if (std::strncmp(b.input, "/input/trigger", 14) == 0 || (p->triggerCurl && std::strcmp(b.input, p->triggerCurl) == 0))
                return true;
        }
        return false;
    }

    xi::GripInRaw GripOffset(int hand) const
    {
        // Fake profiles only change bindings; poses keep the runtime's profile.
        const xi::Profile* p = RuntimeProfile(hand);
        if (!p)
            return xi::GripInRaw{};
        return p->grip[static_cast<int>(m_Family)][hand];
    }

    static int ActionIndex(vr::VRActionHandle_t handle)
    {
        if (handle < kActionBase || handle >= kActionBase + xi::kActionCount)
            return -1;
        const int index = static_cast<int>(handle - kActionBase);
        const xi::Kind kind = xi::Actions()[static_cast<size_t>(index)].kind;
        return (kind == xi::Kind::Button || kind == xi::Kind::Vector2) ? index : -1;
    }

    static int SkeletonHand(vr::VRActionHandle_t handle)
    {
        if (handle == kActionBase + static_cast<vr::VRActionHandle_t>(xi::Action::SkeletonLeft)) return 0;
        if (handle == kActionBase + static_cast<vr::VRActionHandle_t>(xi::Action::SkeletonRight)) return 1;
        return -1;
    }

    // -1 = no restriction, 0/1 = that hand, -2 = a device that is not a hand.
    static int RestrictHand(vr::VRInputValueHandle_t restrict)
    {
        if (restrict == vr::k_ulInvalidInputValueHandle) return -1;
        if (restrict == kSourceLeft || restrict == kOriginLeft) return 0;
        if (restrict == kSourceRight || restrict == kOriginRight) return 1;
        return -2;
    }

    static vr::VRInputValueHandle_t Origin(int hand) { return hand == 0 ? kOriginLeft : kOriginRight; }

    float UpdateTime(const HandState& h) const
    {
        // OpenVR: seconds relative to now (negative = in the past).
        if (h.lastChange <= 0 || m_LastSyncTime <= 0)
            return 0.0f;
        const double seconds = static_cast<double>(h.lastChange - m_LastSyncTime) / 1e9;
        return static_cast<float>(std::clamp(seconds, -3600.0, 0.0));
    }

    uint64_t Intern(const std::string& key)
    {
        std::lock_guard<std::mutex> lock(m_InternMutex);
        auto it = m_Interned.find(key);
        if (it != m_Interned.end())
            return it->second;
        const uint64_t value = m_Interned.size() + 1u;
        m_Interned.emplace(key, value);
        return value;
    }

    uint32_t CountDown() const
    {
        uint32_t n = 0;
        for (const ActionCache& c : m_Cache)
            n += c.aggDown ? 1u : 0u;
        return n;
    }

    // ---- Sync -----------------------------------------------------------------
    RawState ReadFloat(XrAction action, int hand)
    {
        RawState s;
        if (action == XR_NULL_HANDLE)
            return s;
        XrActionStateGetInfo gi{ XR_TYPE_ACTION_STATE_GET_INFO };
        gi.action = action;
        gi.subactionPath = m_Paths[hand];
        XrActionStateFloat st{ XR_TYPE_ACTION_STATE_FLOAT };
        if (XR_FAILED(m_Xr.xrGetActionStateFloat(m_Session, &gi, &st)))
            return s;
        s.active = st.isActive == XR_TRUE;
        s.x = st.currentState;
        s.changed = st.lastChangeTime;
        return s;
    }

    RawState ReadVector(XrAction action, int hand)
    {
        RawState s;
        if (action == XR_NULL_HANDLE)
            return s;
        XrActionStateGetInfo gi{ XR_TYPE_ACTION_STATE_GET_INFO };
        gi.action = action;
        gi.subactionPath = m_Paths[hand];
        XrActionStateVector2f st{ XR_TYPE_ACTION_STATE_VECTOR2F };
        if (XR_FAILED(m_Xr.xrGetActionStateVector2f(m_Session, &gi, &st)))
            return s;
        s.active = st.isActive == XR_TRUE;
        s.x = st.currentState.x;
        s.y = st.currentState.y;
        s.changed = st.lastChangeTime;
        return s;
    }

    // Fake source: the value of the profile's inputs for (action, hand).
    RawState FakeAction(const xi::Profile* p, xi::Action action, int hand) const
    {
        RawState s;
        if (!p)
            return s;
        const std::string prefix = xi::HandPath(static_cast<xi::Hand>(hand));
        for (const xi::Binding& b : p->bindings)
        {
            if (b.action != action || static_cast<int>(b.hand) != hand || b.mode == xi::Mode::Dpad)
                continue;
            const xi::Stick2 v = m_Fake.Get(prefix + b.input);
            s.active = true;
            if (b.mode == xi::Mode::Stick)
            {
                s.x = v.x;
                s.y = v.y;
            }
            else
                s.x = (std::max)(s.x, v.x);
        }
        return s;
    }

    float FakeMax(int hand, const std::vector<std::string>& inputs) const
    {
        float v = 0.0f;
        const std::string prefix = xi::HandPath(static_cast<xi::Hand>(hand));
        for (const std::string& input : inputs)
            v = (std::max)(v, m_Fake.Get(prefix + input).x);
        return v;
    }

    void RefreshProfiles()
    {
        if (!m_ProfileDirty.exchange(false, std::memory_order_acq_rel))
            return;
        for (int h = 0; h < 2; ++h)
        {
            XrInteractionProfileState state{ XR_TYPE_INTERACTION_PROFILE_STATE };
            const XrResult r = m_Xr.xrGetCurrentInteractionProfile(m_Session, m_Paths[h], &state);
            const std::string text = XR_SUCCEEDED(r) ? PathText(state.interactionProfile) : std::string();
            const xi::Profile* p = text.empty() ? nullptr : xi::FindProfileByPath(text.c_str());
            m_ProfileIndex[h].store(p ? static_cast<int>(p - &xi::Profiles()[0]) : -1, std::memory_order_release);
            Log("%s hand interaction profile: %s (%s)%s", h == 0 ? "left" : "right", text.empty() ? "none" : text.c_str(),
                p ? p->key : "not in the table", p ? (std::string(", grip offset: ") + p->gripNote).c_str() : "");
            if (!text.empty() && !p)
                m_PoseSeen[h].store(true, std::memory_order_release);  // a controller we have no table for still has poses
        }
        LogBoundSources();
    }

    void LogBoundSources()
    {
        std::string out;
        int listed = 0;
        for (size_t i = 0; i < xi::kActionCount && listed < 24; ++i)
        {
            if (m_Actions[i] == XR_NULL_HANDLE)
                continue;
            XrBoundSourcesForActionEnumerateInfo ei{ XR_TYPE_BOUND_SOURCES_FOR_ACTION_ENUMERATE_INFO };
            ei.action = m_Actions[i];
            uint32_t n = 0;
            if (XR_FAILED(m_Xr.xrEnumerateBoundSourcesForAction(m_Session, &ei, 0, &n, nullptr)) || n == 0)
                continue;
            std::vector<XrPath> sources(n);
            if (XR_FAILED(m_Xr.xrEnumerateBoundSourcesForAction(m_Session, &ei, n, &n, sources.data())))
                continue;
            out += std::string(xi::Actions()[i].xrName) + "=";
            for (uint32_t s = 0; s < n; ++s)
                out += (s ? "|" : "") + PathText(sources[s]);
            out += " ";
            ++listed;
        }
        Log("bound sources: %s", out.empty() ? "(none reported)" : out.c_str());
    }

    void Sync()
    {
        if (!m_Attached || m_Session == XR_NULL_HANDLE)
            return;
        XrActiveActionSet active[2] = { { m_GameplaySet, XR_NULL_PATH }, { m_ControllerSet, XR_NULL_PATH } };
        XrActionsSyncInfo si{ XR_TYPE_ACTIONS_SYNC_INFO };
        si.countActiveActionSets = 2;
        si.activeActionSets = active;
        const XrResult r = m_Xr.xrSyncActions(m_Session, &si);
        XrTime now = 0;
        {
            LARGE_INTEGER qpc{};
            QueryPerformanceCounter(&qpc);
            if (m_Xr.xrConvertWin32PerformanceCounterToTimeKHR)
                m_Xr.xrConvertWin32PerformanceCounterToTimeKHR(m_Instance, &qpc, &now);
        }
        if (r != m_LastSync)
        {
            Log("xrSyncActions -> %d", static_cast<int>(r));
            m_LastSync = r;
        }
        const bool synced = XR_SUCCEEDED(r);
        if (!synced)
            ++m_SyncErrors;
        ++m_Syncs;
        if (synced)
            RefreshProfiles();

        // Read every action per hand outside the cache lock (xr calls), then
        // apply thresholds under it.
        std::array<std::array<RawState, 2>, xi::kActionCount> raw{};
        InternalState internal[2];
        const bool fake = m_FakeEnabled.load(std::memory_order_acquire);
        if (synced && !fake)
        {
            for (size_t i = 0; i < xi::kActionCount; ++i)
            {
                if (m_Actions[i] == XR_NULL_HANDLE)
                    continue;
                const bool vector = xi::Actions()[i].kind == xi::Kind::Vector2;
                for (int h = 0; h < 2; ++h)
                    raw[i][h] = vector ? ReadVector(m_Actions[i], h) : ReadFloat(m_Actions[i], h);
            }
            for (int h = 0; h < 2; ++h)
            {
                const RawState stick = ReadVector(m_HandStick, h);
                internal[h].stickActive = stick.active;
                internal[h].stick = { stick.x, stick.y };
                internal[h].stickTouch = ReadFloat(m_HandStickTouch, h).x;
                const RawState trigger = ReadFloat(m_CurlTrigger, h);
                const RawState squeeze = ReadFloat(m_CurlSqueeze, h);
                internal[h].curlActive = trigger.active || squeeze.active;
                internal[h].curlTrigger = trigger.x;
                internal[h].curlSqueeze = squeeze.x;
                internal[h].curlTriggerTouch = ReadFloat(m_CurlTriggerTouch, h).x;
                internal[h].curlThumb = ReadFloat(m_CurlThumb, h).x;
            }
        }

        std::lock_guard<std::mutex> lock(m_Mutex);
        m_LastSyncTime = now;
        if (fake && synced)
        {
            for (int h = 0; h < 2; ++h)
            {
                const xi::Profile* p = EffectiveProfile(h);
                if (!p)
                    continue;
                for (size_t i = 0; i < xi::kActionCount; ++i)
                    if (m_Actions[i] != XR_NULL_HANDLE)
                        raw[i][h] = FakeAction(p, static_cast<xi::Action>(i), h);
                const std::string prefix = xi::HandPath(static_cast<xi::Hand>(h));
                if (p->mainAxis)
                {
                    const xi::Stick2 s = m_Fake.Get(prefix + p->mainAxis);
                    internal[h].stickActive = true;
                    internal[h].stick = s;
                    // A stick moved in a test counts as touched unless the test sets the touch.
                    const bool touchSet = p->mainAxisTouch && m_Fake.values.count(prefix + p->mainAxisTouch);
                    internal[h].stickTouch = touchSet ? m_Fake.Get(prefix + p->mainAxisTouch).x
                        : ((s.x != 0.0f || s.y != 0.0f) ? 1.0f : 0.0f);
                }
                internal[h].curlActive = true;
                internal[h].curlTrigger = p->triggerCurl ? m_Fake.Get(prefix + p->triggerCurl).x : 0.0f;
                internal[h].curlSqueeze = p->squeezeCurl ? m_Fake.Get(prefix + p->squeezeCurl).x : 0.0f;
                internal[h].curlTriggerTouch = m_Fake.Get(prefix + "/input/trigger/touch").x;
                internal[h].curlThumb = FakeMax(h, p->thumbTouch[h]);
            }
        }
        for (int h = 0; h < 2; ++h)
            m_Internal[h] = internal[h];

        // Overlay laser (M4): the trigger of a hand pointing at an interactive
        // overlay clicks the overlay and is kept from the game's actions.
        bool engaged[2] = { false, false };
        if (m_PointerHook && synced)
        {
            XrPointerInputs pi;
            for (int h = 0; h < 2; ++h)
            {
                pi.active[h] = Connected(h);
                pi.trigger[h] = internal[h].curlActive ? internal[h].curlTrigger : 0.0f;
                pi.stickY[h] = internal[h].stickActive ? internal[h].stick.y : 0.0f;
            }
            m_PointerHook->PointerStep(now, pi, engaged);
            for (int h = 0; h < 2; ++h)
                if (engaged[h] != m_PointerEngaged[h])
                {
                    m_PointerEngaged[h] = engaged[h];
                    Log("%s trigger %s the overlay laser", h == 0 ? "left" : "right", engaged[h] ? "taken by" : "released from");
                }
        }

        for (size_t i = 0; i < xi::kActionCount; ++i)
        {
            if (m_Actions[i] == XR_NULL_HANDLE)
                continue;
            const xi::Action action = static_cast<xi::Action>(i);
            const bool vector = xi::Actions()[i].kind == xi::Kind::Vector2;
            ActionCache& c = m_Cache[i];
            for (int h = 0; h < 2; ++h)
            {
                HandState& s = c.hand[h];
                const xi::Profile* p = EffectiveProfile(h);
                RawState in = raw[i][h];
                if (engaged[h] && !vector && TriggerBound(p, action, h))
                    in.x = 0.0f;
                s.prevX = s.x;
                s.prevY = s.y;
                const bool wasDown = s.down;
                if (vector)
                {
                    s.active = in.active;
                    xi::Stick2 stick{ in.x, in.y };
                    // A trackpad used as a stick (Vive wand) only steers while touched.
                    if (p && p->mainAxis && p->mainAxisTouch && std::strcmp(p->mainAxis, "/input/trackpad") == 0 &&
                        internal[h].stickActive && internal[h].stickTouch < 0.5f)
                        stick = {};
                    const xi::Stick2 dz = xi::RadialDeadzone(stick, xi::kStickDeadzone);
                    s.x = in.active ? dz.x : 0.0f;
                    s.y = in.active ? dz.y : 0.0f;
                    s.changed = (s.x != s.prevX || s.y != s.prevY);
                    if (s.changed)
                        s.lastChange = in.changed > 0 ? in.changed : now;
                    continue;
                }
                // Button: the binding's threshold for this hand, and the menu
                // dpad derived from the hand's stick.
                float press = xi::kValueClickPress, release = xi::kValueClickRelease;
                const xi::Binding* dpad = nullptr;
                bool haveButton = false;
                if (p)
                {
                    for (const xi::Binding& b : p->bindings)
                    {
                        if (b.action != action || static_cast<int>(b.hand) != h)
                            continue;
                        if (b.mode == xi::Mode::Dpad)
                            dpad = &b;
                        else if (!haveButton)
                        {
                            xi::Thresholds(b, press, release);
                            haveButton = true;
                        }
                    }
                }
                const bool buttonDown = in.active && xi::NextClick(s.buttonDown, in.x, press, release);
                bool dpadDown = false;
                if (dpad && internal[h].stickActive)
                {
                    const bool touched = p->mainAxisTouch ? internal[h].stickTouch > 0.5f : true;
                    dpadDown = xi::NextDpad(s.dpadDown, dpad->dir, internal[h].stick, touched);
                }
                s.active = in.active || (dpad && internal[h].stickActive);
                s.value = in.x;
                s.x = in.x;
                s.y = 0.0f;
                s.buttonDown = buttonDown;
                s.dpadDown = dpadDown;
                s.down = s.active && (buttonDown || dpadDown);
                s.changed = s.down != wasDown;
                if (s.changed)
                    s.lastChange = now;
            }
            if (vector)
                continue;
            const bool wasAgg = c.aggDown;
            c.aggDown = c.hand[0].down || c.hand[1].down;
            c.aggChanged = c.aggDown != wasAgg;
            // Origin: the pressed hand (the latest press), else an active hand.
            int origin = -1;
            if (c.hand[0].down && c.hand[1].down)
                origin = c.hand[1].lastChange >= c.hand[0].lastChange ? 1 : 0;
            else if (c.hand[0].down || c.hand[1].down)
                origin = c.hand[1].down ? 1 : 0;
            else if (c.hand[0].active || c.hand[1].active)
                origin = c.hand[1].active ? 1 : 0;
            c.originHand = origin;
            for (int h = 0; h < 2; ++h)
            {
                if (c.hand[h].changed && m_LoggedChanges < 600)
                {
                    ++m_LoggedChanges;
                    Log("%s %s %s (value %.2f%s)", xi::Actions()[i].xrName, h == 0 ? "left" : "right", c.hand[h].down ? "down" : "up",
                        c.hand[h].value, c.hand[h].dpadDown ? ", dpad" : "");
                }
            }
        }
    }

    // ---- State ------------------------------------------------------------------
    const XrApi& m_Xr;
    XrInstance m_Instance = XR_NULL_HANDLE;
    XrSession m_Session = XR_NULL_HANDLE;
    xi::RuntimeFamily m_Family = xi::RuntimeFamily::Other;
    XrPath m_Paths[2] = { XR_NULL_PATH, XR_NULL_PATH };
    XrActionSet m_GameplaySet = XR_NULL_HANDLE;
    XrActionSet m_ControllerSet = XR_NULL_HANDLE;
    std::array<XrAction, xi::kActionCount> m_Actions{};
    XrAction m_GripPose = XR_NULL_HANDLE, m_AimPose = XR_NULL_HANDLE, m_Haptic = XR_NULL_HANDLE;
    XrAction m_HandStick = XR_NULL_HANDLE, m_HandStickTouch = XR_NULL_HANDLE;
    XrAction m_CurlTrigger = XR_NULL_HANDLE, m_CurlTriggerTouch = XR_NULL_HANDLE, m_CurlSqueeze = XR_NULL_HANDLE, m_CurlThumb = XR_NULL_HANDLE;
    XrSpace m_GripSpace[2] = { XR_NULL_HANDLE, XR_NULL_HANDLE };
    XrSpace m_AimSpace[2] = { XR_NULL_HANDLE, XR_NULL_HANDLE };
    bool m_Ready = false;
    bool m_Attached = false;

    std::atomic<bool> m_ProfileDirty{ true };
    std::atomic<int> m_ProfileIndex[2] = { { -1 }, { -1 } };
    std::atomic<bool> m_PoseSeen[2] = { { false }, { false } };
    std::atomic<bool> m_LoggedPose[2] = { { false }, { false } };

    std::mutex m_Mutex;  // cache + fake source
    std::array<ActionCache, xi::kActionCount> m_Cache{};
    InternalState m_Internal[2];
    XrTime m_LastSyncTime = 0;
    XrResult m_LastSync = XR_SUCCESS;
    uint64_t m_Syncs = 0, m_SyncErrors = 0;
    uint32_t m_LoggedChanges = 0;
    xi::FakeInput m_Fake;
    std::atomic<bool> m_FakeEnabled{ false };
    std::atomic<int> m_FakeProfileIndex{ -1 };

    XrPointerHook* m_PointerHook = nullptr;
    bool m_PointerEngaged[2] = { false, false };

    std::mutex m_TipMutex;
    vr::HmdMatrix34_t m_Tip[2]{};
    bool m_TipValid[2] = { false, false };

    std::atomic<uint64_t> m_HapticCalls{ 0 };
    std::atomic<uint32_t> m_HapticErrors{ 0 };
    std::atomic<bool> m_LoggedHaptic{ false };
    std::atomic<bool> m_LoggedManifest{ false };
    std::set<std::string> m_LoggedUnknown;

    std::mutex m_InternMutex;
    std::map<std::string, uint64_t> m_Interned;
};

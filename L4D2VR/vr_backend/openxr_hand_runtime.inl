// OpenXR hand skeletons (OpenXR port M5). Included by vr_backend_openxr.cpp
// inside its anonymous namespace, before openxr_input_runtime.inl, whose
// IVrInput skeletal getters answer from this class.
//
//   * XR_EXT_hand_tracking when the runtime offers it (SteamVR drives it from
//     the controllers' finger sensors, Monado emulates it for some controllers,
//     Quest runtimes track hands optically): one tracker per hand, joints
//     located in the hand's grip space every input sync and moved into the raw
//     controller frame (= the OpenVR skeleton's model space) with the profile's
//     grip offset. XR_EXT_hand_joints_motion_range asks for the hand conforming
//     to the controller ("with controller") or unobstructed; without it (or
//     where Wine drops the chained struct) openxr_hand_logic.h limits the
//     fingers app-side. XR_EXT_hand_tracking_data_source says whether the data
//     comes from the controller (tracking level Partial) or the hand (Full).
//   * Otherwise (and whenever this frame's joints are not usable) the curl
//     fallback: curls from the controller's trigger/grip/touch/proximity
//     sensors (openxr_hand_logic.h TargetCurls), smoothed, and bones blended
//     from SteamVR's reference poses (tracking level Estimated).
//   * Test only: "hand=<left|right>:<thumb>,<index>,<middle>,<ring>,<pinky>"
//     (or ":off", "hand=off") feeds a synthetic tracked hand with those curls
//     through the real tracked path (retarget, summary, motion range).

namespace xh = l4d2vr_openxr_hands;
namespace xi = l4d2vr_openxr_input;

class XrHandRuntime
{
public:
    explicit XrHandRuntime(const XrApi& xr) : m_Xr(xr) {}

    static std::vector<const char*> WantedExtensions()
    {
        return { XR_EXT_HAND_TRACKING_EXTENSION_NAME, XR_EXT_HAND_JOINTS_MOTION_RANGE_EXTENSION_NAME,
            XR_EXT_HAND_TRACKING_DATA_SOURCE_EXTENSION_NAME };
    }

    // After xrGetSystem: extension functions and whether the system tracks hands.
    void Configure(XrInstance instance, XrSystemId system, const std::set<std::string>& extensions)
    {
        m_Instance = instance;
        m_HasExtension = extensions.count(XR_EXT_HAND_TRACKING_EXTENSION_NAME) != 0;
        m_HasMotionRange = extensions.count(XR_EXT_HAND_JOINTS_MOTION_RANGE_EXTENSION_NAME) != 0;
        m_HasDataSource = extensions.count(XR_EXT_HAND_TRACKING_DATA_SOURCE_EXTENSION_NAME) != 0;
        if (m_HasExtension)
        {
            m_Xr.xrGetInstanceProcAddr(instance, "xrCreateHandTrackerEXT", reinterpret_cast<PFN_xrVoidFunction*>(&m_Create));
            m_Xr.xrGetInstanceProcAddr(instance, "xrDestroyHandTrackerEXT", reinterpret_cast<PFN_xrVoidFunction*>(&m_Destroy));
            m_Xr.xrGetInstanceProcAddr(instance, "xrLocateHandJointsEXT", reinterpret_cast<PFN_xrVoidFunction*>(&m_Locate));
            XrSystemHandTrackingPropertiesEXT ht{ XR_TYPE_SYSTEM_HAND_TRACKING_PROPERTIES_EXT };
            XrSystemProperties sp{ XR_TYPE_SYSTEM_PROPERTIES };
            sp.next = &ht;
            if (XR_SUCCEEDED(m_Xr.xrGetSystemProperties(instance, system, &sp)))
                m_SystemSupports = ht.supportsHandTracking == XR_TRUE;
        }
        m_Supported = m_HasExtension && m_SystemSupports && m_Create && m_Locate;
        Log("hand tracking: extension %d, system supports %d, motion range %d, data source %d -> %s",
            m_HasExtension ? 1 : 0, m_SystemSupports ? 1 : 0, m_HasMotionRange ? 1 : 0, m_HasDataSource ? 1 : 0,
            m_Supported ? "joints when the runtime tracks them, else the controller curl fallback" : "controller curl fallback only");
    }

    void SessionCreated(XrSession session)
    {
        if (!m_Supported || session == XR_NULL_HANDLE)
            return;
        for (int h = 0; h < 2; ++h)
        {
            XrHandTrackerCreateInfoEXT ci{ XR_TYPE_HAND_TRACKER_CREATE_INFO_EXT };
            ci.hand = h == 0 ? XR_HAND_LEFT_EXT : XR_HAND_RIGHT_EXT;
            ci.handJointSet = XR_HAND_JOINT_SET_DEFAULT_EXT;
            XrHandTrackingDataSourceEXT sources[2] = { XR_HAND_TRACKING_DATA_SOURCE_UNOBSTRUCTED_EXT, XR_HAND_TRACKING_DATA_SOURCE_CONTROLLER_EXT };
            XrHandTrackingDataSourceInfoEXT si{ XR_TYPE_HAND_TRACKING_DATA_SOURCE_INFO_EXT };
            si.requestedDataSourceCount = 2;
            si.requestedDataSources = sources;
            if (m_HasDataSource)
                ci.next = &si;
            XrResult r = m_Create(session, &ci, &m_Tracker[h]);
            if (XR_FAILED(r) && m_HasDataSource)
            {
                // Retry plain (a runtime or Wine thunk that rejects the chain).
                ci.next = nullptr;
                r = m_Create(session, &ci, &m_Tracker[h]);
            }
            if (XR_FAILED(r))
                m_Tracker[h] = XR_NULL_HANDLE;
            Log("%s hand tracker: %d", h == 0 ? "left" : "right", static_cast<int>(r));
        }
    }

    // Once per input sync (game thread), per hand. `grip` is the hand's grip
    // action space, `offset` its grip-in-raw transform, `controllerActive`
    // whether the controller's curl actions are active.
    void Update(int hand, XrSpace grip, XrTime time, const xi::GripInRaw& offset, bool controllerActive,
        const xh::CurlSensors& sensors, bool knuckles)
    {
        if (hand < 0 || hand > 1)
            return;
        const auto now = std::chrono::steady_clock::now();
        xh::Joints joints{};
        bool tracked = false;
        int source = 0;  // 0 unknown, 1 hand, 2 controller
        int fakeIndex = -1;
        float fakeCurls[xh::kFingerCount] = {};
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            if (m_State[hand].fake)
            {
                fakeIndex = hand;
                for (int f = 0; f < xh::kFingerCount; ++f)
                    fakeCurls[f] = m_State[hand].fakeCurls[f];
            }
        }
        if (fakeIndex >= 0)
        {
            joints = xh::JointsFromModelBones(hand, xh::ToModel(xh::BlendedBones(hand, fakeCurls, false)));
            tracked = true;
            source = 1;
        }
        else if (m_Tracker[hand] != XR_NULL_HANDLE && grip != XR_NULL_HANDLE && time > 0)
        {
            tracked = LocateJoints(hand, grip, time, offset, joints, source);
        }

        std::lock_guard<std::mutex> lock(m_Mutex);
        HandData& s = m_State[hand];
        const float dt = s.lastUpdate.time_since_epoch().count() ? std::chrono::duration<float>(now - s.lastUpdate).count() : 0.0f;
        s.lastUpdate = now;
        s.knuckles = knuckles;
        s.controllerActive = controllerActive;
        float target[xh::kFingerCount];
        xh::TargetCurls(sensors, target);
        // The fallback curls keep following the controller even while joints
        // arrive, so a lost hand does not jump.
        if (controllerActive)
            xh::SmoothCurls(s.fallbackCurls, target, dt);
        else
            for (float& c : s.fallbackCurls) c = 0.0f;
        const bool wasTracked = s.tracked;
        s.tracked = tracked && xh::JointsUsable(joints);
        if (s.tracked)
        {
            s.joints = joints;
            s.summary = xh::SummaryFromJoints(hand, joints);
            s.source = source;
            ++s.trackedFrames;
        }
        else
        {
            s.summary = xh::SummaryForCurls(hand, s.fallbackCurls);
            ++s.fallbackFrames;
        }
        if (s.tracked != wasTracked || !s.logged)
        {
            s.logged = true;
            if (m_LoggedChanges++ < 40)
                Log("%s hand: %s (curls %.2f %.2f %.2f %.2f %.2f)", hand == 0 ? "left" : "right",
                    s.tracked ? (source == 2 ? "joints from the controller" : source == 1 ? "joints (hand tracking)" : "joints")
                              : "controller curl fallback",
                    s.summary.curl[0], s.summary.curl[1], s.summary.curl[2], s.summary.curl[3], s.summary.curl[4]);
        }
    }

    // The skeletal action is active while the controller or tracked joints are there.
    bool Active(int hand) const
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        return hand >= 0 && hand < 2 && (m_State[hand].controllerActive || m_State[hand].tracked);
    }

    bool Summary(int hand, xh::Summary& out) const
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        if (hand < 0 || hand > 1 || !(m_State[hand].controllerActive || m_State[hand].tracked))
            return false;
        out = m_State[hand].summary;
        return true;
    }

    bool Bones(int hand, bool modelSpace, bool withController, xh::Bones& out)
    {
        if (hand < 0 || hand > 1)
            return false;
        m_WantController[hand].store(withController, std::memory_order_relaxed);
        xh::Joints joints;
        xh::Summary summary;
        bool tracked = false;
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            const HandData& s = m_State[hand];
            if (!(s.controllerActive || s.tracked))
                return false;
            tracked = s.tracked;
            joints = s.joints;
            summary = s.summary;
        }
        if (tracked)
        {
            out = xh::BonesFromJoints(hand, joints, modelSpace, withController, summary);
        }
        else
        {
            const xh::Bones parent = xh::BlendedBones(hand, summary.curl, withController);
            out = modelSpace ? xh::ToModel(parent) : parent;
        }
        return true;
    }

    vr::EVRSkeletalTrackingLevel Level(int hand) const
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        if (hand < 0 || hand > 1)
            return vr::VRSkeletalTracking_Estimated;
        const HandData& s = m_State[hand];
        if (!s.tracked)
            return vr::VRSkeletalTracking_Estimated;
        if (s.source == 2 || (s.source == 0 && s.knuckles))
            return vr::VRSkeletalTracking_Partial;
        return vr::VRSkeletalTracking_Full;
    }

    // Test only: "left:0.1,0.9,1,1,1", "right:off", "off".
    bool SetFake(const std::string& spec, std::string& error)
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        if (spec == "off")
        {
            m_State[0].fake = m_State[1].fake = false;
            Log("fake hand joints off");
            return true;
        }
        const size_t colon = spec.find(':');
        const std::string side = colon == std::string::npos ? spec : spec.substr(0, colon);
        const int hand = side == "left" ? 0 : side == "right" ? 1 : -1;
        if (hand < 0 || colon == std::string::npos)
        {
            error = "hand=<left|right>:<5 curls>|off";
            return false;
        }
        const std::string rest = spec.substr(colon + 1);
        if (rest == "off")
        {
            m_State[hand].fake = false;
            Log("fake %s hand joints off", side.c_str());
            return true;
        }
        float curls[xh::kFingerCount] = {};
        int n = 0;
        size_t start = 0;
        while (n < xh::kFingerCount && start <= rest.size())
        {
            const size_t comma = rest.find(',', start);
            const std::string item = rest.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
            char* end = nullptr;
            const float v = std::strtof(item.c_str(), &end);
            if (item.empty() || !end || *end != '\0' || !std::isfinite(v))
            {
                error = "bad curl '" + item + "'";
                return false;
            }
            curls[n++] = std::clamp(v, 0.0f, 1.0f);
            if (comma == std::string::npos)
                break;
            start = comma + 1;
        }
        if (n != xh::kFingerCount)
        {
            error = "need 5 curls (thumb,index,middle,ring,pinky)";
            return false;
        }
        m_State[hand].fake = true;
        for (int f = 0; f < xh::kFingerCount; ++f)
            m_State[hand].fakeCurls[f] = curls[f];
        Log("fake %s hand joints: curls %.2f %.2f %.2f %.2f %.2f", side.c_str(), curls[0], curls[1], curls[2], curls[3], curls[4]);
        return true;
    }

    std::string StatsLine() const
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        char buffer[256];
        snprintf(buffer, sizeof(buffer), "hands %s left=%s(%llu/%llu) right=%s(%llu/%llu) locateErrors=%llu",
            m_Supported ? "tracking" : "fallback-only",
            m_State[0].tracked ? "joints" : "curls", static_cast<unsigned long long>(m_State[0].trackedFrames),
            static_cast<unsigned long long>(m_State[0].fallbackFrames),
            m_State[1].tracked ? "joints" : "curls", static_cast<unsigned long long>(m_State[1].trackedFrames),
            static_cast<unsigned long long>(m_State[1].fallbackFrames), static_cast<unsigned long long>(m_LocateErrors));
        return buffer;
    }

private:
    struct HandData
    {
        bool controllerActive = false;
        bool tracked = false;
        bool knuckles = false;
        bool logged = false;
        int source = 0;
        xh::Joints joints{};
        xh::Summary summary;
        float fallbackCurls[xh::kFingerCount] = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
        std::chrono::steady_clock::time_point lastUpdate{};
        uint64_t trackedFrames = 0, fallbackFrames = 0;
        bool fake = false;
        float fakeCurls[xh::kFingerCount] = {};
    };

    template<class... Args>
    static void Log(const char* fmt, Args... args)
    {
        std::string format = std::string("[VR][OpenXR][Hands] ") + fmt;
        Game::logMsg(format.c_str(), args...);
    }

    static xh::Pose ToHandPose(const XrPosef& p)
    {
        xh::Pose out;
        out.orientation = xrm::Normalized(xrm::Quat{ p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w });
        out.position = xrm::Vec3{ p.position.x, p.position.y, p.position.z };
        return out;
    }

    // Joints in the grip space, moved into the raw controller frame.
    bool LocateJoints(int hand, XrSpace grip, XrTime time, const xi::GripInRaw& offset, xh::Joints& out, int& source)
    {
        XrHandJointLocationEXT locations[XR_HAND_JOINT_COUNT_EXT]{};
        XrHandJointLocationsEXT result{ XR_TYPE_HAND_JOINT_LOCATIONS_EXT };
        result.jointCount = XR_HAND_JOINT_COUNT_EXT;
        result.jointLocations = locations;
        XrHandTrackingDataSourceStateEXT sourceState{ XR_TYPE_HAND_TRACKING_DATA_SOURCE_STATE_EXT };
        XrHandJointsLocateInfoEXT info{ XR_TYPE_HAND_JOINTS_LOCATE_INFO_EXT };
        info.baseSpace = grip;
        info.time = time;
        XrHandJointsMotionRangeInfoEXT range{ XR_TYPE_HAND_JOINTS_MOTION_RANGE_INFO_EXT };
        range.handJointsMotionRange = m_WantController[hand].load(std::memory_order_relaxed)
            ? XR_HAND_JOINTS_MOTION_RANGE_CONFORMING_TO_CONTROLLER_EXT : XR_HAND_JOINTS_MOTION_RANGE_UNOBSTRUCTED_EXT;
        const bool chains = m_UseChains.load(std::memory_order_relaxed);
        if (chains && m_HasMotionRange)
            info.next = &range;
        if (chains && m_HasDataSource)
            result.next = &sourceState;
        XrResult r = m_Locate(m_Tracker[hand], &info, &result);
        if (XR_FAILED(r) && chains && (m_HasMotionRange || m_HasDataSource))
        {
            // A runtime (or Wine thunk) that rejects the chained structs: go plain from now on.
            m_UseChains.store(false, std::memory_order_relaxed);
            info.next = nullptr;
            result.next = nullptr;
            r = m_Locate(m_Tracker[hand], &info, &result);
            Log("xrLocateHandJointsEXT rejected the motion range / data source structs; retried without them -> %d", static_cast<int>(r));
        }
        if (XR_FAILED(r))
        {
            if (m_LocateErrors++ < 5)
                Log("xrLocateHandJointsEXT(%s) failed: %d", hand == 0 ? "left" : "right", static_cast<int>(r));
            return false;
        }
        if (!result.isActive)
            return false;
        const xrm::Pose gripInRaw = xi::GripInRawPose(offset);
        for (int j = 0; j < xh::kJointCount; ++j)
        {
            const XrSpaceLocationFlags need = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
            if ((locations[j].locationFlags & need) != need)
                return false;
            out[static_cast<size_t>(j)] = xi::Compose(gripInRaw, ToHandPose(locations[j].pose));
        }
        source = 0;
        if (result.next == &sourceState && sourceState.isActive)
            source = sourceState.dataSource == XR_HAND_TRACKING_DATA_SOURCE_CONTROLLER_EXT ? 2 : 1;
        if (!m_LoggedJoints[hand])
        {
            m_LoggedJoints[hand] = true;
            const xh::Pose& w = out[xh::kJointWrist];
            const xh::Pose& t = out[xh::kMiddle4];
            Log("%s hand first joints (raw frame): wrist (%.3f %.3f %.3f) middle tip (%.3f %.3f %.3f) source %d range %s",
                hand == 0 ? "left" : "right", w.position.x, w.position.y, w.position.z, t.position.x, t.position.y, t.position.z, source,
                info.next ? (range.handJointsMotionRange == XR_HAND_JOINTS_MOTION_RANGE_CONFORMING_TO_CONTROLLER_EXT ? "conforming" : "unobstructed") : "default");
        }
        return true;
    }

    const XrApi& m_Xr;
    XrInstance m_Instance = XR_NULL_HANDLE;
    bool m_HasExtension = false, m_HasMotionRange = false, m_HasDataSource = false;
    bool m_SystemSupports = false, m_Supported = false;
    PFN_xrCreateHandTrackerEXT m_Create = nullptr;
    PFN_xrDestroyHandTrackerEXT m_Destroy = nullptr;
    PFN_xrLocateHandJointsEXT m_Locate = nullptr;
    XrHandTrackerEXT m_Tracker[2] = { XR_NULL_HANDLE, XR_NULL_HANDLE };
    std::atomic<bool> m_UseChains{ true };
    std::atomic<bool> m_WantController[2] = { { true }, { true } };
    bool m_LoggedJoints[2] = { false, false };
    uint64_t m_LocateErrors = 0;
    uint32_t m_LoggedChanges = 0;

    mutable std::mutex m_Mutex;
    HandData m_State[2];
};

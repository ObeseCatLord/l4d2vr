// Self-test steps for hand skeletons and finger curls (OpenXR port M5).
// Included by vr.cpp after vr_selftest_overlays.inl.
//
//   fingercurls=<left|right>:<thumb>,<index>,<middle>,<ring>,<pinky>
//                          stand-in skeletal curls for that physical hand on either backend
//                          (vr_selftest.h CurlOverride); <hand>:off or "off" ends it
//   curlcheck=<left|right>:<5 curls>[:<tolerance>]
//                          the curls the mod reads for that physical hand (the getter the native
//                          hands, magazine curls and the VRIK pose relay use) match within the
//                          tolerance (default 0.03); waits up to 2 s for the smoothing
//   skeldump=<name>        every skeletal answer of the backend for both hands
//                          -> l4d2vr_selftest_<name>_skeleton.txt; on OpenXR also checks the
//                          OpenVR skeleton layout (31 bones, SteamVR names/hierarchy, valid data)
//   gloveassets=<dir>|off  load the vr_glove meshes from <dir> (the runtime's folder otherwise)
//   glovecheck=<left|right>[:bones|:curls]
//                          that hand's glove was skinned in the last 0.5 s (from bone data or
//                          the summary curls); waits up to 6 s
//
// With xrinput= (the OpenXR backend's fake action source, also "hand=<side>:<curls>"
// for a synthetic tracked hand) these drive the real OpenXR skeletal path; with
// fingercurls= the OpenVR backend shows the same curls for an A/B screenshot.

namespace l4d2vr_selftest_hands
{
    inline bool ParseCurls(const std::string& text, float out[5])
    {
        int n = 0;
        size_t start = 0;
        while (n < 5)
        {
            const size_t comma = text.find(',', start);
            const std::string item = text.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
            char* end = nullptr;
            const float v = std::strtof(item.c_str(), &end);
            if (item.empty() || !end || *end != '\0' || !std::isfinite(v))
                return false;
            out[n++] = std::clamp(v, 0.0f, 1.0f);
            if (comma == std::string::npos)
                break;
            start = comma + 1;
        }
        return n == 5;
    }

    inline int HandOf(const std::string& side) { return side == "left" ? 0 : side == "right" ? 1 : -1; }

    inline void AppendBones(std::string& out, const char* label, vr::EVRInputError result, const vr::VRBoneTransform_t* bones, int count)
    {
        char line[256];
        std::snprintf(line, sizeof(line), "  %s: result %d\n", label, static_cast<int>(result));
        out += line;
        if (result != vr::VRInputError_None)
            return;
        for (int b = 0; b < count; ++b)
        {
            const vr::VRBoneTransform_t& t = bones[b];
            std::snprintf(line, sizeof(line), "    %2d pos (% .5f % .5f % .5f) rot (% .5f % .5f % .5f % .5f)\n", b,
                t.position.v[0], t.position.v[1], t.position.v[2], t.orientation.x, t.orientation.y, t.orientation.z, t.orientation.w);
            out += line;
        }
    }
}

bool VR::RunHandSelfTestStep(const std::string& step, float t, bool& handled)
{
    namespace sh = l4d2vr_selftest_hands;
    handled = true;
    char detail[256];

    if (step.rfind("fingercurls=", 0) == 0)
    {
        const std::string arg = step.substr(12);
        if (arg == "off")
        {
            l4d2vr_selftest::ClearCurls(0);
            l4d2vr_selftest::ClearCurls(1);
            Game::logMsg("[VR][SelfTest] finger curl overrides off");
            return true;
        }
        const size_t colon = arg.find(':');
        const int hand = sh::HandOf(arg.substr(0, colon));
        const std::string rest = colon == std::string::npos ? std::string() : arg.substr(colon + 1);
        float curls[5];
        if (hand < 0)
        {
            SelfTestReport(step.c_str(), false, "fingercurls=<left|right>:<5 curls>|off");
            return true;
        }
        if (rest == "off")
        {
            l4d2vr_selftest::ClearCurls(hand);
            Game::logMsg("[VR][SelfTest] %s finger curl override off", hand == 0 ? "left" : "right");
            return true;
        }
        if (!sh::ParseCurls(rest, curls))
        {
            SelfTestReport(step.c_str(), false, "need 5 curls (thumb,index,middle,ring,pinky)");
            return true;
        }
        l4d2vr_selftest::SetCurls(hand, curls);
        Game::logMsg("[VR][SelfTest] %s finger curls %.2f %.2f %.2f %.2f %.2f", hand == 0 ? "left" : "right",
            curls[0], curls[1], curls[2], curls[3], curls[4]);
        return t >= 0.3f;
    }

    if (step.rfind("curlcheck=", 0) == 0)
    {
        const std::string arg = step.substr(10);
        const size_t colon = arg.find(':');
        const int hand = sh::HandOf(arg.substr(0, colon));
        std::string rest = colon == std::string::npos ? std::string() : arg.substr(colon + 1);
        float tolerance = 0.03f;
        const size_t tolAt = rest.find(':');
        if (tolAt != std::string::npos)
        {
            tolerance = std::clamp(std::strtof(rest.c_str() + tolAt + 1, nullptr), 0.001f, 1.0f);
            rest.resize(tolAt);
        }
        float expect[5];
        if (hand < 0 || !sh::ParseCurls(rest, expect))
        {
            SelfTestReport(step.c_str(), false, "curlcheck=<left|right>:<5 curls>[:<tolerance>]");
            return true;
        }
        std::array<float, 5> curls{};
        const bool valid = hand == 0 ? GetNativeViewmodelLeftHandOpenVRFingerCurls(curls)
            : GetWorldPoseAnatomicalRightHandOpenVRFingerCurls(curls);
        float worst = 0.0f;
        for (int f = 0; f < 5; ++f)
            worst = (std::max)(worst, std::fabs(curls[static_cast<size_t>(f)] - expect[f]));
        const bool ok = valid && worst <= tolerance;
        if (!ok && t < 2.0f)
            return false;
        VRWorldPoseTrackingSnapshot snapshot{};
        const bool haveSnapshot = ReadWorldPoseTrackingSnapshot(snapshot);
        const bool relayValid = haveSnapshot && (hand == 0 ? snapshot.leftFingerCurlsValid : snapshot.rightFingerCurlsValid);
        const std::array<float, 5>& relay = hand == 0 ? snapshot.leftFingerCurls : snapshot.rightFingerCurls;
        std::snprintf(detail, sizeof(detail),
            "%s valid=%d curls %.3f %.3f %.3f %.3f %.3f (expect %.2f %.2f %.2f %.2f %.2f, worst %.3f, tol %.3f); relay %s %.2f %.2f %.2f %.2f %.2f",
            hand == 0 ? "left" : "right", valid ? 1 : 0, curls[0], curls[1], curls[2], curls[3], curls[4],
            expect[0], expect[1], expect[2], expect[3], expect[4], worst, tolerance,
            relayValid ? "sends" : "native-animation", relay[0], relay[1], relay[2], relay[3], relay[4]);
        SelfTestReport("finger curls", ok, detail);
        return true;
    }

    if (step.rfind("skeldump=", 0) == 0)
    {
        std::string name = step.substr(9);
        for (char& ch : name)
            if (!std::isalnum(static_cast<unsigned char>(ch)) && ch != '-' && ch != '_')
                ch = '_';
        l4d2vr_backend::IVrInput* input = m_Input;
        const bool openxr = m_Backend && m_Backend->Kind() == l4d2vr_backend::RuntimeKind::OpenXR;
        std::string out = std::string("backend ") + (m_Backend ? m_Backend->Name() : "none") + "\n";
        bool layoutOk = openxr;
        std::string problems;
        for (int h = 0; h < 2 && input; ++h)
        {
            const char* path = h == 0 ? "/actions/base/in/skeleton_lefthand" : "/actions/base/in/skeleton_righthand";
            vr::VRActionHandle_t action = vr::k_ulInvalidActionHandle;
            input->GetActionHandle(path, &action);
            vr::InputSkeletalActionData_t data{};
            const vr::EVRInputError dataResult = input->GetSkeletalActionData(action, &data, sizeof(data));
            char line[256];
            std::snprintf(line, sizeof(line), "%s hand %s: action data %d active %d\n", h == 0 ? "left" : "right", path,
                static_cast<int>(dataResult), data.bActive ? 1 : 0);
            out += line;
            vr::EVRSkeletalTrackingLevel level = vr::VRSkeletalTracking_Estimated;
            const vr::EVRInputError levelResult = input->GetSkeletalTrackingLevel(action, &level);
            uint32_t count = 0;
            const vr::EVRInputError countResult = input->GetBoneCount(action, &count);
            std::snprintf(line, sizeof(line), "  tracking level %d (%d), bone count %u (%d)\n", static_cast<int>(level),
                static_cast<int>(levelResult), count, static_cast<int>(countResult));
            out += line;
            if (!openxr)
            {
                // XRizer does not implement GetBoneName/GetBoneHierarchy (it
                // aborts); the OpenVR dump stops at the summary.
                vr::VRSkeletalSummaryData_t summary{};
                const vr::EVRInputError sr = input->GetSkeletalSummaryData(action, vr::VRSummaryType_FromAnimation, &summary);
                std::snprintf(line, sizeof(line), "  summary %d curls %.3f %.3f %.3f %.3f %.3f\n", static_cast<int>(sr),
                    summary.flFingerCurl[0], summary.flFingerCurl[1], summary.flFingerCurl[2], summary.flFingerCurl[3], summary.flFingerCurl[4]);
                out += line;
                continue;
            }
            if (countResult != vr::VRInputError_None || count != 31u)
            {
                layoutOk = false;
                problems += "bone count; ";
                continue;
            }
            vr::BoneIndex_t parents[31]{};
            const vr::EVRInputError hr = input->GetBoneHierarchy(action, parents, 31u);
            out += "  bones:";
            for (int b = 0; b < 31; ++b)
            {
                char boneName[vr::k_unMaxBoneNameLength]{};
                input->GetBoneName(action, static_cast<vr::BoneIndex_t>(b), boneName, sizeof(boneName));
                std::snprintf(line, sizeof(line), " %d:%s<%d", b, boneName, static_cast<int>(parents[b]));
                out += line;
                if (std::string(boneName) != l4d2vr_openxr_hands::BoneName(h, b) || parents[b] != l4d2vr_openxr_hands::Parent(b))
                {
                    layoutOk = false;
                    problems += "bone " + std::to_string(b) + "; ";
                }
            }
            out += "\n";
            if (hr != vr::VRInputError_None)
            {
                layoutOk = false;
                problems += "hierarchy; ";
            }
            vr::VRBoneTransform_t bones[31]{};
            const struct { vr::EVRSkeletalReferencePose pose; const char* name; } refs[] = {
                { vr::VRSkeletalReferencePose_BindPose, "reference bind (parent)" },
                { vr::VRSkeletalReferencePose_OpenHand, "reference open hand (parent)" },
                { vr::VRSkeletalReferencePose_Fist, "reference fist (parent)" },
                { vr::VRSkeletalReferencePose_GripLimit, "reference grip limit (parent)" } };
            for (const auto& r : refs)
            {
                const vr::EVRInputError rr = input->GetSkeletalReferenceTransforms(action, vr::VRSkeletalTransformSpace_Parent, r.pose, bones, 31u);
                sh::AppendBones(out, r.name, rr, bones, 31);
                if (rr != vr::VRInputError_None || !std::all_of(bones, bones + 31, l4d2vr_skeletal::ValidBone))
                {
                    layoutOk = false;
                    problems += std::string(r.name) + "; ";
                }
            }
            const struct { vr::EVRSkeletalTransformSpace space; vr::EVRSkeletalMotionRange range; const char* name; } datas[] = {
                { vr::VRSkeletalTransformSpace_Model, vr::VRSkeletalMotionRange_WithController, "bone data model, with controller" },
                { vr::VRSkeletalTransformSpace_Parent, vr::VRSkeletalMotionRange_WithController, "bone data parent, with controller" },
                { vr::VRSkeletalTransformSpace_Model, vr::VRSkeletalMotionRange_WithoutController, "bone data model, without controller" } };
            for (const auto& d : datas)
            {
                const vr::EVRInputError dr = input->GetSkeletalBoneData(action, d.space, d.range, bones, 31u);
                sh::AppendBones(out, d.name, dr, bones, 31);
                if (data.bActive && (dr != vr::VRInputError_None || !std::all_of(bones, bones + 31, l4d2vr_skeletal::ValidBone)))
                {
                    layoutOk = false;
                    problems += std::string(d.name) + "; ";
                }
            }
            vr::VRSkeletalSummaryData_t summary{};
            const vr::EVRInputError sr = input->GetSkeletalSummaryData(action, vr::VRSummaryType_FromAnimation, &summary);
            std::snprintf(line, sizeof(line), "  summary %d curls %.3f %.3f %.3f %.3f %.3f splay %.3f %.3f %.3f %.3f\n", static_cast<int>(sr),
                summary.flFingerCurl[0], summary.flFingerCurl[1], summary.flFingerCurl[2], summary.flFingerCurl[3], summary.flFingerCurl[4],
                summary.flFingerSplay[0], summary.flFingerSplay[1], summary.flFingerSplay[2], summary.flFingerSplay[3]);
            out += line;
            if (data.bActive && (sr != vr::VRInputError_None || !l4d2vr_skeletal::ValidSummaryCurls(summary)))
            {
                layoutOk = false;
                problems += "summary; ";
            }
        }
        const std::string file = "l4d2vr_selftest_" + name + "_skeleton.txt";
        if (FILE* f = std::fopen(file.c_str(), "wb"))
        {
            std::fwrite(out.data(), 1, out.size(), f);
            std::fclose(f);
        }
        if (openxr)
        {
            std::snprintf(detail, sizeof(detail), "%s: %s", file.c_str(), layoutOk ? "31 SteamVR bones, names, hierarchy, reference poses, bone data and summary" : problems.c_str());
            SelfTestReport("OpenVR hand skeleton (OpenXR)", layoutOk, detail);
        }
        else
        {
            Game::logMsg("[VR][SelfTest] skeleton dump (OpenVR, summary only): %s", file.c_str());
        }
        return true;
    }

    if (step.rfind("gloveassets=", 0) == 0)
    {
        const std::string dir = step.substr(12);
        VrHandSystem::SetTestAssetDirectory(dir == "off" ? std::string() : dir);
        Game::logMsg("[VR][SelfTest] glove assets: %s", dir == "off" ? "the runtime's" : dir.c_str());
        return true;
    }

    if (step.rfind("glovecheck=", 0) == 0)
    {
        const std::string arg = step.substr(11);
        const size_t colon = arg.find(':');
        const int hand = sh::HandOf(arg.substr(0, colon));
        const std::string wantMode = colon == std::string::npos ? std::string() : arg.substr(colon + 1);
        if (hand < 0)
        {
            SelfTestReport(step.c_str(), false, "glovecheck=<left|right>[:bones|:curls]");
            return true;
        }
        const char* mode = "none";
        const bool fresh = m_VrHands && m_VrHands->HandPaletteFresh(static_cast<size_t>(hand), 500u, mode);
        const bool ok = fresh && (wantMode.empty() || wantMode == mode) && m_VrHandsEnabled;
        if (!ok && t < 6.0f)
            return false;
        std::snprintf(detail, sizeof(detail), "%s glove palette fresh=%d from %s (want %s), gloves %s", hand == 0 ? "left" : "right",
            fresh ? 1 : 0, mode, wantMode.empty() ? "any" : wantMode.c_str(), m_VrHandsEnabled ? "on" : "off (native fallback)");
        SelfTestReport("glove skinned", ok, detail);
        return true;
    }

    handled = false;
    return false;
}

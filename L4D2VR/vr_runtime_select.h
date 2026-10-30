#pragma once

// VR runtime selection. Two builds share this header:
//
// Release build (the default; what players get): OpenXR is the only runtime.
// The OpenVR backend is not compiled in, the DLL does not link openvr_api, and
// the release ships no openvr_api.dll or SteamVR action manifest. VrRuntime in
// VR/config.txt and the -l4d2vr_openxr / -l4d2vr_openvr launch options are
// ignored (a request for OpenVR is logged as ignored). Without a usable OpenXR
// runtime the game starts in desktop mode and logs why (DesktopModeAuto=true,
// the default) or shows the error (DesktopModeAuto=false).
//
// Legacy build (developers only; MSBuild /p:L4D2VRLegacyOpenVR=true, which
// defines L4D2VR_LEGACY_OPENVR=1 and links openvr_api.lib): the M1-M5 policy
// below with both backends. VrRuntime=auto|openxr|openvr, overridden by
// -l4d2vr_openxr / -l4d2vr_openvr:
//   * A launch option beats the config key. With both options present,
//     -l4d2vr_openvr wins.
//   * auto = OpenVR.
//   * openxr without a usable OpenXR runtime (no loader next to d3d9.dll, no
//     active runtime, no HMD, no XR_KHR_vulkan_enable, Proton without 32-bit
//     wineopenxr) falls back to OpenVR and says why in the log. The runtime is
//     only probed when OpenXR was explicitly requested (DecideWithProbe).
//   * An unknown config value is treated as auto and logged.
//
// Either way the decision is made once, before Direct3DCreate9 (DXVK's Vulkan
// extension provider depends on it), and published through
// L4D2VR_SelectedVrRuntime(). Desktop mode and -nohmd ignore it.

#include <cctype>
#include <cwctype>
#include <functional>
#include <string>
#include <string_view>

namespace l4d2vr_runtime_select
{
    // True in the legacy developer build that still contains the OpenVR backend.
#if defined(L4D2VR_LEGACY_OPENVR) && L4D2VR_LEGACY_OPENVR
    constexpr bool kLegacyOpenVrBuild = true;
#else
    constexpr bool kLegacyOpenVrBuild = false;
#endif

    // Published values (L4D2VR_SelectedVrRuntime, l4d2vr_backend::RuntimeKind).
    // None (release build only): -nohmd or desktop mode requested, so no VR
    // runtime is used at all; DXVK then adds no VR extension provider.
    enum class Runtime : int
    {
        None = 0,
        OpenVR = 1,
        OpenXR = 2,
    };

    enum class Preference
    {
        Auto,
        OpenVR,
        OpenXR,
    };

    enum class Switch
    {
        None,
        OpenVR,
        OpenXR,
        Both,
    };

    constexpr const char* kConfigKey = "VrRuntime";
    constexpr const wchar_t* kOpenXrSwitch = L"-l4d2vr_openxr";
    constexpr const wchar_t* kOpenVrSwitch = L"-l4d2vr_openvr";

    inline const char* Name(Runtime runtime)
    {
        switch (runtime)
        {
        case Runtime::OpenXR: return "OpenXR";
        case Runtime::None: return "none";
        default: return "OpenVR";
        }
    }

    inline const char* Name(Preference preference)
    {
        switch (preference)
        {
        case Preference::OpenVR: return "openvr";
        case Preference::OpenXR: return "openxr";
        default: return "auto";
        }
    }

    // Parses a VrRuntime value: case-insensitive, surrounding whitespace and
    // one pair of quotes ignored, empty = auto. Returns false (and auto) for
    // anything else.
    inline bool ParsePreference(std::string_view text, Preference& out)
    {
        out = Preference::Auto;
        auto isSpace = [](char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; };
        while (!text.empty() && isSpace(text.front()))
            text.remove_prefix(1);
        while (!text.empty() && isSpace(text.back()))
            text.remove_suffix(1);
        if (text.size() >= 2 &&
            ((text.front() == '"' && text.back() == '"') || (text.front() == '\'' && text.back() == '\'')))
        {
            text = text.substr(1, text.size() - 2);
            while (!text.empty() && isSpace(text.front()))
                text.remove_prefix(1);
            while (!text.empty() && isSpace(text.back()))
                text.remove_suffix(1);
        }

        std::string lower;
        lower.reserve(text.size());
        for (char c : text)
            lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));

        if (lower.empty() || lower == "auto")
            return true;
        if (lower == "openvr")
        {
            out = Preference::OpenVR;
            return true;
        }
        if (lower == "openxr")
        {
            out = Preference::OpenXR;
            return true;
        }
        return false;
    }

    inline bool EqualsIgnoreCase(std::wstring_view a, std::wstring_view b)
    {
        if (a.size() != b.size())
            return false;
        for (size_t i = 0; i < a.size(); ++i)
        {
            if (std::towlower(a[i]) != std::towlower(b[i]))
                return false;
        }
        return true;
    }

    // Folds one command-line argument into the switch seen so far.
    inline Switch AccumulateSwitch(Switch seen, std::wstring_view arg)
    {
        const bool openXr = EqualsIgnoreCase(arg, kOpenXrSwitch);
        const bool openVr = EqualsIgnoreCase(arg, kOpenVrSwitch);
        if (!openXr && !openVr)
            return seen;
        const Switch added = openXr ? Switch::OpenXR : Switch::OpenVR;
        if (seen == Switch::None || seen == added)
            return added;
        return Switch::Both;
    }

    struct Decision
    {
        Runtime runtime = Runtime::OpenVR;
        Preference requested = Preference::Auto;  // after the launch-option override
        bool fromCommandLine = false;
        bool conflictingSwitches = false;          // both launch options given
        bool unknownConfigValue = false;
        bool openXrUnavailable = false;            // openxr requested, fell back to OpenVR
        bool openXrProbed = false;                 // the OpenXR runtime probe ran
        std::string openXrUnavailableReason;       // empty = backend not built
        bool openXrOnlyBuild = false;              // release build: no OpenVR backend
        bool openVrRequestIgnored = false;         // VrRuntime=openvr / -l4d2vr_openvr in a release build
    };

    // configValueValid=false means the config held an unknown value (treated as auto).
    inline Decision Decide(Preference config, bool configValueValid, Switch commandLine, bool openXrBackendAvailable)
    {
        Decision decision;
        decision.unknownConfigValue = !configValueValid;
        decision.requested = configValueValid ? config : Preference::Auto;

        switch (commandLine)
        {
        case Switch::OpenXR:
            decision.requested = Preference::OpenXR;
            decision.fromCommandLine = true;
            break;
        case Switch::OpenVR:
            decision.requested = Preference::OpenVR;
            decision.fromCommandLine = true;
            break;
        case Switch::Both:
            decision.requested = Preference::OpenVR;
            decision.fromCommandLine = true;
            decision.conflictingSwitches = true;
            break;
        default:
            break;
        }

        switch (decision.requested)
        {
        case Preference::OpenXR:
            if (openXrBackendAvailable)
            {
                decision.runtime = Runtime::OpenXR;
            }
            else
            {
                decision.runtime = Runtime::OpenVR;
                decision.openXrUnavailable = true;
            }
            break;
        case Preference::OpenVR:
        case Preference::Auto:
        default:
            // auto stays on OpenVR until the OpenXR backend reaches parity.
            decision.runtime = Runtime::OpenVR;
            break;
        }
        return decision;
    }

    // Decide() plus the OpenXR runtime probe, which runs only when the request
    // resolves to OpenXR (auto and openvr never touch the OpenXR loader). The
    // probe returns true when the runtime is usable, else false and a reason.
    using OpenXrProbe = std::function<bool(std::string& reason)>;

    inline Decision DecideWithProbe(Preference config, bool configValueValid, Switch commandLine,
        bool openXrBackendBuilt, const OpenXrProbe& probe)
    {
        Decision decision = Decide(config, configValueValid, commandLine, openXrBackendBuilt);
        if (decision.runtime != Runtime::OpenXR)
            return decision;
        std::string reason;
        decision.openXrProbed = true;
        const bool usable = probe ? probe(reason) : false;
        if (!usable)
        {
            decision.runtime = Runtime::OpenVR;
            decision.openXrUnavailable = true;
            decision.openXrUnavailableReason = reason.empty() ? std::string("unknown error") : reason;
        }
        return decision;
    }

    // Release (OpenXR-only) build: always OpenXR. The config key and the
    // launch options are still parsed so that a request for OpenVR can be
    // reported as ignored; nothing is probed here (an unusable OpenXR runtime
    // leads to desktop mode, see L4D2VR_DesktopModeActive).
    inline Decision DecideOpenXrOnly(Preference config, bool configValueValid, Switch commandLine)
    {
        Decision decision = Decide(config, configValueValid, commandLine, true);
        decision.openXrOnlyBuild = true;
        decision.openVrRequestIgnored = decision.requested == Preference::OpenVR;
        decision.runtime = Runtime::OpenXR;
        decision.openXrUnavailable = false;
        decision.openXrProbed = false;
        decision.openXrUnavailableReason.clear();
        return decision;
    }

    // The policy of the build this header is compiled into (kLegacyOpenVrBuild),
    // or of the other build for tests.
    inline Decision DecideForBuild(bool legacyOpenVrBuild, Preference config, bool configValueValid, Switch commandLine,
        const OpenXrProbe& probe)
    {
        if (!legacyOpenVrBuild)
            return DecideOpenXrOnly(config, configValueValid, commandLine);
        return DecideWithProbe(config, configValueValid, commandLine, true, probe);
    }

    // One log line describing the decision (without the "[VR] " prefix).
    inline std::string Describe(const Decision& decision, std::string_view rawConfigValue)
    {
        std::string text = "VR runtime: ";
        text += Name(decision.runtime);
        text += " (";
        if (decision.fromCommandLine)
        {
            if (decision.conflictingSwitches)
                text += decision.openXrOnlyBuild ? "both -l4d2vr_openxr and -l4d2vr_openvr given"
                                                 : "both -l4d2vr_openxr and -l4d2vr_openvr given; using -l4d2vr_openvr";
            else
                text += decision.requested == Preference::OpenXR ? "-l4d2vr_openxr" : "-l4d2vr_openvr";
        }
        else
        {
            text += kConfigKey;
            text += "=";
            text += Name(decision.requested);
        }
        text += ")";
        if (decision.openXrOnlyBuild)
        {
            text += decision.openVrRequestIgnored
                ? "; OpenVR is not part of this build (OpenXR-only release; VrRuntime and -l4d2vr_openvr only work in "
                  "legacy builds made with L4D2VR_LEGACY_OPENVR), using OpenXR"
                : "; OpenXR-only build";
        }
        if (decision.unknownConfigValue)
        {
            text += "; unknown ";
            text += kConfigKey;
            text += " value '";
            text += std::string(rawConfigValue);
            text += "' treated as auto";
        }
        if (decision.openXrUnavailable)
        {
            if (decision.openXrUnavailableReason.empty())
                text += "; OpenXR backend not built yet, falling back to OpenVR";
            else
                text += "; OpenXR runtime unavailable (" + decision.openXrUnavailableReason + "), falling back to OpenVR";
        }
        return text;
    }
}

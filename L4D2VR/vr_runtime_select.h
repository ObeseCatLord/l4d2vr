#pragma once

// VR runtime selection (OpenXR port M1): VrRuntime=auto|openxr|openvr in
// VR/config.txt, overridden by the -l4d2vr_openxr / -l4d2vr_openvr launch
// options. The decision is made once, before Direct3DCreate9 (DXVK's Vulkan
// extension provider depends on it), and published through
// L4D2VR_SelectedVrRuntime(). Desktop mode ignores it.
//
// Policy:
//   * A launch option beats the config key. With both options present,
//     -l4d2vr_openvr wins (the runtime that works everywhere today).
//   * auto = OpenVR, until the OpenXR backend reaches parity.
//   * openxr without an OpenXR backend in this build, or without a usable
//     OpenXR runtime (no loader next to d3d9.dll, no active runtime, no HMD,
//     no XR_KHR_vulkan_enable, Proton without 32-bit wineopenxr), falls back
//     to OpenVR and says why in the log. The runtime is only probed when
//     OpenXR was explicitly requested (DecideWithProbe).
//   * An unknown config value is treated as auto and logged.

#include <cctype>
#include <cwctype>
#include <functional>
#include <string>
#include <string_view>

namespace l4d2vr_runtime_select
{
    // Published values (L4D2VR_SelectedVrRuntime, l4d2vr_backend::RuntimeKind).
    enum class Runtime : int
    {
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
        return runtime == Runtime::OpenXR ? "OpenXR" : "OpenVR";
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

    // One log line describing the decision (without the "[VR] " prefix).
    inline std::string Describe(const Decision& decision, std::string_view rawConfigValue)
    {
        std::string text = "VR runtime: ";
        text += Name(decision.runtime);
        text += " (";
        if (decision.fromCommandLine)
        {
            if (decision.conflictingSwitches)
                text += "both -l4d2vr_openxr and -l4d2vr_openvr given; using -l4d2vr_openvr";
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

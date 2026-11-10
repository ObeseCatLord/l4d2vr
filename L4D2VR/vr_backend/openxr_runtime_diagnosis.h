#pragma once

// Why OpenXR is unavailable, in words a player can act on (OpenXR port M6).
//
// Std-only so the wording and the decisions are unit tested on every platform
// (tests/vr_openxr_diagnosis_tests.cpp). The Win32 side that gathers the facts
// (registry, files, Wine) is openxr_loader_probe.h GatherRuntimeFacts.
//
// Two stages:
//   1. Preflight(facts): before the loader is touched. Decides whether an
//      OpenXR runtime can exist at all (Windows: an active runtime registered
//      in the 32-bit registry view whose manifest exists; Proton: a 32-bit
//      wineopenxr.dll in the prefix). When not, the mod starts in desktop mode
//      right away (DesktopModeAuto) with this reason.
//   2. LoaderFailureReason(result, facts, wineVrState): the loader or runtime
//      answered with an error (XR_ERROR_RUNTIME_UNAVAILABLE and friends).

#include <algorithm>
#include <cctype>
#include <string>

namespace l4d2vr_openxr
{
    // XrResult values used here (checked against openxr.h in openxr_loader_probe.h).
    constexpr int kXrErrorRuntimeFailure = -2;
    constexpr int kXrErrorInitializationFailed = -6;
    constexpr int kXrErrorInstanceLost = -13;
    constexpr int kXrErrorFormFactorUnavailable = -35;
    constexpr int kXrErrorRuntimeUnavailable = -51;

    // HKCU\Software\Wine\VR "state" as seen by the mod (Proton's steam.exe
    // writes it): 1 = ready, 0 = still starting, -1 = the OpenVR runtime failed.
    constexpr int kWineVrStateUnknown = -100;  // not Wine, or not read
    constexpr int kWineVrStateKeyMissing = -2;  // steam.exe never created the key

    struct RuntimeFacts
    {
        bool wine = false;
        // Windows: HKLM\SOFTWARE\Khronos\OpenXR\1 "ActiveRuntime" in the 32-bit
        // registry view (what the loader of a 32-bit game reads) and the 64-bit view.
        std::string activeRuntime32;
        std::string activeRuntime64;
        bool activeRuntime32Exists = false;   // that 32-bit manifest file exists
        std::string runtimeJsonEnv;           // XR_RUNTIME_JSON in the Win32 environment
        // The 32-bit manifest that sits next to the active 64-bit one
        // (steamxr_win64.json -> steamxr_win32.json, *_64.json -> *_32.json),
        // see ThirtyTwoBitSibling; empty when the name has no 64 in it.
        std::string sibling32Of64;
        bool sibling32Of64Exists = false;
        // Proton: the prefix has a 32-bit wineopenxr.dll (syswow64).
        bool wineOpenXr32 = false;
    };

    struct Preflight
    {
        bool tryRuntime = false;
        std::string reason;  // why not (empty when tryRuntime)
    };

    inline const char* WindowsRuntimeFix()
    {
        return "Make a 32-bit capable OpenXR runtime the active one: SteamVR 2.17 or newer (SteamVR Settings > OpenXR > "
               "Set SteamVR as OpenXR Runtime), Virtual Desktop (VDXR) or Meta Quest Link. Windows Mixed Reality's own "
               "OpenXR runtime is 64-bit only: use SteamVR with the Windows Mixed Reality for SteamVR driver instead";
    }

    inline const char* ProtonVersionFix()
    {
        return "Run the game with Proton Hotfix (2026-10-07 or newer) or Proton Experimental (11.0-20261014 or newer) "
               "and the launch option PROTON_USE_WOW64=1 (the L4D2VR installer sets both)";
    }

    inline std::string LowerAscii(std::string text)
    {
        std::transform(text.begin(), text.end(), text.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return text;
    }

    inline std::string ManifestFileName(const std::string& path)
    {
        const size_t slash = path.find_last_of("\\/");
        return slash == std::string::npos ? path : path.substr(slash + 1);
    }

    // Same manifest? (case-insensitive, either slash; Windows paths)
    inline bool SameManifest(const std::string& a, const std::string& b)
    {
        auto norm = [](const std::string& path) {
            std::string out = LowerAscii(path);
            std::replace(out.begin(), out.end(), '/', '\\');
            return out;
        };
        return !a.empty() && norm(a) == norm(b);
    }

    // SteamVR's runtime manifests: steamxr_win64.json and (2.17+) steamxr_win32.json.
    inline bool IsSteamVrManifest(const std::string& path)
    {
        return LowerAscii(ManifestFileName(path)).rfind("steamxr_win", 0) == 0;
    }

    // The 32-bit manifest a runtime ships next to its 64-bit one: the last "64"
    // in the file name becomes "32" (steamxr_win64.json -> steamxr_win32.json,
    // oculus_openxr_64.json -> oculus_openxr_32.json). Empty when the name has
    // no "64" (such runtimes register both views themselves).
    inline std::string ThirtyTwoBitSibling(const std::string& manifest64)
    {
        const std::string name = ManifestFileName(manifest64);
        const size_t at = name.rfind("64");
        if (name.empty() || at == std::string::npos)
            return std::string();
        std::string sibling = manifest64;
        sibling.replace(manifest64.size() - name.size() + at, 2, "32");
        return sibling;
    }

    // Which runtime the 32-bit game should load when the registry's 32-bit view
    // does not match the user's choice. The OpenXR loader of a 32-bit process
    // only reads HKLM\SOFTWARE\WOW6432Node\Khronos\OpenXR\1\ActiveRuntime. A
    // runtime that registered only the 64-bit view (SteamVR set as the OpenXR
    // runtime before 2.17, which added steamxr_win32.json) leaves that empty,
    // stale or pointing at an older choice (e.g. Oculus), although the 32-bit
    // manifest of the active runtime is installed. The mod then hands that
    // manifest to its own loader through XR_RUNTIME_JSON (this process only, no
    // registry change, no admin rights).
    //   * no 32-bit registration, or its manifest is gone: the active 64-bit
    //     runtime's 32-bit sibling, any runtime;
    //   * a different 32-bit registration: only when SteamVR is the active
    //     (64-bit) runtime; other runtimes register both views themselves.
    // Never under Wine and never over an XR_RUNTIME_JSON the player set.
    struct RuntimeOverride
    {
        std::string manifest;  // empty: keep the registry's choice
        std::string why;
    };

    inline RuntimeOverride Choose32BitRuntimeOverride(const RuntimeFacts& facts)
    {
        RuntimeOverride out;
        if (facts.wine || !facts.runtimeJsonEnv.empty() || facts.activeRuntime64.empty() ||
            facts.sibling32Of64.empty() || !facts.sibling32Of64Exists ||
            SameManifest(facts.sibling32Of64, facts.activeRuntime32))
            return out;
        const std::string active = IsSteamVrManifest(facts.activeRuntime64) ? "SteamVR" : facts.activeRuntime64;
        if (facts.activeRuntime32.empty())
            out.why = "no 32-bit OpenXR runtime is registered (HKLM\\SOFTWARE\\WOW6432Node\\Khronos\\OpenXR\\1), but the "
                "active OpenXR runtime (" + active + ") has a 32-bit runtime";
        else if (!facts.activeRuntime32Exists)
            out.why = "the registered 32-bit OpenXR runtime (" + facts.activeRuntime32 + ") does not exist, but the "
                "active OpenXR runtime (" + active + ") has a 32-bit runtime";
        else if (IsSteamVrManifest(facts.activeRuntime64))
            out.why = "the 32-bit OpenXR registration points to another runtime (" + facts.activeRuntime32 +
                ") than the active one (SteamVR)";
        else
            return out;
        out.manifest = facts.sibling32Of64;
        return out;
    }

    // An old SteamVR (before 2.17) is the active runtime: it has no 32-bit runtime.
    inline bool IsOldSteamVr(const RuntimeFacts& facts)
    {
        return IsSteamVrManifest(facts.activeRuntime64) && !facts.sibling32Of64.empty() && !facts.sibling32Of64Exists;
    }

    inline Preflight CheckPreflight(const RuntimeFacts& facts)
    {
        Preflight out;
        if (facts.wine)
        {
            if (!facts.wineOpenXr32)
            {
                out.reason = std::string("this Proton version has no 32-bit OpenXR support (no syswow64\\wineopenxr.dll). ")
                    + ProtonVersionFix();
                return out;
            }
            out.tryRuntime = true;
            return out;
        }
        if (!facts.runtimeJsonEnv.empty())
        {
            // Developer override; the loader decides.
            out.tryRuntime = true;
            return out;
        }
        if (facts.activeRuntime32.empty())
        {
            if (facts.activeRuntime64.empty())
                out.reason = "no OpenXR runtime is installed (no active OpenXR runtime is registered)";
            else if (IsOldSteamVr(facts))
                out.reason = "the active OpenXR runtime is SteamVR (" + facts.activeRuntime64 + "), but this SteamVR "
                    "has no 32-bit OpenXR runtime (" + ManifestFileName(facts.sibling32Of64) + " is missing), and Left 4 "
                    "Dead 2 is a 32-bit game. Update SteamVR to 2.17 or newer (Steam > Library > SteamVR), then open "
                    "SteamVR Settings > OpenXR and press Set SteamVR as OpenXR Runtime";
            else
                out.reason = "the active OpenXR runtime (" + facts.activeRuntime64 +
                    ") is 64-bit only, and Left 4 Dead 2 is a 32-bit game. " + WindowsRuntimeFix();
            return out;
        }
        if (!facts.activeRuntime32Exists)
        {
            out.reason = "the active 32-bit OpenXR runtime manifest (" + facts.activeRuntime32 +
                ") does not exist. Reinstall the runtime or select it again. " + WindowsRuntimeFix();
            return out;
        }
        out.tryRuntime = true;
        return out;
    }

    inline bool IsRuntimeUnavailableResult(int result)
    {
        return result == kXrErrorRuntimeUnavailable || result == kXrErrorInstanceLost ||
            result == kXrErrorRuntimeFailure || result == kXrErrorInitializationFailed;
    }

    // Proton: what the Wine VR state says about the OpenVR runtime Proton
    // starts before wineopenxr works.
    inline std::string WineVrStateReason(int state)
    {
        if (state == kWineVrStateKeyMissing)
            return "Proton did not prepare VR for this launch. Start the game from Steam (Proton only enables VR for a "
                   "Steam game launch with SteamGameId)";
        if (state == -1)
            return "Proton could not start an OpenVR runtime. Under Proton, OpenXR only works when an OpenVR runtime is "
                   "registered and working: SteamVR, or XRizer / OpenComposite for Monado and WiVRn (the L4D2VR "
                   "installer can set up XRizer). Check that your OpenXR runtime (Monado, WiVRn or SteamVR) is running";
        if (state == 0)
            return "Proton's VR setup did not finish in time (its OpenVR runtime did not start). Check that your OpenXR "
                   "runtime (Monado, WiVRn or SteamVR) is running and that the OpenVR runtime (XRizer, OpenComposite "
                   "or SteamVR) works";
        return std::string();
    }

    inline std::string LoaderFailureReason(int result, const RuntimeFacts& facts, int wineVrState)
    {
        const std::string code = " (result " + std::to_string(result) + ")";
        if (!IsRuntimeUnavailableResult(result))
            return "the OpenXR loader failed" + code;
        if (facts.wine)
        {
            const std::string state = WineVrStateReason(wineVrState);
            if (!state.empty())
                return "no OpenXR runtime" + code + ": " + state;
            return "no OpenXR runtime" + code + ": wineopenxr found no host OpenXR runtime. Start Monado, WiVRn or "
                   "SteamVR (and set it as the active OpenXR runtime) before the game";
        }
        if (!facts.runtimeJsonEnv.empty())
            return "the OpenXR runtime " + facts.runtimeJsonEnv + " is not available" + code +
                ". Start it (and switch on the headset) before the game";
        if (!facts.activeRuntime32.empty())
            return "the active 32-bit OpenXR runtime (" + facts.activeRuntime32 + ") is not available" + code +
                ". Start it (and switch on the headset) before the game";
        return "no active OpenXR runtime with a 32-bit manifest" + code + ". " + WindowsRuntimeFix();
    }

    inline std::string NoVulkanReason()
    {
        return "the active OpenXR runtime does not support Vulkan (XR_KHR_vulkan_enable), which L4D2VR renders "
               "through. Windows Mixed Reality's own runtime is Direct3D only: use SteamVR instead";
    }

    inline std::string NoHeadsetReason(const std::string& runtimeName)
    {
        return "the OpenXR runtime" + (runtimeName.empty() ? std::string() : " '" + runtimeName + "'") +
            " found no headset (XR_ERROR_FORM_FACTOR_UNAVAILABLE). Connect and switch on the headset, then restart "
            "the game";
    }

    // A one-line, console-safe version of a reason (no quotes or semicolons,
    // which the Source console would treat as syntax), for the in-game echo.
    inline std::string ConsoleSafe(const std::string& text, size_t maxLength = 480)
    {
        std::string out;
        out.reserve(text.size());
        for (char c : text)
        {
            if (c == '"' || c == '\'')
                continue;
            if (c == ';' || c == '\n' || c == '\r' || c == '\t')
                c = ',';
            out.push_back(c);
        }
        if (out.size() > maxLength)
            out.resize(maxLength);
        return out;
    }
}

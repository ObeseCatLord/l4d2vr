// OpenXR "why is there no VR" decisions and wording
// (L4D2VR/vr_backend/openxr_runtime_diagnosis.h), plus the openvrpaths.vrpath
// reader the OpenXR-only release uses instead of openvr_api
// (L4D2VR/vr_backend/openvr_paths.h). Std-only.
#include "../L4D2VR/vr_backend/openxr_runtime_diagnosis.h"
#include "../L4D2VR/vr_backend/openvr_paths.h"

#include <cstdio>
#include <cstdlib>
#include <string>

static int checks = 0;
static void Check(bool passed, const char* contract)
{
    ++checks;
    if (!passed)
    {
        std::fprintf(stderr, "FAIL: %s\n", contract);
        std::exit(1);
    }
}

static bool Contains(const std::string& text, const char* part)
{
    return text.find(part) != std::string::npos;
}

int main()
{
    namespace xrm = l4d2vr_openxr;
    namespace ovp = l4d2vr_openvr_paths;

    // ---- Windows preflight ------------------------------------------------
    xrm::RuntimeFacts none;
    xrm::Preflight p = xrm::CheckPreflight(none);
    Check(!p.tryRuntime && Contains(p.reason, "no OpenXR runtime is installed"), "nothing registered: desktop at once");

    xrm::RuntimeFacts wmr;
    wmr.activeRuntime64 = "C:\\Windows\\system32\\MixedRealityRuntime.json";
    p = xrm::CheckPreflight(wmr);
    Check(!p.tryRuntime, "64-bit-only runtime: the loader is not tried");
    Check(Contains(p.reason, "is 64-bit only") && Contains(p.reason, "MixedRealityRuntime.json"),
        "64-bit-only runtime: names the runtime and the problem");
    Check(Contains(p.reason, "SteamVR 2.17") && Contains(p.reason, "Virtual Desktop") && Contains(p.reason, "Meta Quest Link"),
        "64-bit-only runtime: names the supported runtimes");
    Check(Contains(p.reason, "Windows Mixed Reality for SteamVR"), "64-bit-only runtime: the WMR route");

    xrm::RuntimeFacts stale;
    stale.activeRuntime32 = "C:\\Program Files\\Old\\runtime32.json";
    stale.activeRuntime32Exists = false;
    p = xrm::CheckPreflight(stale);
    Check(!p.tryRuntime && Contains(p.reason, "does not exist") && Contains(p.reason, "runtime32.json"),
        "registered 32-bit manifest missing: not tried, named");

    xrm::RuntimeFacts steamvr;
    steamvr.activeRuntime32 = "C:\\Program Files (x86)\\Steam\\steamapps\\common\\SteamVR\\steamxr_win32.json";
    steamvr.activeRuntime32Exists = true;
    steamvr.activeRuntime64 = "C:\\Program Files (x86)\\Steam\\steamapps\\common\\SteamVR\\steamxr_win64.json";
    p = xrm::CheckPreflight(steamvr);
    Check(p.tryRuntime && p.reason.empty(), "a 32-bit runtime is tried");

    xrm::RuntimeFacts envOverride;
    envOverride.runtimeJsonEnv = "D:\\dev\\runtime.json";
    Check(xrm::CheckPreflight(envOverride).tryRuntime, "XR_RUNTIME_JSON lets the loader decide");

    // ---- Proton preflight ---------------------------------------------------
    xrm::RuntimeFacts oldProton;
    oldProton.wine = true;
    oldProton.activeRuntime32 = "C:\\openxr\\wineopenxr64.json";  // Proton writes the key even without a 32-bit DLL
    oldProton.activeRuntime32Exists = true;
    oldProton.wineOpenXr32 = false;
    p = xrm::CheckPreflight(oldProton);
    Check(!p.tryRuntime, "Proton without 32-bit wineopenxr: not tried");
    Check(Contains(p.reason, "Proton Hotfix") && Contains(p.reason, "Proton Experimental") && Contains(p.reason, "PROTON_USE_WOW64=1"),
        "old Proton: says which Proton and launch option");
    xrm::RuntimeFacts newProton = oldProton;
    newProton.wineOpenXr32 = true;
    Check(xrm::CheckPreflight(newProton).tryRuntime, "Proton with 32-bit wineopenxr: tried");
    xrm::RuntimeFacts protonNoKey;
    protonNoKey.wine = true;
    protonNoKey.wineOpenXr32 = true;
    Check(xrm::CheckPreflight(protonNoKey).tryRuntime, "under Wine the registry is not what decides");

    // ---- Loader failures ----------------------------------------------------
    std::string r = xrm::LoaderFailureReason(xrm::kXrErrorRuntimeUnavailable, newProton, -1);
    Check(Contains(r, "(result -51)") && Contains(r, "OpenVR runtime") && Contains(r, "XRizer"),
        "Proton, VR state -1: the OpenVR runtime Proton needs");
    r = xrm::LoaderFailureReason(xrm::kXrErrorRuntimeUnavailable, newProton, xrm::kWineVrStateKeyMissing);
    Check(Contains(r, "Start the game from Steam") && Contains(r, "SteamGameId"), "Proton, no VR key: not a Steam launch");
    r = xrm::LoaderFailureReason(xrm::kXrErrorRuntimeUnavailable, newProton, 0);
    Check(Contains(r, "did not finish in time"), "Proton, VR state 0: timeout");
    r = xrm::LoaderFailureReason(xrm::kXrErrorRuntimeUnavailable, newProton, 1);
    Check(Contains(r, "wineopenxr found no host OpenXR runtime") && Contains(r, "Monado"), "Proton, VR state 1: host runtime");
    r = xrm::LoaderFailureReason(xrm::kXrErrorRuntimeUnavailable, steamvr, xrm::kWineVrStateUnknown);
    Check(Contains(r, "steamxr_win32.json") && Contains(r, "is not available") && Contains(r, "(result -51)"),
        "Windows, registered 32-bit runtime unavailable");
    r = xrm::LoaderFailureReason(xrm::kXrErrorRuntimeUnavailable, envOverride, xrm::kWineVrStateUnknown);
    Check(Contains(r, "D:\\dev\\runtime.json") && Contains(r, "is not available"), "Windows, XR_RUNTIME_JSON runtime unavailable: named");
    r = xrm::LoaderFailureReason(xrm::kXrErrorRuntimeUnavailable, none, xrm::kWineVrStateUnknown);
    Check(Contains(r, "no active OpenXR runtime with a 32-bit manifest"), "Windows, nothing known: the M2 wording plus the fix");

    // ---- 32-bit registration that does not match the active runtime ---------
    const std::string sv64 = "C:\\Program Files (x86)\\Steam\\steamapps\\common\\SteamVR\\steamxr_win64.json";
    const std::string sv32 = "C:\\Program Files (x86)\\Steam\\steamapps\\common\\SteamVR\\steamxr_win32.json";
    Check(xrm::ThirtyTwoBitSibling(sv64) == sv32, "steamxr_win64.json -> steamxr_win32.json");
    Check(xrm::ThirtyTwoBitSibling("C:\\Oculus\\oculus_openxr_64.json") == "C:\\Oculus\\oculus_openxr_32.json", "_64 -> _32");
    Check(xrm::ThirtyTwoBitSibling("C:\\x64\\virtualdesktop-openxr.json").empty(), "no 64 in the file name: no sibling");
    xrm::RuntimeFacts svOnly64;  // SteamVR set as the runtime before 2.17: only the 64-bit view written
    svOnly64.activeRuntime64 = sv64;
    svOnly64.sibling32Of64 = sv32;
    svOnly64.sibling32Of64Exists = true;
    xrm::RuntimeOverride o = xrm::Choose32BitRuntimeOverride(svOnly64);
    Check(o.manifest == sv32 && Contains(o.why, "no 32-bit OpenXR runtime is registered"), "64-bit-only SteamVR registration: its 32-bit manifest");
    xrm::RuntimeFacts svOther32 = svOnly64;  // an older 32-bit choice (Oculus) left behind
    svOther32.activeRuntime32 = "C:\\Oculus\\oculus_openxr_32.json";
    svOther32.activeRuntime32Exists = true;
    o = xrm::Choose32BitRuntimeOverride(svOther32);
    Check(o.manifest == sv32 && Contains(o.why, "oculus_openxr_32.json"), "SteamVR active, 32-bit view elsewhere: SteamVR");
    xrm::RuntimeFacts svMatch = svOnly64;
    svMatch.activeRuntime32 = "c:/program files (x86)/steam/steamapps/common/steamvr/STEAMXR_WIN32.json";
    svMatch.activeRuntime32Exists = true;
    Check(xrm::Choose32BitRuntimeOverride(svMatch).manifest.empty(), "matching registration (case, slashes): untouched");
    xrm::RuntimeFacts oculusOther32;  // Oculus active, SteamVR 32-bit: runtimes that write both views are trusted
    oculusOther32.activeRuntime64 = "C:\\Oculus\\oculus_openxr_64.json";
    oculusOther32.sibling32Of64 = "C:\\Oculus\\oculus_openxr_32.json";
    oculusOther32.sibling32Of64Exists = true;
    oculusOther32.activeRuntime32 = sv32;
    oculusOther32.activeRuntime32Exists = true;
    Check(xrm::Choose32BitRuntimeOverride(oculusOther32).manifest.empty(), "non-SteamVR active with its own 32-bit choice: untouched");
    xrm::RuntimeFacts svWine = svOnly64;
    svWine.wine = true;
    Check(xrm::Choose32BitRuntimeOverride(svWine).manifest.empty(), "never under Proton");
    xrm::RuntimeFacts svEnv = svOnly64;
    svEnv.runtimeJsonEnv = "D:\\dev\\runtime.json";
    Check(xrm::Choose32BitRuntimeOverride(svEnv).manifest.empty(), "never over the player's XR_RUNTIME_JSON");
    xrm::RuntimeFacts oldSteamVr = svOnly64;  // SteamVR before 2.17: no steamxr_win32.json
    oldSteamVr.sibling32Of64Exists = false;
    Check(xrm::Choose32BitRuntimeOverride(oldSteamVr).manifest.empty(), "old SteamVR: nothing to switch to");
    p = xrm::CheckPreflight(oldSteamVr);
    Check(!p.tryRuntime && Contains(p.reason, "Update SteamVR to 2.17") && Contains(p.reason, "steamxr_win32.json is missing"),
        "old SteamVR: says to update SteamVR");
    r = xrm::LoaderFailureReason(-38, steamvr, xrm::kWineVrStateUnknown);
    Check(r == "the OpenXR loader failed (result -38)", "other loader errors keep their code");
    Check(xrm::IsRuntimeUnavailableResult(xrm::kXrErrorInstanceLost) && xrm::IsRuntimeUnavailableResult(xrm::kXrErrorRuntimeFailure) &&
        xrm::IsRuntimeUnavailableResult(xrm::kXrErrorInitializationFailed) && !xrm::IsRuntimeUnavailableResult(-38),
        "which results mean no runtime");
    Check(Contains(xrm::NoVulkanReason(), "XR_KHR_vulkan_enable") && Contains(xrm::NoVulkanReason(), "Windows Mixed Reality"),
        "no Vulkan: names the extension and WMR");
    Check(Contains(xrm::NoHeadsetReason("SteamVR/OpenXR"), "'SteamVR/OpenXR' found no headset"), "no headset: names the runtime");
    Check(Contains(xrm::NoHeadsetReason(""), "the OpenXR runtime found no headset"), "no headset without a name");

    // ---- In-game console text ------------------------------------------------
    const std::string safe = xrm::ConsoleSafe("a \"quoted\"; 'b'\nc");
    Check(safe == "a quoted, b,c", "console text drops quotes and splits no commands");
    Check(xrm::ConsoleSafe(std::string(600, 'x'), 100).size() == 100, "console text is bounded");

    // ---- openvrpaths.vrpath ---------------------------------------------------
    const char* windowsFile =
        "{\n\t\"config\" : [ \"C:\\\\Program Files (x86)\\\\Steam\\\\config\" ],\n"
        "\t\"external_drivers\" : null,\n\t\"jsonid\" : \"vrpathreg\",\n"
        "\t\"log\" : [ \"C:\\\\Program Files (x86)\\\\Steam\\\\logs\" ],\n"
        "\t\"runtime\" : [ \"C:\\\\Program Files (x86)\\\\Steam\\\\steamapps\\\\common\\\\SteamVR\\\\\", \"D:\\\\Other\" ],\n"
        "\t\"version\" : 1\n}\n";
    Check(ovp::FirstRuntime(windowsFile) == "C:\\Program Files (x86)\\Steam\\steamapps\\common\\SteamVR",
        "Windows SteamVR path, escapes decoded, trailing separator dropped");
    Check(ovp::RuntimeEntries(windowsFile).size() == 2, "both runtime entries are read");
    Check(ovp::FirstRuntime("{\"jsonid\":\"vrpathreg\",\"runtime\":[\"/home/u/xrizer\"],\"version\":1}") == "/home/u/xrizer",
        "compact Linux file (installer / Envision style)");
    Check(ovp::FirstRuntime("{\"runtime\":[]}").empty(), "empty runtime list");
    Check(ovp::FirstRuntime("{\"config\":[\"x\"]}").empty(), "no runtime key");
    Check(ovp::FirstRuntime("{\"nested\":{\"runtime\":[\"wrong\"]},\"runtime\":[\"right\"]}") == "right",
        "only the top-level runtime key counts");
    Check(ovp::FirstRuntime("{\"runtime\":[\"C:\\\\vrclient\\\\\"]}") == "C:\\vrclient", "Proton's stub path");
    Check(ovp::FirstRuntime("{\"runtime\":[\"broken").empty(), "truncated file");
    Check(ovp::FirstRuntime("not json").empty(), "garbage");
    Check(ovp::FirstRuntime("{\"runtime\":[\"\\u00e9t\\u00e9\"]}") == "\xC3\xA9t\xC3\xA9", "\\u escapes become UTF-8");
    Check(ovp::FirstRuntime("{\"a\":\"{[\",\"runtime\":[\"ok\"]}") == "ok", "brackets inside strings are ignored");

    std::printf("vr_openxr_diagnosis: %d checks passed\n", checks);
    return 0;
}

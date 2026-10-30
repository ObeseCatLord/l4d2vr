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
    Check(Contains(r, "no active OpenXR runtime with a 32-bit manifest"), "Windows, nothing known: the M2 wording plus the fix");
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

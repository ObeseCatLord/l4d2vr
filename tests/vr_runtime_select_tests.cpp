#include "../L4D2VR/vr_runtime_select.h"
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
    namespace sel = l4d2vr_runtime_select;
    using sel::Preference;
    using sel::Runtime;
    using sel::Switch;

    // Config values: case-insensitive, trimmed, quotes allowed, empty = auto.
    Preference p = Preference::OpenVR;
    Check(sel::ParsePreference("auto", p) && p == Preference::Auto, "auto parses");
    Check(sel::ParsePreference("", p) && p == Preference::Auto, "empty value is auto");
    Check(sel::ParsePreference("   ", p) && p == Preference::Auto, "blank value is auto");
    Check(sel::ParsePreference("openxr", p) && p == Preference::OpenXR, "openxr parses");
    Check(sel::ParsePreference(" OpenXR\t", p) && p == Preference::OpenXR, "openxr is case-insensitive and trimmed");
    Check(sel::ParsePreference("OPENVR", p) && p == Preference::OpenVR, "openvr parses in upper case");
    Check(sel::ParsePreference("\"openvr\"", p) && p == Preference::OpenVR, "double-quoted value parses");
    Check(sel::ParsePreference("' openxr '", p) && p == Preference::OpenXR, "single-quoted padded value parses");
    Check(!sel::ParsePreference("steamvr", p) && p == Preference::Auto, "unknown value is rejected as auto");
    Check(!sel::ParsePreference("open xr", p) && p == Preference::Auto, "near-miss value is rejected");
    Check(!sel::ParsePreference("\"", p) && p == Preference::Auto, "lone quote is rejected");

    // Launch options: exact names, case-insensitive; both together = Both.
    Check(sel::AccumulateSwitch(Switch::None, L"-l4d2vr_openxr") == Switch::OpenXR, "-l4d2vr_openxr is seen");
    Check(sel::AccumulateSwitch(Switch::None, L"-L4D2VR_OPENVR") == Switch::OpenVR, "-l4d2vr_openvr is case-insensitive");
    Check(sel::AccumulateSwitch(Switch::None, L"-l4d2vr_openxr2") == Switch::None, "longer argument does not match");
    Check(sel::AccumulateSwitch(Switch::None, L"-l4d2vrdesktop") == Switch::None, "other mod switches do not match");
    Check(sel::AccumulateSwitch(Switch::None, L"l4d2vr_openxr") == Switch::None, "the dash is required");
    Check(sel::AccumulateSwitch(Switch::OpenXR, L"-novid") == Switch::OpenXR, "unrelated arguments keep the switch");
    Check(sel::AccumulateSwitch(Switch::OpenXR, L"-l4d2vr_openxr") == Switch::OpenXR, "a repeated switch stays itself");
    Check(sel::AccumulateSwitch(Switch::OpenXR, L"-l4d2vr_openvr") == Switch::Both, "both switches are a conflict");
    Check(sel::AccumulateSwitch(Switch::Both, L"-l4d2vr_openxr") == Switch::Both, "a conflict stays a conflict");

    // Decision without an OpenXR backend (M1): always OpenVR.
    auto d = sel::Decide(Preference::Auto, true, Switch::None, false);
    Check(d.runtime == Runtime::OpenVR && d.requested == Preference::Auto && !d.fromCommandLine && !d.openXrUnavailable,
        "auto selects OpenVR");
    d = sel::Decide(Preference::OpenVR, true, Switch::None, false);
    Check(d.runtime == Runtime::OpenVR && !d.openXrUnavailable, "openvr selects OpenVR");
    d = sel::Decide(Preference::OpenXR, true, Switch::None, false);
    Check(d.runtime == Runtime::OpenVR && d.requested == Preference::OpenXR && d.openXrUnavailable,
        "openxr without the backend falls back to OpenVR");
    Check(Contains(sel::Describe(d, "openxr"), "OpenXR backend not built yet"), "the fallback is logged");
    Check(Contains(sel::Describe(d, "openxr"), "VR runtime: OpenVR (VrRuntime=openxr)"), "the log names the config request");
    d = sel::Decide(Preference::Auto, true, Switch::OpenXR, false);
    Check(d.runtime == Runtime::OpenVR && d.fromCommandLine && d.openXrUnavailable, "-l4d2vr_openxr without the backend falls back");
    Check(Contains(sel::Describe(d, ""), "(-l4d2vr_openxr)"), "the log names the launch option");

    // Launch options override the config key.
    d = sel::Decide(Preference::OpenXR, true, Switch::OpenVR, true);
    Check(d.runtime == Runtime::OpenVR && d.fromCommandLine && !d.openXrUnavailable, "-l4d2vr_openvr beats VrRuntime=openxr");
    d = sel::Decide(Preference::OpenVR, true, Switch::OpenXR, true);
    Check(d.runtime == Runtime::OpenXR && d.fromCommandLine, "-l4d2vr_openxr beats VrRuntime=openvr");
    d = sel::Decide(Preference::OpenXR, true, Switch::Both, true);
    Check(d.runtime == Runtime::OpenVR && d.conflictingSwitches, "both launch options pick OpenVR");
    Check(Contains(sel::Describe(d, ""), "using -l4d2vr_openvr"), "the conflict is logged");

    // With an OpenXR backend: only an explicit request selects it; auto stays OpenVR until parity.
    d = sel::Decide(Preference::OpenXR, true, Switch::None, true);
    Check(d.runtime == Runtime::OpenXR && !d.openXrUnavailable, "openxr selects OpenXR when built");
    Check(!Contains(sel::Describe(d, "openxr"), "not built"), "no fallback note when OpenXR is built");
    d = sel::Decide(Preference::Auto, true, Switch::None, true);
    Check(d.runtime == Runtime::OpenVR, "auto stays on OpenVR until OpenXR reaches parity");

    // Unknown config values read as auto and are reported.
    d = sel::Decide(Preference::Auto, false, Switch::None, false);
    Check(d.runtime == Runtime::OpenVR && d.unknownConfigValue && d.requested == Preference::Auto, "unknown value is auto");
    Check(Contains(sel::Describe(d, "steamvr"), "unknown VrRuntime value 'steamvr' treated as auto"), "unknown value is logged");
    d = sel::Decide(Preference::OpenXR, false, Switch::None, true);
    Check(d.runtime == Runtime::OpenVR, "an invalid parse never selects OpenXR");

    // M2: the OpenXR runtime probe runs only for an explicit OpenXR request.
    int probes = 0;
    auto usable = [&](std::string& reason) { ++probes; reason.clear(); return true; };
    auto noRuntime = [&](std::string& reason) { ++probes; reason = "no active OpenXR runtime with a 32-bit manifest"; return false; };
    d = sel::DecideWithProbe(Preference::Auto, true, Switch::None, true, noRuntime);
    Check(d.runtime == Runtime::OpenVR && !d.openXrProbed && probes == 0, "auto never probes OpenXR");
    d = sel::DecideWithProbe(Preference::OpenVR, true, Switch::None, true, noRuntime);
    Check(d.runtime == Runtime::OpenVR && probes == 0, "openvr never probes OpenXR");
    d = sel::DecideWithProbe(Preference::OpenXR, true, Switch::OpenVR, true, noRuntime);
    Check(d.runtime == Runtime::OpenVR && probes == 0, "-l4d2vr_openvr over VrRuntime=openxr never probes OpenXR");
    d = sel::DecideWithProbe(Preference::OpenXR, true, Switch::None, true, usable);
    Check(d.runtime == Runtime::OpenXR && d.openXrProbed && !d.openXrUnavailable && probes == 1, "openxr with a usable runtime selects OpenXR");
    d = sel::DecideWithProbe(Preference::Auto, true, Switch::OpenXR, true, usable);
    Check(d.runtime == Runtime::OpenXR && d.fromCommandLine && probes == 2, "-l4d2vr_openxr with a usable runtime selects OpenXR");
    Check(sel::Describe(d, "") == "VR runtime: OpenXR (-l4d2vr_openxr)", "the OpenXR selection log line");
    d = sel::DecideWithProbe(Preference::OpenXR, true, Switch::None, true, noRuntime);
    Check(d.runtime == Runtime::OpenVR && d.openXrUnavailable && d.openXrProbed && probes == 3,
        "openxr without a runtime declines to OpenVR");
    Check(Contains(sel::Describe(d, "openxr"), "OpenXR runtime unavailable (no active OpenXR runtime with a 32-bit manifest), falling back to OpenVR"),
        "the decline reason is logged");
    d = sel::DecideWithProbe(Preference::OpenXR, true, Switch::None, true, [](std::string&) { return false; });
    Check(d.runtime == Runtime::OpenVR && Contains(sel::Describe(d, ""), "(unknown error)"), "a silent probe failure still declines with a reason");
    d = sel::DecideWithProbe(Preference::OpenXR, true, Switch::None, false, usable);
    Check(d.runtime == Runtime::OpenVR && probes == 3 && Contains(sel::Describe(d, ""), "not built yet"),
        "a build without the backend never probes");
    d = sel::DecideWithProbe(Preference::OpenXR, true, Switch::None, true, sel::OpenXrProbe());
    Check(d.runtime == Runtime::OpenVR && d.openXrUnavailable, "no probe function declines");

    // M6 release (OpenXR-only) policy: always OpenXR, OpenVR requests ignored and said so, no probe.
    const int probesBefore = probes;
    d = sel::DecideOpenXrOnly(Preference::Auto, true, Switch::None);
    Check(d.runtime == Runtime::OpenXR && d.openXrOnlyBuild && !d.openVrRequestIgnored && !d.openXrProbed,
        "release: auto selects OpenXR");
    Check(sel::Describe(d, "auto") == "VR runtime: OpenXR (VrRuntime=auto); OpenXR-only build", "release: log line for auto");
    d = sel::DecideOpenXrOnly(Preference::OpenVR, true, Switch::None);
    Check(d.runtime == Runtime::OpenXR && d.openVrRequestIgnored, "release: VrRuntime=openvr is ignored");
    Check(Contains(sel::Describe(d, "openvr"), "VR runtime: OpenXR (VrRuntime=openvr); OpenVR is not part of this build"),
        "release: the ignored openvr request is logged");
    Check(Contains(sel::Describe(d, "openvr"), "L4D2VR_LEGACY_OPENVR"), "release: the log names the legacy build option");
    d = sel::DecideOpenXrOnly(Preference::Auto, true, Switch::OpenVR);
    Check(d.runtime == Runtime::OpenXR && d.openVrRequestIgnored && d.fromCommandLine, "release: -l4d2vr_openvr is ignored");
    Check(Contains(sel::Describe(d, ""), "(-l4d2vr_openvr); OpenVR is not part of this build"), "release: the ignored switch is logged");
    d = sel::DecideOpenXrOnly(Preference::OpenXR, true, Switch::Both);
    Check(d.runtime == Runtime::OpenXR && d.conflictingSwitches && d.openVrRequestIgnored, "release: both switches still OpenXR");
    Check(!Contains(sel::Describe(d, ""), "using -l4d2vr_openvr"), "release: the conflict line never claims OpenVR");
    d = sel::DecideOpenXrOnly(Preference::OpenXR, true, Switch::None);
    Check(d.runtime == Runtime::OpenXR && !d.openVrRequestIgnored && !d.openXrUnavailable, "release: openxr selects OpenXR");
    d = sel::DecideOpenXrOnly(Preference::Auto, false, Switch::None);
    Check(d.runtime == Runtime::OpenXR && d.unknownConfigValue && Contains(sel::Describe(d, "steamvr"), "'steamvr' treated as auto"),
        "release: an unknown value is still reported");
    d = sel::DecideForBuild(false, Preference::OpenVR, true, Switch::None, noRuntime);
    Check(d.runtime == Runtime::OpenXR && probes == probesBefore, "release: DecideForBuild never probes and never picks OpenVR");
    d = sel::DecideForBuild(true, Preference::OpenXR, true, Switch::None, noRuntime);
    Check(d.runtime == Runtime::OpenVR && d.openXrUnavailable && probes == probesBefore + 1,
        "legacy: DecideForBuild keeps the M2 probe and OpenVR fallback");
    d = sel::DecideForBuild(true, Preference::Auto, true, Switch::None, noRuntime);
    Check(d.runtime == Runtime::OpenVR && !d.openXrOnlyBuild, "legacy: auto stays OpenVR");

    // This test binary is built in the configuration under test (CMake L4D2VR_LEGACY_OPENVR).
    d = sel::DecideForBuild(sel::kLegacyOpenVrBuild, Preference::Auto, true, Switch::None, noRuntime);
#if defined(L4D2VR_LEGACY_OPENVR) && L4D2VR_LEGACY_OPENVR
    Check(sel::kLegacyOpenVrBuild && d.runtime == Runtime::OpenVR, "legacy configuration: auto = OpenVR");
    std::printf("configuration: legacy (OpenVR backend built)\n");
#else
    Check(!sel::kLegacyOpenVrBuild && d.runtime == Runtime::OpenXR, "release configuration: auto = OpenXR");
    std::printf("configuration: release (OpenXR only)\n");
#endif

    // Published values are stable (DXVK compares against 0 = none, 1 = OpenVR, 2 = OpenXR).
    Check(static_cast<int>(Runtime::None) == 0, "published none value");
    Check(std::string(sel::Name(Runtime::None)) == "none", "none name");
    Check(static_cast<int>(Runtime::OpenVR) == 1 && static_cast<int>(Runtime::OpenXR) == 2, "published runtime values");
    Check(std::string(sel::Name(Runtime::OpenXR)) == "OpenXR" && std::string(sel::Name(Preference::OpenVR)) == "openvr", "names");

    std::printf("vr_runtime_select: %d checks passed\n", checks);
    return 0;
}

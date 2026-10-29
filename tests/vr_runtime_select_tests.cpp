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

    // Published values are stable (DXVK compares against 1 = OpenVR).
    Check(static_cast<int>(Runtime::OpenVR) == 1 && static_cast<int>(Runtime::OpenXR) == 2, "published runtime values");
    Check(std::string(sel::Name(Runtime::OpenXR)) == "OpenXR" && std::string(sel::Name(Preference::OpenVR)) == "openvr", "names");

    std::printf("vr_runtime_select: %d checks passed\n", checks);
    return 0;
}

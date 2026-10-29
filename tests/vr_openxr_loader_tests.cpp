// The bundled 32-bit Khronos loader and the OpenXR backend's first stage
// (L4D2VR/vr_backend/openxr_loader_probe.h), run for real on Windows: the
// loader next to this test loads, and without an OpenXR runtime (the build VM)
// the probe declines cleanly with a readable reason instead of crashing. With
// a runtime (a player's PC) it must report XR_KHR_vulkan_enable or decline.
#include "../L4D2VR/vr_backend/openxr_loader_probe.h"
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

static std::string ExeDirectory()
{
    char path[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameA(nullptr, path, MAX_PATH);
    std::string dir(path, n);
    const size_t slash = dir.find_last_of("\\/");
    return slash == std::string::npos ? std::string(".") : dir.substr(0, slash);
}

int main()
{
    namespace xrm = l4d2vr_openxr;
    namespace sel = l4d2vr_runtime_select;

    // A missing loader declines with a reason.
    xrm::LoaderProbe missing;
    Check(!xrm::ProbeLoader(ExeDirectory() + "\\no_such_dir\\openxr_loader.dll", false, missing), "missing loader declines");
    Check(missing.module == nullptr && missing.reason.find("openxr_loader.dll not found") == 0, "missing loader reason");

    // The bundled loader (copied next to this test by CMake).
    const std::string loaderPath = ExeDirectory() + "\\openxr_loader.dll";
    HMODULE raw = LoadLibraryA(loaderPath.c_str());
    Check(raw != nullptr, "the bundled 32-bit openxr_loader.dll loads in a 32-bit process");
    Check(GetProcAddress(raw, "xrGetInstanceProcAddr") != nullptr, "the loader exports xrGetInstanceProcAddr");
    FreeLibrary(raw);

    xrm::LoaderProbe probe;
    const DWORD start = GetTickCount();
    const bool usable = xrm::ProbeLoader(loaderPath, false, probe);
    const DWORD elapsed = GetTickCount() - start;
    if (usable)
    {
        Check(probe.module != nullptr && probe.extensions.count("XR_KHR_vulkan_enable") == 1, "a usable runtime offers XR_KHR_vulkan_enable");
        std::printf("note: an OpenXR runtime is active here (%zu extensions)\n", probe.extensions.size());
        FreeLibrary(probe.module);
    }
    else
    {
        Check(probe.module == nullptr, "a declined probe unloads the loader");
        Check(!probe.reason.empty(), "a declined probe has a reason");
        std::printf("note: no usable OpenXR runtime here: %s (result %d, %lu ms)\n", probe.reason.c_str(),
            static_cast<int>(probe.enumerateResult), static_cast<unsigned long>(elapsed));
        if (probe.enumerateResult == XR_ERROR_RUNTIME_UNAVAILABLE)
            Check(probe.reason == "no active OpenXR runtime with a 32-bit manifest", "RUNTIME_UNAVAILABLE reason");
    }
    Check(elapsed < 10000, "the probe answers quickly");

    // The selection policy with that real probe: an explicit OpenXR request
    // without a runtime falls back to OpenVR and says why.
    std::string reasonSeen;
    const sel::Decision decision = sel::DecideWithProbe(sel::Preference::OpenXR, true, sel::Switch::None, true,
        [&](std::string& reason) {
            xrm::LoaderProbe p;
            const bool ok = xrm::ProbeLoader(loaderPath, false, p);
            if (p.module)
                FreeLibrary(p.module);
            reason = p.reason;
            reasonSeen = p.reason;
            return ok;
        });
    if (usable)
        Check(decision.runtime == sel::Runtime::OpenXR, "with a runtime, openxr selects OpenXR");
    else
    {
        Check(decision.runtime == sel::Runtime::OpenVR && decision.openXrUnavailable, "without a runtime, openxr falls back to OpenVR");
        const std::string line = sel::Describe(decision, "openxr");
        Check(line.find(reasonSeen) != std::string::npos, "the fallback line names the reason");
        std::printf("log line: [VR] %s\n", line.c_str());
    }

    std::printf("vr_openxr_loader: %d checks passed\n", checks);
    return 0;
}

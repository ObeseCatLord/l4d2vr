#pragma once

// First stage of the OpenXR backend (OpenXR port M2): load the bundled Khronos
// loader and ask it for the active runtime's instance extensions. Header-only
// (Win32 + OpenXR headers, no mod code) so the "no runtime" path can be tested
// on Windows without a game (tests/vr_openxr_loader_tests.cpp).
//
// Outcomes the selection policy relies on (vr_runtime_select.h): a missing
// loader, no active runtime (XR_ERROR_RUNTIME_UNAVAILABLE, e.g. no 32-bit
// manifest or a Proton without 32-bit wineopenxr) and a runtime without
// XR_KHR_vulkan_enable all decline with a readable reason and never crash.

#include <Windows.h>

#ifndef XR_NO_PROTOTYPES
#define XR_NO_PROTOTYPES
#endif
#include <openxr/openxr.h>

#include <set>
#include <string>
#include <vector>

namespace l4d2vr_openxr
{
    struct LoaderProbe
    {
        HMODULE module = nullptr;                          // stays loaded on success
        PFN_xrGetInstanceProcAddr getInstanceProcAddr = nullptr;
        XrResult enumerateResult = XR_SUCCESS;
        std::set<std::string> extensions;
        std::string reason;                                // empty on success
    };

    inline std::string RuntimeUnavailableReason(XrResult result, bool wine)
    {
        if (result == XR_ERROR_RUNTIME_UNAVAILABLE || result == XR_ERROR_INSTANCE_LOST || result == XR_ERROR_RUNTIME_FAILURE)
        {
            if (wine)
                return "no active OpenXR runtime (Proton needs Hotfix or Experimental with PROTON_USE_WOW64=1, "
                       "a Steam launch and an OpenVR runtime such as XRizer)";
            return "no active OpenXR runtime with a 32-bit manifest";
        }
        return "the OpenXR loader failed (" + std::to_string(static_cast<int>(result)) + ")";
    }

    // Loads loaderPath and enumerates the runtime's instance extensions.
    // True when the runtime is reachable and offers XR_KHR_vulkan_enable.
    // On failure the loader is unloaded again and `reason` says why.
    inline bool ProbeLoader(const std::string& loaderPath, bool wine, LoaderProbe& out)
    {
        out = LoaderProbe();
        out.module = LoadLibraryA(loaderPath.c_str());
        if (!out.module)
        {
            out.reason = "openxr_loader.dll not found next to d3d9.dll (error " + std::to_string(GetLastError()) + ")";
            return false;
        }
        auto fail = [&](std::string why) {
            out.reason = std::move(why);
            FreeLibrary(out.module);
            out.module = nullptr;
            out.getInstanceProcAddr = nullptr;
            return false;
        };
        out.getInstanceProcAddr = reinterpret_cast<PFN_xrGetInstanceProcAddr>(GetProcAddress(out.module, "xrGetInstanceProcAddr"));
        if (!out.getInstanceProcAddr)
            return fail("openxr_loader.dll has no xrGetInstanceProcAddr");
        PFN_xrEnumerateInstanceExtensionProperties enumerate = nullptr;
        out.getInstanceProcAddr(XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties",
            reinterpret_cast<PFN_xrVoidFunction*>(&enumerate));
        if (!enumerate)
            return fail("openxr_loader.dll lacks xrEnumerateInstanceExtensionProperties");
        uint32_t count = 0;
        out.enumerateResult = enumerate(nullptr, 0, &count, nullptr);
        if (XR_FAILED(out.enumerateResult))
            return fail(RuntimeUnavailableReason(out.enumerateResult, wine));
        std::vector<XrExtensionProperties> properties(count, { XR_TYPE_EXTENSION_PROPERTIES });
        out.enumerateResult = enumerate(nullptr, count, &count, properties.data());
        if (XR_FAILED(out.enumerateResult))
            return fail(RuntimeUnavailableReason(out.enumerateResult, wine));
        for (uint32_t i = 0; i < count; ++i)
            out.extensions.insert(properties[i].extensionName);
        if (!out.extensions.count("XR_KHR_vulkan_enable"))
            return fail("the OpenXR runtime does not offer XR_KHR_vulkan_enable");
        return true;
    }
}

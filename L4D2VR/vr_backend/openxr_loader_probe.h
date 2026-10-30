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
// GatherRuntimeFacts + openxr_runtime_diagnosis.h turn the situation into a
// reason a player can act on (64-bit-only runtime, old Proton, no OpenVR
// runtime for Proton, ...).

#include <Windows.h>

#ifndef XR_NO_PROTOTYPES
#define XR_NO_PROTOTYPES
#endif
#include <openxr/openxr.h>

#include "openxr_runtime_diagnosis.h"

#include <set>
#include <string>
#include <vector>

namespace l4d2vr_openxr
{
    static_assert(kXrErrorRuntimeFailure == XR_ERROR_RUNTIME_FAILURE, "XrResult value");
    static_assert(kXrErrorInitializationFailed == XR_ERROR_INITIALIZATION_FAILED, "XrResult value");
    static_assert(kXrErrorInstanceLost == XR_ERROR_INSTANCE_LOST, "XrResult value");
    static_assert(kXrErrorFormFactorUnavailable == XR_ERROR_FORM_FACTOR_UNAVAILABLE, "XrResult value");
    static_assert(kXrErrorRuntimeUnavailable == XR_ERROR_RUNTIME_UNAVAILABLE, "XrResult value");

    // HKLM\SOFTWARE\Khronos\OpenXR\1 "ActiveRuntime" in one registry view
    // (KEY_WOW64_32KEY = what a 32-bit loader reads, KEY_WOW64_64KEY).
    inline std::string ReadActiveRuntime(REGSAM view)
    {
        HKEY key = nullptr;
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Khronos\\OpenXR\\1", 0, KEY_READ | view, &key) != ERROR_SUCCESS)
            return std::string();
        char buffer[2048] = {};
        DWORD size = sizeof(buffer) - 1, type = 0;
        const LONG r = RegQueryValueExA(key, "ActiveRuntime", nullptr, &type, reinterpret_cast<BYTE*>(buffer), &size);
        RegCloseKey(key);
        if (r != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ))
            return std::string();
        std::string value(buffer, strnlen(buffer, sizeof(buffer)));
        if (type == REG_EXPAND_SZ)
        {
            char expanded[2048] = {};
            const DWORD n = ExpandEnvironmentStringsA(value.c_str(), expanded, sizeof(expanded));
            if (n > 0 && n <= sizeof(expanded))
                value = expanded;
        }
        return value;
    }

    inline bool FileExists(const std::string& path)
    {
        if (path.empty())
            return false;
        const DWORD attributes = GetFileAttributesA(path.c_str());
        return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
    }

    // Everything Preflight needs; cheap (registry and file checks, no loader).
    inline RuntimeFacts GatherRuntimeFacts(bool wine)
    {
        RuntimeFacts facts;
        facts.wine = wine;
        facts.activeRuntime32 = ReadActiveRuntime(KEY_WOW64_32KEY);
        facts.activeRuntime64 = ReadActiveRuntime(KEY_WOW64_64KEY);
        facts.activeRuntime32Exists = FileExists(facts.activeRuntime32);
        char value[1024] = {};
        const DWORD n = GetEnvironmentVariableA("XR_RUNTIME_JSON", value, sizeof(value));
        if (n > 0 && n < sizeof(value))
            facts.runtimeJsonEnv = value;
        if (wine)
        {
            // The 32-bit wineopenxr lives in syswow64 (Proton Hotfix >= 20261007,
            // Experimental >= 11.0-20261014); older Protons only have the 64-bit one.
            char dir[MAX_PATH] = {};
            UINT len = GetSystemWow64DirectoryA(dir, MAX_PATH);
            if (len == 0 || len >= MAX_PATH)
                len = GetSystemDirectoryA(dir, MAX_PATH);
            if (len > 0 && len < MAX_PATH)
                facts.wineOpenXr32 = FileExists(std::string(dir) + "\\wineopenxr.dll");
        }
        return facts;
    }

    struct LoaderProbe
    {
        HMODULE module = nullptr;                          // stays loaded on success
        PFN_xrGetInstanceProcAddr getInstanceProcAddr = nullptr;
        XrResult enumerateResult = XR_SUCCESS;
        std::set<std::string> extensions;
        std::string reason;                                // empty on success
    };

    inline std::string RuntimeUnavailableReason(XrResult result, bool wine, const RuntimeFacts* facts, int wineVrState)
    {
        if (facts)
            return LoaderFailureReason(static_cast<int>(result), *facts, wineVrState);
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
    // With `facts` (GatherRuntimeFacts) the failure reasons come from
    // openxr_runtime_diagnosis.h; without, the short M2 wording.
    inline bool ProbeLoader(const std::string& loaderPath, bool wine, LoaderProbe& out,
        const RuntimeFacts* facts = nullptr, int wineVrState = kWineVrStateUnknown)
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
            return fail(RuntimeUnavailableReason(out.enumerateResult, wine, facts, wineVrState));
        std::vector<XrExtensionProperties> properties(count, { XR_TYPE_EXTENSION_PROPERTIES });
        out.enumerateResult = enumerate(nullptr, count, &count, properties.data());
        if (XR_FAILED(out.enumerateResult))
            return fail(RuntimeUnavailableReason(out.enumerateResult, wine, facts, wineVrState));
        for (uint32_t i = 0; i < count; ++i)
            out.extensions.insert(properties[i].extensionName);
        if (!out.extensions.count("XR_KHR_vulkan_enable"))
            return fail(facts ? NoVulkanReason() : std::string("the OpenXR runtime does not offer XR_KHR_vulkan_enable"));
        return true;
    }
}

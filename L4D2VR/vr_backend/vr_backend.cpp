// Backend choice and the D3D9 device bring-up shared by both backends.
//
// The release build is OpenXR-only: the OpenVR backend (vr_backend_openvr.cpp)
// is compiled only into legacy developer builds (L4D2VR_LEGACY_OPENVR=1).

#include "vr_backend.h"

#include <cstdio>

namespace l4d2vr_backend
{
namespace
{
    IVrBackend& CreateBackend(int selected)
    {
#if defined(L4D2VR_LEGACY_OPENVR) && L4D2VR_LEGACY_OPENVR
        if (selected == static_cast<int>(RuntimeKind::OpenVR))
            return OpenVrBackendInstance();
#else
        (void)selected;
#endif
        // OpenXR, and also "no runtime" (-nohmd, desktop mode): the OpenXR
        // backend stays inert until something calls Init.
        return OpenXrBackendInstance();
    }
}

IVrBackend& Active()
{
    static IVrBackend& backend = CreateBackend(L4D2VR_SelectedVrRuntime());
    return backend;
}
}

extern "C" bool L4D2VR_VrBackendInitForDevice(uint32_t* renderWidth, uint32_t* renderHeight, char* errorString, uint32_t errorStringSize)
{
    l4d2vr_backend::IVrBackend& backend = l4d2vr_backend::Active();
    const char* initError = nullptr;
    if (!backend.Init(initError))
    {
        if (errorString && errorStringSize)
            std::snprintf(errorString, errorStringSize, "%s: %s", l4d2vr_backend::InitFailurePrefix(backend),
                initError ? initError : "");
        return false;
    }
    l4d2vr_backend::IVrSystem* system = backend.System();
    if (!system)
    {
        if (errorString && errorStringSize)
            std::snprintf(errorString, errorStringSize, "VR_Init failed: %s runtime has no system interface", backend.Name());
        return false;
    }
    system->GetRecommendedRenderTargetSize(renderWidth, renderHeight);
    return true;
}

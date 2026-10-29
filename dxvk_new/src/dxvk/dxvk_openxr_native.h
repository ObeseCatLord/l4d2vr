#pragma once

#include <mutex>

#include "dxvk_extension_provider.h"

namespace dxvk {

  class DxvkInstance;

  /**
   * \brief OpenXR extension provider for the L4D2VR OpenXR backend
   *
   * Asks the mod's OpenXR instance (L4D2VR/vr_backend/vr_backend_openxr.cpp)
   * for the Vulkan instance and device extensions its runtime needs
   * (xrGetVulkanInstanceExtensionsKHR / xrGetVulkanDeviceExtensionsKHR,
   * XR_KHR_vulkan_enable). Works on native Windows runtimes and under
   * Proton's wineopenxr alike. Registered only when the mod selected OpenXR,
   * so OpenVR and desktop players never load the OpenXR loader.
   */
  class DxvkXrNativeProvider : public DxvkExtensionProvider {

  public:

    std::string_view getName();

    DxvkNameSet getInstanceExtensions();

    DxvkNameSet getDeviceExtensions(
            uint32_t      adapterId);

    void initInstanceExtensions();

    void initDeviceExtensions(
      const DxvkInstance* instance);

    static DxvkXrNativeProvider s_instance;

  private:

    dxvk::mutex m_mutex;
    bool        m_initializedInsExt = false;
    bool        m_initializedDevExt = false;
    DxvkNameSet m_insExtensions;
    DxvkNameSet m_devExtensions;

    static DxvkNameSet query(bool device);

  };

  /**
   * \brief The physical device the OpenXR runtime renders on
   *
   * xrGetVulkanGraphicsDeviceKHR for this VkInstance; VK_NULL_HANDLE when the
   * runtime names none. DxvkInstance::queryAdapters moves it to adapter 0:
   * L4D2 creates its D3D9 device on adapter 0, and the OpenXR session must be
   * created on this device.
   */
  VkPhysicalDevice L4D2VRGetOpenXrPhysicalDevice(VkInstance instance);

}

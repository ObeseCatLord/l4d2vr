#include "dxvk_instance.h"
#include "dxvk_openxr_native.h"

#include <sstream>
#include <vector>

// Defined by the mod (L4D2VR/vr_backend/vr_backend_openxr.cpp).
extern "C" bool L4D2VR_XrGetVulkanInstanceExtensions(char* buffer, uint32_t capacity, uint32_t* length);
extern "C" bool L4D2VR_XrGetVulkanDeviceExtensions(char* buffer, uint32_t capacity, uint32_t* length);
extern "C" bool L4D2VR_XrGetVulkanGraphicsDevice(void* vkInstance, void** physicalDevice);

namespace dxvk {

  DxvkXrNativeProvider DxvkXrNativeProvider::s_instance;


  std::string_view DxvkXrNativeProvider::getName() {
    return "OpenXR (L4D2VR)";
  }


  DxvkNameSet DxvkXrNativeProvider::getInstanceExtensions() {
    std::lock_guard<dxvk::mutex> lock(m_mutex);
    return m_insExtensions;
  }


  DxvkNameSet DxvkXrNativeProvider::getDeviceExtensions(uint32_t adapterId) {
    // xrGetVulkanDeviceExtensionsKHR is per system, not per adapter; the
    // runtime's adapter is pinned to index 0 and only the created device uses
    // them.
    std::lock_guard<dxvk::mutex> lock(m_mutex);
    return m_devExtensions;
  }


  void DxvkXrNativeProvider::initInstanceExtensions() {
    std::lock_guard<dxvk::mutex> lock(m_mutex);
    if (m_initializedInsExt)
      return;
    m_insExtensions = query(false);
    m_initializedInsExt = true;
  }


  void DxvkXrNativeProvider::initDeviceExtensions(const DxvkInstance* instance) {
    std::lock_guard<dxvk::mutex> lock(m_mutex);
    if (m_initializedDevExt)
      return;
    m_devExtensions = query(true);
    m_initializedDevExt = true;
  }


  DxvkNameSet DxvkXrNativeProvider::query(bool device) {
    auto fn = device ? &L4D2VR_XrGetVulkanDeviceExtensions : &L4D2VR_XrGetVulkanInstanceExtensions;
    uint32_t length = 0;
    if (!fn(nullptr, 0, &length) || length == 0) {
      Logger::warn(str::format("OpenXR (L4D2VR): no Vulkan ", device ? "device" : "instance", " extension list"));
      return DxvkNameSet();
    }
    std::vector<char> buffer(length + 1, '\0');
    if (!fn(buffer.data(), uint32_t(buffer.size()), &length))
      return DxvkNameSet();

    DxvkNameSet result;
    std::stringstream stream(std::string(buffer.data()));
    std::string name;
    while (std::getline(stream, name, ' ')) {
      if (!name.empty())
        result.add(name.c_str());
    }
    return result;
  }


  VkPhysicalDevice L4D2VRGetOpenXrPhysicalDevice(VkInstance instance) {
    void* runtimeDevice = nullptr;
    if (!L4D2VR_XrGetVulkanGraphicsDevice(reinterpret_cast<void*>(instance), &runtimeDevice) || !runtimeDevice)
      return VK_NULL_HANDLE;
    return reinterpret_cast<VkPhysicalDevice>(runtimeDevice);
  }

}

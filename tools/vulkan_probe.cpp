// Physical-device discovery only. This does not create a device or execute FG.
#include <vulkan/vulkan.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

std::string quoted(const std::string& text) {
  std::ostringstream result;
  result << '"';
  const char hex[] = "0123456789abcdef";
  for (unsigned char ch : text) {
    if (ch == '"' || ch == '\\') {
      result << '\\' << char(ch);
    } else if (ch < 32) {
      result << "\\u00" << hex[ch >> 4] << hex[ch & 15];
    } else {
      result << char(ch);
    }
  }
  result << '"';
  return result.str();
}

std::string version(uint32_t value) {
  return std::to_string(VK_API_VERSION_MAJOR(value)) + "." +
    std::to_string(VK_API_VERSION_MINOR(value)) + "." +
    std::to_string(VK_API_VERSION_PATCH(value));
}

void check(VkResult result, const char* operation) {
  if (result != VK_SUCCESS)
    throw std::runtime_error(std::string(operation) + ": VkResult " + std::to_string(result));
}

template<typename T, typename Function>
std::vector<T> enumerate(Function call, const char* operation) {
  for (int attempt = 0; attempt < 4; attempt++) {
    uint32_t count = 0;
    check(call(&count, nullptr), operation);
    std::vector<T> items(count);
    VkResult result = call(&count, items.data());
    if (result == VK_INCOMPLETE)
      continue;
    check(result, operation);
    items.resize(count);
    return items;
  }
  throw std::runtime_error(std::string(operation) + ": enumeration did not stabilize");
}

class Loader {
public:
  Loader() {
#ifdef _WIN32
    module = LoadLibraryW(L"vulkan-1.dll");
    if (module)
      get = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(module, "vkGetInstanceProcAddr"));
#else
    module = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (module)
      get = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(module, "vkGetInstanceProcAddr"));
#endif
    if (!module || !get)
      throw std::runtime_error("Vulkan loader unavailable");
  }

  ~Loader() {
#ifdef _WIN32
    if (module) FreeLibrary(module);
#else
    if (module) dlclose(module);
#endif
  }

  template<typename T>
  T function(VkInstance instance, const char* name) {
    auto address = get(instance, name);
    if (!address)
      throw std::runtime_error(std::string("Vulkan entry point unavailable: ") + name);
    return reinterpret_cast<T>(address);
  }

  PFN_vkGetInstanceProcAddr get = nullptr;

private:
#ifdef _WIN32
  HMODULE module = nullptr;
#else
  void* module = nullptr;
#endif
};

class Instance {
public:
  Instance(Loader& loader, uint32_t apiVersion) {
    auto create = loader.function<PFN_vkCreateInstance>(VK_NULL_HANDLE, "vkCreateInstance");
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "L4D2 FG research probe";
    app.apiVersion = apiVersion;
    VkInstanceCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    info.pApplicationInfo = &app;
    check(create(&info, nullptr, &handle), "vkCreateInstance");
    destroy = loader.function<PFN_vkDestroyInstance>(handle, "vkDestroyInstance");
  }
  ~Instance() {
    if (handle && destroy) destroy(handle, nullptr);
  }
  VkInstance handle = VK_NULL_HANDLE;
private:
  PFN_vkDestroyInstance destroy = nullptr;
};

std::string describe_device(Loader& loader, VkInstance instance, VkPhysicalDevice device,
                           const std::string& match, bool& selected) {
  auto getProperties = loader.function<PFN_vkGetPhysicalDeviceProperties2>(instance, "vkGetPhysicalDeviceProperties2");
  auto getFeatures = loader.function<PFN_vkGetPhysicalDeviceFeatures2>(instance, "vkGetPhysicalDeviceFeatures2");
  auto getQueues = loader.function<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(instance, "vkGetPhysicalDeviceQueueFamilyProperties");
  auto getFormats = loader.function<PFN_vkGetPhysicalDeviceFormatProperties>(instance, "vkGetPhysicalDeviceFormatProperties");
  auto getMemory = loader.function<PFN_vkGetPhysicalDeviceMemoryProperties>(instance, "vkGetPhysicalDeviceMemoryProperties");
  auto getExtensions = loader.function<PFN_vkEnumerateDeviceExtensionProperties>(instance, "vkEnumerateDeviceExtensionProperties");

  VkPhysicalDeviceProperties2 properties{};
  properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
  getProperties(device, &properties);
  bool hasCore12 = properties.properties.apiVersion >= VK_API_VERSION_1_2;
  VkPhysicalDeviceDriverProperties driver{};
  driver.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
  VkPhysicalDeviceSubgroupProperties subgroup{};
  subgroup.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES;
  subgroup.pNext = hasCore12 ? &driver : nullptr;
  properties.pNext = &subgroup;
  getProperties(device, &properties);

  VkPhysicalDeviceVulkan12Features features12{};
  features12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
  VkPhysicalDeviceFeatures2 features{};
  features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
  features.pNext = hasCore12 ? &features12 : nullptr;
  getFeatures(device, &features);
  auto extensions = enumerate<VkExtensionProperties>([&](uint32_t* count, VkExtensionProperties* data) {
    return getExtensions(device, nullptr, count, data);
  }, "vkEnumerateDeviceExtensionProperties");

  uint32_t queueCount = 0;
  getQueues(device, &queueCount, nullptr);
  std::vector<VkQueueFamilyProperties> queues(queueCount);
  getQueues(device, &queueCount, queues.data());
  queues.resize(queueCount);
  VkPhysicalDeviceMemoryProperties memory{};
  getMemory(device, &memory);
  selected = match.empty() || std::string(properties.properties.deviceName).find(match) != std::string::npos;
  std::ostringstream out;
  out << "{\"name\":" << quoted(properties.properties.deviceName)
      << ",\"selected\":" << (selected ? "true" : "false")
      << ",\"vendor_id\":" << properties.properties.vendorID
      << ",\"device_id\":" << properties.properties.deviceID
      << ",\"device_type\":" << properties.properties.deviceType
      << ",\"api_version\":" << quoted(version(properties.properties.apiVersion))
      << ",\"driver_version_raw\":" << properties.properties.driverVersion
      << ",\"driver_name\":" << (hasCore12 ? quoted(driver.driverName) : "null")
      << ",\"driver_info\":" << (hasCore12 ? quoted(driver.driverInfo) : "null")
      << ",\"supported_features\":{\"timeline_semaphore\":" << features12.timelineSemaphore
      << ",\"shader_float16\":" << features12.shaderFloat16
      << ",\"storage_image_read_without_format\":" << features.features.shaderStorageImageReadWithoutFormat
      << ",\"storage_image_write_without_format\":" << features.features.shaderStorageImageWriteWithoutFormat
      << ",\"storage_image_extended_formats\":" << features.features.shaderStorageImageExtendedFormats
      << "},\"subgroup\":{\"size\":" << subgroup.subgroupSize
      << ",\"supported_stages\":" << subgroup.supportedStages
      << ",\"supported_operations\":" << subgroup.supportedOperations
      << ",\"quad_operations_in_all_stages\":" << subgroup.quadOperationsInAllStages << "}"
      << ",\"queue_families\":[";
  for (size_t index = 0; index < queues.size(); index++) {
    if (index) out << ',';
    out << "{\"index\":" << index << ",\"count\":" << queues[index].queueCount
        << ",\"flags\":" << queues[index].queueFlags << ",\"surface_present_support\":null}";
  }
  out << "],\"formats\":[";
  const VkFormat formats[] = {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM,
    VK_FORMAT_R16G16_SFLOAT, VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32_UINT,
    VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT, VK_FORMAT_D32_SFLOAT_S8_UINT};
  for (size_t index = 0; index < sizeof(formats) / sizeof(formats[0]); index++) {
    VkFormatProperties format{};
    getFormats(device, formats[index], &format);
    if (index) out << ',';
    out << "{\"format\":" << formats[index] << ",\"optimal_tiling_features\":" << format.optimalTilingFeatures << "}";
  }
  out << "],\"memory_heaps\":[";
  for (uint32_t index = 0; index < memory.memoryHeapCount; index++) {
    if (index) out << ',';
    out << "{\"size_bytes\":" << memory.memoryHeaps[index].size
        << ",\"flags\":" << memory.memoryHeaps[index].flags << "}";
  }
  out << "],\"extensions\":[";
  for (size_t index = 0; index < extensions.size(); index++) {
    if (index) out << ',';
    out << quoted(extensions[index].extensionName);
  }
  out << "]}";
  return out.str();
}

int probe(const std::string& match) {
  Loader loader;
  uint32_t loaderVersion = VK_API_VERSION_1_0;
  auto enumerateVersion = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(
    loader.get(VK_NULL_HANDLE, "vkEnumerateInstanceVersion"));
  if (enumerateVersion) check(enumerateVersion(&loaderVersion), "vkEnumerateInstanceVersion");
  if (loaderVersion < VK_API_VERSION_1_1)
    throw std::runtime_error("Vulkan 1.1 required for this physical-device probe");
  Instance instance(loader, std::min(loaderVersion, uint32_t(VK_API_VERSION_1_3)));
  auto getDevices = loader.function<PFN_vkEnumeratePhysicalDevices>(instance.handle, "vkEnumeratePhysicalDevices");
  auto devices = enumerate<VkPhysicalDevice>([&](uint32_t* count, VkPhysicalDevice* data) {
    return getDevices(instance.handle, count, data);
  }, "vkEnumeratePhysicalDevices");
  std::vector<std::string> descriptions;
  uint32_t selectedCount = 0;
  for (auto device : devices) {
    bool selected = false;
    descriptions.push_back(describe_device(loader, instance.handle, device, match, selected));
    selectedCount += selected ? 1u : 0u;
  }
  std::cout << "{\"schema_version\":1,\"status\":"
            << quoted(selectedCount ? "observed" : "no_matching_device")
            << ",\"validation_level\":\"physical_device_queries_only\","
            << "\"gpu_fg_execution\":\"unrun\",\"logical_device_enabled_features\":null,"
            << "\"loader_version\":" << quoted(version(loaderVersion))
            << ",\"match\":" << quoted(match) << ",\"devices\":[";
  for (size_t index = 0; index < descriptions.size(); index++) {
    if (index) std::cout << ',';
    std::cout << descriptions[index];
  }
  std::cout << "]}\n";
  return selectedCount ? 0 : 3;
}

} // namespace

int main(int argc, char** argv) {
  std::string match;
  if (argc == 3 && std::string(argv[1]) == "--match") {
    match = argv[2];
  } else if (argc != 1) {
    std::cerr << "Usage: vulkan_probe [--match B580]\n";
    return 1;
  }
  try {
    return probe(match);
  } catch (const std::exception& error) {
    std::cout << "{\"schema_version\":1,\"status\":\"error\","
              << "\"validation_level\":\"physical_device_queries_only\","
              << "\"gpu_fg_execution\":\"unrun\",\"error\":" << quoted(error.what()) << "}\n";
    return 2;
  }
}

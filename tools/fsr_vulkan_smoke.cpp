// Headless functional test of the pinned official FI + OF shaders and host
// code. CPU fixture uploads/readbacks are test-only, never a proposed game IPC
// path.
#include "fsr_component_backend.h"
#include <array>
#include <atomic>
#include <cmath>
#include <csignal>
#include <cstring>
#include <execinfo.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <unistd.h>

namespace {
constexpr uint32_t W = 320, H = 192;
std::atomic<uint32_t> validationErrors{0};
void vkcheck(VkResult result, const char *op) {
  if (result != VK_SUCCESS)
    throw std::runtime_error(std::string(op) + ": " + std::to_string(result));
}
VKAPI_ATTR VkBool32 VKAPI_CALL
validation(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
           VkDebugUtilsMessageTypeFlagsEXT,
           const VkDebugUtilsMessengerCallbackDataEXT *data, void *) {
  if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
    ++validationErrors;
  std::cerr << data->pMessage << '\n';
  return VK_FALSE;
}
struct Driver {
  VkInstance instance{};
  VkDebugUtilsMessengerEXT messenger{};
  VkPhysicalDevice physical{};
  VkDevice device{};
  VkQueue queue{};
  uint32_t family = 0;
  VkCommandPool pool{};
  VkCommandBuffer cmd{};
  std::string name;
  Driver() {
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "Official FSR component smoke";
    app.apiVersion = VK_API_VERSION_1_2;
    const char *layer = "VK_LAYER_KHRONOS_validation";
    const char *extensions[]{VK_EXT_DEBUG_UTILS_EXTENSION_NAME,
                             VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME};
    VkDebugUtilsMessengerCreateInfoEXT debug{
        VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    debug.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT |
                            VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;
    debug.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                        VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                        VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    debug.pfnUserCallback = validation;
    VkValidationFeatureEnableEXT sync =
        VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
    VkValidationFeaturesEXT validationFeatures{
        VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT};
    validationFeatures.enabledValidationFeatureCount = 1;
    validationFeatures.pEnabledValidationFeatures = &sync;
    validationFeatures.pNext = &debug;
    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app;
    ci.enabledLayerCount = 1;
    ci.enabledExtensionCount = 2;
    ci.ppEnabledLayerNames = &layer;
    ci.ppEnabledExtensionNames = extensions;
    ci.pNext = &validationFeatures;
    vkcheck(vkCreateInstance(&ci, nullptr, &instance), "Instance");
    auto createDebug = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
    if (!createDebug)
      throw std::runtime_error("Validation messenger unavailable");
    vkcheck(createDebug(instance, &debug, nullptr, &messenger), "Validation");
    uint32_t count = 0;
    vkcheck(vkEnumeratePhysicalDevices(instance, &count, nullptr), "Devices");
    if (!count)
      throw std::runtime_error("No Vulkan device");
    std::vector<VkPhysicalDevice> devices(count);
    vkcheck(vkEnumeratePhysicalDevices(instance, &count, devices.data()),
            "Devices");
    physical = devices.front();
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(physical, &props);
    name = props.deviceName;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
    std::vector<VkQueueFamilyProperties> queues(count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, queues.data());
    for (; family < count; ++family)
      if (queues[family].queueFlags & VK_QUEUE_COMPUTE_BIT)
        break;
    if (family == count)
      throw std::runtime_error("No compute queue");
    VkPhysicalDeviceFeatures supported{}, features{};
    vkGetPhysicalDeviceFeatures(physical, &supported);
    if (!supported.shaderStorageImageExtendedFormats ||
        !supported.shaderStorageImageReadWithoutFormat ||
        !supported.shaderStorageImageWriteWithoutFormat)
      throw std::runtime_error("Missing SDK storage image features");
    features.shaderStorageImageExtendedFormats = VK_TRUE;
    features.shaderStorageImageReadWithoutFormat = VK_TRUE;
    features.shaderStorageImageWriteWithoutFormat = VK_TRUE;
    float priority = 1;
    VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qi.queueFamilyIndex = family;
    qi.queueCount = 1;
    qi.pQueuePriorities = &priority;
    VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    di.queueCreateInfoCount = 1;
    di.pQueueCreateInfos = &qi;
    di.pEnabledFeatures = &features;
    vkcheck(vkCreateDevice(physical, &di, nullptr, &device), "Device");
    vkGetDeviceQueue(device, family, 0, &queue);
    VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pi.queueFamilyIndex = family;
    vkcheck(vkCreateCommandPool(device, &pi, nullptr, &pool), "Command pool");
    VkCommandBufferAllocateInfo ai{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    vkcheck(vkAllocateCommandBuffers(device, &ai, &cmd), "Command buffer");
  }
  ~Driver() {
    if (device) {
      vkDeviceWaitIdle(device);
      if (pool)
        vkDestroyCommandPool(device, pool, nullptr);
      vkDestroyDevice(device, nullptr);
    }
    if (messenger)
      reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
          vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"))(
          instance, messenger, nullptr);
    if (instance)
      vkDestroyInstance(instance, nullptr);
  }
  uint32_t memory(uint32_t bits, VkMemoryPropertyFlags flags) {
    VkPhysicalDeviceMemoryProperties props{};
    vkGetPhysicalDeviceMemoryProperties(physical, &props);
    for (uint32_t i = 0; i < props.memoryTypeCount; ++i)
      if ((bits & (1u << i)) &&
          (props.memoryTypes[i].propertyFlags & flags) == flags)
        return i;
    throw std::runtime_error("No suitable memory type");
  }
  void begin() {
    vkcheck(vkResetCommandPool(device, pool, 0), "Reset pool");
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    vkcheck(vkBeginCommandBuffer(cmd, &bi), "Begin");
  }
  void submit() {
    vkcheck(vkEndCommandBuffer(cmd), "End");
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    vkcheck(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE), "Submit");
    vkcheck(vkQueueWaitIdle(queue), "Wait");
  }
};
struct Buffer {
  Driver &d;
  VkBuffer buffer{};
  VkDeviceMemory allocation{};
  void *mapped{};
  Buffer(Driver &driver, size_t size) : d(driver) {
    VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    ci.size = size;
    ci.usage =
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    vkcheck(vkCreateBuffer(d.device, &ci, nullptr, &buffer), "Buffer");
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(d.device, buffer, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex =
        d.memory(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    vkcheck(vkAllocateMemory(d.device, &ai, nullptr, &allocation),
            "Buffer memory");
    vkcheck(vkBindBufferMemory(d.device, buffer, allocation, 0), "Bind buffer");
    vkcheck(vkMapMemory(d.device, allocation, 0, size, 0, &mapped), "Map");
  }
  ~Buffer() {
    if (mapped)
      vkUnmapMemory(d.device, allocation);
    vkDestroyBuffer(d.device, buffer, nullptr);
    vkFreeMemory(d.device, allocation, nullptr);
  }
};
VkFormat format(FfxSurfaceFormat f) {
  switch (f) {
  case FFX_SURFACE_FORMAT_R8G8B8A8_UNORM:
    return VK_FORMAT_R8G8B8A8_UNORM;
  case FFX_SURFACE_FORMAT_R32_FLOAT:
    return VK_FORMAT_R32_SFLOAT;
  case FFX_SURFACE_FORMAT_R32_UINT:
    return VK_FORMAT_R32_UINT;
  case FFX_SURFACE_FORMAT_R16G16_FLOAT:
    return VK_FORMAT_R16G16_SFLOAT;
  case FFX_SURFACE_FORMAT_R16G16_SINT:
    return VK_FORMAT_R16G16_SINT;
  case FFX_SURFACE_FORMAT_R32G32_FLOAT:
    return VK_FORMAT_R32G32_SFLOAT;
  default:
    throw std::runtime_error("Unsupported external format");
  }
}
struct Image {
  Driver &d;
  VkImage image{};
  VkDeviceMemory memory{};
  FfxResource resource{};
  Image(Driver &driver, FfxResourceDescription desc) : d(driver) {
    if (desc.type != FFX_RESOURCE_TYPE_TEXTURE2D || desc.mipCount != 1)
      throw std::runtime_error("Unexpected SDK shared resource");
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = format(desc.format);
    ci.extent = {desc.width, desc.height, 1};
    ci.mipLevels = ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
               VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
               VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    vkcheck(vkCreateImage(d.device, &ci, nullptr, &image), "Image");
    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(d.device, image, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex =
        d.memory(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vkcheck(vkAllocateMemory(d.device, &ai, nullptr, &memory), "Image memory");
    vkcheck(vkBindImageMemory(d.device, image, memory, 0), "Bind image");
    resource =
        ffxGetResourceVK(reinterpret_cast<void *>(image), desc, L"fixture",
                         FFX_RESOURCE_STATE_UNORDERED_ACCESS);
    barrier(VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    VkClearColorValue zero{};
    VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(d.cmd, image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1,
                         &range);
  }
  Image(Driver &d, FfxSurfaceFormat f)
      : Image(d, {FFX_RESOURCE_TYPE_TEXTURE2D, f, W, H, 1, 1,
                  FFX_RESOURCE_FLAGS_NONE, FFX_RESOURCE_USAGE_UAV}) {}
  ~Image() {
    vkDestroyImage(d.device, image, nullptr);
    vkFreeMemory(d.device, memory, nullptr);
  }
  void barrier(VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.oldLayout = from;
    b.newLayout = to;
    b.srcAccessMask =
        from == VK_IMAGE_LAYOUT_UNDEFINED
            ? 0
            : VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(d.cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &b);
  }
  void upload(Buffer &buffer) {
    barrier(VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {W, H, 1};
    vkCmdCopyBufferToImage(d.cmd, buffer.buffer, image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    barrier(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
  }
  void download(Buffer &buffer) {
    barrier(VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {W, H, 1};
    vkCmdCopyImageToBuffer(d.cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           buffer.buffer, 1, &copy);
    barrier(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
    VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(d.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &b, 0, nullptr, 0,
                         nullptr);
  }
};
void ppm(const std::filesystem::path &path, const uint8_t *rgba) {
  std::ofstream out(path, std::ios::binary);
  out << "P6\n" << W << ' ' << H << "\n255\n";
  for (size_t p = 0; p < W * H; ++p)
    out.write(reinterpret_cast<const char *>(rgba + 4 * p), 3);
  if (!out)
    throw std::runtime_error("PPM write failed");
}
void run(const std::filesystem::path &out, bool negativeCurrent,
         bool negativeFormat) {
  std::filesystem::create_directories(out);
  Driver d;
  std::ofstream report(out / "frames.jsonl");
  std::cout << "{\"device\":\"" << d.name
            << "\",\"validation_enabled\":true}\n";
  // Moving, stationary, then moving with separate HUD-less scene input.
  for (int cycle = 0; cycle < 3; ++cycle) {
    ldb_fsr::Components sdk(d.device, d.physical, W, H);
    d.begin();
    Image color(d, FFX_SURFACE_FORMAT_R8G8B8A8_UNORM),
        hudless(d, FFX_SURFACE_FORMAT_R8G8B8A8_UNORM),
        depth(d, FFX_SURFACE_FORMAT_R32_FLOAT),
        motion(d, FFX_SURFACE_FORMAT_R32G32_FLOAT),
        result(d, FFX_SURFACE_FORMAT_R8G8B8A8_UNORM),
        flow(d, sdk.opticalResources.opticalFlowVector.resourceDescription),
        scd(d, sdk.opticalResources.opticalFlowSCD.resourceDescription),
        dilatedDepth(d, sdk.frameResources.dilatedDepth.resourceDescription),
        dilatedMotion(
            d, sdk.frameResources.dilatedMotionVectors.resourceDescription),
        previousDepth(d, sdk.frameResources.reconstructedPrevNearestDepth
                             .resourceDescription);
    d.submit();
    Buffer colorUpload(d, W * H * 4), hudlessUpload(d, W * H * 4),
        depthUpload(d, W * H * 4), motionUpload(d, W * H * 8),
        readback(d, W * H * 4);
    for (int frame = 0; frame < 8; ++frame) {
      // A static camera and a textured moving object with known independent MV.
      // Frame 4 jumps and resets history; later frames must recover midpoint
      // motion.
      bool reset = frame == 0 || frame == 4;
      bool effectiveReset = reset || frame == 6;
      uint64_t sourceFrame = frame < 6 ? frame : frame + 2;
      int local = frame < 4 ? frame : frame - 4;
      int dx = cycle == 1 ? 0 : 8, dy = cycle == 1 ? 0 : 4;
      int left = 64 + local * dx + (frame >= 4 ? 70 : 0);
      int top = 52 + local * dy;
      auto *rgba = static_cast<uint8_t *>(colorUpload.mapped);
      auto *z = static_cast<float *>(depthUpload.mapped);
      auto *mv = static_cast<float *>(motionUpload.mapped);
      for (uint32_t y = 0; y < H; ++y)
        for (uint32_t x = 0; x < W; ++x) {
          size_t p = size_t(y) * W + x;
          bool object = int(x) >= left && int(x) < left + 48 && int(y) >= top &&
                        int(y) < top + 48;
          uint8_t tex = uint8_t(
              ((object ? x - left : x) * 17 + (object ? y - top : y) * 31) %
              37);
          rgba[4 * p] = object ? 190 + tex : 22 + tex;
          rgba[4 * p + 1] = object ? 35 + tex : 35 + tex;
          rgba[4 * p + 2] = object ? 25 + tex : 70 + tex;
          rgba[4 * p + 3] = 255;
          z[p] = object ? 0.8f : 0.95f;
          mv[2 * p] = object && !effectiveReset ? -float(dx) : 0;
          mv[2 * p + 1] = object && !effectiveReset ? -float(dy) : 0;
        }
      std::memcpy(hudlessUpload.mapped, colorUpload.mapped, W * H * 4);
      if (cycle == 2)
        for (uint32_t y = 10; y < 26; ++y)
          for (uint32_t x = 10; x < 50; ++x) {
            size_t p = size_t(y) * W + x;
            rgba[4 * p] = 15;
            rgba[4 * p + 1] = 200 + frame;
            rgba[4 * p + 2] = 15;
          }
      std::string prefix = std::to_string(cycle) + "-" + std::to_string(frame);
      ppm(out / (prefix + "-current.ppm"), rgba);
      d.begin();
      color.upload(colorUpload);
      hudless.upload(hudlessUpload);
      depth.upload(depthUpload);
      motion.upload(motionUpload);
      auto motionInput = motion.resource;
      if (negativeFormat)
        motionInput.description.format = FFX_SURFACE_FORMAT_R32_FLOAT;
      ldb_camera::Matrix vp{};
      vp[0] = 1.f / (std::tan(0.5f) * (float(W) / H));
      vp[5] = 1.f / std::tan(0.5f);
      vp[10] = 100.f / 99.9f;
      vp[11] = -10.f / 99.9f;
      vp[14] = 1;
      ldb_fsr::Camera camera;
      if (!ldb_fsr::cameraFromViewProjection(vp, 1.f, camera))
        throw std::runtime_error("Failed to derive fixture camera metadata");
      sdk.dispatch(d.cmd, color.resource, depth.resource, motionInput,
                   result.resource, flow.resource, scd.resource,
                   dilatedDepth.resource, dilatedMotion.resource,
                   previousDepth.resource, sourceFrame, reset, 16.6667f, camera,
                   cycle == 2 ? hudless.resource : FfxResource{});
      if (negativeCurrent)
        color.download(readback);
      else
        result.download(readback);
      d.submit();
      auto *pixels = static_cast<const uint8_t *>(readback.mapped);
      ppm(out / (prefix + "-generated.ppm"), pixels);
      double sumX = 0, sumY = 0;
      uint32_t n = 0, different = 0;
      uint32_t minX = W, minY = H, maxX = 0, maxY = 0;
      for (uint32_t y = 0; y < H; ++y)
        for (uint32_t x = 0; x < W; ++x) {
          size_t p = size_t(y) * W + x;
          if (pixels[4 * p] > 150 && pixels[4 * p] > pixels[4 * p + 1] + 70 &&
              pixels[4 * p] > pixels[4 * p + 2] + 70) {
            sumX += x;
            sumY += y;
            ++n;
            minX = std::min(minX, x);
            minY = std::min(minY, y);
            maxX = std::max(maxX, x);
            maxY = std::max(maxY, y);
          }
          if (std::memcmp(pixels + 4 * p, rgba + 4 * p, 3))
            ++different;
        }
      if (!n)
        throw std::runtime_error("Generated frame has no moving object");
      double expectedX = left + 23.5 - (effectiveReset ? 0 : dx * 0.5);
      double expectedY = top + 23.5 - (effectiveReset ? 0 : dy * 0.5);
      double error = std::hypot(sumX / n - expectedX, sumY / n - expectedY);
      report << "{\"cycle\":" << cycle << ",\"frame\":" << frame
             << ",\"source_frame\":" << sourceFrame
             << ",\"reset\":" << (effectiveReset ? "true" : "false")
             << ",\"object_pixels\":" << n
             << ",\"different_pixels\":" << different
             << ",\"centroid_x\":" << sumX / n << ",\"centroid_y\":" << sumY / n
             << ",\"expected_x\":" << expectedX
             << ",\"expected_y\":" << expectedY
             << ",\"midpoint_error_pixels\":" << error << "}\n";
      report.flush();
      if (error > 1.5)
        throw std::runtime_error(
            "Official generated object failed independent midpoint check");
      if (n < 2280 || n > 2328 ||
          std::abs(double(minX) - (expectedX - 23.5)) > 1 ||
          std::abs(double(maxX) - (expectedX + 23.5)) > 1 ||
          std::abs(double(minY) - (expectedY - 23.5)) > 1 ||
          std::abs(double(maxY) - (expectedY + 23.5)) > 1)
        throw std::runtime_error("Generated object silhouette failed");
      if (cycle != 1 && !effectiveReset && different < 100)
        throw std::runtime_error("Generated output duplicated current source");
      if ((cycle == 1 || effectiveReset) && different != 0)
        throw std::runtime_error(
            "Stationary/reset output changed original source");
      if (cycle == 2)
        for (uint32_t y = 10; y < 26; ++y)
          for (uint32_t x = 10; x < 50; ++x) {
            size_t p = size_t(y) * W + x;
            if (std::memcmp(pixels + 4 * p, rgba + 4 * p, 3))
              throw std::runtime_error(
                  "HUD-less composition changed current HUD");
          }
      if (validationErrors)
        throw std::runtime_error("Vulkan validation errors");
    }
    // Images are destroyed after completed GPU work; SDK uses no retained image
    // reference after unregister. Context destruction follows these resources.
  }
  if (validationErrors)
    throw std::runtime_error("Vulkan validation errors");
  std::cout << "{\"status\":\"passed\",\"source_frames\":24,\"context_cycles\":"
               "3,\"validation_errors\":0}\n";
}
} // namespace
int main(int argc, char **argv) {
  std::signal(SIGSEGV, [](int) {
    void *stack[40];
    backtrace_symbols_fd(stack, backtrace(stack, 40), STDERR_FILENO);
    _exit(139);
  });
  try {
    if (argc < 2 || argc > 3)
      throw std::runtime_error("Usage: fsr_vulkan_smoke OUTPUT_DIR "
                               "[--negative-current|--negative-format]");
    std::string option = argc == 3 ? argv[2] : "";
    if (!option.empty() && option != "--negative-current" &&
        option != "--negative-format")
      throw std::runtime_error("Unknown fixture option");
    run(argv[1], option == "--negative-current", option == "--negative-format");
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}

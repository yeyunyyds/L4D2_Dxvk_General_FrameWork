#pragma once
#include "camera_state.h"
#include "dxvk_capture_shared.h"
#include "frame_generate_spirv.h"
#include "frame_quality_spirv.h"
#include "fsr_backend_api.h"
#include <algorithm>
#include <memory>
namespace dxvk_capture {
// Own Vulkan histories only. The device/interop is borrowed from the live Host
// device.
class ResidentFg {
public:
  enum Slot {
    CurrentDepth,
    PreviousDepth,
    CurrentNear,
    PreviousNear,
    CurrentUi,
    PreviousUi,
    CurrentFinal,
    PreviousFinal,
    Output,
    Mask,
    CurrentNearBefore,
    CurrentNearAfter,
    PreviousNearBefore,
    PreviousNearAfter,
    SlotCount
  };

  struct Params {
    std::array<float, 96> rows{};
    std::array<uint32_t, 4> flags{};
    std::array<float, 4> thresholds{.0002f, .16f, 0, 0};
  };

private:
  Capture c;
  struct FsrLibrary {
    HMODULE module{};
    const LdbFsrApi *api{};
    void *context{};
    ~FsrLibrary() {
      if (context && api)
        api->destroy(context);
      if (module)
        FreeLibrary(module);
    }
  };
  std::unique_ptr<FsrLibrary> fsr;

  struct OwnedImage {
    VkImage image{};
    VkImageView view{};
    VkDeviceMemory memory{};
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    VkFormat format = VK_FORMAT_UNDEFINED;
  };

  std::array<OwnedImage, SlotCount> images{};

  bool recording = false, queryPending = false;
  VkQueryPool timestamps{};
  uint32_t timestampBits = 0;
  double timestampPeriod = 0;
  LARGE_INTEGER clockFrequency{};

public:
  struct Quality {
    uint32_t total = 0, invalid = 0, foreground = 0, reprojected = 0;
    uint32_t nearColour = 0, nearDepth = 0, ui = 0, motion = 0;
    uint32_t motionPixels() const { return motion & 0x7fffffffu; }
    bool localEffectRisk() const { return (motion & 0x80000000u) != 0; }
    double invalidSceneRatio() const {
      return total > foreground ? double(invalid) / double(total - foreground)
                                : 0;
    }
    double foregroundRatio() const {
      return total ? double(foreground) / total : 0;
    }
    double sceneReprojectionRatio() const {
      return total > foreground
                 ? double(reprojected) / double(total - foreground)
                 : 0;
    }
    const char *rejectReason(uint64_t expected,
                             bool localRiskProtected = false) const {
      if (total != expected ||
          uint64_t(invalid) + foreground + reprojected > total ||
          nearColour > total || nearDepth > total || ui > total ||
          motionPixels() > reprojected)
        return "invalid_quality_counts";
      if (localEffectRisk() && !localRiskProtected)
        return "unsafe_thin_effect";
      if (foregroundRatio() > .8)
        return "excessive_foreground_coverage";
      if (invalidSceneRatio() > .15)
        return "invalid_scene_coverage";
      if (sceneReprojectionRatio() < .05)
        return "insufficient_scene_reprojection";
      return nullptr;
    }
    bool reject(uint64_t expected, bool localRiskProtected = false) const {
      return rejectReason(expected, localRiskProtected) != nullptr;
    }
  };
  struct Counters {
    uint32_t submissions = 0;
    double fenceMs = 0, dispatchMs = -1;
  };

private:
  Counters counters;
  VkSampler sampler{};
  VkShaderModule shader{}, qualityShader{};
  VkDescriptorSetLayout setLayout{};
  VkPipelineLayout pipelineLayout{};
  VkPipeline pipeline{}, qualityPipeline{};
  VkDescriptorPool descriptors{};
  VkDescriptorSet set{};

  VkBuffer uniform{}, readback{}, qualityBuffer{}, qualityReadback{};
  VkDeviceMemory qualityMemory{}, qualityReadbackMemory{};
  VkDeviceSize qualityBytes = 0, qualityResultOffset = 0;
  void *qualityMap = nullptr;
  VkDeviceMemory uniformMemory{}, readbackMemory{};
  void *uniformMap = nullptr;
  void *readbackMap = nullptr;

#define FN(name) PFN_##name name = nullptr
  FN(vkCmdFillBuffer);
  FN(vkCmdCopyBuffer);
  FN(vkCreateQueryPool);
  FN(vkDestroyQueryPool);
  FN(vkCmdResetQueryPool);
  FN(vkCmdWriteTimestamp);
  FN(vkGetQueryPoolResults);
  FN(vkCreateImageView);
  FN(vkDestroyImageView);
  FN(vkCreateSampler);
  FN(vkDestroySampler);
  FN(vkCmdClearColorImage);
  FN(vkCmdClearDepthStencilImage);

  FN(vkCreateShaderModule);
  FN(vkDestroyShaderModule);
  FN(vkCreateDescriptorSetLayout);
  FN(vkDestroyDescriptorSetLayout);
  FN(vkCreatePipelineLayout);
  FN(vkDestroyPipelineLayout);
  FN(vkCreateComputePipelines);
  FN(vkDestroyPipeline);
  FN(vkCreateDescriptorPool);
  FN(vkDestroyDescriptorPool);
  FN(vkAllocateDescriptorSets);
  FN(vkUpdateDescriptorSets);
  FN(vkCmdBindPipeline);
  FN(vkCmdBindDescriptorSets);
  FN(vkCmdDispatch);
  FN(vkFlushMappedMemoryRanges);

#undef FN
  void barrier(VkImage image, VkImageAspectFlags aspect, VkImageLayout before,
               VkImageLayout after) {
    VkImageMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcAccessMask =
        before == VK_IMAGE_LAYOUT_UNDEFINED
            ? 0
            : (VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.oldLayout = before;
    b.newLayout = after;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {aspect, 0, 1, 0, 1};

    c.vkCmdPipelineBarrier(c.command,
                           before == VK_IMAGE_LAYOUT_UNDEFINED
                               ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
                               : VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                           VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                           nullptr, 1, &b);
  }
  void transition(OwnedImage &image, VkImageLayout layout) {
    barrier(image.image, image.aspect, image.layout, layout);
    image.layout = layout;
  }
  void begin() {
    if (recording)
      return;
    vkcheck(c.vkResetCommandPool(c.device, c.pool, 0), "FG command reset");
    vkcheck(c.vkResetFences(c.device, 1, &c.fence), "FG fence reset");
    VkCommandBufferBeginInfo b{};
    b.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    b.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkcheck(c.vkBeginCommandBuffer(c.command, &b), "FG begin");
    recording = true;
  }
  void end() {
    recording = false;
    vkcheck(c.vkEndCommandBuffer(c.command), "FG end");
    VkSubmitInfo s{};
    s.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    s.commandBufferCount = 1;
    s.pCommandBuffers = &c.command;
    c.interop->LockSubmissionQueue();
    VkResult result = c.vkQueueSubmit(c.queue, 1, &s, c.fence);
    c.interop->ReleaseSubmissionQueue();
    vkcheck(result, "FG submit");
    c.pending = true;
    LARGE_INTEGER start{}, stop{};
    QueryPerformanceCounter(&start);
    vkcheck(c.vkWaitForFences(c.device, 1, &c.fence, VK_TRUE, 5'000'000'000ull),
            "FG wait");
    c.pending = false;
    QueryPerformanceCounter(&stop);
    counters.submissions++;
    counters.fenceMs += double(stop.QuadPart - start.QuadPart) * 1000.0 /
                        clockFrequency.QuadPart;
    if (queryPending) {
      uint64_t ticks[2]{};
      VkResult q = vkGetQueryPoolResults(c.device, timestamps, 0, 2,
                                         sizeof(ticks), ticks, sizeof(uint64_t),
                                         VK_QUERY_RESULT_64_BIT);
      if (q == VK_SUCCESS) {
        uint64_t mask = timestampBits == 64
                            ? ~uint64_t(0)
                            : ((uint64_t(1) << timestampBits) - 1);
        counters.dispatchMs =
            double((ticks[1] - ticks[0]) & mask) * timestampPeriod / 1e6;
      }
      queryPending = false;
    }
  }
  void buffer(VkBuffer &buffer, VkDeviceMemory &memory, void *&mapped,
              VkDeviceSize bytes, VkBufferUsageFlags usage) {
    VkBufferCreateInfo b{};
    b.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    b.size = bytes;
    b.usage = usage;
    b.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    vkcheck(c.vkCreateBuffer(c.device, &b, nullptr, &buffer), "FG buffer");
    VkMemoryRequirements r{};
    c.vkGetBufferMemoryRequirements(c.device, buffer, &r);
    memory = c.allocate(r, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    vkcheck(c.vkBindBufferMemory(c.device, buffer, memory, 0),
            "FG buffer bind");
    vkcheck(c.vkMapMemory(c.device, memory, 0, VK_WHOLE_SIZE, 0, &mapped),
            "FG buffer map");
  }

public:
  uint32_t width = 0, height = 0;
  VkFormat depthFormat = VK_FORMAT_UNDEFINED, finalFormat = VK_FORMAT_UNDEFINED;
  explicit ResidentFg(ID3D9VkInteropDevice *interop) : c(interop) {
    QueryPerformanceFrequency(&clockFrequency);
    auto gi = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(
        GetModuleHandleW(L"vulkan-1.dll"), "vkGetInstanceProcAddr"));
    auto get = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
        gi(c.instance, "vkGetDeviceProcAddr"));
#define LOAD(name)                                                             \
  name = reinterpret_cast<PFN_##name>(get(c.device, #name));                   \
  if (!name)                                                                   \
  throw std::runtime_error(#name " missing")
    LOAD(vkCmdFillBuffer);
    LOAD(vkCmdCopyBuffer);
    LOAD(vkCreateQueryPool);
    LOAD(vkDestroyQueryPool);
    LOAD(vkCmdResetQueryPool);
    LOAD(vkCmdWriteTimestamp);
    LOAD(vkGetQueryPoolResults);
    LOAD(vkCreateImageView);
    LOAD(vkDestroyImageView);
    LOAD(vkCreateSampler);
    LOAD(vkDestroySampler);
    LOAD(vkCmdClearColorImage);
    LOAD(vkCmdClearDepthStencilImage);
    LOAD(vkCreateShaderModule);
    LOAD(vkDestroyShaderModule);
    LOAD(vkCreateDescriptorSetLayout);
    LOAD(vkDestroyDescriptorSetLayout);
    LOAD(vkCreatePipelineLayout);
    LOAD(vkDestroyPipelineLayout);
    LOAD(vkCreateComputePipelines);
    LOAD(vkDestroyPipeline);
    LOAD(vkCreateDescriptorPool);
    LOAD(vkDestroyDescriptorPool);
    LOAD(vkAllocateDescriptorSets);
    LOAD(vkUpdateDescriptorSets);
    LOAD(vkCmdBindPipeline);
    LOAD(vkCmdBindDescriptorSets);
    LOAD(vkCmdDispatch);
    LOAD(vkFlushMappedMemoryRanges);
#undef LOAD
  }
  ~ResidentFg() {
    if (c.pending) {
      c.interop->LockSubmissionQueue();
      c.vkQueueWaitIdle(c.queue);
      c.interop->ReleaseSubmissionQueue();
      c.pending = false;
    }
    fsr.reset(); // Borrowed image views must die before their Host histories.
    if (timestamps)
      vkDestroyQueryPool(c.device, timestamps, nullptr);
    if (qualityPipeline)
      vkDestroyPipeline(c.device, qualityPipeline, nullptr);
    if (qualityShader)
      vkDestroyShaderModule(c.device, qualityShader, nullptr);
    if (pipeline)
      vkDestroyPipeline(c.device, pipeline, nullptr);
    if (pipelineLayout)
      vkDestroyPipelineLayout(c.device, pipelineLayout, nullptr);
    if (descriptors)
      vkDestroyDescriptorPool(c.device, descriptors, nullptr);
    if (setLayout)
      vkDestroyDescriptorSetLayout(c.device, setLayout, nullptr);
    if (shader)
      vkDestroyShaderModule(c.device, shader, nullptr);
    if (sampler)
      vkDestroySampler(c.device, sampler, nullptr);
    if (qualityMap)
      c.vkUnmapMemory(c.device, qualityReadbackMemory);
    if (qualityReadback)
      c.vkDestroyBuffer(c.device, qualityReadback, nullptr);
    if (qualityReadbackMemory)
      c.vkFreeMemory(c.device, qualityReadbackMemory, nullptr);
    if (qualityBuffer)
      c.vkDestroyBuffer(c.device, qualityBuffer, nullptr);
    if (qualityMemory)
      c.vkFreeMemory(c.device, qualityMemory, nullptr);
    if (uniformMap)
      c.vkUnmapMemory(c.device, uniformMemory);
    if (uniform)
      c.vkDestroyBuffer(c.device, uniform, nullptr);
    if (uniformMemory)
      c.vkFreeMemory(c.device, uniformMemory, nullptr);
    if (readbackMap)
      c.vkUnmapMemory(c.device, readbackMemory);
    if (readback)
      c.vkDestroyBuffer(c.device, readback, nullptr);
    if (readbackMemory)
      c.vkFreeMemory(c.device, readbackMemory, nullptr);
    for (auto &i : images) {
      if (i.view)
        vkDestroyImageView(c.device, i.view, nullptr);
      if (i.image)
        c.vkDestroyImage(c.device, i.image, nullptr);
      if (i.memory)
        c.vkFreeMemory(c.device, i.memory, nullptr);
    }
  }
  void initialize(uint32_t w, uint32_t h, VkFormat depth, VkFormat color) {
    static_assert(sizeof(Params) == 416, "FG uniform ABI");
    if (!w || !h || w > 4096 || h > 4096 || uint64_t(w) * h > 4096ull * 2160)
      throw std::runtime_error("FG extent budget");
    if ((color != VK_FORMAT_B8G8R8A8_UNORM &&
         color != VK_FORMAT_R8G8B8A8_UNORM) ||
        (depth != VK_FORMAT_D32_SFLOAT &&
         depth != VK_FORMAT_D24_UNORM_S8_UINT &&
         depth != VK_FORMAT_D32_SFLOAT_S8_UINT))
      throw std::runtime_error("FG format unsupported");
    width = w;
    height = h;
    depthFormat = depth;
    finalFormat = color;
    auto gi = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(
        GetModuleHandleW(L"vulkan-1.dll"), "vkGetInstanceProcAddr"));
    auto props = reinterpret_cast<PFN_vkGetPhysicalDeviceFormatProperties>(
        gi(c.instance, "vkGetPhysicalDeviceFormatProperties"));
    auto family =
        reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
            gi(c.instance, "vkGetPhysicalDeviceQueueFamilyProperties"));
    uint32_t count = 0;
    family(c.physical, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    family(c.physical, &count, families.data());
    if (c.family >= count ||
        !(families[c.family].queueFlags & VK_QUEUE_COMPUTE_BIT))
      throw std::runtime_error("FG queue lacks compute");
    timestampBits = families[c.family].timestampValidBits;
    VkPhysicalDeviceProperties properties{};
    auto getProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
        gi(c.instance, "vkGetPhysicalDeviceProperties"));
    getProperties(c.physical, &properties);
    timestampPeriod = properties.limits.timestampPeriod;
    if (timestampBits && timestampPeriod > 0 &&
        properties.limits.timestampComputeAndGraphics) {
      VkQueryPoolCreateInfo q{};
      q.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
      q.queryType = VK_QUERY_TYPE_TIMESTAMP;
      q.queryCount = 2;
      vkcheck(vkCreateQueryPool(c.device, &q, nullptr, &timestamps),
              "FG timestamp pool");
    }
    VkCommandPoolCreateInfo pool{};
    pool.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool.queueFamilyIndex = c.family;
    vkcheck(c.vkCreateCommandPool(c.device, &pool, nullptr, &c.pool),
            "FG pool");
    VkCommandBufferAllocateInfo commands{};
    commands.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    commands.commandPool = c.pool;
    commands.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    commands.commandBufferCount = 1;
    vkcheck(c.vkAllocateCommandBuffers(c.device, &commands, &c.command),
            "FG command");
    VkFenceCreateInfo fence{};
    fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    vkcheck(c.vkCreateFence(c.device, &fence, nullptr, &c.fence), "FG fence");
    for (size_t n = 0; n < SlotCount; n++) {
      auto &i = images[n];
      const bool ds = n < CurrentUi, out = n == Output || n == Mask;
      i.format =
          ds ? depth
             : (out ? VK_FORMAT_R8G8B8A8_UNORM
                    : (n >= CurrentNearBefore ? VK_FORMAT_R16G16B16A16_SFLOAT
                                              : color));
      i.aspect = ds ? (depth == VK_FORMAT_D32_SFLOAT
                           ? VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT)
                           : VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT |
                                                VK_IMAGE_ASPECT_STENCIL_BIT))
                    : VkImageAspectFlags(VK_IMAGE_ASPECT_COLOR_BIT);
      VkFormatProperties fp{};
      props(c.physical, i.format, &fp);
      VkFormatFeatureFlags needed =
          out ? VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT |
                    VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT
              : VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
      if ((fp.optimalTilingFeatures & needed) != needed)
        throw std::runtime_error("FG format feature missing");
      VkImageCreateInfo info{};
      info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
      info.imageType = VK_IMAGE_TYPE_2D;
      info.format = i.format;
      info.extent = {w, h, 1};
      info.mipLevels = 1;
      info.arrayLayers = 1;
      info.samples = VK_SAMPLE_COUNT_1_BIT;
      info.tiling = VK_IMAGE_TILING_OPTIMAL;
      info.usage =
          VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
          (out ? VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
               : VK_IMAGE_USAGE_SAMPLED_BIT);
      info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
      vkcheck(c.vkCreateImage(c.device, &info, nullptr, &i.image),
              "FG history image");
      VkMemoryRequirements r{};
      c.vkGetImageMemoryRequirements(c.device, i.image, &r);
      i.memory = c.allocate(r, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
      vkcheck(c.vkBindImageMemory(c.device, i.image, i.memory, 0),
              "FG history bind");
      VkImageViewCreateInfo v{};
      v.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
      v.image = i.image;
      v.viewType = VK_IMAGE_VIEW_TYPE_2D;
      v.format = i.format;
      v.subresourceRange = {ds ? VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT)
                               : VkImageAspectFlags(VK_IMAGE_ASPECT_COLOR_BIT),
                            0, 1, 0, 1};
      vkcheck(vkCreateImageView(c.device, &v, nullptr, &i.view),
              "FG image view");
    }
    begin();
    for (size_t n = 0; n < SlotCount; n++) {
      auto &i = images[n];
      transition(i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
      VkImageSubresourceRange range{i.aspect, 0, 1, 0, 1};
      if (n < CurrentUi) {
        VkClearDepthStencilValue value{1, 0};
        vkCmdClearDepthStencilImage(c.command, i.image, i.layout, &value, 1,
                                    &range);
      } else {
        VkClearColorValue value{};
        vkCmdClearColorImage(c.command, i.image, i.layout, &value, 1, &range);
      }
      transition(i, (n == Output || n == Mask)
                        ? VK_IMAGE_LAYOUT_GENERAL
                        : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    end();
    VkSamplerCreateInfo sam{};
    sam.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sam.magFilter = VK_FILTER_NEAREST;
    sam.minFilter = VK_FILTER_NEAREST;
    sam.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sam.addressModeU = sam.addressModeV = sam.addressModeW =
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sam.maxLod = 0;
    vkcheck(vkCreateSampler(c.device, &sam, nullptr, &sampler), "FG sampler");
    buffer(uniform, uniformMemory, uniformMap, sizeof(Params),
           VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    static_assert(sizeof(Quality) == 32, "FG quality ABI");
    VkBufferCreateInfo qb{};
    qb.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    qualityResultOffset =
        VkDeviceSize((w + 7) / 8) * ((h + 7) / 8) * sizeof(Quality);
    qualityBytes = qualityResultOffset + sizeof(Quality);
    qb.size = qualityBytes;
    qb.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
               VK_BUFFER_USAGE_TRANSFER_DST_BIT |
               VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    vkcheck(c.vkCreateBuffer(c.device, &qb, nullptr, &qualityBuffer),
            "FG quality GPU buffer");
    VkMemoryRequirements qr{};
    c.vkGetBufferMemoryRequirements(c.device, qualityBuffer, &qr);
    qualityMemory = c.allocate(qr, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vkcheck(c.vkBindBufferMemory(c.device, qualityBuffer, qualityMemory, 0),
            "FG quality GPU memory");
    buffer(qualityReadback, qualityReadbackMemory, qualityMap, sizeof(Quality),
           VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    VkShaderModuleCreateInfo sm{};
    sm.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    sm.codeSize = sizeof(frameGenerateSpirv);
    sm.pCode = frameGenerateSpirv;
    vkcheck(vkCreateShaderModule(c.device, &sm, nullptr, &shader), "FG shader");
    std::array<VkDescriptorSetLayoutBinding, 16> bindings{};
    for (uint32_t n = 0; n < 16; n++) {
      bindings[n].binding = n;
      bindings[n].descriptorCount = 1;
      bindings[n].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
      bindings[n].descriptorType =
          (n < 8 || (n >= 11 && n < 15))
              ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
              : (n == 15  ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER
                 : n == 9 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                          : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
    }
    VkDescriptorSetLayoutCreateInfo sl{};
    sl.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    sl.bindingCount = 16;
    sl.pBindings = bindings.data();
    vkcheck(vkCreateDescriptorSetLayout(c.device, &sl, nullptr, &setLayout),
            "FG set layout");
    VkPipelineLayoutCreateInfo pl{};
    pl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &setLayout;
    vkcheck(vkCreatePipelineLayout(c.device, &pl, nullptr, &pipelineLayout),
            "FG pipeline layout");
    VkComputePipelineCreateInfo cp{};
    cp.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    cp.layout = pipelineLayout;
    cp.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cp.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cp.stage.module = shader;
    cp.stage.pName = "main";
    vkcheck(vkCreateComputePipelines(c.device, VK_NULL_HANDLE, 1, &cp, nullptr,
                                     &pipeline),
            "FG pipeline");
    VkShaderModuleCreateInfo qs{};
    qs.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    qs.codeSize = sizeof(frameQualitySpirv);
    qs.pCode = frameQualitySpirv;
    vkcheck(vkCreateShaderModule(c.device, &qs, nullptr, &qualityShader),
            "FG quality shader");
    cp.stage.module = qualityShader;
    vkcheck(vkCreateComputePipelines(c.device, VK_NULL_HANDLE, 1, &cp, nullptr,
                                     &qualityPipeline),
            "FG quality reduction pipeline");
    std::array<VkDescriptorPoolSize, 4> sizes{
        {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 12},
         {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2},
         {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1},
         {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}}};
    VkDescriptorPoolCreateInfo dp{};
    dp.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dp.maxSets = 1;
    dp.poolSizeCount = 4;
    dp.pPoolSizes = sizes.data();
    vkcheck(vkCreateDescriptorPool(c.device, &dp, nullptr, &descriptors),
            "FG descriptors");
    VkDescriptorSetAllocateInfo da{};
    da.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    da.descriptorPool = descriptors;
    da.descriptorSetCount = 1;
    da.pSetLayouts = &setLayout;
    vkcheck(vkAllocateDescriptorSets(c.device, &da, &set), "FG descriptor set");
  }
  bool matches(const ImageInfo &image) const {
    return image.create.extent.width == width &&
           image.create.extent.height == height &&
           image.create.samples == VK_SAMPLE_COUNT_1_BIT;
  }
  void copyInto(const ImageInfo &source, Slot slot, VkImageAspectFlags aspect) {
    auto &dst = images[slot];
    if (!matches(source) || source.create.format != dst.format)
      throw std::runtime_error("FG history source changed");
    begin();
    transition(dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageCopy r{};
    r.srcSubresource = {aspect, 0, 0, 1};
    r.dstSubresource = r.srcSubresource;
    r.extent = {width, height, 1};
    c.vkCmdCopyImage(c.command, source.image,
                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst.image,
                     dst.layout, 1, &r);
    transition(dst, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    end();
  }
  Counters takeCounters() {
    auto result = counters;
    counters = {};
    return result;
  }
  void generate(const Params &params, bool deferSubmit = false) {
    std::memcpy(uniformMap, &params, sizeof(params));
    VkMappedMemoryRange range{};
    range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
    range.memory = uniformMemory;
    range.size = VK_WHOLE_SIZE;
    vkcheck(vkFlushMappedMemoryRanges(c.device, 1, &range), "FG uniform flush");
    std::array<VkDescriptorImageInfo, 16> infos{};
    std::array<VkWriteDescriptorSet, 16> writes{};
    VkDescriptorBufferInfo bufferInfo{uniform, 0, sizeof(Params)};
    VkDescriptorBufferInfo qualityInfo{qualityBuffer, 0, qualityBytes};
    for (uint32_t n = 0; n < 16; n++) {
      writes[n].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      writes[n].dstSet = set;
      writes[n].dstBinding = n;
      writes[n].descriptorCount = 1;
      writes[n].descriptorType =
          (n < 8 || (n >= 11 && n < 15))
              ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
              : (n == 15  ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER
                 : n == 9 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                          : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
      if (n == 9)
        writes[n].pBufferInfo = &bufferInfo;
      else if (n == 15)
        writes[n].pBufferInfo = &qualityInfo;
      else {
        uint32_t slot = n >= 11 ? uint32_t(CurrentNearBefore) + n - 11
                                : (n == 10 ? uint32_t(Mask) : n);
        infos[n] = {(n < 8 || n >= 11) ? sampler : VK_NULL_HANDLE,
                    images[slot].view, images[slot].layout};
        writes[n].pImageInfo = &infos[n];
      }
    }
    vkUpdateDescriptorSets(c.device, 16, writes.data(), 0, nullptr);
    begin();
    VkBufferMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    b.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_UNIFORM_READ_BIT;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.buffer = uniform;
    b.size = VK_WHOLE_SIZE;
    c.vkCmdPipelineBarrier(c.command, VK_PIPELINE_STAGE_HOST_BIT,
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr,
                           1, &b, 0, nullptr);
    transition(images[Output], VK_IMAGE_LAYOUT_GENERAL);
    transition(images[Mask], VK_IMAGE_LAYOUT_GENERAL);
    vkCmdBindPipeline(c.command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(c.command, VK_PIPELINE_BIND_POINT_COMPUTE,
                            pipelineLayout, 0, 1, &set, 0, nullptr);
    if (timestamps) {
      vkCmdResetQueryPool(c.command, timestamps, 0, 2);
      vkCmdWriteTimestamp(c.command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                          timestamps, 0);
    }
    vkCmdFillBuffer(c.command, qualityBuffer, 0, qualityBytes, 0);
    VkBufferMemoryBarrier clear{};
    clear.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    clear.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    clear.dstAccessMask =
        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    clear.srcQueueFamilyIndex = clear.dstQueueFamilyIndex =
        VK_QUEUE_FAMILY_IGNORED;
    clear.buffer = qualityBuffer;
    clear.size = qualityBytes;
    c.vkCmdPipelineBarrier(c.command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr,
                           1, &clear, 0, nullptr);
    vkCmdDispatch(c.command, (width + 7) / 8, (height + 7) / 8, 1);
    if (timestamps) {
      vkCmdWriteTimestamp(c.command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                          timestamps, 1);
      queryPending = true;
    }
    clear.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    clear.dstAccessMask =
        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    c.vkCmdPipelineBarrier(c.command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr,
                           1, &clear, 0, nullptr);
    vkCmdBindPipeline(c.command, VK_PIPELINE_BIND_POINT_COMPUTE,
                      qualityPipeline);
    vkCmdDispatch(c.command, 1, 1, 1);
    clear.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    clear.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    c.vkCmdPipelineBarrier(c.command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 1,
                           &clear, 0, nullptr);
    VkBufferCopy copy{qualityResultOffset, 0, sizeof(Quality)};
    vkCmdCopyBuffer(c.command, qualityBuffer, qualityReadback, 1, &copy);
    clear.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    clear.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    clear.buffer = qualityReadback;
    clear.size = sizeof(Quality);
    c.vkCmdPipelineBarrier(c.command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &clear,
                           0, nullptr);
    if (!deferSubmit)
      end();
  }
  static bool wantsFsr() {
    wchar_t value[16]{};
    return GetEnvironmentVariableW(L"LDB_FG_BACKEND", value, 16) == 3 &&
           std::wstring(value) == L"fsr";
  }
  int generateFsr(const ldb_camera::Matrix &current,
                  const ldb_camera::Matrix &previous, uint64_t frame,
                  double frameTimeMs, bool hasUi, bool preflight = true,
                  bool previousNear = false) {
    if (recording)
      end();
    if (preflight && readQuality().reject(uint64_t(width) * height, true))
      return 2;
    if (!fsr) {
      auto loaded = std::make_unique<FsrLibrary>();
      HMODULE dxvk{};
      auto address = (*reinterpret_cast<void ***>(c.interop))[0];
      if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                  GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              reinterpret_cast<LPCWSTR>(address), &dxvk))
        throw std::runtime_error("FSR DXVK module unavailable");
      using Features = uint32_t(__stdcall *)(IDirect3DDevice9 *);
      auto features = reinterpret_cast<Features>(
          GetProcAddress(dxvk, "LdbFsrDeviceFeatures"));
      Com<IDirect3DDevice9> device;
      hr(c.interop->QueryInterface(__uuidof(IDirect3DDevice9),
                                   reinterpret_cast<void **>(device.put())),
         "FSR real D3D9 device");
      if (!features || features(device.get()) != 15u)
        throw std::runtime_error(
            "FSR requires packaged DXVK with enabled storage-image features");
      std::wstring executable(32768, L'\0');
      DWORD count = GetModuleFileNameW(nullptr, executable.data(),
                                       DWORD(executable.size()));
      if (!count || count >= executable.size())
        throw std::runtime_error("FSR executable path");
      executable.resize(count);
      auto path = std::filesystem::path(executable).parent_path() /
                  L"LDBFsrBackend64.dll";
      loaded->module = LoadLibraryExW(path.c_str(), nullptr,
                                      LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
                                          LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
      if (!loaded->module)
        throw std::runtime_error(
            "FSR backend DLL missing or dependencies unavailable");
      auto get = reinterpret_cast<LdbFsrGetApi>(
          GetProcAddress(loaded->module, "LdbFsrGetInterface"));
      loaded->api = get ? get(LDB_FSR_API_VERSION) : nullptr;
      if (!loaded->api || loaded->api->version != LDB_FSR_API_VERSION ||
          loaded->api->size != sizeof(LdbFsrApi))
        throw std::runtime_error("FSR backend ABI mismatch");
      char message[512]{};
      if (loaded->api->create(c.device, c.physical, width, height,
                              &loaded->context, message, sizeof(message)) < 0)
        throw std::runtime_error(std::string("FSR create: ") + message);
      fsr = std::move(loaded);
    }
    LdbFsrFrame f;
    f.width = width;
    f.height = height;
    f.flags = (hasUi ? 1u : 0u) | (previousNear ? 4u : 0u);
    f.frame = frame;
    f.depthFormat = depthFormat;
    f.finalFormat = finalFormat;
    f.frameTimeMs = std::isfinite(frameTimeMs) && frameTimeMs > 0
                        ? float(frameTimeMs)
                        : 16.6667f;
    ldb_camera::Matrix transform{};
    if (!ldb_camera::transform(previous, current, transform))
      throw std::runtime_error("FSR camera transform");
    std::copy(transform.begin(), transform.end(), f.currentToPrevious);
    std::copy(current.begin(), current.end(), f.viewProjection);
    const Slot slots[]{CurrentDepth,  PreviousDepth,      CurrentFinal,
                       PreviousFinal, CurrentUi,          Mask,
                       Output,        PreviousNearBefore, PreviousNearAfter};
    for (size_t n = 0; n < 9; ++n)
      f.images[n] = images[slots[n]].image;
    begin();
    char message[512]{};
    int result = fsr->api->dispatch(fsr->context, c.command, &f, message,
                                    sizeof(message));
    if (result < 0)
      throw std::runtime_error(std::string("FSR dispatch: ") + message);
    end();
    return result;
  }
  Quality readQuality() {
    if (recording || c.pending)
      throw std::runtime_error("FG quality read before completion");
    VkMappedMemoryRange range{};
    range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
    range.memory = qualityReadbackMemory;
    range.size = VK_WHOLE_SIZE;
    vkcheck(c.vkInvalidateMappedMemoryRanges(c.device, 1, &range),
            "FG quality invalidate");
    Quality q{};
    std::memcpy(&q, qualityMap, sizeof(q));
    return q;
  }
  void writeBack(const ImageInfo &destination, Slot source) {
    if (!matches(destination) || (destination.create.format != finalFormat) ||
        !(destination.create.usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT))
      throw std::runtime_error("FG writable backbuffer changed");
    auto &image = images[source];
    VkImageLayout original = image.layout;
    begin();
    transition(image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkImageCopy r{};
    r.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    r.dstSubresource = r.srcSubresource;
    r.extent = {width, height, 1};
    c.vkCmdCopyImage(c.command, image.image, image.layout, destination.image,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
    transition(image, original);
    end();
  }
  void advance() {
    std::swap(images[CurrentDepth], images[PreviousDepth]);
    std::swap(images[CurrentNear], images[PreviousNear]);
    std::swap(images[CurrentUi], images[PreviousUi]);
    std::swap(images[CurrentFinal], images[PreviousFinal]);
    std::swap(images[CurrentNearBefore], images[PreviousNearBefore]);
    std::swap(images[CurrentNearAfter], images[PreviousNearAfter]);
  }
  void save(Slot slot, const std::filesystem::path &path, uint32_t stride = 3) {
    if (!readback)
      buffer(readback, readbackMemory, readbackMap,
             VkDeviceSize(width) * height * 4,
             VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    auto &image = images[slot];
    VkImageLayout original = image.layout;
    begin();
    transition(image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy r{};
    r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    r.imageExtent = {width, height, 1};
    c.vkCmdCopyImageToBuffer(c.command, image.image, image.layout, readback, 1,
                             &r);
    transition(image, original);
    VkBufferMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.buffer = readback;
    b.size = VK_WHOLE_SIZE;
    c.vkCmdPipelineBarrier(c.command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &b, 0,
                           nullptr);
    end();
    VkMappedMemoryRange range{};
    range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
    range.memory = readbackMemory;
    range.size = VK_WHOLE_SIZE;
    vkcheck(c.vkInvalidateMappedMemoryRanges(c.device, 1, &range),
            "FG debug invalidate");
    uint32_t w = (width + stride - 1) / stride,
             h = (height + stride - 1) / stride;
    std::ofstream out(path, std::ios::binary);
    out << "P6\n" << w << ' ' << h << "\n255\n";
    const bool bgra = slot != Mask && finalFormat == VK_FORMAT_B8G8R8A8_UNORM;
    const auto *bytes = static_cast<const uint8_t *>(readbackMap);
    for (uint32_t y = 0; y < h; y++)
      for (uint32_t x = 0; x < w; x++) {
        size_t i =
            (size_t(std::min(y * stride + stride / 2, height - 1)) * width +
             std::min(x * stride + stride / 2, width - 1)) *
            4;
        char rgb[3] = {char(bytes[i + (bgra ? 2 : 0)]), char(bytes[i + 1]),
                       char(bytes[i + (bgra ? 0 : 2)])};
        out.write(rgb, 3);
      }
    out.close();
    if (!out)
      throw std::runtime_error("FG debug write failed");
  }
  std::string deviceName() const { return c.physicalProperties.deviceName; }
};
} // namespace dxvk_capture

#include "fsr_backend_api.h"
#include "fsr_component_backend.h"
#include "fsr_compose_spirv.h"
#include "fsr_inputs_spirv.h"
#include <cstring>
#include <map>
#include <vector>

namespace {
void vkcheck(VkResult r, const char *what) {
  if (r != VK_SUCCESS)
    throw std::runtime_error(std::string(what) + ": " + std::to_string(r));
}
VkFormat format(FfxSurfaceFormat f) {
  switch (f) {
  case FFX_SURFACE_FORMAT_R8G8B8A8_UNORM:
    return VK_FORMAT_R8G8B8A8_UNORM;
  case FFX_SURFACE_FORMAT_R32_FLOAT:
    return VK_FORMAT_R32_SFLOAT;
  case FFX_SURFACE_FORMAT_R32G32_FLOAT:
    return VK_FORMAT_R32G32_SFLOAT;
  case FFX_SURFACE_FORMAT_R16G16_FLOAT:
    return VK_FORMAT_R16G16_SFLOAT;
  case FFX_SURFACE_FORMAT_R16G16_SINT:
    return VK_FORMAT_R16G16_SINT;
  case FFX_SURFACE_FORMAT_R32_UINT:
    return VK_FORMAT_R32_UINT;
  default:
    throw std::runtime_error("FSR image format");
  }
}
struct Image {
  VkImage image{};
  VkImageView view{};
  VkDeviceMemory memory{};
  FfxResource resource{};
};
struct Backend {
  VkDevice device;
  VkPhysicalDevice physical;
  uint32_t width, height;
  std::unique_ptr<ldb_fsr::Components> sdk;
  std::vector<Image> images;
  std::map<VkImage, VkImageView> borrowedViews;
  VkDescriptorSetLayout setLayout{};
  VkPipelineLayout layout{};
  VkDescriptorPool pool{};
  VkDescriptorSet inputSet{}, composeSet{};
  VkPipeline inputs{}, compose{};
  VkSampler sampler{};
  VkBuffer uniform{};
  VkDeviceMemory uniformMemory{};
  void *mapped{};
  bool initialized = false, history = false;
  uint64_t lastFrame = 0;
  uint32_t lastFlags = 0;
  Backend(VkDevice d, VkPhysicalDevice p, uint32_t w, uint32_t h)
      : device(d), physical(p), width(w), height(h) {
    sdk = std::make_unique<ldb_fsr::Components>(d, p, w, h);
    for (auto f :
         {FFX_SURFACE_FORMAT_R8G8B8A8_UNORM, FFX_SURFACE_FORMAT_R8G8B8A8_UNORM,
          FFX_SURFACE_FORMAT_R32_FLOAT, FFX_SURFACE_FORMAT_R32G32_FLOAT,
          FFX_SURFACE_FORMAT_R8G8B8A8_UNORM})
      image({FFX_RESOURCE_TYPE_TEXTURE2D, f, w, h, 1, 1,
             FFX_RESOURCE_FLAGS_NONE, FFX_RESOURCE_USAGE_UAV});
    image(sdk->opticalResources.opticalFlowVector.resourceDescription);
    image(sdk->opticalResources.opticalFlowSCD.resourceDescription);
    image(sdk->frameResources.dilatedDepth.resourceDescription);
    image(sdk->frameResources.dilatedMotionVectors.resourceDescription);
    image(
        sdk->frameResources.reconstructedPrevNearestDepth.resourceDescription);
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = 80;
    bi.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    vkcheck(vkCreateBuffer(d, &bi, nullptr, &uniform), "FSR parameters");
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(d, uniform, &req);
    uniformMemory = allocate(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    vkcheck(vkBindBufferMemory(d, uniform, uniformMemory, 0),
            "FSR parameters memory");
    vkcheck(vkMapMemory(d, uniformMemory, 0, 80, 0, &mapped),
            "FSR parameters map");
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = si.minFilter = VK_FILTER_NEAREST;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW =
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    vkcheck(vkCreateSampler(d, &si, nullptr, &sampler), "FSR sampler");
    VkDescriptorSetLayoutBinding bindings[11]{};
    for (uint32_t i = 0; i < 11; ++i)
      bindings[i] = {i,
                     i < 6    ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                     : i < 10 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                              : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                     1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo li{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 11;
    li.pBindings = bindings;
    vkcheck(vkCreateDescriptorSetLayout(d, &li, nullptr, &setLayout),
            "FSR descriptor layout");
    VkPipelineLayoutCreateInfo pi{
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pi.setLayoutCount = 1;
    pi.pSetLayouts = &setLayout;
    vkcheck(vkCreatePipelineLayout(d, &pi, nullptr, &layout),
            "FSR pipeline layout");
    inputs = pipeline(fsrInputsSpirv, sizeof(fsrInputsSpirv));
    compose = pipeline(fsrComposeSpirv, sizeof(fsrComposeSpirv));
    VkDescriptorPoolSize sizes[] = {
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 12},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 8},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2}};
    VkDescriptorPoolCreateInfo dp{
        VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dp.maxSets = 2;
    dp.poolSizeCount = 3;
    dp.pPoolSizes = sizes;
    vkcheck(vkCreateDescriptorPool(d, &dp, nullptr, &pool),
            "FSR descriptor pool");
    VkDescriptorSetAllocateInfo ai{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &setLayout;
    vkcheck(vkAllocateDescriptorSets(d, &ai, &inputSet), "FSR input set");
    vkcheck(vkAllocateDescriptorSets(d, &ai, &composeSet), "FSR compose set");
  }
  ~Backend() {
    sdk.reset();
    if (pool)
      vkDestroyDescriptorPool(device, pool, nullptr);
    if (inputs)
      vkDestroyPipeline(device, inputs, nullptr);
    if (compose)
      vkDestroyPipeline(device, compose, nullptr);
    if (layout)
      vkDestroyPipelineLayout(device, layout, nullptr);
    if (setLayout)
      vkDestroyDescriptorSetLayout(device, setLayout, nullptr);
    for (auto [image, view] : borrowedViews)
      vkDestroyImageView(device, view, nullptr);
    for (auto &i : images) {
      vkDestroyImageView(device, i.view, nullptr);
      vkDestroyImage(device, i.image, nullptr);
      vkFreeMemory(device, i.memory, nullptr);
    }
    if (sampler)
      vkDestroySampler(device, sampler, nullptr);
    if (mapped)
      vkUnmapMemory(device, uniformMemory);
    if (uniform)
      vkDestroyBuffer(device, uniform, nullptr);
    if (uniformMemory)
      vkFreeMemory(device, uniformMemory, nullptr);
  }
  VkDeviceMemory allocate(VkMemoryRequirements req,
                          VkMemoryPropertyFlags flags) {
    VkPhysicalDeviceMemoryProperties p{};
    vkGetPhysicalDeviceMemoryProperties(physical, &p);
    uint32_t n = 0;
    for (; n < p.memoryTypeCount; ++n)
      if ((req.memoryTypeBits & (1u << n)) &&
          (p.memoryTypes[n].propertyFlags & flags) == flags)
        break;
    if (n == p.memoryTypeCount)
      throw std::runtime_error("FSR compatible memory missing");
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = n;
    VkDeviceMemory m{};
    vkcheck(vkAllocateMemory(device, &ai, nullptr, &m), "FSR allocation");
    return m;
  }
  VkImageView view(VkImage image, VkFormat fmt,
                   VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT) {
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = fmt;
    vi.subresourceRange = {aspect, 0, 1, 0, 1};
    VkImageView v{};
    vkcheck(vkCreateImageView(device, &vi, nullptr, &v), "FSR image view");
    return v;
  }
  VkImageView borrowed(VkImage i, VkFormat f,
                       VkImageAspectFlags a = VK_IMAGE_ASPECT_COLOR_BIT) {
    auto it = borrowedViews.find(i);
    if (it != borrowedViews.end())
      return it->second;
    return borrowedViews[i] = view(i, f, a);
  }
  void image(FfxResourceDescription desc) {
    Image i;
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = format(desc.format);
    ci.extent = {desc.width, desc.height, 1};
    ci.mipLevels = ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
               VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
               VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    vkcheck(vkCreateImage(device, &ci, nullptr, &i.image), "FSR image");
    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(device, i.image, &req);
    i.memory = allocate(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vkcheck(vkBindImageMemory(device, i.image, i.memory, 0),
            "FSR image memory");
    i.view = view(i.image, ci.format);
    i.resource =
        ffxGetResourceVK(reinterpret_cast<void *>(i.image), desc,
                         L"LDB FSR owned", FFX_RESOURCE_STATE_UNORDERED_ACCESS);
    images.push_back(i);
  }
  VkPipeline pipeline(const uint32_t *code, size_t bytes) {
    VkShaderModuleCreateInfo si{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    si.codeSize = bytes;
    si.pCode = code;
    VkShaderModule shader{};
    vkcheck(vkCreateShaderModule(device, &si, nullptr, &shader),
            "FSR adapter shader");
    VkComputePipelineCreateInfo ci{
        VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.layout = layout;
    ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                nullptr,
                0,
                VK_SHADER_STAGE_COMPUTE_BIT,
                shader,
                "main",
                nullptr};
    VkPipeline p{};
    auto r =
        vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &ci, nullptr, &p);
    vkDestroyShaderModule(device, shader, nullptr);
    vkcheck(r, "FSR adapter pipeline");
    return p;
  }
  void barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout a,
               VkImageLayout b) {
    VkImageMemoryBarrier x{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    x.image = image;
    x.oldLayout = a;
    x.newLayout = b;
    x.srcAccessMask =
        a == VK_IMAGE_LAYOUT_UNDEFINED
            ? 0
            : VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT;
    x.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    x.srcQueueFamilyIndex = x.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    x.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &x);
  }
  int dispatch(VkCommandBuffer cmd, const LdbFsrFrame &f) {
    if (f.size != sizeof(f) || f.width != width || f.height != height || !cmd)
      throw std::runtime_error("FSR ABI/extent changed");
    for (auto i : f.images)
      if (!i)
        throw std::runtime_error("FSR null input image");
    ldb_fsr::Camera camera;
    ldb_camera::Matrix vp{};
    std::copy(f.viewProjection, f.viewProjection + 16, vp.begin());
    if (!ldb_fsr::cameraFromViewProjection(vp, .0254f, camera))
      throw std::runtime_error("FSR unsupported camera projection");
    struct Params {
      float rows[16];
      uint32_t flags[4];
    } p{};
    static_assert(sizeof(p) == 80);
    std::memcpy(p.rows, f.currentToPrevious, 64);
    p.flags[0] = width;
    p.flags[1] = height;
    p.flags[2] =
        (f.flags & 5u) | (f.finalFormat == VK_FORMAT_B8G8R8A8_UNORM ? 2u : 0u);
    std::memcpy(mapped, &p, sizeof(p));
    std::vector<VkDescriptorImageInfo> infos(10);
    std::vector<VkWriteDescriptorSet> writes(11);
    for (uint32_t n = 0; n < 6; ++n)
      infos[n] = {sampler,
                  borrowed(f.images[n],
                           n < 2    ? f.depthFormat
                           : n == 5 ? VK_FORMAT_R8G8B8A8_UNORM
                                    : f.finalFormat,
                           n < 2 ? VK_IMAGE_ASPECT_DEPTH_BIT
                                 : VK_IMAGE_ASPECT_COLOR_BIT),
                  n == 5 ? VK_IMAGE_LAYOUT_GENERAL
                         : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    for (uint32_t n = 6; n < 10; ++n)
      infos[n] = {VK_NULL_HANDLE, images[n - 6].view, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorBufferInfo ub{uniform, 0, 80};
    for (uint32_t n = 0; n < 11; ++n) {
      auto &w = writes[n];
      w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
      w.dstSet = inputSet;
      w.dstBinding = n;
      w.descriptorCount = 1;
      w.descriptorType = n < 6    ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                         : n < 10 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                                  : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
      if (n == 10)
        w.pBufferInfo = &ub;
      else
        w.pImageInfo = &infos[n];
    }
    vkUpdateDescriptorSets(device, 11, writes.data(), 0, nullptr);
    VkDescriptorImageInfo composite[] = {
        {sampler, images[4].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        infos[2],
        infos[5],
        {sampler, borrowed(f.images[7], VK_FORMAT_R16G16B16A16_SFLOAT),
         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {sampler, borrowed(f.images[8], VK_FORMAT_R16G16B16A16_SFLOAT),
         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {VK_NULL_HANDLE, borrowed(f.images[6], VK_FORMAT_R8G8B8A8_UNORM),
         VK_IMAGE_LAYOUT_GENERAL}};
    VkWriteDescriptorSet cw[7]{};
    uint32_t bindings[] = {0, 1, 2, 3, 4, 6, 10};
    for (uint32_t n = 0; n < 7; ++n) {
      cw[n] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
      cw[n].dstSet = composeSet;
      cw[n].dstBinding = bindings[n];
      cw[n].descriptorCount = 1;
      cw[n].descriptorType = n < 5 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                             : n == 5 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                                      : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
      if (n == 6)
        cw[n].pBufferInfo = &ub;
      else
        cw[n].pImageInfo = &composite[n];
    }
    vkUpdateDescriptorSets(device, 7, cw, 0, nullptr);
    if (!initialized) {
      for (auto &i : images) {
        barrier(cmd, i.image, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_GENERAL);
        VkClearColorValue zero{};
        VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cmd, i.image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1,
                             &range);
        barrier(cmd, i.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
      }
      initialized = true;
    }
    VkMemoryBarrier memory{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    memory.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
    memory.dstAccessMask = VK_ACCESS_UNIFORM_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_HOST_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &memory, 0,
                         nullptr, 0, nullptr);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, inputs);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1,
                            &inputSet, 0, nullptr);
    vkCmdDispatch(cmd, (width + 7) / 8, (height + 7) / 8, 1);
    for (int n = 0; n < 4; ++n)
      barrier(cmd, images[n].image, VK_IMAGE_LAYOUT_GENERAL,
              VK_IMAGE_LAYOUT_GENERAL);
    bool reset = !history || lastFrame == UINT64_MAX ||
                 f.frame != lastFrame + 1 || lastFlags != (f.flags & 1u);
    sdk->dispatch(cmd, images[0].resource, images[2].resource,
                  images[3].resource, images[4].resource, images[5].resource,
                  images[6].resource, images[7].resource, images[8].resource,
                  images[9].resource, f.frame, reset,
                  std::max(f.frameTimeMs, .01f), camera,
                  (f.flags & 1u) ? images[1].resource : FfxResource{});
    barrier(cmd, images[4].image, VK_IMAGE_LAYOUT_GENERAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    barrier(cmd, f.images[6], VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, compose);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1,
                            &composeSet, 0, nullptr);
    vkCmdDispatch(cmd, (width + 7) / 8, (height + 7) / 8, 1);
    barrier(cmd, images[4].image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_IMAGE_LAYOUT_GENERAL);
    barrier(cmd, f.images[6], VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
    history = true;
    lastFrame = f.frame;
    lastFlags = f.flags & 1u;
    return reset ? 1 : 0;
  }
};
void error(char *out, size_t size, const char *message) {
  if (size && out) {
    std::strncpy(out, message, size - 1);
    out[size - 1] = 0;
  }
}
int create(VkDevice d, VkPhysicalDevice p, uint32_t w, uint32_t h,
           void **result, char *message, size_t size) {
  try {
    if (!result)
      throw std::runtime_error("FSR null context output");
    *result = nullptr;
    *result = new Backend(d, p, w, h);
    return 0;
  } catch (const std::exception &e) {
    error(message, size, e.what());
    return -1;
  }
}
int dispatch(void *context, VkCommandBuffer cmd, const LdbFsrFrame *frame,
             char *message, size_t size) {
  try {
    if (!context || !frame)
      throw std::runtime_error("FSR missing context/frame");
    return static_cast<Backend *>(context)->dispatch(cmd, *frame);
  } catch (const std::exception &e) {
    error(message, size, e.what());
    return -1;
  }
}
void destroy(void *context) { delete static_cast<Backend *>(context); }
const LdbFsrApi api{1, sizeof(LdbFsrApi), create, dispatch, destroy};
} // namespace
#ifdef _WIN32
#define FSR_EXPORT __declspec(dllexport)
#else
#define FSR_EXPORT
#endif
extern "C" FSR_EXPORT const LdbFsrApi *LdbFsrGetInterface(uint32_t version) {
  return version == 1 ? &api : nullptr;
}

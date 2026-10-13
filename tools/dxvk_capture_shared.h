#pragma once
// Shared DXVK synchronous diagnostic snapshot implementation.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d9.h>
#include <vulkan/vulkan.h>
#include "dxvk_interop.h"
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <limits>
namespace dxvk_capture {
// IEEE 754 binary16 decoding; raw bytes remain unchanged in HDR exports.
inline float halfFloat(uint16_t bits) {
  const int exponent = (bits >> 10) & 31;
  const int mantissa = bits & 1023;
  if (exponent == 31) {
    if (mantissa) return std::numeric_limits<float>::quiet_NaN();
    return bits & 0x8000 ? -std::numeric_limits<float>::infinity() : std::numeric_limits<float>::infinity();
  }
  float value = exponent ? std::ldexp(float(1024 + mantissa), exponent - 25) : std::ldexp(float(mantissa), -24);
  return bits & 0x8000 ? -value : value;
}
inline uint8_t hdrPreview(float value) {
  if (!std::isfinite(value) || value <= 0) return 0;
  return uint8_t(std::lround((value / (1.0f + value)) * 255.0f));
}

std::string quote(const std::string& text) {
  std::ostringstream out;
  out << '"';
  const char hex[] = "0123456789abcdef";
  for (unsigned char ch : text) {
    if (ch == '"' || ch == '\\') out << '\\' << char(ch);
    else if (ch < 32) out << "\\u00" << hex[ch >> 4] << hex[ch & 15];
    else out << char(ch);
  }
  return out.str() + '"';
}

std::string utf8(const std::wstring& text) {
  if (text.empty()) return {};
  int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), int(text.size()), nullptr, 0, nullptr, nullptr);
  if (!length) throw std::runtime_error("Invalid Unicode argument");
  std::string output(size_t(length), '\0');
  WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), int(text.size()), output.data(), length, nullptr, nullptr);
  return output;
}

void hr(HRESULT result, const char* operation) {
  if (FAILED(result)) throw std::runtime_error(std::string(operation) + ": HRESULT " + std::to_string(uint32_t(result)));
}
void vkcheck(VkResult result, const char* operation) {
  if (result != VK_SUCCESS) throw std::runtime_error(std::string(operation) + ": VkResult " + std::to_string(result));
}

template<typename T> class Com {
public:
  ~Com() { reset(); }
  Com() = default;
  Com(const Com&) = delete;
  Com& operator=(const Com&) = delete;
  T* get() const { return value; }
  T* operator->() const { return value; }
  T** put() { reset(); return &value; }
  void reset() { if (value) { value->Release(); value = nullptr; } }
private:
  T* value = nullptr;
};

struct ImageInfo {
  VkImage image = VK_NULL_HANDLE;
  VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
  VkImageCreateInfo create{};
};

ImageInfo imageInfo(IDirect3DSurface9* surface, Com<ID3D9VkInteropTexture>& interop) {
  hr(surface->QueryInterface(__uuidof(ID3D9VkInteropTexture), reinterpret_cast<void**>(interop.put())), "Surface Vulkan interop QI");
  ImageInfo info;
  info.create.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  hr(interop->GetVulkanImageInfo(&info.image, &info.layout, &info.create), "GetVulkanImageInfo");
  if (!info.image || info.create.samples != VK_SAMPLE_COUNT_1_BIT || info.create.mipLevels != 1 || info.create.arrayLayers != 1 ||
      !(info.create.usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT))
    throw std::runtime_error("Capture requires a single-sample, single-subresource transfer-source image");
  return info;
}

std::string imageJson(const ImageInfo& info) {
  std::ostringstream out;
  out << "{\"format\":" << info.create.format << ",\"width\":" << info.create.extent.width
      << ",\"height\":" << info.create.extent.height << ",\"samples\":" << info.create.samples
      << ",\"usage\":" << info.create.usage << ",\"flush_layout\":" << info.layout << '}';
  return out.str();
}

class Capture {
public:
  explicit Capture(ID3D9VkInteropDevice* interop) : interop(interop) {
    interop->GetVulkanHandles(&instance, &physical, &device);
    interop->GetSubmissionQueue(&queue, nullptr, &family);
    if (!instance || !physical || !device || !queue) throw std::runtime_error("Null DXVK Vulkan handle");
    HMODULE loader = LoadLibraryW(L"vulkan-1.dll");
    if (!loader) throw std::runtime_error("Vulkan loader missing");
    auto getInstance = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(loader, "vkGetInstanceProcAddr"));
    if (!getInstance) throw std::runtime_error("vkGetInstanceProcAddr missing");
    auto getDevice = reinterpret_cast<PFN_vkGetDeviceProcAddr>(getInstance(instance, "vkGetDeviceProcAddr"));
    auto memoryProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(getInstance(instance, "vkGetPhysicalDeviceMemoryProperties"));
    auto properties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(getInstance(instance, "vkGetPhysicalDeviceProperties"));
    if (!getDevice || !memoryProperties || !properties) throw std::runtime_error("Vulkan query entry point missing");
    memoryProperties(physical, &memory);
    properties(physical, &physicalProperties);
#define LOAD(name) name = reinterpret_cast<PFN_##name>(getDevice(device, #name)); if (!name) throw std::runtime_error(#name " missing")
    LOAD(vkCreateImage); LOAD(vkDestroyImage); LOAD(vkGetImageMemoryRequirements);
    LOAD(vkAllocateMemory); LOAD(vkFreeMemory); LOAD(vkBindImageMemory);
    LOAD(vkCreateBuffer); LOAD(vkDestroyBuffer); LOAD(vkGetBufferMemoryRequirements); LOAD(vkBindBufferMemory);
    LOAD(vkCreateCommandPool); LOAD(vkDestroyCommandPool); LOAD(vkAllocateCommandBuffers);
    LOAD(vkResetCommandPool); LOAD(vkBeginCommandBuffer); LOAD(vkEndCommandBuffer);
    LOAD(vkCmdPipelineBarrier); LOAD(vkCmdCopyImage); LOAD(vkCmdCopyImageToBuffer);
    LOAD(vkCreateFence); LOAD(vkDestroyFence); LOAD(vkResetFences); LOAD(vkWaitForFences);
    LOAD(vkQueueSubmit); LOAD(vkQueueWaitIdle); LOAD(vkMapMemory); LOAD(vkUnmapMemory); LOAD(vkInvalidateMappedMemoryRanges);
#undef LOAD
  }

  ~Capture() {
    if (pending) {
      interop->LockSubmissionQueue();
      vkQueueWaitIdle(queue);
      interop->ReleaseSubmissionQueue();
    }
    if (mapped) vkUnmapMemory(device, bufferMemory);
    if (fence) vkDestroyFence(device, fence, nullptr);
    if (pool) vkDestroyCommandPool(device, pool, nullptr);
    if (buffer) vkDestroyBuffer(device, buffer, nullptr);
    if (bufferMemory) vkFreeMemory(device, bufferMemory, nullptr);
    for (size_t index = 0; index < 2; index++) {
      if (images[index]) vkDestroyImage(device, images[index], nullptr);
      if (imageMemory[index]) vkFreeMemory(device, imageMemory[index], nullptr);
    }
  }

  void initialize(const ImageInfo& color, const ImageInfo& depth) {
    width = color.create.extent.width; height = color.create.extent.height;
    formats = {color.create.format, depth.create.format};
    if (depth.create.extent.width != width || depth.create.extent.height != height)
      throw std::runtime_error("Color and depth dimensions differ");
    if (formats[0] != VK_FORMAT_B8G8R8A8_UNORM && formats[0] != VK_FORMAT_R8G8B8A8_UNORM && formats[0] != VK_FORMAT_R16G16B16A16_SFLOAT)
      throw std::runtime_error("Diagnostic readback supports RGBA8/BGRA8 and RGBA16F only");
    colorBytes = formats[0] == VK_FORMAT_R16G16B16A16_SFLOAT ? 8u : 4u;
    depthOffset = VkDeviceSize(width) * height * colorBytes;
    if (formats[1] != VK_FORMAT_D32_SFLOAT && formats[1] != VK_FORMAT_D24_UNORM_S8_UINT && formats[1] != VK_FORMAT_D32_SFLOAT_S8_UINT)
      throw std::runtime_error("Unsupported diagnostic depth format");
    aspects = {VK_IMAGE_ASPECT_COLOR_BIT, formats[1] == VK_FORMAT_D32_SFLOAT ? VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT)
      : VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)};
    std::array<ImageInfo, 2> inputs{color, depth};
    for (size_t index = 0; index < 2; index++) {
      VkImageCreateInfo info{};
      info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
      info.imageType = VK_IMAGE_TYPE_2D; info.format = inputs[index].create.format;
      info.extent = {width, height, 1}; info.mipLevels = 1; info.arrayLayers = 1;
      info.samples = VK_SAMPLE_COUNT_1_BIT; info.tiling = VK_IMAGE_TILING_OPTIMAL;
      info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
      info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
      vkcheck(vkCreateImage(device, &info, nullptr, &images[index]), "vkCreateImage snapshot");
      VkMemoryRequirements requirements{};
      vkGetImageMemoryRequirements(device, images[index], &requirements);
      imageMemory[index] = allocate(requirements, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
      vkcheck(vkBindImageMemory(device, images[index], imageMemory[index], 0), "vkBindImageMemory");
    }
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = depthOffset + VkDeviceSize(width) * height * 4;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    vkcheck(vkCreateBuffer(device, &bufferInfo, nullptr, &buffer), "vkCreateBuffer readback");
    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(device, buffer, &requirements);
    bufferMemory = allocate(requirements, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    vkcheck(vkBindBufferMemory(device, buffer, bufferMemory, 0), "vkBindBufferMemory");
    vkcheck(vkMapMemory(device, bufferMemory, 0, VK_WHOLE_SIZE, 0, &mapped), "vkMapMemory");
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.queueFamilyIndex = family;
    vkcheck(vkCreateCommandPool(device, &poolInfo, nullptr, &pool), "vkCreateCommandPool");
    VkCommandBufferAllocateInfo commands{};
    commands.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    commands.commandPool = pool; commands.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; commands.commandBufferCount = 1;
    vkcheck(vkAllocateCommandBuffers(device, &commands, &command), "vkAllocateCommandBuffers");
    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    vkcheck(vkCreateFence(device, &fenceInfo, nullptr, &fence), "vkCreateFence");
  }

  void copy(const ImageInfo& color, const ImageInfo& depth) {
    std::array<ImageInfo, 2> sources{color, depth};
    vkcheck(vkResetCommandPool(device, pool, 0), "vkResetCommandPool");
    vkcheck(vkResetFences(device, 1, &fence), "vkResetFences");
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkcheck(vkBeginCommandBuffer(command, &begin), "vkBeginCommandBuffer");
    for (size_t index = 0; index < 2; index++) {
      barrier(images[index], aspects[index], initialized ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, initialized ? VK_ACCESS_TRANSFER_READ_BIT : 0, VK_ACCESS_TRANSFER_WRITE_BIT);
      VkImageCopy region{};
      VkImageAspectFlags copyAspect = index ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
      region.srcSubresource = {copyAspect, 0, 0, 1}; region.dstSubresource = region.srcSubresource;
      region.extent = {width, height, 1};
      vkCmdCopyImage(command, sources[index].image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
      barrier(images[index], aspects[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
      VkBufferImageCopy readback{};
      readback.bufferOffset = index ? depthOffset : 0;
      readback.imageSubresource = region.srcSubresource; readback.imageExtent = region.extent;
      vkCmdCopyImageToBuffer(command, images[index], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &readback);
    }
    VkBufferMemoryBarrier hostBarrier{};
    hostBarrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    hostBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; hostBarrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    hostBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; hostBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    hostBarrier.buffer = buffer; hostBarrier.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &hostBarrier, 0, nullptr);
    vkcheck(vkEndCommandBuffer(command), "vkEndCommandBuffer");
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO; submit.commandBufferCount = 1; submit.pCommandBuffers = &command;
    interop->LockSubmissionQueue();
    VkResult submitted = vkQueueSubmit(queue, 1, &submit, fence);
    interop->ReleaseSubmissionQueue();
    vkcheck(submitted, "vkQueueSubmit capture");
    pending = true;
    vkcheck(vkWaitForFences(device, 1, &fence, VK_TRUE, 5'000'000'000ull), "vkWaitForFences capture (5s)");
    pending = false; initialized = true;
    VkMappedMemoryRange range{};
    range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE; range.memory = bufferMemory; range.size = VK_WHOLE_SIZE;
    vkcheck(vkInvalidateMappedMemoryRanges(device, 1, &range), "vkInvalidateMappedMemoryRanges");
  }

  void save(const std::filesystem::path& prefix, uint32_t stride=1) const {
    if(!stride || stride>8) throw std::runtime_error("Invalid export stride");
    const uint32_t outWidth=(width+stride-1)/stride, outHeight=(height+stride-1)/stride;
    std::ofstream color(std::filesystem::path(prefix.wstring() + L"-color.ppm"), std::ios::binary);
    std::ofstream depth(std::filesystem::path(prefix.wstring() + L"-depth.pgm"), std::ios::binary);
    std::ofstream raw(std::filesystem::path(prefix.wstring() + L"-depth.f32"), std::ios::binary);
    std::ofstream hdr;
    if (colorBytes == 8) hdr.open(std::filesystem::path(prefix.wstring() + L"-color.rgba16f"), std::ios::binary);
    color << "P6\n" << outWidth << ' ' << outHeight << "\n255\n";
    depth << "P5\n" << outWidth << ' ' << outHeight << "\n65535\n";
    auto bytes = static_cast<const uint8_t*>(mapped);
    std::vector<char> rgb(size_t(outWidth) * 3), pgm(size_t(outWidth) * 2);
    std::vector<float> values(outWidth);
    for (uint32_t oy = 0; oy < outHeight; oy++) {
      uint32_t y=std::min(oy*stride+stride/2,height-1);
      const auto* row = bytes + size_t(y) * width * colorBytes;

      for (uint32_t x = 0; x < outWidth; x++) {
        uint32_t sx=std::min(x*stride+stride/2,width-1);
        const auto* rgba = row + size_t(sx) * colorBytes;
        if(colorBytes==8) hdr.write(reinterpret_cast<const char*>(rgba),8);
        if (colorBytes == 8) {
          for (size_t channel = 0; channel < 3; channel++) {
            uint16_t bits = 0; std::memcpy(&bits, rgba + channel * 2, 2);
            rgb[size_t(x) * 3 + channel] = char(hdrPreview(halfFloat(bits)));
          }
        } else {
          rgb[size_t(x) * 3] = char(rgba[formats[0] == VK_FORMAT_B8G8R8A8_UNORM ? 2 : 0]);
          rgb[size_t(x) * 3 + 1] = char(rgba[1]);
          rgb[size_t(x) * 3 + 2] = char(rgba[formats[0] == VK_FORMAT_B8G8R8A8_UNORM ? 0 : 2]);
        }
        const auto* address = bytes + size_t(depthOffset) + (size_t(y) * width + sx) * 4;
        float value = 0;
        if (formats[1] == VK_FORMAT_D24_UNORM_S8_UINT) {
          uint32_t packed = 0; std::memcpy(&packed, address, 4);
          value = float(packed & 0xffffffu) / float(0xffffffu);
        } else std::memcpy(&value, address, 4);
        values[x] = value;
        uint16_t preview = std::isfinite(value) ? uint16_t(std::fmax(0.0f, std::fmin(1.0f, value)) * 65535.0f) : 0;
        pgm[size_t(x) * 2] = char(preview >> 8); pgm[size_t(x) * 2 + 1] = char(preview & 255);
      }
      color.write(rgb.data(), std::streamsize(rgb.size()));
      depth.write(pgm.data(), std::streamsize(pgm.size()));
      raw.write(reinterpret_cast<const char*>(values.data()), std::streamsize(values.size() * sizeof(float)));
    }
    color.close(); depth.close(); raw.close();
    if (colorBytes == 8) hdr.close();
    if (!color || !depth || !raw || (colorBytes == 8 && !hdr)) throw std::runtime_error("Snapshot file write failed");
  }

  void verify(uint32_t background) const {
    auto bytes = static_cast<const uint8_t*>(mapped);
    auto testColor = [&](uint32_t x, uint32_t y, uint32_t expected) {
      auto pixel = bytes + (size_t(y) * width + x) * 4;
      uint8_t red = pixel[formats[0] == VK_FORMAT_B8G8R8A8_UNORM ? 2 : 0];
      uint8_t green = pixel[1], blue = pixel[formats[0] == VK_FORMAT_B8G8R8A8_UNORM ? 0 : 2];
      if (std::abs(int(red) - int((expected >> 16) & 255)) > 1 ||
          std::abs(int(green) - int((expected >> 8) & 255)) > 1 || std::abs(int(blue) - int(expected & 255)) > 1)
        throw std::runtime_error("GPU color snapshot does not match the rendered scene");
    };
    testColor(width / 2, height / 2, D3DCOLOR_XRGB(32, 220, 64));
    testColor(2, 2, background);
    auto depthAt = [&](uint32_t x, uint32_t y) {
      auto address = bytes + size_t(depthOffset) + (size_t(y) * width + x) * 4;
      if (formats[1] == VK_FORMAT_D24_UNORM_S8_UINT) {
        uint32_t packed = 0; std::memcpy(&packed, address, 4);
        return float(packed & 0xffffffu) / float(0xffffffu);
      }
      float value = 0; std::memcpy(&value, address, 4); return value;
    };
    if (!std::isfinite(depthAt(width/2, height/2)) || std::abs(depthAt(width/2, height/2) - 0.25f) > 0.0001f ||
        !std::isfinite(depthAt(2, 2)) || std::abs(depthAt(2, 2) - 1.0f) > 0.0001f)
      throw std::runtime_error("GPU depth snapshot does not match near triangle / far clear depth");
  }

  VkInstance instance = VK_NULL_HANDLE;
  VkPhysicalDevice physical = VK_NULL_HANDLE;
  VkDevice device = VK_NULL_HANDLE;
  uint32_t family = 0;
  VkPhysicalDeviceProperties physicalProperties{};
  bool allocated() const { return pool != VK_NULL_HANDLE; }
  std::array<VkImageAspectFlags, 2> aspects{};

private:
  VkDeviceMemory allocate(const VkMemoryRequirements& requirements, VkMemoryPropertyFlags flags) {
    for (uint32_t index = 0; index < memory.memoryTypeCount; index++) {
      if ((requirements.memoryTypeBits & (1u << index)) && (memory.memoryTypes[index].propertyFlags & flags) == flags) {
        VkMemoryAllocateInfo allocation{};
        allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO; allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = index;
        VkDeviceMemory output = VK_NULL_HANDLE;
        vkcheck(vkAllocateMemory(device, &allocation, nullptr, &output), "vkAllocateMemory"); return output;
      }
    }
    throw std::runtime_error("No suitable Vulkan memory type");
  }
  void barrier(VkImage image, VkImageAspectFlags aspect, VkImageLayout before, VkImageLayout after,
               VkAccessFlags sourceAccess, VkAccessFlags destAccess) {
    VkImageMemoryBarrier barrierInfo{};
    barrierInfo.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrierInfo.srcAccessMask = sourceAccess; barrierInfo.dstAccessMask = destAccess;
    barrierInfo.oldLayout = before; barrierInfo.newLayout = after;
    barrierInfo.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; barrierInfo.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrierInfo.image = image; barrierInfo.subresourceRange = {aspect, 0, 1, 0, 1};
    vkCmdPipelineBarrier(command, sourceAccess ? VK_PIPELINE_STAGE_TRANSFER_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
      VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrierInfo);
  }
  ID3D9VkInteropDevice* interop;
  VkPhysicalDeviceMemoryProperties memory{};
  VkQueue queue = VK_NULL_HANDLE;
  uint32_t width = 0, height = 0, colorBytes = 4;
  VkDeviceSize depthOffset = 0;
  std::array<VkFormat, 2> formats{};
  std::array<VkImage, 2> images{};
  std::array<VkDeviceMemory, 2> imageMemory{};
  VkBuffer buffer = VK_NULL_HANDLE;
  VkDeviceMemory bufferMemory = VK_NULL_HANDLE;
  VkCommandPool pool = VK_NULL_HANDLE;
  VkCommandBuffer command = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;
  void* mapped = nullptr;
  bool pending = false, initialized = false;
#define MEMBER(name) PFN_##name name = nullptr
  MEMBER(vkCreateImage); MEMBER(vkDestroyImage); MEMBER(vkGetImageMemoryRequirements);
  MEMBER(vkAllocateMemory); MEMBER(vkFreeMemory); MEMBER(vkBindImageMemory);
  MEMBER(vkCreateBuffer); MEMBER(vkDestroyBuffer); MEMBER(vkGetBufferMemoryRequirements); MEMBER(vkBindBufferMemory);
  MEMBER(vkCreateCommandPool); MEMBER(vkDestroyCommandPool); MEMBER(vkAllocateCommandBuffers);
  MEMBER(vkResetCommandPool); MEMBER(vkBeginCommandBuffer); MEMBER(vkEndCommandBuffer);
  MEMBER(vkCmdPipelineBarrier); MEMBER(vkCmdCopyImage); MEMBER(vkCmdCopyImageToBuffer);
  MEMBER(vkCreateFence); MEMBER(vkDestroyFence); MEMBER(vkResetFences); MEMBER(vkWaitForFences);
  MEMBER(vkQueueSubmit); MEMBER(vkQueueWaitIdle); MEMBER(vkMapMemory); MEMBER(vkUnmapMemory); MEMBER(vkInvalidateMappedMemoryRanges);
#undef MEMBER
};

}

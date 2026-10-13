// Standalone diagnostic scene. No game injection, IPC changes or frame generation.
#define NOMINMAX
#include <windows.h>
#include <d3d9.h>
#include <vulkan/vulkan.h>
#include "dxvk_interop.h"

#include <array>
#include <climits>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

static_assert(sizeof(void*) == 8, "Build this diagnostic for Windows x64");

namespace {

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

struct Options {
  std::filesystem::path dll;
  std::string adapter = "B580";
  std::string runId;
  std::string depthFormat = "auto";
  uint32_t frames = 60, width = 640, height = 360;
  int resetFrame = -2;
};

Options options(int argc, wchar_t** argv) {
  Options out;
  for (int index = 1; index < argc; index += 2) {
    if (index + 1 >= argc) throw std::runtime_error("Arguments require values");
    std::wstring key = argv[index], value = argv[index + 1];
    if (key == L"--dxvk") out.dll = value;
    else if (key == L"--adapter") out.adapter = utf8(value);
    else if (key == L"--run-id") out.runId = utf8(value);
    else if (key == L"--depth-format") {
      out.depthFormat = utf8(value);
      if (out.depthFormat != "auto" && out.depthFormat != "d32" && out.depthFormat != "d24")
        throw std::runtime_error("Depth format must be auto, d32 or d24");
    }
    else {
      size_t consumed = 0;
      int number = std::stoi(value, &consumed);
      if (consumed != value.size()) throw std::runtime_error("Invalid numeric argument");
      if (key == L"--frames" && number >= 2 && number <= 1000) out.frames = uint32_t(number);
      else if (key == L"--width" && number >= 128 && number <= 4096) out.width = uint32_t(number);
      else if (key == L"--height" && number >= 128 && number <= 4096) out.height = uint32_t(number);
      else if (key == L"--reset-frame") out.resetFrame = number;
      else throw std::runtime_error("Unknown option or invalid range");
    }
  }
  if (out.dll.empty() || out.runId.empty() || out.adapter.empty())
    throw std::runtime_error("Required: --dxvk <x64 DLL> --run-id <id>; optional --adapter B580 --frames 60 --width 640 --height 360 --reset-frame 30");
  if (out.resetFrame == -2) out.resetFrame = int(out.frames / 2);
  if (out.resetFrame < -1 || out.resetFrame == 0 || out.resetFrame >= int(out.frames))
    throw std::runtime_error("Reset frame must be -1 (disabled) or between 1 and frames-1");
  out.dll = std::filesystem::absolute(out.dll);
  return out;
}

HMODULE loadDxvk(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  IMAGE_DOS_HEADER dos{};
  input.read(reinterpret_cast<char*>(&dos), sizeof(dos));
  if (!input || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < int(sizeof(dos)) || dos.e_lfanew > 16 * 1024 * 1024)
    throw std::runtime_error("Selected DLL is not a valid PE file");
  input.seekg(dos.e_lfanew);
  DWORD signature = 0;
  IMAGE_FILE_HEADER header{};
  input.read(reinterpret_cast<char*>(&signature), sizeof(signature));
  input.read(reinterpret_cast<char*>(&header), sizeof(header));
  if (!input || signature != IMAGE_NT_SIGNATURE || header.Machine != IMAGE_FILE_MACHINE_AMD64 || !(header.Characteristics & IMAGE_FILE_DLL))
    throw std::runtime_error("Selected backend must be a Windows AMD64 DLL");
  auto module = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
  if (!module) throw std::runtime_error("LoadLibraryExW failed: " + std::to_string(GetLastError()));
  // Keep DXVK loaded until process exit, after all driver/device threads stop.
  return module;
}

class Window {
public:
  Window(uint32_t width, uint32_t height) {
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"L4D2DxvkCaptureDiagnostic";
    if (!RegisterClassW(&wc)) throw std::runtime_error("RegisterClassW failed");
    RECT area{0, 0, LONG(width), LONG(height)};
    if (!AdjustWindowRect(&area, WS_OVERLAPPEDWINDOW, FALSE)) throw std::runtime_error("AdjustWindowRect failed");
    handle = CreateWindowW(wc.lpszClassName, L"DXVK capture diagnostic", WS_OVERLAPPEDWINDOW,
      CW_USEDEFAULT, CW_USEDEFAULT, area.right-area.left, area.bottom-area.top, nullptr, nullptr, wc.hInstance, nullptr);
    if (!handle) throw std::runtime_error("CreateWindowW failed");
    ShowWindow(handle, SW_SHOW);
  }
  ~Window() { if (handle) DestroyWindow(handle); }
  void pump() {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
      if (message.message == WM_QUIT) throw std::runtime_error("Window closed before capture completed");
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
    if (!IsWindow(handle)) throw std::runtime_error("Window closed before capture completed");
  }
  HWND handle = nullptr;
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
    if (formats[0] != VK_FORMAT_B8G8R8A8_UNORM && formats[0] != VK_FORMAT_R8G8B8A8_UNORM)
      throw std::runtime_error("Diagnostic readback supports UNORM RGBA8/BGRA8 only");
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
    bufferInfo.size = VkDeviceSize(width) * height * 8;
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
      readback.bufferOffset = index * VkDeviceSize(width) * height * 4;
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
      auto address = bytes + size_t(width) * height * 4 + (size_t(y) * width + x) * 4;
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
  uint32_t width = 0, height = 0;
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

void draw(IDirect3DDevice9* device, uint32_t frame, uint32_t width, uint32_t height, uint32_t background) {
  hr(device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, background, 1.0f, 0), "Clear");
  hr(device->BeginScene(), "BeginScene");
  hr(device->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE), "ZENABLE");
  hr(device->SetRenderState(D3DRS_ZWRITEENABLE, TRUE), "ZWRITEENABLE");
  hr(device->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESS), "ZFUNC");
  hr(device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE), "CULLMODE");
  hr(device->SetRenderState(D3DRS_LIGHTING, FALSE), "LIGHTING");
  hr(device->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE), "SRGBWRITEENABLE");
  hr(device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1), "COLOROP");
  hr(device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE), "COLORARG1");
  hr(device->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE), "SetFVF");
  struct Vertex { float x, y, z, rhw; DWORD color; };
  float shift = std::sin(float(frame) * 0.1f) * float(width) * 0.04f;
  std::array<Vertex, 3> vertices{{
    {float(width)*0.1f+shift, float(height)*0.1f, 0.25f, 1, D3DCOLOR_XRGB(32,220,64)},
    {float(width)*0.9f+shift, float(height)*0.1f, 0.25f, 1, D3DCOLOR_XRGB(32,220,64)},
    {float(width)*0.5f+shift, float(height)*0.9f, 0.25f, 1, D3DCOLOR_XRGB(32,220,64)}
  }};
  hr(device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, vertices.data(), sizeof(Vertex)), "DrawPrimitiveUP");
  hr(device->EndScene(), "EndScene");
}

void run(const Options& args) {
  Window window(args.width, args.height);
  HMODULE module = loadDxvk(args.dll);
  using Create9 = IDirect3D9* (WINAPI*)(UINT);
  auto create = reinterpret_cast<Create9>(GetProcAddress(module, "Direct3DCreate9"));
  if (!create) throw std::runtime_error("Selected DLL does not export Direct3DCreate9");
  Com<IDirect3D9> api;
  *api.put() = create(D3D_SDK_VERSION);
  if (!api.get()) throw std::runtime_error("Direct3DCreate9 failed");
  UINT selected = UINT_MAX;
  D3DADAPTER_IDENTIFIER9 identifier{};
  for (UINT index = 0; index < api->GetAdapterCount(); index++) {
    hr(api->GetAdapterIdentifier(index, 0, &identifier), "GetAdapterIdentifier");
    if (std::string(identifier.Description).find(args.adapter) != std::string::npos) { selected = index; break; }
  }
  if (selected == UINT_MAX) throw std::runtime_error("No adapter matching " + args.adapter + "; no fallback device used");
  D3DFORMAT depthFormat = args.depthFormat == "d24" ? D3DFMT_D24S8 : D3DFMT_D32F_LOCKABLE;
  if (args.depthFormat == "auto" && FAILED(api->CheckDeviceFormat(selected, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, D3DUSAGE_DEPTHSTENCIL, D3DRTYPE_SURFACE, depthFormat)))
    depthFormat = D3DFMT_D24S8;
  D3DPRESENT_PARAMETERS params{};
  params.BackBufferWidth = args.width; params.BackBufferHeight = args.height;
  params.BackBufferFormat = D3DFMT_A8R8G8B8; params.BackBufferCount = 1;
  params.SwapEffect = D3DSWAPEFFECT_DISCARD; params.hDeviceWindow = window.handle;
  params.Windowed = TRUE; params.EnableAutoDepthStencil = TRUE; params.AutoDepthStencilFormat = depthFormat;
  params.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
  Com<IDirect3DDevice9> device;
  hr(api->CreateDevice(selected, D3DDEVTYPE_HAL, window.handle, D3DCREATE_HARDWARE_VERTEXPROCESSING,
    &params, device.put()), "CreateDevice");
  Com<ID3D9VkInteropDevice> interop;
  hr(device->QueryInterface(__uuidof(ID3D9VkInteropDevice), reinterpret_cast<void**>(interop.put())), "Device Vulkan interop QI");
  auto capture = std::make_unique<Capture>(interop.get());
  if (std::string(capture->physicalProperties.deviceName).find(args.adapter) == std::string::npos)
    throw std::runtime_error("Physical Vulkan device does not match the requested adapter");
  std::cout << "{\"event\":\"device\",\"run_id\":" << quote(args.runId) << ",\"adapter\":" << quote(identifier.Description)
    << ",\"backend\":" << quote(utf8(args.dll.wstring())) << ",\"frames_requested\":" << args.frames
    << ",\"reset_frame\":" << args.resetFrame << ",\"width\":" << params.BackBufferWidth << ",\"height\":" << params.BackBufferHeight
    << ",\"depth_d3d_format\":" << depthFormat << ",\"physical_device_name\":" << quote(capture->physicalProperties.deviceName)
    << ",\"physical_vendor_id\":" << capture->physicalProperties.vendorID << ",\"submission_queue_family\":" << capture->family
    << ",\"gpu_fg_execution\":\"unrun\",\"diagnostic_gpu_waits\":true}\n" << std::flush;
  uint32_t generation = 0;
  LARGE_INTEGER frequency{}, start{}, stop{};
  if (!QueryPerformanceFrequency(&frequency)) throw std::runtime_error("QueryPerformanceFrequency failed");
  for (uint32_t frame = 0; frame < args.frames; frame++) {
    window.pump();
    if (int(frame) == args.resetFrame) {
      capture.reset();
      params.BackBufferWidth = args.width - 64; params.BackBufferHeight = args.height - 64;
      hr(device->Reset(&params), "Reset");
      generation++;
      std::cout << "{\"event\":\"reset\",\"run_id\":" << quote(args.runId) << ",\"generation\":" << generation
        << ",\"before_frame\":" << frame << ",\"width\":" << params.BackBufferWidth << ",\"height\":" << params.BackBufferHeight << "}\n";
    }
    QueryPerformanceCounter(&start);
    uint32_t background = D3DCOLOR_XRGB(16 + frame % 100, 32, 192);
    draw(device.get(), frame, params.BackBufferWidth, params.BackBufferHeight, background);
    {
      Com<IDirect3DSurface9> colorSurface, depthSurface;
      hr(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, colorSurface.put()), "GetBackBuffer");
      hr(device->GetDepthStencilSurface(depthSurface.put()), "GetDepthStencilSurface");
      Com<ID3D9VkInteropTexture> colorInterop, depthInterop;
      auto color = imageInfo(colorSurface.get(), colorInterop), depth = imageInfo(depthSurface.get(), depthInterop);
      if (!capture) capture = std::make_unique<Capture>(interop.get());
      if (!capture->allocated()) capture->initialize(color, depth);
      VkImageSubresourceRange colorRange{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
      interop->TransitionTextureLayout(colorInterop.get(), &colorRange, color.layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
      VkImageSubresourceRange depthRange{capture->aspects[1],0,1,0,1};
      interop->TransitionTextureLayout(depthInterop.get(), &depthRange, depth.layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
      interop->FlushRenderingCommands();
      if (!interop->WaitForResource(colorSurface.get(), 0) || !interop->WaitForResource(depthSurface.get(), 0))
        throw std::runtime_error("DXVK resource wait failed");
      capture->copy(color, depth);
      capture->verify(background);
      interop->TransitionTextureLayout(colorInterop.get(), &colorRange, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, color.layout);
      interop->TransitionTextureLayout(depthInterop.get(), &depthRange, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, depth.layout);
      interop->FlushRenderingCommands();
      if (!interop->WaitForResource(colorSurface.get(), 0) || !interop->WaitForResource(depthSurface.get(), 0))
        throw std::runtime_error("DXVK restoration wait failed");
      hr(device->Present(nullptr, nullptr, nullptr, nullptr), "Present");
      QueryPerformanceCounter(&stop);
      double elapsed = double(stop.QuadPart - start.QuadPart) * 1000.0 / double(frequency.QuadPart);
      std::cout << "{\"event\":\"frame\",\"run_id\":" << quote(args.runId) << ",\"render_frame_id\":" << frame
        << ",\"generation\":" << generation << ",\"color\":" << imageJson(color) << ",\"depth\":" << imageJson(depth)
        << ",\"color_copy_verified\":true,\"depth_copy_verified\":true,\"source_layout_restored\":true,\"present_succeeded\":true"
        << ",\"diagnostic_cpu_ms\":" << elapsed << "}\n" << std::flush;
    }
  }
  capture.reset();
  std::cout << "{\"event\":\"completed\",\"run_id\":" << quote(args.runId) << ",\"frames\":" << args.frames
    << ",\"resets\":" << generation << ",\"gpu_fg_execution\":\"unrun\"}\n" << std::flush;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
  std::string runId;
  try {
    auto args = options(argc, argv); runId = args.runId;
    run(args); return 0;
  } catch (const std::exception& error) {
    std::cout << "{\"event\":\"error\",\"run_id\":" << quote(runId) << ",\"message\":" << quote(error.what()) << "}\n" << std::flush;
    return 2;
  }
}

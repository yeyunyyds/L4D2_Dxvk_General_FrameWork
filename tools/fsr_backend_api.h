#pragma once
#include <cstddef>
#include <cstdint>
#include <vulkan/vulkan.h>
// Version 2 requires regional thin-feature protection before output is
// accepted.
inline constexpr uint32_t LDB_FSR_API_VERSION = 2;
struct LdbFsrFrame {
  uint32_t size = sizeof(LdbFsrFrame), width = 0, height = 0, flags = 0;
  uint64_t frame = 0;
  VkImage images[9]{}; // depth, prev depth, final, prev final, pre-UI, mask,
                       // output, prev near before/after
  VkFormat depthFormat{}, finalFormat{};
  float currentToPrevious[16]{}, viewProjection[16]{};
  float frameTimeMs = 16.6667f;
};
struct LdbFsrApi {
  uint32_t version, size;
  int (*create)(VkDevice, VkPhysicalDevice, uint32_t, uint32_t, void **, char *,
                size_t);
  int (*dispatch)(void *, VkCommandBuffer, const LdbFsrFrame *, char *, size_t);
  void (*destroy)(void *);
};
static_assert(sizeof(void *) != 8 || sizeof(LdbFsrFrame) == 240,
              "FSR x64 frame ABI");
using LdbFsrGetApi = const LdbFsrApi *(*)(uint32_t);

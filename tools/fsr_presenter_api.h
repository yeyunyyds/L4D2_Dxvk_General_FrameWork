#pragma once
#include <cmath>
#include <cstdint>

// Stable C ABI between the packaged x64 DXVK and the x64 Bridge Host. Vulkan
// handles are represented as pointer-sized integers so this header does not
// make the Bridge build depend on DXVK's private Vulkan headers.
inline constexpr uint32_t LDB_FG_DEVICE_API_VERSION = 1;
inline constexpr uint32_t LDB_FG_DEVICE_API_READY = 1u << 0;
inline constexpr uint32_t LDB_FG_DEVICE_API_PRESENTER_QUEUES = 1u << 1;
inline constexpr uint32_t LDB_FG_DEVICE_API_ASYNC_COMPUTE_QUEUE = 1u << 2;

// Frame inputs follow the ownership model used by Asteria/Danyria: a ticket
// identifies one swapchain generation and the images stay valid until the
// presenter retires that ticket. The first Host producer uses camera motion
// reconstructed from depth; a later DXVK producer may provide a motion image.
inline constexpr uint32_t LDB_FG_INPUT_API_VERSION = 1;
inline constexpr uint32_t LDB_FG_INPUT_COLOR = 1u << 0;
inline constexpr uint32_t LDB_FG_INPUT_DEPTH = 1u << 1;
inline constexpr uint32_t LDB_FG_INPUT_CAMERA_MOTION = 1u << 2;
inline constexpr uint32_t LDB_FG_INPUT_MOTION_IMAGE = 1u << 3;
inline constexpr uint32_t LDB_FG_INPUT_HUDLESS = 1u << 4;
inline constexpr uint32_t LDB_FG_INPUT_UI = 1u << 5;
inline constexpr uint32_t LDB_FG_INPUT_GPU_READY = 1u << 6;
inline constexpr uint32_t LDB_FG_INPUT_RESET = 1u << 7;
inline constexpr uint32_t LDB_FG_INPUT_PRESENT_TICKET = 1u << 8;

struct LdbFgDeviceInfoV1 {
  uint32_t version = LDB_FG_DEVICE_API_VERSION;
  uint32_t size = sizeof(LdbFgDeviceInfoV1);
  uint32_t flags = 0;
  uint32_t graphicsFamily = 0;
  uint32_t requestedGraphicsQueues = 0;
  uint32_t reservedQueueCount = 0;
  uint32_t featureFlags = 0;
  uint32_t reserved = 0;
  uintptr_t instance = 0;
  uintptr_t physicalDevice = 0;
  uintptr_t device = 0;
  uintptr_t gameQueue = 0;
  uintptr_t asyncComputeQueue = 0;
  uintptr_t presentQueue = 0;
  uintptr_t imageAcquireQueue = 0;
};

struct LdbFgFrameTicketV1 {
  uint64_t generation = 0;
  uint64_t frameId = 0;
  uintptr_t swapchain = 0;
  uint32_t imageIndex = UINT32_MAX;
  uint32_t reserved = 0;
  uint64_t acquireSequence = 0;
};

struct LdbFgFrameInputsV1 {
  uint32_t version = LDB_FG_INPUT_API_VERSION;
  uint32_t size = sizeof(LdbFgFrameInputsV1);
  uint32_t flags = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t colorFormat = 0;
  uint32_t depthFormat = 0;
  uint32_t motionFormat = 0;
  LdbFgFrameTicketV1 ticket{};
  uintptr_t color = 0;
  uintptr_t depth = 0;
  uintptr_t motion = 0;
  uintptr_t hudless = 0;
  uintptr_t ui = 0;
  uint32_t colorLayout = 0;
  uint32_t depthLayout = 0;
  uint32_t motionLayout = 0;
  uint32_t hudlessLayout = 0;
  uint32_t uiLayout = 0;
  uint32_t reserved = 0;
  uintptr_t readySemaphore = 0;
  uint64_t readyValue = 0;
  float currentViewProjection[16]{};
  float previousViewProjection[16]{};
  float frameTimeMs = 0;
  float cameraNear = 0;
  float cameraFar = 0;
  float cameraVerticalFov = 0;
  float metersPerUnit = 0;
  float cameraPosition[3]{};
  float cameraUp[3]{};
  float cameraRight[3]{};
  float cameraForward[3]{};
};

inline bool LdbFgValidateFrameInputsV1(
    const LdbFgFrameInputsV1 &input) noexcept {
  const uint32_t required =
      LDB_FG_INPUT_COLOR | LDB_FG_INPUT_DEPTH | LDB_FG_INPUT_CAMERA_MOTION;
  if (input.version != LDB_FG_INPUT_API_VERSION ||
      input.size != sizeof(input) || (input.flags & required) != required ||
      !input.width || !input.height || input.width > 4096 ||
      input.height > 4096 || !input.ticket.generation || !input.color ||
      !input.depth || !input.colorFormat || !input.depthFormat ||
      !std::isfinite(input.frameTimeMs) || !(input.frameTimeMs > 0) ||
      !std::isfinite(input.cameraNear) || !(input.cameraNear > 0) ||
      !std::isfinite(input.cameraFar) ||
      !(input.cameraFar > input.cameraNear) ||
      !std::isfinite(input.cameraVerticalFov) ||
      !(input.cameraVerticalFov > 0 && input.cameraVerticalFov < 3.14f) ||
      !std::isfinite(input.metersPerUnit) || !(input.metersPerUnit > 0))
    return false;
  if ((input.flags & LDB_FG_INPUT_MOTION_IMAGE) &&
      (!input.motion || !input.motionFormat))
    return false;
  if ((input.flags & LDB_FG_INPUT_HUDLESS) && !input.hudless)
    return false;
  if ((input.flags & LDB_FG_INPUT_UI) && !input.ui)
    return false;
  if ((input.flags & LDB_FG_INPUT_GPU_READY) &&
      (!input.readySemaphore || !input.readyValue))
    return false;
  if ((input.flags & LDB_FG_INPUT_PRESENT_TICKET) &&
      (!input.ticket.swapchain || input.ticket.imageIndex == UINT32_MAX ||
       !input.ticket.acquireSequence))
    return false;
  for (float value : input.currentViewProjection)
    if (!std::isfinite(value))
      return false;
  for (float value : input.previousViewProjection)
    if (!std::isfinite(value))
      return false;
  return true;
}

using PFN_LdbFgQueryDeviceInfoV1 = uint32_t(__stdcall *)(
    void *, LdbFgDeviceInfoV1 *);

static_assert(sizeof(uintptr_t) == 8, "FG device ABI is x64 only");
static_assert(sizeof(LdbFgDeviceInfoV1) == 88, "FG device ABI changed");
static_assert(sizeof(LdbFgFrameTicketV1) == 40, "FG ticket ABI changed");
static_assert(sizeof(LdbFgFrameInputsV1) == 352, "FG input ABI changed");

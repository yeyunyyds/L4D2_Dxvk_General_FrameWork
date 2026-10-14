#pragma once
#include "fsr_camera.h"
#include <FidelityFX/host/backends/vk/ffx_vk.h>
#include <FidelityFX/host/ffx_frameinterpolation.h>
#include <FidelityFX/host/ffx_opticalflow.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

// Pinned SDK entry point. The SDK queries physical capabilities, so constrain
// permutations to the FP32/unforced-wave profile actually built and enabled.
FfxErrorCode GetDeviceCapabilitiesVK(FfxInterface *, FfxDeviceCapabilities *);

namespace ldb_fsr {
inline PFN_vkVoidFunction VKAPI_CALL deviceProc(VkDevice device,
                                                const char *name) {
  auto fn = vkGetDeviceProcAddr(device, name);
  // SDK asks for the KHR spelling despite using a Vulkan 1.2 device. This
  // operation is core since 1.1; use the equivalent enabled core entry point.
  if (!fn && std::string(name) == "vkGetBufferMemoryRequirements2KHR")
    fn = vkGetDeviceProcAddr(device, "vkGetBufferMemoryRequirements2");
  return fn;
}
inline void check(FfxErrorCode result, const char *operation) {
  if (result != FFX_OK)
    throw std::runtime_error(std::string(operation) + ": " +
                             std::to_string(result));
}
inline FfxErrorCode fp32Capabilities(FfxInterface *backend,
                                     FfxDeviceCapabilities *caps) {
  auto result = GetDeviceCapabilitiesVK(backend, caps);
  if (result == FFX_OK) {
    caps->fp16Supported = false;
    caps->maximumSupportedShaderModel =
        std::min(caps->maximumSupportedShaderModel, FFX_SHADER_MODEL_6_5);
  }
  return result;
}

// One serialized render-device owner, called with that device's queue ordering.
// Does not change the game's thread mode or own a swapchain. The caller owns
// all external resources and must complete GPU work before
// destruction/recreation.
class Components {
  struct DeviceLease {
    static std::atomic_flag &active() {
      static std::atomic_flag a = ATOMIC_FLAG_INIT;
      return a;
    }
    DeviceLease() {
      // The pinned SDK stores VkDeviceContext globally. Reject concurrent
      // component instances rather than let another device overwrite it.
      if (active().test_and_set())
        throw std::runtime_error("FSR component owner already active");
    }
    ~DeviceLease() { active().clear(); }
  } lease;
  std::vector<uint64_t> scratch;
  FfxInterface backend{};
  std::unique_ptr<FfxOpticalflowContext> optical;
  std::unique_ptr<FfxFrameInterpolationContext> interpolation;
  FfxDimensions2D extent{};
  uint64_t lastFrame = 0;
  bool history = false;

public:
  FfxOpticalflowSharedResourceDescriptions opticalResources{};
  FfxFrameInterpolationSharedResourceDescriptions frameResources{};
  Components(VkDevice device, VkPhysicalDevice physical, uint32_t width,
             uint32_t height)
      : extent{width, height} {
    if (width < 64 || height < 64)
      throw std::runtime_error("FSR component extent must be at least 64x64");
    scratch.resize((ffxGetScratchMemorySizeVK(physical, 2) + 7) / 8);
    VkDeviceContext context{device, physical, deviceProc};
    check(ffxGetInterfaceVK(&backend, ffxGetDeviceVK(&context), scratch.data(),
                            scratch.size() * sizeof(uint64_t), 2),
          "SDK interface");
    backend.fpGetDeviceCapabilities = fp32Capabilities;
    optical = std::make_unique<FfxOpticalflowContext>();
    FfxOpticalflowContextDescription of{};
    of.backendInterface = backend;
    of.resolution = extent;
    check(ffxOpticalflowContextCreate(optical.get(), &of),
          "Optical flow create");
    try {
      interpolation = std::make_unique<FfxFrameInterpolationContext>();
      FfxFrameInterpolationContextDescription fi{};
      fi.flags =
          FFX_FRAMEINTERPOLATION_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS |
          FFX_FRAMEINTERPOLATION_ENABLE_DEBUG_CHECKING;
      fi.maxRenderSize = fi.displaySize = extent;
      fi.backBufferFormat = fi.previousInterpolationSourceFormat =
          FFX_SURFACE_FORMAT_R8G8B8A8_UNORM;
      fi.backendInterface = backend;
      check(ffxFrameInterpolationContextCreate(interpolation.get(), &fi),
            "Frame interpolation create");
      check(ffxOpticalflowGetSharedResourceDescriptions(optical.get(),
                                                        &opticalResources),
            "OF resources");
      check(ffxFrameInterpolationGetSharedResourceDescriptions(
                interpolation.get(), &frameResources),
            "FI resources");
    } catch (...) {
      // Failed creation cannot be used. Successfully-created OF can be
      // released.
      ffxOpticalflowContextDestroy(optical.get());
      throw;
    }
  }
  ~Components() {
    if (interpolation)
      ffxFrameInterpolationContextDestroy(interpolation.get());
    if (optical)
      ffxOpticalflowContextDestroy(optical.get());
  }
  Components(const Components &) = delete;
  Components &operator=(const Components &) = delete;

  void dispatch(VkCommandBuffer cmd, FfxResource color, FfxResource depth,
                FfxResource motion, FfxResource output, FfxResource flow,
                FfxResource scd, FfxResource dilatedDepth,
                FfxResource dilatedMotion, FfxResource previousDepth,
                uint64_t frame, bool reset, float frameTimeMs,
                const Camera &camera, FfxResource hudless = {}) {
    if (!cmd || !color.resource || !depth.resource || !motion.resource ||
        !output.resource || !flow.resource || !scd.resource ||
        !dilatedDepth.resource || !dilatedMotion.resource ||
        !previousDepth.resource || !std::isfinite(frameTimeMs) ||
        !(frameTimeMs > 0) || !std::isfinite(camera.nearPlane) ||
        !(camera.nearPlane > 0) || !std::isfinite(camera.farPlane) ||
        !(camera.farPlane > camera.nearPlane) ||
        !std::isfinite(camera.verticalFov) ||
        !(camera.verticalFov > 0 && camera.verticalFov < 3.14f) ||
        !std::isfinite(camera.metersPerUnit) || !(camera.metersPerUnit > 0))
      throw std::runtime_error("Invalid FSR frame inputs");
    auto image = [](const FfxResource &r, FfxSurfaceFormat format, uint32_t w,
                    uint32_t h) {
      return r.resource && r.description.type == FFX_RESOURCE_TYPE_TEXTURE2D &&
             r.description.format == format && r.description.width == w &&
             r.description.height == h && r.description.depth == 1 &&
             r.description.mipCount == 1;
    };
    if (!image(color, FFX_SURFACE_FORMAT_R8G8B8A8_UNORM, extent.width,
               extent.height) ||
        !image(output, FFX_SURFACE_FORMAT_R8G8B8A8_UNORM, extent.width,
               extent.height) ||
        !image(depth, FFX_SURFACE_FORMAT_R32_FLOAT, extent.width,
               extent.height) ||
        !image(motion, FFX_SURFACE_FORMAT_R32G32_FLOAT, extent.width,
               extent.height) ||
        !image(dilatedDepth, FFX_SURFACE_FORMAT_R32_FLOAT, extent.width,
               extent.height) ||
        !image(dilatedMotion, FFX_SURFACE_FORMAT_R16G16_FLOAT, extent.width,
               extent.height) ||
        !image(previousDepth, FFX_SURFACE_FORMAT_R32_UINT, extent.width,
               extent.height) ||
        !image(flow, FFX_SURFACE_FORMAT_R16G16_SINT,
               opticalResources.opticalFlowVector.resourceDescription.width,
               opticalResources.opticalFlowVector.resourceDescription.height) ||
        !image(scd, FFX_SURFACE_FORMAT_R32_UINT, 3, 1) ||
        (hudless.resource && !image(hudless, FFX_SURFACE_FORMAT_R8G8B8A8_UNORM,
                                    extent.width, extent.height)))
      throw std::runtime_error("FSR resource contract mismatch");
    std::array<void *, 9> resources{
        color.resource,        depth.resource,         motion.resource,
        output.resource,       flow.resource,          scd.resource,
        dilatedDepth.resource, dilatedMotion.resource, previousDepth.resource};
    for (size_t i = 0; i < resources.size(); ++i)
      for (size_t j = i + 1; j < resources.size(); ++j)
        if (resources[i] == resources[j])
          throw std::runtime_error("Aliased FSR resources");
    if (hudless.resource && hudless.resource == output.resource)
      throw std::runtime_error("Aliased FSR HUD-less output");
    for (const auto &axis : {camera.up, camera.right, camera.forward}) {
      double length = 0;
      for (float v : axis)
        length += double(v) * v;
      if (!std::isfinite(length) || std::abs(length - 1.0) > 0.01)
        throw std::runtime_error("Invalid FSR camera basis");
    }
    auto dot = [](auto a, auto b) {
      return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    };
    if (std::abs(dot(camera.up, camera.right)) > 0.01f ||
        std::abs(dot(camera.up, camera.forward)) > 0.01f ||
        std::abs(dot(camera.right, camera.forward)) > 0.01f)
      throw std::runtime_error("Non-orthogonal FSR camera basis");
    for (float v : camera.position)
      if (!std::isfinite(v))
        throw std::runtime_error("Invalid FSR camera position");
    reset =
        reset || !history || lastFrame == UINT64_MAX || frame != lastFrame + 1;
    FfxOpticalflowDispatchDescription of{};
    of.commandList = ffxGetCommandListVK(cmd);
    of.color = color;
    of.opticalFlowVector = flow;
    of.opticalFlowSCD = scd;
    of.reset = reset;
    of.backbufferTransferFunction = FFX_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
    of.minMaxLuminance = {0, 1};
    check(ffxOpticalflowContextDispatch(optical.get(), &of),
          "Optical flow dispatch");
    FfxFrameInterpolationPrepareDescription prep{};
    prep.commandList = of.commandList;
    prep.renderSize = extent;
    prep.motionVectorScale = {1, 1}; // Current -> previous in display pixels.
    prep.frameTimeDelta = frameTimeMs;
    prep.cameraNear = camera.nearPlane;
    prep.cameraFar = camera.farPlane;
    prep.viewSpaceToMetersFactor = camera.metersPerUnit;
    prep.cameraFovAngleVertical = camera.verticalFov;
    prep.depth = depth;
    prep.motionVectors = motion;
    prep.frameID = frame;
    prep.dilatedDepth = dilatedDepth;
    prep.dilatedMotionVectors = dilatedMotion;
    prep.reconstructedPrevDepth = previousDepth;
    std::copy(camera.position.begin(), camera.position.end(),
              prep.cameraPosition);
    std::copy(camera.up.begin(), camera.up.end(), prep.cameraUp);
    std::copy(camera.right.begin(), camera.right.end(), prep.cameraRight);
    std::copy(camera.forward.begin(), camera.forward.end(), prep.cameraForward);
    check(ffxFrameInterpolationPrepare(interpolation.get(), &prep),
          "FI prepare");
    FfxFrameInterpolationDispatchDescription fi{};
    fi.commandList = of.commandList;
    fi.displaySize = fi.renderSize = extent;
    fi.currentBackBuffer = color;
    fi.currentBackBuffer_HUDLess = hudless;
    fi.output = output;
    fi.interpolationRect = {0, 0, extent.width, extent.height};
    fi.opticalFlowVector = flow;
    fi.opticalFlowSceneChangeDetection = scd;
    fi.opticalFlowBufferSize = {flow.description.width,
                                flow.description.height};
    fi.opticalFlowScale = {1.f / extent.width, 1.f / extent.height};
    fi.opticalFlowBlockSize = 8;
    fi.cameraNear = camera.nearPlane;
    fi.cameraFar = camera.farPlane;
    fi.cameraFovAngleVertical = camera.verticalFov;
    fi.viewSpaceToMetersFactor = camera.metersPerUnit;
    fi.frameTimeDelta = frameTimeMs;
    fi.reset = reset;
    fi.backBufferTransferFunction = FFX_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
    fi.minMaxLuminance[1] = 1;
    fi.frameID = frame;
    fi.dilatedDepth = dilatedDepth;
    fi.dilatedMotionVectors = dilatedMotion;
    fi.reconstructedPrevDepth = previousDepth;
    check(ffxFrameInterpolationDispatch(interpolation.get(), &fi),
          "FI dispatch");
    lastFrame = frame;
    history = true;
  }
};
} // namespace ldb_fsr

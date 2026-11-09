#pragma once

#include "fsr_camera.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <windows.h>

namespace ldb_fg {

inline constexpr uint32_t ASTERIA_HOST_FRAME_API_VERSION_2 = 2;
inline constexpr uint32_t ASTERIA_HOST_FRAME_MAIN_VIEW = 1u << 0;
inline constexpr uint32_t ASTERIA_HOST_FRAME_SCENE_DEPTH = 1u << 1;
inline constexpr uint32_t ASTERIA_HOST_FRAME_CAMERA = 1u << 2;
inline constexpr uint32_t ASTERIA_HOST_FRAME_HUD_BOUNDARY = 1u << 3;
inline constexpr uint32_t ASTERIA_HOST_FRAME_HISTORY_RESET = 1u << 4;
inline constexpr uint32_t ASTERIA_HOST_FRAME_GPU_INPUTS = 1u << 5;

// Same-process control ABI. Resource identities are diagnostic D3D9 Host
// identities in v1; Vulkan image ownership remains with the Layer.
struct AsteriaHostFrameV2 {
  uint32_t structSize = sizeof(AsteriaHostFrameV2);
  uint32_t apiVersion = ASTERIA_HOST_FRAME_API_VERSION_2;
  uint32_t flags = 0;
  uint32_t reserved = 0;
  uint64_t generation = 0;
  uint64_t frameId = 0;
  uint64_t qpc = 0;
  uint64_t sceneColorId = 0;
  uint64_t sceneDepthId = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t worldDraws = 0;
  uint32_t nearDraws = 0;
  float deltaMilliseconds = 0;
  float cameraNear = 0;
  float cameraFar = 0;
  float cameraVerticalFov = 0;
  float metersPerUnit = 0;
  float cameraPosition[3]{};
  float cameraUp[3]{};
  float cameraRight[3]{};
  float cameraForward[3]{};
  uint64_t vkDevice = 0;
  uint64_t depthImage = 0;
  uint64_t motionImage = 0;
  uint32_t depthFormat = 0;
  uint32_t motionFormat = 0;
  uint32_t depthLayout = 0;
  uint32_t motionLayout = 0;
};

struct AsteriaHostFrameStatsV2 {
  uint32_t structSize = sizeof(AsteriaHostFrameStatsV2);
  uint32_t apiVersion = ASTERIA_HOST_FRAME_API_VERSION_2;
  uint64_t submitted = 0;
  uint64_t accepted = 0;
  uint64_t consumed = 0;
  uint64_t rejected = 0;
  uint64_t staleAtPresent = 0;
  uint64_t latestGeneration = 0;
  uint64_t latestFrameId = 0;
  uint64_t latestConsumedFrameId = 0;
};

static_assert(sizeof(AsteriaHostFrameV2) == 184,
              "Asteria Host frame ABI changed");
static_assert(sizeof(AsteriaHostFrameStatsV2) == 72,
              "Asteria Host frame stats ABI changed");

using PFN_AsteriaSubmitHostFrameV2 =
    int32_t(__stdcall *)(const AsteriaHostFrameV2 *);
using PFN_AsteriaQueryHostFrameStatsV2 =
    int32_t(__stdcall *)(AsteriaHostFrameStatsV2 *);

class AsteriaHostControl {
  HMODULE m_module = nullptr;
  PFN_AsteriaSubmitHostFrameV2 m_submit = nullptr;
  PFN_AsteriaQueryHostFrameStatsV2 m_query = nullptr;
  uint64_t m_attempted = 0, m_accepted = 0;

  void resolve() noexcept {
    if (m_module)
      return;
    m_module = GetModuleHandleW(L"AsteriaFrameGenerationLayer64.dll");
    if (!m_module)
      return;
    m_submit = reinterpret_cast<PFN_AsteriaSubmitHostFrameV2>(
        GetProcAddress(m_module, "AsteriaSubmitHostFrameV2"));
    m_query = reinterpret_cast<PFN_AsteriaQueryHostFrameStatsV2>(
        GetProcAddress(m_module, "AsteriaQueryHostFrameStatsV2"));
  }

public:
  bool available() noexcept {
    resolve();
    return m_submit && m_query;
  }

  bool submit(const ldb_camera::Matrix &viewProjection, uint64_t generation,
              uint64_t frameId, uint64_t qpc, double deltaMilliseconds,
              uint64_t sceneColorId, uint64_t sceneDepthId, uint32_t width,
              uint32_t height, uint32_t worldDraws, uint32_t nearDraws,
              bool hudBoundary, bool reset, uintptr_t vkDevice,
              uintptr_t depthImage, uintptr_t motionImage,
              uint32_t depthFormat, uint32_t motionFormat,
              uint32_t depthLayout, uint32_t motionLayout) noexcept {
    ++m_attempted;
    if (!available())
      return false;
    ldb_fsr::Camera camera;
    if (!ldb_fsr::cameraFromViewProjection(viewProjection, .0254f, camera) ||
        !std::isfinite(deltaMilliseconds) || deltaMilliseconds <= 0 ||
        deltaMilliseconds > 1000)
      return false;
    AsteriaHostFrameV2 input;
    input.flags = ASTERIA_HOST_FRAME_MAIN_VIEW |
                  ASTERIA_HOST_FRAME_SCENE_DEPTH |
                  ASTERIA_HOST_FRAME_CAMERA |
                  ASTERIA_HOST_FRAME_GPU_INPUTS |
                  (hudBoundary ? ASTERIA_HOST_FRAME_HUD_BOUNDARY : 0u) |
                  (reset ? ASTERIA_HOST_FRAME_HISTORY_RESET : 0u);
    input.generation = generation;
    input.frameId = frameId;
    input.qpc = qpc;
    input.sceneColorId = sceneColorId;
    input.sceneDepthId = sceneDepthId;
    input.width = width;
    input.height = height;
    input.worldDraws = worldDraws;
    input.nearDraws = nearDraws;
    input.deltaMilliseconds = float(deltaMilliseconds);
    input.cameraNear = camera.nearPlane;
    input.cameraFar = camera.farPlane;
    input.cameraVerticalFov = camera.verticalFov;
    input.metersPerUnit = camera.metersPerUnit;
    std::copy(camera.position.begin(), camera.position.end(),
              input.cameraPosition);
    std::copy(camera.up.begin(), camera.up.end(), input.cameraUp);
    std::copy(camera.right.begin(), camera.right.end(), input.cameraRight);
    std::copy(camera.forward.begin(), camera.forward.end(),
              input.cameraForward);
    input.vkDevice = uint64_t(vkDevice);
    input.depthImage = uint64_t(depthImage);
    input.motionImage = uint64_t(motionImage);
    input.depthFormat = depthFormat;
    input.motionFormat = motionFormat;
    input.depthLayout = depthLayout;
    input.motionLayout = motionLayout;
    const bool accepted = m_submit(&input) == 0;
    m_accepted += accepted ? 1 : 0;
    return accepted;
  }

  bool query(AsteriaHostFrameStatsV2 &stats) noexcept {
    if (!available())
      return false;
    stats = {};
    return m_query(&stats) == 0;
  }

  uint64_t attempted() const noexcept { return m_attempted; }
  uint64_t accepted() const noexcept { return m_accepted; }
};

} // namespace ldb_fg

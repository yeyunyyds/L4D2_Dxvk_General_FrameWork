#pragma once

#include "fsr_camera.h"

#include <algorithm>
#include <array>
#include <atomic>
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
inline constexpr uint32_t ASTERIA_FG_INPUT_API_V1 = 1;
inline constexpr uint32_t ASTERIA_FG_INPUT_API_V2 = 2;
inline constexpr int32_t ASTERIA_FG_INPUT_OK = 0;
inline constexpr uint32_t ASTERIA_FG_MOTION_ENGINE = 1;
inline constexpr uint32_t ASTERIA_FG_FORMAT_R32_FLOAT = 1;
inline constexpr uint32_t ASTERIA_FG_FORMAT_RG32_FLOAT = 3;
inline constexpr uint32_t ASTERIA_FG_FORMAT_RGBA8_UNORM = 4;
inline constexpr uint32_t ASTERIA_FG_FORMAT_BGRA8_UNORM = 5;
inline constexpr uint32_t ASTERIA_FG_LAYOUT_SHADER_READ_ONLY = 1;
inline constexpr uint32_t ASTERIA_FG_DEPTH_SCENE = 1u;
inline constexpr uint32_t ASTERIA_FG_GPU_COMPLETED = 2u;
inline constexpr uint32_t ASTERIA_FG_HUD_ABSENT = 4u;
inline constexpr uint32_t ASTERIA_FG_HISTORY_RESET = 8u;
inline constexpr uint32_t ASTERIA_FG_GPU_ORDERED = 16u;

#pragma pack(push, 8)
struct AsteriaFrameInputRegistrationV1 {
  uint32_t struct_size = sizeof(AsteriaFrameInputRegistrationV1);
  uint32_t api_version = ASTERIA_FG_INPUT_API_V1;
  uint32_t max_live_leases = 3;
  uint32_t reserved = 0;
};
struct AsteriaFrameInputTicketV1 {
  uint32_t struct_size = sizeof(AsteriaFrameInputTicketV1);
  uint32_t api_version = ASTERIA_FG_INPUT_API_V1;
  uint64_t registration = 0, device = 0, swapchain = 0, generation = 0,
           frame_id = 0;
  uint32_t width = 0, height = 0, image_index = 0, reserved = 0;
  uint64_t acquire_id = 0;
};
using AsteriaFrameInputLeaseCallbackV1 = void(__stdcall *)(void *);
struct AsteriaFrameInputSubmissionV2 {
  uint32_t struct_size = sizeof(AsteriaFrameInputSubmissionV2);
  uint32_t api_version = ASTERIA_FG_INPUT_API_V2;
  AsteriaFrameInputTicketV1 ticket{};
  uint64_t depth_image = 0, motion_image = 0, hudless_image = 0, ui_image = 0;
  uint64_t producer_queue = 0;
  uint32_t depth_format = 0, motion_format = 0, hudless_format = 0,
           ui_format = 0;
  uint32_t image_layout = ASTERIA_FG_LAYOUT_SHADER_READ_ONLY;
  uint32_t motion_source = ASTERIA_FG_MOTION_ENGINE;
  uint32_t flags = 0, reserved = 0;
  float delta_milliseconds = 0, near_plane = 0, far_plane = 0,
        vertical_fov_radians = 0, meters_per_unit = 0;
  float position[3]{}, up[3]{}, right[3]{}, forward[3]{}, motion_scale[2]{};
  void *lease_context = nullptr;
  AsteriaFrameInputLeaseCallbackV1 retain_lease = nullptr,
                                    release_lease = nullptr;
};
#pragma pack(pop)
static_assert(sizeof(AsteriaFrameInputRegistrationV1) == 16,
              "Asteria registration ABI changed");
static_assert(sizeof(AsteriaFrameInputTicketV1) == 72,
              "Asteria ticket ABI changed");
static_assert(sizeof(AsteriaFrameInputSubmissionV2) == 256,
              "Asteria native input V2 ABI changed");

using PFN_AsteriaRegisterFrameInputProviderV1 =
    int32_t(__stdcall *)(const AsteriaFrameInputRegistrationV1 *, uint64_t *);
using PFN_AsteriaUnregisterFrameInputProviderV1 = int32_t(__stdcall *)(uint64_t);
using PFN_AsteriaQueryFrameInputTicketV1 = int32_t(__stdcall *)(
    uint64_t, uint64_t, uint64_t, AsteriaFrameInputTicketV1 *);
using PFN_AsteriaSubmitFrameInputsV2 =
    int32_t(__stdcall *)(const AsteriaFrameInputSubmissionV2 *);

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
  PFN_AsteriaRegisterFrameInputProviderV1 m_registerProvider = nullptr;
  PFN_AsteriaUnregisterFrameInputProviderV1 m_unregisterProvider = nullptr;
  PFN_AsteriaQueryFrameInputTicketV1 m_queryTicket = nullptr;
  PFN_AsteriaSubmitFrameInputsV2 m_submitInputs = nullptr;
  uint64_t m_registration = 0;
  uint64_t m_attempted = 0, m_accepted = 0;
  uint64_t m_nativeQueries = 0, m_nativeTickets = 0,
           m_nativeSubmitted = 0, m_nativeAccepted = 0, m_nativeBusy = 0;
  struct LeaseSlot {
    uint32_t index = 0;
    std::atomic<uint32_t> references{0};
    std::atomic<uint64_t> *released = nullptr;
    bool writing = false;
  };
  // Lease callbacks can run after Bridge State destruction during DLL/device
  // teardown. Keep their tiny control block process-lifetime; the OS reclaims
  // it after the Vulkan layer releases its retained Host module reference.
  std::atomic<uint64_t> *m_nativeReleased = new std::atomic<uint64_t>{0};
  std::array<LeaseSlot, 3> *m_slots = new std::array<LeaseSlot, 3>{};

  static void __stdcall retainLease(void *context) noexcept {
    auto *slot = static_cast<LeaseSlot *>(context);
    if (slot)
      slot->references.fetch_add(1, std::memory_order_acq_rel);
  }
  static void __stdcall releaseLease(void *context) noexcept {
    auto *slot = static_cast<LeaseSlot *>(context);
    if (!slot)
      return;
    const auto before = slot->references.fetch_sub(1, std::memory_order_acq_rel);
    if (before && slot->released)
      slot->released->fetch_add(1, std::memory_order_relaxed);
  }

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
    m_registerProvider = reinterpret_cast<PFN_AsteriaRegisterFrameInputProviderV1>(
        GetProcAddress(m_module, "AsteriaRegisterFrameInputProviderV1"));
    m_unregisterProvider = reinterpret_cast<PFN_AsteriaUnregisterFrameInputProviderV1>(
        GetProcAddress(m_module, "AsteriaUnregisterFrameInputProviderV1"));
    m_queryTicket = reinterpret_cast<PFN_AsteriaQueryFrameInputTicketV1>(
        GetProcAddress(m_module, "AsteriaQueryFrameInputTicketV1"));
    m_submitInputs = reinterpret_cast<PFN_AsteriaSubmitFrameInputsV2>(
        GetProcAddress(m_module, "AsteriaSubmitFrameInputsV2"));
  }

  bool ensureNative() noexcept {
    resolve();
    if (!m_registerProvider || !m_unregisterProvider || !m_queryTicket ||
        !m_submitInputs)
      return false;
    if (m_registration)
      return true;
    AsteriaFrameInputRegistrationV1 registration;
    return m_registerProvider(&registration, &m_registration) ==
               ASTERIA_FG_INPUT_OK &&
           m_registration;
  }

public:
  AsteriaHostControl() noexcept {
    for (uint32_t i = 0; i < m_slots->size(); ++i) {
      (*m_slots)[i].index = i;
      (*m_slots)[i].released = m_nativeReleased;
    }
  }
  ~AsteriaHostControl() {
    shutdown();
  }
  AsteriaHostControl(const AsteriaHostControl &) = delete;
  AsteriaHostControl &operator=(const AsteriaHostControl &) = delete;

  void shutdown() noexcept {
    if (m_registration && m_unregisterProvider) {
      m_unregisterProvider(m_registration);
      m_registration = 0;
    }
  }

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

  int acquireNativeSlot(uintptr_t device,
                        AsteriaFrameInputTicketV1 &ticket) noexcept {
    ++m_nativeQueries;
    if (!ensureNative() || !device)
      return -1;
    ticket = {};
    if (m_queryTicket(m_registration, uint64_t(device), 0, &ticket) !=
        ASTERIA_FG_INPUT_OK)
      return -1;
    ++m_nativeTickets;
    for (auto &slot : *m_slots) {
      if (!slot.writing &&
          slot.references.load(std::memory_order_acquire) == 0) {
        slot.writing = true;
        return int(slot.index);
      }
    }
    ++m_nativeBusy;
    return -1;
  }

  void cancelNativeSlot(uint32_t slot) noexcept {
    if (slot < m_slots->size())
      (*m_slots)[slot].writing = false;
  }

  bool submitNative(const AsteriaFrameInputTicketV1 &ticket, uint32_t slot,
                    const ldb_camera::Matrix &viewProjection,
                    double deltaMilliseconds, bool reset,
                    uintptr_t depthImage, uintptr_t motionImage,
                    uintptr_t hudlessImage, uintptr_t uiImage,
                    uintptr_t producerQueue,
                    uint32_t depthFormat, uint32_t motionFormat,
                    uint32_t hudlessFormat, uint32_t uiFormat,
                    uint32_t imageLayout) noexcept {
    ++m_nativeSubmitted;
    if (slot >= m_slots->size() || !m_submitInputs) {
      cancelNativeSlot(slot);
      return false;
    }
    ldb_fsr::Camera camera;
    if (!ldb_fsr::cameraFromViewProjection(viewProjection, .0254f, camera) ||
        !std::isfinite(deltaMilliseconds) || deltaMilliseconds <= 0 ||
        deltaMilliseconds > 1000) {
      cancelNativeSlot(slot);
      return false;
    }
    AsteriaFrameInputSubmissionV2 input;
    input.ticket = ticket;
    input.depth_image = uint64_t(depthImage);
    input.motion_image = uint64_t(motionImage);
    input.hudless_image = uint64_t(hudlessImage);
    input.ui_image = uint64_t(uiImage);
    input.producer_queue = uint64_t(producerQueue);
    input.depth_format = depthFormat == 100 ? ASTERIA_FG_FORMAT_R32_FLOAT : 0;
    input.motion_format = motionFormat == 103 ? ASTERIA_FG_FORMAT_RG32_FLOAT : 0;
    input.hudless_format =
        hudlessFormat == 44 ? ASTERIA_FG_FORMAT_BGRA8_UNORM
                            : hudlessFormat == 37
                                  ? ASTERIA_FG_FORMAT_RGBA8_UNORM
                                  : 0;
    input.ui_format =
        uiFormat == 37 ? ASTERIA_FG_FORMAT_RGBA8_UNORM : 0;
    input.image_layout = imageLayout == 5
                             ? ASTERIA_FG_LAYOUT_SHADER_READ_ONLY
                             : 0;
    input.flags = ASTERIA_FG_DEPTH_SCENE | ASTERIA_FG_GPU_ORDERED |
                  ASTERIA_FG_HUD_ABSENT |
                  (reset ? ASTERIA_FG_HISTORY_RESET : 0u);
    input.delta_milliseconds = float(deltaMilliseconds);
    input.near_plane = camera.nearPlane;
    input.far_plane = camera.farPlane;
    input.vertical_fov_radians = camera.verticalFov;
    input.meters_per_unit = camera.metersPerUnit;
    std::copy(camera.position.begin(), camera.position.end(), input.position);
    std::copy(camera.up.begin(), camera.up.end(), input.up);
    std::copy(camera.right.begin(), camera.right.end(), input.right);
    std::copy(camera.forward.begin(), camera.forward.end(), input.forward);
    input.motion_scale[0] = float(ticket.width);
    input.motion_scale[1] = float(ticket.height);
    input.lease_context = &(*m_slots)[slot];
    input.retain_lease = &retainLease;
    input.release_lease = &releaseLease;
    const bool accepted = m_submitInputs(&input) == ASTERIA_FG_INPUT_OK;
    (*m_slots)[slot].writing = false;
    if (accepted)
      ++m_nativeAccepted;
    return accepted;
  }

  uint64_t nativeQueries() const noexcept { return m_nativeQueries; }
  uint64_t nativeTickets() const noexcept { return m_nativeTickets; }
  uint64_t nativeSubmitted() const noexcept { return m_nativeSubmitted; }
  uint64_t nativeAccepted() const noexcept { return m_nativeAccepted; }
  uint64_t nativeReleased() const noexcept {
    return m_nativeReleased->load(std::memory_order_relaxed);
  }
  uint64_t nativeBusy() const noexcept { return m_nativeBusy; }
};

} // namespace ldb_fg

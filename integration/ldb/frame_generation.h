#pragma once
#include <cstdint>
#include <d3d9.h>
#include <windows.h>
#if defined(_WIN64)
#include "fsr_presenter_api.h"
#include "frame_math.h"
#include "resident_frame_generator.h"
#include "asteria_host_control.h"
#include <array>
#include <cmath>
#include <filesystem>
#include <stdexcept>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <vector>
namespace ldb_fg {
using namespace dxvk_capture;

inline std::wstring readEnv(const wchar_t *key) {
  DWORD n = GetEnvironmentVariableW(key, nullptr, 0);

  if (!n)
    return {};
  std::wstring s(n, L'\0');
  DWORD used = GetEnvironmentVariableW(key, s.data(), n);

  if (!used || used >= n)
    return {};
  s.resize(used);
  return s;
}
struct Config {
  bool enabled = false, generatedOnly = false, providerOnly = false,
       hotkeys = true, fastInputs = false, hudBypass = false,
       viewmodel = false, dynamicModels = false, fpbProtect = false,
       mergeSubmissions = true;
  float dynamicResidualThreshold = 0.08f;
  bool dynamicResidualOverrideRejected = false;
  float firstPersonFallbackStart = 0.28f;
  bool firstPersonFallbackOverrideRejected = false;
  uint32_t dynamicMaskDilation = 1;
  bool dynamicMaskDilationOverrideRejected = false;
  uint32_t captures = 4, pacingMs = 0;
  bool autoCapture = false;
  std::filesystem::path root;

  Config() noexcept {
    try {
      enabled = readEnv(L"LDB_FG_ENABLE") == L"1";

      if (!enabled)
        return;

      auto mode = readEnv(L"LDB_FG_MODE");

      if (!mode.empty() && mode != L"paired" && mode != L"generated" &&
          mode != L"provider")
        throw std::runtime_error("FG mode");

      generatedOnly = mode == L"generated";
      providerOnly = mode == L"provider";
      fastInputs = readEnv(L"LDB_FG_FAST_INPUTS") == L"1";
      hudBypass = readEnv(L"LDB_FG_HUD_BYPASS") == L"1";
      viewmodel = readEnv(L"LDB_FG_VIEWMODEL") == L"1";
      dynamicModels = readEnv(L"LDB_FG_DYNAMIC_MODELS") == L"1";
      fpbProtect = readEnv(L"LDB_FG_FPB_PROTECT") == L"1";
      dynamicResidualThreshold = dynamicModels && fpbProtect ? 0.035f : 0.08f;
      const auto residualOverride =
          readEnv(L"LDB_FG_DYNAMIC_RESIDUAL_THRESHOLD");
      if (!residualOverride.empty() && residualOverride != L"auto") {
        try {
          size_t used = 0;
          const float value = std::stof(residualOverride, &used);
          if (used != residualOverride.size() || !std::isfinite(value) ||
              value < 0.001f || value > 0.5f)
            throw std::out_of_range("FG dynamic residual threshold");
          dynamicResidualThreshold = value;
        } catch (...) {
          // Bad diagnostic value should not disable frame generation.
          dynamicResidualOverrideRejected = true;
        }
      }
      const auto fallbackOverride = readEnv(L"LDB_FG_FIRST_PERSON_FALLBACK_START");
      if (!fallbackOverride.empty()) {
        try {
          size_t used = 0;
          const float value = std::stof(fallbackOverride, &used);
          if (used != fallbackOverride.size() || !std::isfinite(value) ||
              value < 0.0f || value > 1.0f)
            throw std::out_of_range("FG first person fallback start");
          firstPersonFallbackStart = value;
        } catch (...) {
          firstPersonFallbackOverrideRejected = true;
        }
      }
      const auto dilationOverride = readEnv(L"LDB_FG_DYNAMIC_MASK_DILATION");
      if (!dilationOverride.empty()) {
        try {
          size_t used = 0;
          const auto value = std::stoul(dilationOverride, &used);
          if (used != dilationOverride.size() || value > 1u)
            throw std::out_of_range("FG dynamic mask dilation");
          dynamicMaskDilation = uint32_t(value);
        } catch (...) {
          dynamicMaskDilationOverrideRejected = true;
        }
      }
      mergeSubmissions = readEnv(L"LDB_FG_MERGE_SUBMISSIONS") != L"0";
      autoCapture = readEnv(L"LDB_FG_AUTO_CAPTURE") == L"1";
      auto pacing = readEnv(L"LDB_FG_PACING_MS");
      if (!pacing.empty()) {
        size_t used = 0;
        auto value = std::stoul(pacing, &used);
        if (used != pacing.size() || value > 8)
          throw std::runtime_error("FG pacing budget");
        pacingMs = uint32_t(value);
      }
      hotkeys = readEnv(L"LDB_FG_HOTKEYS") != L"0";
      root = readEnv(L"LDB_FG_LOG_DIR");

      if (root.empty())
        root = std::filesystem::temp_directory_path() / "LDB-FG";

      if (!root.is_absolute())
        throw std::runtime_error("FG log path");

      auto count = readEnv(L"LDB_FG_CAPTURE_FRAMES");

      if (!count.empty()) {
        size_t used = 0;
        auto value = std::stoul(count, &used);

        if (used != count.size() || value > 16)
          throw std::runtime_error("FG capture count");

        captures = uint32_t(value);
      }
    } catch (...) {
      enabled = false;
    }
  }
};

inline const Config &config() {
  static const Config c;
  return c;
}
inline uint64_t id(const void *p) {
  return uint64_t(reinterpret_cast<uintptr_t>(p));
}

struct InteropDeviceProbe {
  VkInstance instance = VK_NULL_HANDLE;
  VkPhysicalDevice physical = VK_NULL_HANDLE;
  VkDevice device = VK_NULL_HANDLE;
  VkQueue submissionQueue = VK_NULL_HANDLE;
  uint32_t submissionFamily = UINT32_MAX;
  std::vector<VkQueueFamilyProperties> families;
  bool available = false;
};

inline InteropDeviceProbe probeInteropDevice(IDirect3DDevice9 *device) {
  InteropDeviceProbe result;
  Com<ID3D9VkInteropDevice> interop;
  if (!device || FAILED(device->QueryInterface(
                     __uuidof(ID3D9VkInteropDevice),
                     reinterpret_cast<void **>(interop.put()))))
    return result;
  interop->GetVulkanHandles(&result.instance, &result.physical,
                            &result.device);
  interop->GetSubmissionQueue(&result.submissionQueue, nullptr,
                              &result.submissionFamily);
  result.available = result.instance && result.physical && result.device &&
                     result.submissionQueue &&
                     result.submissionFamily != UINT32_MAX;
  if (!result.available)
    return result;
  HMODULE loader = GetModuleHandleW(L"vulkan-1.dll");
  auto getInstance = loader ? reinterpret_cast<PFN_vkGetInstanceProcAddr>(
                                  GetProcAddress(loader, "vkGetInstanceProcAddr"))
                            : nullptr;
  auto getFamilies = getInstance
                         ? reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
                               getInstance(result.instance,
                                           "vkGetPhysicalDeviceQueueFamilyProperties"))
                         : nullptr;
  if (getFamilies) {
    uint32_t count = 0;
    getFamilies(result.physical, &count, nullptr);
    result.families.resize(count);
    if (count)
      getFamilies(result.physical, &count, result.families.data());
    result.families.resize(count);
  }
  return result;
}

inline std::string interopTopologyJson(const InteropDeviceProbe &probe) {
  std::string result = "[";
  for (uint32_t i = 0; i < probe.families.size(); ++i) {
    if (i)
      result += ',';
    const auto &family = probe.families[i];
    result += "{\"family\":" + std::to_string(i) +
              ",\"flags\":" + std::to_string(family.queueFlags) +
              ",\"physical\":" +
              std::to_string(family.queueCount) +
              ",\"submission\":" +
              std::string(i == probe.submissionFamily ? "true" : "false") +
              '}';
  }
  return result + ']';
}
struct State {
  IDirect3DDevice9 *device = nullptr;
  Com<ID3D9VkInteropDevice> cachedInterop;
  Com<IDirect3DSurface9> hudSnapshot;
  Com<IDirect3DSurface9> viewmodelBeforeSnapshot, viewmodelAfterSnapshot;
  std::unique_ptr<ResidentFg> gpu;
  InteropDeviceProbe interopProbe;
  AsteriaHostControl asteria;

  ldb_camera::Candidate candidate;
  ldb_camera::Matrix currentMatrix{}, previousMatrix{};

  uint64_t frame = 0, draws = 0, generated = 0, logical = 0, sceneColor = 0,
           sceneDepth = 0, previousColor = 0, previousDepth = 0;

  uint32_t generation = 0, captures = 0, captureLimit = 4, previousFlags = 0;
  uint32_t acceptedCaptures = 0, rejectedCaptures = 0;
  uint64_t presenterInputsAccepted = 0, presenterInputsRejected = 0;
  bool scene = false, nearSeen = false, nearCopied = false, uiCopied = false,
       history = false, paused = false, generatedOnly = false,
       providerOnly = false, disabled = false;

  uint64_t nearTarget = 0, nearDraws = 0, nearCapturedDraws = 0;
  bool nearBeforeCopied = false, nearColourCopied = false, nearInvalid = false;
  bool f8 = false, f9 = false;
  std::filesystem::path directory;
  std::ofstream log;
  LARGE_INTEGER frequency{};
  int64_t nextCapture = 0, previousArrival = 0;
  double previousPacingMs = 0, sourceIntervalMs = 0;
  uint32_t burstRemaining = 0;
  HANDLE pacingTimer = nullptr;
  bool pacingUnavailable = false;
  struct LiveProtection {
    float residual = 0.08f;
    float fallbackStart = 0.28f;
    uint32_t dilation = 1;
    bool debugWhite = false;
    ldb_fpb_projection::Settings projectedBody{};
  };
  LiveProtection liveProtection{};
  std::filesystem::path liveIniPath;
  std::filesystem::file_time_type liveIniStamp{};
  bool liveIniSeen = false;
  bool liveIniRejectedSeen = false;
  std::filesystem::file_time_type liveIniRejectedStamp{};
  uint64_t liveIniPollTick = 0;
  int64_t now() const {
    LARGE_INTEGER q{};
    QueryPerformanceCounter(&q);
    return q.QuadPart;
  }
  double ms(int64_t ticks) const {
    return double(ticks) * 1000.0 / frequency.QuadPart;
  }
  void scheduleCapture() {
    nextCapture = now() + frequency.QuadPart * 2;
    burstRemaining = config().captures;
  }

  // Residual, mask and camera-proxy FPB protection parameters are live.
  // Structural settings (enabled, HUD collection, Layer injection) remain
  // startup-only. The proxy needs no game entity or Visual model hook.
  // Poll timestamps, not INI text, every 500 ms; avoid per-frame disk IO.
  void refreshLiveProtection(uint32_t uid) noexcept {
    if (!providerOnly || liveIniPath.empty()) return;
    const uint64_t tick = GetTickCount64();
    if (tick - liveIniPollTick < 500) return;
    liveIniPollTick = tick;
    std::error_code ec;
    const auto stamp = std::filesystem::last_write_time(liveIniPath, ec);
    if (ec || (liveIniSeen && stamp == liveIniStamp)) return;
    try {
      auto readValue = [&](const wchar_t *key, const wchar_t *fallback) {
        wchar_t value[96]{};
        const DWORD n = GetPrivateProfileStringW(
            L"frame_generation", key, fallback, value, 96, liveIniPath.c_str());
        if (n >= 95) throw std::runtime_error("FG INI value too long");
        return std::wstring(value, n);
      };
      auto parseFloat = [&](const std::wstring &value, float minimum,
                            float maximum) {
        size_t used = 0;
        const float parsed = std::stof(value, &used);
        if (used != value.size() || !std::isfinite(parsed) ||
            parsed < minimum || parsed > maximum)
          throw std::out_of_range("FG live numeric parameter");
        return parsed;
      };
      LiveProtection updated = liveProtection;
      const auto residual = readValue(L"dynamic_residual_threshold", L"auto");
      updated.residual = residual == L"auto"
          ? (config().dynamicModels && config().fpbProtect ? 0.035f : 0.08f)
          : parseFloat(residual, 0.001f, 0.5f);
      updated.fallbackStart = parseFloat(
          readValue(L"first_person_fallback_start", L"0.28"), 0.0f, 1.0f);
      const auto dilation = readValue(L"dynamic_mask_dilation", L"1");
      if (dilation != L"0" && dilation != L"1")
        throw std::invalid_argument("FG live dilation must be 0 or 1");
      updated.dilation = dilation == L"1" ? 1u : 0u;
      const auto debug = readValue(L"mask_debug_white", L"0");
      if (debug != L"0" && debug != L"1")
        throw std::invalid_argument("FG live debug mask must be 0 or 1");
      updated.debugWhite = debug == L"1";
      const auto projection = readValue(L"fpb_projection_protect", L"0");
      if (projection != L"0" && projection != L"1")
        throw std::invalid_argument("FG FPB projection enable must be 0 or 1");
      updated.projectedBody.enabled = projection == L"1";
      updated.projectedBody.forward = parseFloat(
          readValue(L"fpb_projection_forward", L"-13"), -48.0f, 48.0f);
      updated.projectedBody.right = parseFloat(
          readValue(L"fpb_projection_right", L"-1"), -48.0f, 48.0f);
      updated.projectedBody.up = parseFloat(
          readValue(L"fpb_projection_up", L"-1"), -48.0f, 48.0f);
      updated.projectedBody.width = parseFloat(
          readValue(L"fpb_projection_width", L"1.0"), 0.5f, 2.5f);
      updated.projectedBody.torsoScreenWidth = parseFloat(
          readValue(L"fpb_projection_torso_screen_width", L"1.0"),
          0.1f, 1.0f);
      updated.projectedBody.eyeHeight = parseFloat(
          readValue(L"fpb_projection_eye_height", L"64"), 24.0f, 85.0f);
      // Reject an edit raced with another editor write; do not consume the
      // original mtime, so the next poll retries the completed file.
      std::error_code verifiedError;
      const auto verifiedStamp = std::filesystem::last_write_time(
          liveIniPath, verifiedError);
      if (verifiedError || verifiedStamp != stamp) return;
      if (updated.residual == liveProtection.residual &&
          updated.fallbackStart == liveProtection.fallbackStart &&
          updated.dilation == liveProtection.dilation &&
          updated.debugWhite == liveProtection.debugWhite &&
          updated.projectedBody.enabled == liveProtection.projectedBody.enabled &&
          updated.projectedBody.forward == liveProtection.projectedBody.forward &&
          updated.projectedBody.right == liveProtection.projectedBody.right &&
          updated.projectedBody.up == liveProtection.projectedBody.up &&
          updated.projectedBody.width == liveProtection.projectedBody.width &&
          updated.projectedBody.torsoScreenWidth ==
              liveProtection.projectedBody.torsoScreenWidth &&
          updated.projectedBody.eyeHeight == liveProtection.projectedBody.eyeHeight) {
        liveIniSeen = true;
        liveIniStamp = stamp;
        liveIniRejectedSeen = false;
        return;
      }
      if (gpu) gpu->updateProtectionShader(updated.fallbackStart,
                                           updated.dilation);
      // Host and Vulkan Layer share a process environment. The Layer selects
      // among four already-created pipelines at the next generated frame.
      // One atomic mode value avoids a transient white/dilation mismatch
      // when Asteria's asynchronous callback reads the process environment.
      const unsigned mode = (updated.debugWhite ? 1u : 0u) |
                            (updated.dilation == 0u ? 2u : 0u);
      const std::wstring modeText = std::to_wstring(mode);
      if (!SetEnvironmentVariableW(L"LDB_FG_RUNTIME_COMPOSITE_MODE",
                                   modeText.c_str()))
        throw std::runtime_error("FG Layer live environment update failed");
      liveProtection = updated;
      liveIniSeen = true;
      liveIniStamp = stamp;
      liveIniRejectedSeen = false;
      emit("live_protection_updated", uid,
           "\"residual\":" + std::to_string(updated.residual) +
           ",\"fallback_start\":" + std::to_string(updated.fallbackStart) +
           ",\"dilation\":" + std::to_string(updated.dilation) +
           ",\"debug_white\":" +
           std::string(updated.debugWhite ? "true" : "false") +
           ",\"fpb_projection\":" +
           std::string(updated.projectedBody.enabled ? "true" : "false") +
           ",\"fpb_forward\":" + std::to_string(updated.projectedBody.forward) +
           ",\"fpb_right\":" + std::to_string(updated.projectedBody.right) +
           ",\"fpb_up\":" + std::to_string(updated.projectedBody.up) +
           ",\"fpb_width\":" + std::to_string(updated.projectedBody.width) +
           ",\"fpb_torso_screen_width\":" +
           std::to_string(updated.projectedBody.torsoScreenWidth) +
           ",\"fpb_eye_height\":" +
           std::to_string(updated.projectedBody.eyeHeight));
    } catch (const std::exception &error) {
      // Failed edits remain retryable at the next poll. Rate-limit logging
      // for the same malformed INI revision to one event per file timestamp.
      if (!liveIniRejectedSeen || liveIniRejectedStamp != stamp) {
        emit("live_protection_rejected", uid,
             "\"reason\":" + quote(error.what()));
        liveIniRejectedStamp = stamp;
        liveIniRejectedSeen = true;
      }
    }
  }

  explicit State(IDirect3DDevice9 *d)
      : device(d), interopProbe(probeInteropDevice(d)),
        generatedOnly(config().generatedOnly),
        providerOnly(config().providerOnly) {
    liveProtection = {config().dynamicResidualThreshold,
                      config().firstPersonFallbackStart,
                      config().dynamicMaskDilation,
                      readEnv(L"LDB_FG_MASK_DEBUG_WHITE") == L"1"};
    if (readEnv(L"GF_FG_ISOLATION").empty()) {
      std::array<wchar_t, 32768> hostPath{};
      const DWORD pathLength = GetModuleFileNameW(
          nullptr, hostPath.data(), DWORD(hostPath.size()));
      if (pathLength && pathLength < hostPath.size())
        liveIniPath = std::filesystem::path(
            std::wstring(hostPath.data(), pathLength)).parent_path() /
            L"fg-config.ini";
    }
    captureLimit = config().captures;
    directory = config().root / ("fg-" + std::to_string(GetCurrentProcessId()) +
                                 "-" + std::to_string(GetTickCount64()) + "-" +
                                 std::to_string(id(d)));
    std::filesystem::create_directories(directory);
    log.open(directory / "fg-events.jsonl", std::ios::binary);

    if (!log)
      throw std::runtime_error("FG log unavailable");

    QueryPerformanceFrequency(&frequency);
    if (config().autoCapture)
      scheduleCapture();
    emit("session", 0,
         "\"qpc_frequency\":" + std::to_string(frequency.QuadPart) +
             ",\"full_size_gpu_history\":true,\"pixel_cpu_upload\":false,"
             "\"prototype\":true,\"mode\":" +
             quote(providerOnly ? "host_provider"
                                : (generatedOnly ? "generated" : "paired")) +
             ",\"backend\":" +
             quote(ResidentFg::wantsFsr() ? "fsr_official" : "custom") +
             ",\"motion_source\":" +
             quote(ResidentFg::wantsFsr() ? "camera_plus_bounded_patch"
                                          : "prototype_patch_matching") +
             ",\"interop_device_api\":" +
             std::string(interopProbe.available ? "true" : "false") +
             ",\"interop_submission_family\":" +
             std::to_string(interopProbe.submissionFamily) +
             ",\"interop_queue_families\":" +
             interopTopologyJson(interopProbe) +
             ",\"presenter_input_api\":" +
             std::to_string(LDB_FG_INPUT_API_VERSION) +
             ",\"host_layer_control_api\":" +
             std::to_string(ASTERIA_HOST_FRAME_API_VERSION_2) +
             ",\"host_layer_control_available\":" +
             std::string(asteria.available() ? "true" : "false") +
             ",\"fast_inputs\":" +
             std::string(config().fastInputs ? "true" : "false") +
             ",\"hud_bypass_requested\":" +
             std::string(config().hudBypass ? "true" : "false") +
             ",\"viewmodel_requested\":" +
             std::string(config().viewmodel ? "true" : "false") +
             ",\"dynamic_model_protection_requested\":" +
             std::string(config().dynamicModels ? "true" : "false") +
             ",\"fpb_protection_requested\":" +
             std::string(config().fpbProtect ? "true" : "false") +
             ",\"dynamic_residual_threshold\":" +
             std::to_string(config().dynamicResidualThreshold) +
             ",\"dynamic_residual_override_rejected\":" +
             std::string(config().dynamicResidualOverrideRejected ? "true" : "false") +
             ",\"first_person_fallback_start\":" +
             std::to_string(config().firstPersonFallbackStart) +
             ",\"first_person_fallback_override_rejected\":" +
             std::string(config().firstPersonFallbackOverrideRejected ? "true" : "false") +
             ",\"dynamic_mask_dilation\":" +
             std::to_string(config().dynamicMaskDilation) +
             ",\"dynamic_mask_dilation_override_rejected\":" +
             std::string(config().dynamicMaskDilationOverrideRejected ? "true" : "false") +
             ",\"merge_submissions\":" +
             std::string(config().mergeSubmissions ? "true" : "false") +
             ",\"qpc_frequency\":" +
             std::to_string(frequency.QuadPart) +
             ",\"local_effect_policy\":" +
             quote(ResidentFg::wantsFsr() ? "regional_current_and_previous"
                                          : "whole_frame_rejection") +
             ",\"auto_capture\":" +
             std::string(config().autoCapture ? "true" : "false") +
             ",\"pacing_budget_ms\":" + std::to_string(config().pacingMs));
  }
  ~State() {
    AsteriaHostFrameStatsV2 stats;
    const bool haveStats = asteria.query(stats);
    // Stop new provider submissions before considering Host GPU teardown.
    asteria.shutdown();
    const uint64_t outstandingLeases =
        asteria.nativeAccepted() > asteria.nativeReleased()
            ? asteria.nativeAccepted() - asteria.nativeReleased()
            : 0;
    // A callback-owned current input may remain in the Layer until its
    // swapchain/device teardown runs after Bridge shutdown. Keep the borrowed
    // Vulkan allocations process-lifetime in that case; device destruction
    // reclaims them and the late callback uses its own lifetime control block.
    if (outstandingLeases)
      (void)gpu.release();
    else
      gpu.reset();
    if (pacingTimer) {
      CancelWaitableTimer(pacingTimer);
      CloseHandle(pacingTimer);
    }
    emit("summary", 0,
         "\"logical_presents\":" + std::to_string(logical) +
             ",\"generated_presents\":" + std::to_string(generated) +
             ",\"presenter_inputs_accepted\":" +
             std::to_string(presenterInputsAccepted) +
             ",\"presenter_inputs_rejected\":" +
             std::to_string(presenterInputsRejected) +
             ",\"host_frames_attempted\":" +
             std::to_string(asteria.attempted()) +
             ",\"host_frames_accepted_local\":" +
             std::to_string(asteria.accepted()) +
             ",\"host_layer_stats_available\":" +
             std::string(haveStats ? "true" : "false") +
             ",\"host_frames_accepted_layer\":" +
             std::to_string(haveStats ? stats.accepted : 0) +
             ",\"host_frames_consumed_layer\":" +
             std::to_string(haveStats ? stats.consumed : 0) +
             ",\"native_queries_total\":" +
             std::to_string(asteria.nativeQueries()) +
             ",\"native_tickets_total\":" +
             std::to_string(asteria.nativeTickets()) +
             ",\"native_submitted_total\":" +
             std::to_string(asteria.nativeSubmitted()) +
             ",\"native_accepted_total\":" +
             std::to_string(asteria.nativeAccepted()) +
             ",\"native_released_total\":" +
             std::to_string(asteria.nativeReleased()) +
             ",\"native_busy_total\":" +
             std::to_string(asteria.nativeBusy()) +
             ",\"native_leases_outstanding\":" +
             std::to_string(outstandingLeases) +
             ",\"gpu_resources_retained_for_layer_teardown\":" +
             std::string(outstandingLeases ? "true" : "false"));
  }
  void emit(const char *event, uint32_t uid,
            const std::string &fields = {}) noexcept {
    try {
      if (!log || log.tellp() > std::streampos(64ull * 1024 * 1024))
        return;

      LARGE_INTEGER q{};
      QueryPerformanceCounter(&q);
      log << "{\"event\":" << quote(event) << ",\"frame\":" << frame
          << ",\"generation\":" << generation << ",\"uid\":" << uid
          << ",\"cpu_qpc\":" << q.QuadPart;

      if (!fields.empty())
        log << ',' << fields;
      log << "}\n";
      log.flush();
    } catch (...) {
    }
  }
  void clearFrame() {
    candidate = {};
    scene = false;
    nearSeen = false;
    nearCopied = false;
    nearBeforeCopied = nearColourCopied = nearInvalid = false;
    nearTarget = nearDraws = nearCapturedDraws = 0;
    uiCopied = false;
    draws = 0;
  }
  bool snapshotSurface(IDirect3DSurface9 *surface,
                       Com<IDirect3DSurface9> &snapshotSurface) {
    if (!surface)
      return false;
    D3DSURFACE_DESC source{}, snapshot{};
    if (FAILED(surface->GetDesc(&source)))
      return false;
    if (snapshotSurface.get() &&
        (FAILED(snapshotSurface->GetDesc(&snapshot)) ||
         snapshot.Width != source.Width || snapshot.Height != source.Height ||
         snapshot.Format != source.Format))
      snapshotSurface.reset();
    if (!snapshotSurface.get() &&
        FAILED(device->CreateRenderTarget(
            source.Width, source.Height, source.Format, D3DMULTISAMPLE_NONE, 0,
            FALSE, snapshotSurface.put(), nullptr)))
      return false;
    // StretchRect stays in DXVK's game command stream. HUD and viewmodel
    // snapshots therefore require no interop flush or CPU resource wait at
    // the draw boundary.
    return SUCCEEDED(device->StretchRect(surface, nullptr, snapshotSurface.get(),
                                         nullptr, D3DTEXF_NONE));
  }
  bool snapshotBeforeHud(IDirect3DSurface9 *back) {
    return snapshotSurface(back, hudSnapshot);
  }
  void invalidate(const char *reason, uint32_t uid, bool release = false) {
    history = false;
    clearFrame();

    if (release) {
      hudSnapshot.reset();
      viewmodelBeforeSnapshot.reset();
      viewmodelAfterSnapshot.reset();
      gpu.reset();
    }
    emit("history_invalidated", uid, "\"reason\":" + quote(reason));
  }
  void hotkeys(uint32_t uid) {
    if (!config().hotkeys)
      return;

    D3DDEVICE_CREATION_PARAMETERS p{};

    if (FAILED(device->GetCreationParameters(&p)))
      return;

    HWND focus = GetForegroundWindow();

    if (!focus || !p.hFocusWindow ||
        GetAncestor(focus, GA_ROOT) != GetAncestor(p.hFocusWindow, GA_ROOT)) {
      f8 = f9 = false;
      return;
    }
    bool a = (GetAsyncKeyState(VK_F8) & 0x8000) != 0,
         b = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;

    if (a && !f8) {
      paused = !paused;
      invalidate("toggle", uid, true);

      if (!paused)
        disabled = false;
      if (config().autoCapture)
        scheduleCapture();
      emit("mode", uid,
           "\"enabled\":" + std::string(paused ? "false" : "true") +
               ",\"mode\":" +
               quote(providerOnly ? "host_provider"
                                  : (generatedOnly ? "generated" : "paired")));
    }
    if (b && !f9) {
      if (!providerOnly)
        generatedOnly = !generatedOnly;
      captureLimit = std::min(16u, captures + config().captures);
      if (config().autoCapture)
        scheduleCapture();
      emit("mode", uid,
           "\"enabled\":" + std::string(paused ? "false" : "true") +
               ",\"mode\":" +
               quote(providerOnly ? "host_provider"
                                  : (generatedOnly ? "generated" : "paired")));
    }
    f8 = a;
    f9 = b;
  }
  ID3D9VkInteropDevice *interopDevice() {
    if (!cachedInterop.get())
      hr(device->QueryInterface(__uuidof(ID3D9VkInteropDevice),
                                reinterpret_cast<void **>(cachedInterop.put())),
         "FG interop");
    return cachedInterop.get();
  }
  template <typename Operation>
  void withTransferredSurface(IDirect3DSurface9 *surface, ResidentFg::Slot slot,
                              bool write, Operation &&operation) {
    Com<ID3D9VkInteropTexture> texture;
    auto info = imageInfo(surface, texture);
    auto *interop = interopDevice();

    VkImageAspectFlags aspects =
        slot < ResidentFg::CurrentUi
            ? (info.create.format == VK_FORMAT_D32_SFLOAT
                   ? VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT)
                   : VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT |
                                        VK_IMAGE_ASPECT_STENCIL_BIT))
            : VkImageAspectFlags(VK_IMAGE_ASPECT_COLOR_BIT);

    VkImageSubresourceRange range{aspects, 0, 1, 0, 1};
    VkImageLayout target = write ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL
                                 : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

    struct Restore {
      ID3D9VkInteropDevice *d;
      ID3D9VkInteropTexture *t;
      VkImageSubresourceRange range;
      VkImageLayout target, original;
      bool armed = true;
      void apply() noexcept {
        if (armed) {
          d->TransitionTextureLayout(t, &range, target, original);
          d->FlushRenderingCommands();
          armed = false;
        }
      }
      ~Restore() { apply(); }
    } restore{interop, texture.get(), range, target, info.layout};

    interop->TransitionTextureLayout(texture.get(), &range, info.layout, target);
    interop->FlushRenderingCommands();

    if (!interop->WaitForResource(surface, 0))
      throw std::runtime_error("FG source wait");

    operation(info, slot < ResidentFg::CurrentUi
                        ? VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT)
                        : VkImageAspectFlags(VK_IMAGE_ASPECT_COLOR_BIT));
    restore.apply();

    if (!interop->WaitForResource(surface, 0))
      throw std::runtime_error("FG restore wait");
  }
  template <size_t Count, typename Operation>
  void withTransferredColorSources(
      const std::array<IDirect3DSurface9 *, Count> &surfaces,
      Operation &&operation) {
    std::array<Com<ID3D9VkInteropTexture>, Count> textures;
    std::array<ImageInfo, Count> infos;
    for (size_t i = 0; i < Count; ++i)
      infos[i] = imageInfo(surfaces[i], textures[i]);
    auto *interop = interopDevice();
    VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    constexpr VkImageLayout target = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    struct RestoreSources {
      ID3D9VkInteropDevice *device;
      std::array<ID3D9VkInteropTexture *, Count> textures;
      std::array<VkImageLayout, Count> originals;
      VkImageSubresourceRange range;
      bool armed = true;
      void apply() noexcept {
        if (!armed)
          return;
        for (size_t i = 0; i < Count; ++i)
          device->TransitionTextureLayout(textures[i], &range,
                                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                           originals[i]);
        device->FlushRenderingCommands();
        armed = false;
      }
      ~RestoreSources() { apply(); }
    } restore{interop, {}, {}, range};
    for (size_t i = 0; i < Count; ++i) {
      restore.textures[i] = textures[i].get();
      restore.originals[i] = infos[i].layout;
    }
    for (size_t i = 0; i < Count; ++i)
      interop->TransitionTextureLayout(textures[i].get(), &range,
                                       infos[i].layout, target);
    interop->FlushRenderingCommands();
    for (auto *surface : surfaces)
      if (!interop->WaitForResource(surface, 0))
        throw std::runtime_error("FG color source wait");
    operation(infos);
    restore.apply();
    for (auto *surface : surfaces)
      if (!interop->WaitForResource(surface, 0))
        throw std::runtime_error("FG color restore wait");
  }
  void transfer(IDirect3DSurface9 *surface, ResidentFg::Slot slot,
                bool write = false) {
    withTransferredSurface(surface, slot, write,
      [&](const ImageInfo &info, VkImageAspectFlags aspect) {
        if (write)
          gpu->writeBack(info, slot);
        else
          gpu->copyInto(info, slot, aspect);
      });
  }
  void ensureGpu(IDirect3DSurface9 *depth) {
    Com<IDirect3DSurface9> back;
    hr(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, back.put()),
       "FG backbuffer");
    Com<ID3D9VkInteropTexture> di, bi;
    auto d = imageInfo(depth, di), b = imageInfo(back.get(), bi);

    if (d.create.extent.width != b.create.extent.width ||
        d.create.extent.height != b.create.extent.height)
      throw std::runtime_error("FG scene/backbuffer extent differs");

    if (gpu && (gpu->width != b.create.extent.width ||
                gpu->height != b.create.extent.height ||
                gpu->depthFormat != d.create.format ||
                gpu->finalFormat != b.create.format))
      invalidate("extent_or_format", 0, true);

    if (!gpu) {
      gpu = std::make_unique<ResidentFg>(
           interopDevice(), liveProtection.fallbackStart,
           liveProtection.dilation);
      gpu->initialize(b.create.extent.width, b.create.extent.height,
                      d.create.format, b.create.format,
                      asteria.consumerFamily(
                          reinterpret_cast<uintptr_t>(interopProbe.device)));
      if (config().autoCapture)
        scheduleCapture();
      emit("gpu_initialized", 0,
           "\"width\":" + std::to_string(gpu->width) +
               ",\"height\":" + std::to_string(gpu->height) +
               ",\"provider_consumer_family\":" +
               std::to_string(gpu->providerConsumerFamily()) +
               ",\"provider_concurrent_sharing\":" +
               std::string(gpu->providerConcurrentSharing() ? "true"
                                                             : "false") +
               ",\"physical_device\":" + quote(gpu->deviceName()));
    }
  }
  void sceneCopy(uint32_t uid) {
    if (scene || !candidate.matches || candidate.conflict)
      return;

    Com<IDirect3DSurface9> rt, ds;

    if (FAILED(device->GetRenderTarget(0, rt.put())) ||
        FAILED(device->GetDepthStencilSurface(ds.put())) || !rt.get() ||
        !ds.get() || id(rt.get()) != sceneColor || id(ds.get()) != sceneDepth)
      return;

    auto saved = candidate;
    ensureGpu(ds.get());
    candidate = saved;
    transfer(ds.get(), ResidentFg::CurrentDepth);
    currentMatrix = candidate.matrix;
    scene = true;

    if (frame < 16)
      emit("scene", uid,
           "\"matrix_register\":" + std::to_string(candidate.reg) +
               ",\"matching_draws\":" + std::to_string(candidate.matches));
  }
  void nearBegin() {
    if (!scene || !gpu || nearInvalid ||
        (config().fastInputs && !config().viewmodel))
      return;
    Com<IDirect3DSurface9> rt;
    D3DSURFACE_DESC desc{};
    if (FAILED(device->GetRenderTarget(0, rt.put())) || !rt.get() ||
        FAILED(rt->GetDesc(&desc)) || desc.Format != D3DFMT_A16B16G16R16F ||
        desc.Width != gpu->width || desc.Height != gpu->height) {
      nearInvalid = true;
      return;
    }
    if (nearBeforeCopied) {
      if (id(rt.get()) != nearTarget)
        nearInvalid = true;
      return;
    }
    if (config().fastInputs) {
      if (!snapshotSurface(rt.get(), viewmodelBeforeSnapshot)) {
        nearInvalid = true;
        return;
      }
    } else
      transfer(rt.get(), ResidentFg::CurrentNearBefore);
    nearTarget = id(rt.get());
    nearBeforeCopied = true;
  }
  void nearCopy(uint32_t uid) {
    if (!scene || !nearSeen || !gpu || !nearDraws ||
        nearCapturedDraws == nearDraws ||
        (config().fastInputs && !config().viewmodel))
      return;
    Com<IDirect3DSurface9> rt, ds;
    if (nearBeforeCopied && !nearInvalid &&
        SUCCEEDED(device->GetRenderTarget(0, rt.put())) && rt.get() &&
        id(rt.get()) == nearTarget) {
      if (config().fastInputs)
        nearColourCopied =
            snapshotSurface(rt.get(), viewmodelAfterSnapshot);
      else {
        transfer(rt.get(), ResidentFg::CurrentNearAfter);
        nearColourCopied = true;
      }
    }
    if (!config().fastInputs &&
        SUCCEEDED(device->GetDepthStencilSurface(ds.put())) && ds.get() &&
        id(ds.get()) == sceneDepth) {
      transfer(ds.get(), ResidentFg::CurrentNear);
      nearCopied = true;
    }
    nearCapturedDraws = nearDraws;
    if (frame < 16)
      emit("near_stage", uid,
           "\"colour_mask_valid\":" +
               std::string(nearColourCopied ? "true" : "false"));
  }
  void boundary(uint32_t uid) {
    if (disabled || paused)
      return;

    sceneCopy(uid);
    nearCopy(uid);
  }
  void viewport(const D3DVIEWPORT9 *next, uint32_t uid) {
    if (disabled || paused || !next)
      return;

    if (next->MinZ == 0 && next->MaxZ > 0 && next->MaxZ <= .2f) {
      sceneCopy(uid);
      if (!config().fastInputs || config().viewmodel)
        nearSeen = scene;
    } else
      nearCopy(uid);
  }
  void beforeDraw(uint32_t uid) {
    if (disabled || paused)
      return;

    D3DVIEWPORT9 v{};

    if (FAILED(device->GetViewport(&v)))
      return;

    if (v.MinZ == 0 && v.MaxZ > 0 && v.MaxZ <= .2f) {
      sceneCopy(uid);
      if (!config().fastInputs || config().viewmodel) {
        nearSeen = scene;
        nearBegin();
      }
    } else if (nearSeen)
      nearCopy(uid);

    if (config().fastInputs && !config().hudBypass)
      return;

    if (!scene || !gpu || uiCopied || v.X || v.Y || v.Width != gpu->width ||
        v.Height != gpu->height)
      return;

    DWORD blend = 0, z = 0;

    if (FAILED(device->GetRenderState(D3DRS_ALPHABLENDENABLE, &blend)) ||
        FAILED(device->GetRenderState(D3DRS_ZENABLE, &z)) || !blend ||
        z != D3DZB_FALSE)
      return;

    Com<IDirect3DSurface9> rt, back;

    if (FAILED(device->GetRenderTarget(0, rt.put())) ||
        FAILED(
            device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, back.put())) ||
        rt.get() != back.get())
      return;

    uiCopied = snapshotBeforeHud(back.get());

    if (!uiCopied)
      return;

    if (frame < 16)
      emit("ui_boundary", uid,
           "\"classification\":\"screen_alpha_on_backbuffer_candidate\"");
  }
  void afterDraw(uint32_t uid, HRESULT result) {
    if (disabled || paused || FAILED(result))
      return;
    D3DVIEWPORT9 observed{};
    if ((!config().fastInputs || config().viewmodel) && scene && nearSeen &&
        SUCCEEDED(device->GetViewport(&observed)) &&
        observed.MinZ == 0 && observed.MaxZ > 0 && observed.MaxZ <= .2f) {
      nearDraws++;
      nearColourCopied = false;
    }
    if (scene)
      return;
    D3DVIEWPORT9 v{};

    if (FAILED(device->GetViewport(&v)) || v.X || v.Y || v.MinZ != 0 ||
        v.MaxZ != 1)
      return;

    DWORD z = 0, zw = 0;

    if (FAILED(device->GetRenderState(D3DRS_ZENABLE, &z)) ||
        FAILED(device->GetRenderState(D3DRS_ZWRITEENABLE, &zw)) ||
        z != D3DZB_TRUE || !zw)
      return;

    Com<IDirect3DSurface9> rt, ds;
    Com<IDirect3DVertexShader9> vs;

    if (FAILED(device->GetRenderTarget(0, rt.put())) ||
        FAILED(device->GetDepthStencilSurface(ds.put())) ||
        FAILED(device->GetVertexShader(vs.put())) || !rt.get() || !ds.get() ||
        !vs.get())
      return;

    D3DSURFACE_DESC desc{};

    if (FAILED(rt->GetDesc(&desc)) || desc.Format != D3DFMT_A16B16G16R16F ||
        desc.Width != v.Width || desc.Height != v.Height)
      return;

    Com<IDirect3DSurface9> back;
    D3DSURFACE_DESC bd{};

    if (FAILED(
            device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, back.put())) ||
        FAILED(back->GetDesc(&bd)) || bd.Width != desc.Width ||
        bd.Height != desc.Height)
      return;

    UINT bytes = 0;
    hr(vs->GetFunction(nullptr, &bytes), "FG shader size");

    if (!bytes || bytes > 1024 * 1024 || bytes % 4)
      return;

    std::vector<uint8_t> code(bytes);
    hr(vs->GetFunction(code.data(), &bytes), "FG shader data");
    int reg = ldb_camera::viewProjectionRegister(code);

    if (reg < 0)
      return;

    ldb_camera::Matrix matrix{};

    if (FAILED(device->GetVertexShaderConstantF(UINT(reg), matrix.data(), 4)))
      return;

    std::array<double, 16> inv{};

    if (!ldb_camera::inverse(matrix, inv))
      return;

    if (candidate.matches &&
        (sceneColor != id(rt.get()) || sceneDepth != id(ds.get()))) {
      candidate = {};
    }
    if (candidate.matches && candidate.matrix != matrix)
      candidate.conflict = true;

    candidate.matrix = matrix;
    candidate.reg = reg;
    candidate.sequence = uid;
    candidate.matches++;
    sceneColor = id(rt.get());
    sceneDepth = id(ds.get());
    draws++;
  }
  template <class F>
  HRESULT present(uint32_t uid, F &&backend, bool partial,
                  IDirect3DSwapChain9 *chain) {
    const int64_t arrival = now();
    double interval =
        previousArrival ? ms(arrival - previousArrival) - previousPacingMs : 0;
    if (interval > 0 && interval < 100)
      sourceIntervalMs = sourceIntervalMs > 0
                             ? .9 * sourceIntervalMs + .1 * interval
                             : interval;
    previousArrival = arrival;
    previousPacingMs = 0;
    hotkeys(uid);
    refreshLiveProtection(uid);
    bool captured = false, generatedFrame = false, qualityRejected = false;
    ResidentFg::Quality quality{};
    double captureMs = 0, generatedPresentMs = 0, logicalPresentMs = 0;
    int64_t generatedStart = 0;
    bool wrote = false, ready = false, fsrWarmup = false;
    bool presenterInputValid = false;
    uint32_t presenterInputFlags = 0;
    HRESULT extra = S_OK;
    Com<IDirect3DSurface9> back;

    try {
      // The frame can end while the near-plane viewport is still active.
      // Finalize its queued after-snapshot before exporting provider inputs.
      nearCopy(uid);
      Com<IDirect3DSwapChain9> primary;
      hr(device->GetSwapChain(0, primary.put()), "FG primary swapchain");
      bool primaryChain = !chain || chain == primary.get();

      if (!disabled && !paused && !partial && primaryChain && scene &&
          !candidate.conflict &&
          (!nearSeen || (nearColourCopied && !nearInvalid) ||
           (config().fastInputs && !config().viewmodel)) && gpu) {
        hr(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, back.put()),
           "FG final backbuffer");
        if (!providerOnly || !config().mergeSubmissions)
          transfer(back.get(), ResidentFg::CurrentFinal);
        ready = true;
        bool providerGpuInputsReady = false;
        bool nativeFrameAccepted = false;
        int nativeSlot = -1;
        AsteriaFrameInputTicketV1 nativeTicket{};
        ResidentFg::ProviderImages providerImages{};
        if (providerOnly) {
          const bool validHistory =
              history && previousColor == sceneColor &&
              previousDepth == sceneDepth;
          ResidentFg::Params providerParams{};
          if (ldb_camera::frameTransforms(
                  validHistory ? previousMatrix : currentMatrix,
                  currentMatrix, providerParams.rows)) {
            nativeSlot = asteria.acquireNativeSlot(
                gpu->providerImages(0).device,
                nativeTicket);
            if (nativeSlot >= 0) {
              const bool hudOverlay = config().hudBypass && uiCopied &&
                                      hudSnapshot.get();
              const bool viewmodelOverlay =
                  config().viewmodel && nearColourCopied && !nearInvalid &&
                  viewmodelBeforeSnapshot.get() &&
                  viewmodelAfterSnapshot.get();
              bool sceneOnly =
                  config().fastInputs && !hudOverlay && !viewmodelOverlay;
              const uint32_t providerFlags =
                (nearCopied ? 1u : 0u) | (uiCopied ? 2u : 0u) |
                (gpu->finalFormat == VK_FORMAT_B8G8R8A8_UNORM ? 4u : 0u) |
                (viewmodelOverlay ? 8u : 0u) |
                (config().dynamicModels ? 16u : 0u) |
                (validHistory ? 32u : 0u);
              providerParams.flags = {gpu->width, gpu->height, providerFlags,
                                      previousFlags};
              providerParams.thresholds[2] = 1.0f;
              // First pass: reuse the existing dynamic-pixel alpha overlay.
              // FPB moves its render-only skeleton relative to the camera, so
              // subtle changed pixels can miss the normal 0.08 residual guard.
              // This lowers sensitivity only in provider mode when explicitly
              // requested; the shader, motion vectors and base path stay intact.
              // It is a heuristic, not an exact body segmentation mask.
              providerParams.thresholds[3] = liveProtection.residual;
              if (liveProtection.projectedBody.enabled)
                gpu->ensureProjectionPipeline();
              const auto projectedBody = ldb_fpb_projection::project(
                  currentMatrix, gpu->width, gpu->height,
                  liveProtection.projectedBody);
              // Even without HUD/viewmodel, the projected body requires the
              // provider UI composite. Do not mark its input scene-only.
              if (projectedBody.count && gpu->canOverlayProjectedBody())
                sceneOnly = false;
              if (liveProtection.projectedBody.enabled &&
                  (frame < 16 || frame % 120 == 0))
                emit("fpb_projection", uid,
                     "\"regions\":" + std::to_string(projectedBody.count) +
                     ",\"estimated_pixels\":" +
                     std::to_string(projectedBody.upperBoundPixels) +
                     ",\"coverage_rejected\":" +
                     std::string(projectedBody.rejectedCoverage ? "true" : "false") +
                     ",\"gpu_overlay_available\":" +
                     std::string(gpu->canOverlayProjectedBody() ? "true" : "false"));
              try {
                if (!config().mergeSubmissions) {
                  if (hudOverlay)
                    transfer(hudSnapshot.get(), ResidentFg::CurrentUi);
                  if (viewmodelOverlay) {
                    transfer(viewmodelBeforeSnapshot.get(),
                             ResidentFg::CurrentNearBefore);
                    transfer(viewmodelAfterSnapshot.get(),
                             ResidentFg::CurrentNearAfter);
                  }
                }
                if (config().mergeSubmissions) {
                  if (hudOverlay && viewmodelOverlay) {
                    withTransferredColorSources<4>(
                        {back.get(), hudSnapshot.get(),
                         viewmodelBeforeSnapshot.get(),
                         viewmodelAfterSnapshot.get()},
                        [&](const auto &infos) {
                          gpu->copyInto(infos[0], ResidentFg::CurrentFinal,
                                        VK_IMAGE_ASPECT_COLOR_BIT, true);
                          gpu->copyInto(infos[1], ResidentFg::CurrentUi,
                                        VK_IMAGE_ASPECT_COLOR_BIT, true);
                          gpu->copyInto(infos[2],
                                        ResidentFg::CurrentNearBefore,
                                        VK_IMAGE_ASPECT_COLOR_BIT, true);
                          gpu->copyInto(infos[3], ResidentFg::CurrentNearAfter,
                                        VK_IMAGE_ASPECT_COLOR_BIT, true);
                          gpu->generate(providerParams, false, true,
                                        uint32_t(nativeSlot), sceneOnly, true,
                                        &projectedBody);
                        });
                  } else if (viewmodelOverlay) {
                    withTransferredColorSources<3>(
                        {back.get(), viewmodelBeforeSnapshot.get(),
                         viewmodelAfterSnapshot.get()},
                        [&](const auto &infos) {
                          gpu->copyInto(infos[0], ResidentFg::CurrentFinal,
                                        VK_IMAGE_ASPECT_COLOR_BIT, true);
                          gpu->copyInto(infos[1],
                                        ResidentFg::CurrentNearBefore,
                                        VK_IMAGE_ASPECT_COLOR_BIT, true);
                          gpu->copyInto(infos[2], ResidentFg::CurrentNearAfter,
                                        VK_IMAGE_ASPECT_COLOR_BIT, true);
                          gpu->generate(providerParams, false, true,
                                        uint32_t(nativeSlot), sceneOnly, true,
                                        &projectedBody);
                        });
                  } else if (hudOverlay) {
                    withTransferredColorSources<2>(
                        {back.get(), hudSnapshot.get()},
                        [&](const auto &infos) {
                          gpu->copyInto(infos[0], ResidentFg::CurrentFinal,
                                        VK_IMAGE_ASPECT_COLOR_BIT, true);
                          gpu->copyInto(infos[1], ResidentFg::CurrentUi,
                                        VK_IMAGE_ASPECT_COLOR_BIT, true);
                          gpu->generate(providerParams, false, true,
                                        uint32_t(nativeSlot), sceneOnly, true,
                                        &projectedBody);
                        });
                  } else {
                    withTransferredSurface(
                        back.get(), ResidentFg::CurrentFinal, false,
                        [&](const ImageInfo &info, VkImageAspectFlags aspect) {
                          gpu->copyInto(info, ResidentFg::CurrentFinal, aspect,
                                        true);
                          gpu->generate(providerParams, false, true,
                                        uint32_t(nativeSlot), sceneOnly, true,
                                        &projectedBody);
                        });
                  }
                } else {
                  gpu->generate(providerParams, false, true,
                                uint32_t(nativeSlot), sceneOnly, false,
                                &projectedBody);
                }
              } catch (...) {
                asteria.cancelNativeSlot(uint32_t(nativeSlot));
                throw;
              }
              quality = gpu->lastCompletedQuality();
              providerImages = gpu->providerImages(uint32_t(nativeSlot));
              providerGpuInputsReady = true;
              nativeFrameAccepted = asteria.submitNative(
                  nativeTicket, uint32_t(nativeSlot), currentMatrix,
                  sourceIntervalMs, !history, providerImages.depth,
                  providerImages.motion, providerImages.hudless,
                  providerImages.ui, providerImages.queue,
                  providerImages.depthFormat,
                  providerImages.motionFormat, providerImages.hudlessFormat,
                  providerImages.uiFormat, providerImages.depthLayout,
                  sceneOnly);
              generatedFrame = nativeFrameAccepted;
            }
          }
        }
        const bool hostFrameAccepted = config().fastInputs && providerOnly
            ? nativeFrameAccepted
            : providerGpuInputsReady && asteria.submit(
                  currentMatrix, uint64_t(generation) + 1, frame + 1,
                  uint64_t(arrival), sourceIntervalMs, sceneColor, sceneDepth,
                  gpu->width, gpu->height,
                  uint32_t(std::min<uint64_t>(draws, UINT32_MAX)),
                  uint32_t(std::min<uint64_t>(nearDraws, UINT32_MAX)), uiCopied,
                  !history, providerImages.device, providerImages.depth,
                  providerImages.motion, providerImages.depthFormat,
                  providerImages.motionFormat, providerImages.depthLayout,
                  providerImages.motionLayout);
        if (frame < 16 || frame % 120 == 0) {
          AsteriaHostFrameStatsV2 hostStats;
          const bool haveHostStats = asteria.query(hostStats);
          emit("host_layer_frame", uid,
               "\"accepted\":" +
                   std::string(hostFrameAccepted ? "true" : "false") +
                   ",\"native_accepted\":" +
                   std::string(nativeFrameAccepted ? "true" : "false") +
                   ",\"native_ticket_frame\":" +
                   std::to_string(nativeTicket.frame_id) +
                   ",\"native_slot\":" + std::to_string(nativeSlot) +
                   ",\"native_queries_total\":" +
                   std::to_string(asteria.nativeQueries()) +
                   ",\"native_tickets_total\":" +
                   std::to_string(asteria.nativeTickets()) +
                   ",\"native_submitted_total\":" +
                   std::to_string(asteria.nativeSubmitted()) +
                   ",\"native_accepted_total\":" +
                   std::to_string(asteria.nativeAccepted()) +
                   ",\"native_released_total\":" +
                   std::to_string(asteria.nativeReleased()) +
                   ",\"native_busy_total\":" +
                   std::to_string(asteria.nativeBusy()) +
                   ",\"main_view\":true,\"scene_depth_observed\":true"
                   ",\"camera_observed\":true,\"gpu_inputs_ready\":" +
                   std::string(providerGpuInputsReady ? "true" : "false") +
                   ",\"gpu_motion_pixels\":" +
                   std::to_string(providerGpuInputsReady
                                      ? quality.motionPixels()
                                      : 0) +
                   ",\"hud_boundary_observed\":" +
                   std::string(uiCopied ? "true" : "false") +
                   ",\"frame_time_ms\":" +
                   std::to_string(sourceIntervalMs) +
                   ",\"layer_stats_available\":" +
                   std::string(haveHostStats ? "true" : "false") +
                   ",\"layer_accepted_total\":" +
                   std::to_string(haveHostStats ? hostStats.accepted : 0) +
                   ",\"layer_consumed_total\":" +
                   std::to_string(haveHostStats ? hostStats.consumed : 0));
        }
        {
          try {
            auto input = gpu->presenterInputs(
                currentMatrix, history ? previousMatrix : currentMatrix, frame,
                uint64_t(generation) + 1, sourceIntervalMs, uiCopied,
                !history);
            presenterInputFlags = input.flags;
            presenterInputValid = LdbFgValidateFrameInputsV1(input);
            if (presenterInputValid)
              ++presenterInputsAccepted;
            else
              ++presenterInputsRejected;
            if (frame < 16 || frame % 120 == 0)
              emit("presenter_input", uid,
                   "\"valid\":" +
                       std::string(presenterInputValid ? "true" : "false") +
                       ",\"flags\":" +
                       std::to_string(presenterInputFlags) +
                       ",\"width\":" + std::to_string(input.width) +
                       ",\"height\":" + std::to_string(input.height) +
                       ",\"frame_time_ms\":" +
                       std::to_string(input.frameTimeMs) +
                       ",\"motion_source\":\"depth_camera_reconstruction\""
                       ",\"gpu_ready_token\":false"
                       ",\"present_ticket\":false");
          } catch (const std::exception &e) {
            ++presenterInputsRejected;
            emit("presenter_input_rejected", uid,
                 "\"message\":" + quote(e.what()));
          }
        }
        if (!providerOnly && ResidentFg::wantsFsr() && !history) {
          gpu->generateFsr(currentMatrix, currentMatrix, frame,
                           sourceIntervalMs, uiCopied, false);
          emit("fsr_history_primed", uid, "\"backend\":\"fsr_official\"");
        }

        if (!providerOnly && history && previousColor == sceneColor &&
            previousDepth == sceneDepth) {
          ResidentFg::Params params{};

          if (ldb_camera::frameTransforms(previousMatrix, currentMatrix,
                                          params.rows)) {
            uint32_t flags =
                (nearCopied ? 1u : 0u) | (uiCopied ? 2u : 0u) |
                (gpu->finalFormat == VK_FORMAT_B8G8R8A8_UNORM ? 4u : 0u) |
                (nearColourCopied ? 8u : 0u) | 16u;
            params.flags = {gpu->width, gpu->height, flags, previousFlags};
            bool captureDue =
                config().autoCapture
                    ? (captures < 32 && burstRemaining && now() >= nextCapture)
                    : captures < captureLimit;
            gpu->generate(params, !captureDue);
            if (ResidentFg::wantsFsr()) {
              int status = gpu->generateFsr(currentMatrix, previousMatrix,
                                            frame, sourceIntervalMs, uiCopied,
                                            true, (previousFlags & 8u) != 0);
              fsrWarmup = status == 1;
              emit(status == 2 ? "fsr_skipped" : "fsr_dispatch", uid,
                   "\"status\":" + std::to_string(status) +
                       ",\"backend\":\"fsr_official\"");
            }

            bool captureAllowed = true;
            if (ResidentFg::wantsFsr()) {
              quality = gpu->readQuality();
              qualityRejected =
                  fsrWarmup ||
                  quality.reject(uint64_t(gpu->width) * gpu->height, true);
              // Reserve most automatic evidence for output eligible for
              // Present. Retain a smaller rejected sample to diagnose
              // protective fallback.
              if (config().autoCapture)
                captureAllowed = qualityRejected ? rejectedCaptures < 8
                                                 : acceptedCaptures < 24;
            }
            if (captureDue && captureAllowed) {
              auto captureStart = now();
              std::string prefix = std::to_string(frame);
              gpu->save(ResidentFg::Output,
                        directory / (prefix + "-generated.ppm"));
              gpu->save(ResidentFg::Mask, directory / (prefix + "-mask.ppm"));
              gpu->save(ResidentFg::CurrentFinal,
                        directory / (prefix + "-current.ppm"));
              gpu->save(ResidentFg::PreviousFinal,
                        directory / (prefix + "-previous.ppm"));

              if (uiCopied)
                gpu->save(ResidentFg::CurrentUi,
                          directory / (prefix + "-before-ui.ppm"));
              captures++;
              if (ResidentFg::wantsFsr()) {
                if (qualityRejected)
                  ++rejectedCaptures;
                else
                  ++acceptedCaptures;
              }
              captured = true;
              captureMs = ms(now() - captureStart);
              emit(
                  "capture", uid,
                  "\"index\":" + std::to_string(captures) + ",\"mode\":" +
            quote(providerOnly ? "host_provider"
                               : (generatedOnly ? "generated" : "paired")) +
                      (ResidentFg::wantsFsr()
                           ? ",\"quality_accepted\":" +
                                 std::string(qualityRejected ? "false" : "true")
                           : ""));
              if (config().autoCapture && --burstRemaining == 0) {
                nextCapture = now() + frequency.QuadPart * 20;
                burstRemaining = config().captures;
              }
            }
            wrote = true;
            generatedFrame = true;
            transfer(back.get(), ResidentFg::Output, true);

            quality = gpu->readQuality();
            qualityRejected =
                fsrWarmup || quality.reject(uint64_t(gpu->width) * gpu->height,
                                            ResidentFg::wantsFsr());
            if (qualityRejected) {
              transfer(back.get(), ResidentFg::CurrentFinal, true);
              wrote = false;
              generatedFrame = false;
              emit(
                  "frame_rejected", uid,
                  "\"reason\":" +
                      quote(fsrWarmup ? "fsr_history_warmup"
                                      : quality.rejectReason(
                                            uint64_t(gpu->width) * gpu->height,
                                            ResidentFg::wantsFsr())) +
                      ",\"invalid_scene_"
                      "ratio\":" +
                      std::to_string(quality.invalidSceneRatio()) +
                      ",\"invalid_pixels\":" + std::to_string(quality.invalid) +
                      ",\"total_pixels\":" + std::to_string(quality.total) +
                      ",\"foreground_pixels\":" +
                      std::to_string(quality.foreground) +
                      ",\"original_restored\":true,\"presented\":false");
            }
            if (!generatedOnly && !qualityRejected) {
              generatedStart = now();
              extra = backend();
              generatedPresentMs = ms(now() - generatedStart);
              emit("generated_present", uid,
                   "\"hresult\":" + std::to_string(uint32_t(extra)) +
                       ",\"mode\":\"paired\",\"phase\":0.5,\"backbuffer_"
                       "written\":true");

              if (SUCCEEDED(extra))
                generated++;
              back.reset();
              hr(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO,
                                       back.put()),
                 "FG backbuffer after generated Present");
              transfer(back.get(), ResidentFg::CurrentFinal, true);
              wrote = false;
              // Bounded experimental spacing; do not add delay when Present
              // already blocks. Subtract our own delay from the next interval
              // estimate to avoid feedback.
              double budget =
                  std::min(double(config().pacingMs), sourceIntervalMs * .5);
              double remaining = budget - ms(now() - generatedStart);
              if (SUCCEEDED(extra) && remaining >= .5 && !pacingUnavailable) {
                if (!pacingTimer) {
                  // Windows 10 1803+ high-resolution timer; skip on older
                  // systems.
                  pacingTimer = CreateWaitableTimerExW(nullptr, nullptr, 0x2,
                                                       TIMER_ALL_ACCESS);
                  if (!pacingTimer) {
                    pacingUnavailable = true;
                    emit("pacing_unavailable", uid);
                  }
                }
                if (pacingTimer) {
                  LARGE_INTEGER due{};
                  due.QuadPart = -int64_t(remaining * 10000);
                  auto before = now();
                  if (SetWaitableTimer(pacingTimer, &due, 0, nullptr, nullptr,
                                       FALSE))
                    WaitForSingleObject(pacingTimer, 20);
                  previousPacingMs = ms(now() - before);
                }
              }
            }
          } else {
            history = false;
            emit("frame_fallback", uid,
                 "\"reason\":\"camera_cut_or_invalid_midpoint\"");
          }
        } else if (frame < 16)
          emit("warmup", uid);

      } else {
        history = false;

        if (frame < 16)
          emit("frame_fallback", uid,
               "\"reason\":\"missing_stage_or_unsupported_present\"");
      }
    } catch (const std::exception &e) {
      history = false;
      disabled = true;
      emit("fg_error", uid, "\"message\":" + quote(e.what()));

      if (wrote && gpu) {
        try {
          back.reset();
          hr(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, back.put()),
             "FG fallback backbuffer");
          transfer(back.get(), ResidentFg::CurrentFinal, true);
          wrote = false;
        } catch (...) {
        }
      }
    }
    // Exactly one logical backend call;
    // Paired mode adds one earlier generated call.
    auto logicalStart = now();
    HRESULT result = backend();
    logicalPresentMs = ms(now() - logicalStart);
    logical++;

    if (wrote && generatedOnly) {
      if (SUCCEEDED(result))
        generated++;
      emit("generated_present", uid,
           "\"hresult\":" + std::to_string(uint32_t(result)) +
               ",\"mode\":\"generated\",\"phase\":0.5,\"backbuffer_written\":"
               "true");
      try {
        back.reset();
        hr(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, back.put()),
           "FG restore real after preview");
        transfer(back.get(), ResidentFg::CurrentFinal, true);
      } catch (const std::exception &e) {
        disabled = true;
        emit("fg_error", uid, "\"message\":" + quote(e.what()));
      }
    }
    if (ready && !disabled && !paused && SUCCEEDED(result)) {
      previousMatrix = currentMatrix;
      previousColor = sceneColor;
      previousDepth = sceneDepth;
      previousFlags =
          (nearCopied ? 1u : 0u) | (uiCopied ? 2u : 0u) |
          (nearColourCopied ? 8u : 0u) |
          (config().dynamicModels ? 16u : 0u);
      gpu->advance();
      history = true;
    } else
      history = false;

    if (frame < 256 || frame % 120 == 0)
      emit(
          "logical_present", uid,
          "\"hresult\":" + std::to_string(uint32_t(result)) +
              ",\"generated_total\":" + std::to_string(generated) +
              ",\"logical_total\":" + std::to_string(logical) +
              ",\"near_preserved\":" +
              std::string(nearCopied ? "true" : "false") +
              ",\"near_colour_mask_valid\":" +
              std::string(nearColourCopied && !nearInvalid ? "true" : "false") +
              ",\"viewmodel_overlay_available\":" +
              std::string(config().viewmodel && nearColourCopied &&
                                  !nearInvalid &&
                                  viewmodelBeforeSnapshot.get() &&
                                  viewmodelAfterSnapshot.get()
                              ? "true"
                              : "false") +
              ",\"near_draws\":" + std::to_string(nearDraws) +
              ",\"ui_boundary_observed\":" +
              std::string(uiCopied ? "true" : "false"));

    auto metrics = gpu ? gpu->takeCounters() : ResidentFg::Counters{};
    emit(
        "frame_metrics", uid,
        "\"enabled\":" + std::string(!paused && !disabled ? "true" : "false") +
            ",\"backend\":" +
            quote(ResidentFg::wantsFsr() ? "fsr_official_with_guards"
                                         : "custom") +
            ",\"gpu_dispatch_scope\":" +
            quote(ResidentFg::wantsFsr() ? "preflight_only" : "generation") +
            ",\"mode\":" +
            quote(providerOnly ? "host_provider"
                               : (generatedOnly ? "generated" : "paired")) +
            ",\"generated\":" + std::string(generatedFrame ? "true" : "false") +
            ",\"captured\":" + std::string(captured ? "true" : "false") +
            ",\"quality_rejected\":" +
            std::string(qualityRejected ? "true" : "false") +
            ",\"quality_total_pixels\":" + std::to_string(quality.total) +
            ",\"foreground_pixels\":" + std::to_string(quality.foreground) +
            ",\"near_colour_pixels\":" + std::to_string(quality.nearColour) +
            ",\"dynamic_protected_pixels\":" +
            std::to_string(providerOnly ? quality.nearColour : 0) +
            ",\"near_depth_pixels\":" + std::to_string(quality.nearDepth) +
            ",\"ui_pixels\":" + std::to_string(quality.ui) +
            ",\"local_effect_risk\":" +
            std::string(quality.localEffectRisk() ? "true" : "false") +
            ",\"motion_interpolated_pixels\":" +
            std::to_string(quality.motionPixels()) +
            ",\"scene_reprojected_pixels\":" +
            std::to_string(quality.reprojected) + ",\"foreground_ratio\":" +
            std::to_string(quality.foregroundRatio()) +
            ",\"scene_reprojection_ratio\":" +
            std::to_string(quality.sceneReprojectionRatio()) +
            ",\"invalid_scene_ratio\":" +
            std::to_string(quality.invalidSceneRatio()) +
            ",\"near_preserved\":" +
            std::string(nearCopied ? "true" : "false") +
            ",\"near_colour_mask_valid\":" +
            std::string(nearColourCopied && !nearInvalid ? "true" : "false") +
            ",\"near_draws\":" + std::to_string(nearDraws) +
            ",\"ui_boundary_observed\":" +
            std::string(uiCopied ? "true" : "false") +
            ",\"arrival_qpc\":" + std::to_string(arrival) +
            ",\"host_work_ms\":" + std::to_string(ms(now() - arrival)) +
            ",\"capture_ms\":" + std::to_string(captureMs) +
            ",\"gpu_dispatch_ms\":" + std::to_string(metrics.dispatchMs) +
            ",\"fence_wait_ms\":" + std::to_string(metrics.fenceMs) +
            ",\"gpu_submissions\":" + std::to_string(metrics.submissions) +
            ",\"generated_present_ms\":" + std::to_string(generatedPresentMs) +
            ",\"logical_present_ms\":" + std::to_string(logicalPresentMs) +
            ",\"pacing_wait_ms\":" + std::to_string(previousPacingMs) +
            ",\"source_interval_ms\":" +
            std::to_string(sourceIntervalMs) +
            ",\"fast_inputs\":" +
            std::string(config().fastInputs ? "true" : "false") +
            ",\"hud_bypass_requested\":" +
            std::string(config().hudBypass ? "true" : "false") +
            ",\"hud_boundary_available\":" +
            std::string(uiCopied ? "true" : "false") +
            ",\"viewmodel_requested\":" +
            std::string(config().viewmodel ? "true" : "false") +
            ",\"dynamic_model_protection_requested\":" +
            std::string(config().dynamicModels ? "true" : "false") +
            ",\"viewmodel_overlay_available\":" +
            std::string(config().viewmodel && nearColourCopied &&
                                !nearInvalid &&
                                viewmodelBeforeSnapshot.get() &&
                                viewmodelAfterSnapshot.get()
                            ? "true"
                            : "false") +
            ",\"merge_submissions\":" +
            std::string(config().mergeSubmissions ? "true" : "false") +
            ",\"native_queries_total\":" +
            std::to_string(asteria.nativeQueries()) +
            ",\"native_tickets_total\":" +
            std::to_string(asteria.nativeTickets()) +
            ",\"native_submitted_total\":" +
            std::to_string(asteria.nativeSubmitted()) +
            ",\"native_accepted_total\":" +
            std::to_string(asteria.nativeAccepted()) +
            ",\"native_released_total\":" +
            std::to_string(asteria.nativeReleased()) +
            ",\"native_busy_total\":" +
            std::to_string(asteria.nativeBusy()) +
            ",\"presenter_input_valid\":" +
            std::string(presenterInputValid ? "true" : "false") +
            ",\"presenter_input_flags\":" +
            std::to_string(presenterInputFlags) +
            ",\"generated_present_start_qpc\":" +
            std::to_string(generatedStart) + ",\"logical_present_start_qpc\":" +
            std::to_string(logicalStart) + ",\"paired_start_gap_ms\":" +
            std::to_string(generatedStart ? ms(logicalStart - generatedStart)
                                          : 0));
    clearFrame();
    frame++;
    return result;
  }
};

inline std::map<IDirect3DDevice9 *, std::unique_ptr<State>> &states() {
  static std::map<IDirect3DDevice9 *, std::unique_ptr<State>> s;
  return s;
}
inline std::mutex &mutex() {
  static std::mutex m;
  return m;
}
template <class F> inline void observe(IDirect3DDevice9 *d, F &&f) noexcept {
  if (!config().enabled || !d)
    return;

  try {
    std::lock_guard<std::mutex> lock(mutex());
    auto &s = states()[d];

    if (!s)
      s = std::make_unique<State>(d);
    try {
      f(*s);
    } catch (const std::exception &e) {
      s->disabled = true;
      s->history = false;
      s->emit("fg_error", 0, "\"message\":" + quote(e.what()));
    }
  } catch (...) {
  }
}
inline void boundary(IDirect3DDevice9 *d, uint32_t uid) noexcept {
  observe(d, [&](State &s) { s.boundary(uid); });
}
inline void viewport(IDirect3DDevice9 *d, const D3DVIEWPORT9 *v,
                     uint32_t uid) noexcept {
  observe(d, [&](State &s) { s.viewport(v, uid); });
}
inline void beforeDraw(IDirect3DDevice9 *d, uint32_t uid) noexcept {
  observe(d, [&](State &s) { s.beforeDraw(uid); });
}
inline void afterDraw(IDirect3DDevice9 *d, uint32_t uid,
                      HRESULT result) noexcept {
  observe(d, [&](State &s) { s.afterDraw(uid, result); });
}
inline void clear(IDirect3DDevice9 *d, uint32_t uid, HRESULT result) noexcept {
  observe(d, [&](State &s) {
    if (SUCCEEDED(result) && !s.scene)
      s.candidate = {};
    (void)uid;
  });
}
inline void reset(IDirect3DDevice9 *d, uint32_t uid, HRESULT result) noexcept {
  observe(d, [&](State &s) {
    s.invalidate("reset", uid, true);
    if (SUCCEEDED(result))
      s.generation++;
    s.disabled = false;
  });
}
inline void beforeReset(IDirect3DDevice9 *d) noexcept {
  if (!config().enabled || !d)
    return;
  try {
    std::lock_guard<std::mutex> lock(mutex());
    auto found = states().find(d);
    if (found != states().end()) {
      found->second->hudSnapshot.reset();
      found->second->viewmodelBeforeSnapshot.reset();
      found->second->viewmodelAfterSnapshot.reset();
    }
  } catch (...) {
  }
}
inline void destroy(IDirect3DDevice9 *d) noexcept {
  if (!config().enabled)
    return;

  try {
    std::lock_guard<std::mutex> lock(mutex());
    states().erase(d);
  } catch (...) {
  }
}
template <class F>
inline HRESULT present(IDirect3DDevice9 *d, uint32_t uid, F &&backend,
                       bool partial = false,
                       IDirect3DSwapChain9 *chain = nullptr) {
  if (!config().enabled || !d)
    return backend();

  std::lock_guard<std::mutex> lock(mutex());
  std::unique_ptr<State> *p = nullptr;

  try {
    p = &states()[d];

    if (!*p)
      *p = std::make_unique<State>(d);
  } catch (...) {
    return backend();
  }
  return (*p)->present(uid, std::forward<F>(backend), partial, chain);
}
} // namespace ldb_fg
#else
namespace ldb_fg {
inline void boundary(IDirect3DDevice9 *, uint32_t) noexcept {}
inline void viewport(IDirect3DDevice9 *, const D3DVIEWPORT9 *,
                     uint32_t) noexcept {}
inline void beforeDraw(IDirect3DDevice9 *, uint32_t) noexcept {}
inline void afterDraw(IDirect3DDevice9 *, uint32_t, HRESULT) noexcept {}
inline void clear(IDirect3DDevice9 *, uint32_t, HRESULT) noexcept {}
inline void reset(IDirect3DDevice9 *, uint32_t, HRESULT) noexcept {}
inline void beforeReset(IDirect3DDevice9 *) noexcept {}
inline void destroy(IDirect3DDevice9 *) noexcept {}
template <class F>
inline HRESULT present(IDirect3DDevice9 *, uint32_t, F &&backend, bool = false,
                       IDirect3DSwapChain9 * = nullptr) {
  return backend();
}
} // namespace ldb_fg
#endif

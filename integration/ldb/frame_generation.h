#pragma once
#include <cstdint>
#include <d3d9.h>
#include <windows.h>
#if defined(_WIN64)
#include "frame_math.h"
#include "resident_frame_generator.h"
#include <map>
#include <memory>
#include <mutex>
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
  bool enabled = false, generatedOnly = false, hotkeys = true;
  uint32_t captures = 4, pacingMs = 0;
  bool autoCapture = false;
  std::filesystem::path root;

  Config() noexcept {
    try {
      enabled = readEnv(L"LDB_FG_ENABLE") == L"1";

      if (!enabled)
        return;

      auto mode = readEnv(L"LDB_FG_MODE");

      if (!mode.empty() && mode != L"paired" && mode != L"generated")
        throw std::runtime_error("FG mode");

      generatedOnly = mode == L"generated";
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
struct State {
  IDirect3DDevice9 *device = nullptr;
  std::unique_ptr<ResidentFg> gpu;

  ldb_camera::Candidate candidate;
  ldb_camera::Matrix currentMatrix{}, previousMatrix{};

  uint64_t frame = 0, draws = 0, generated = 0, logical = 0, sceneColor = 0,
           sceneDepth = 0, previousColor = 0, previousDepth = 0;

  uint32_t generation = 0, captures = 0, captureLimit = 4, previousFlags = 0;
  bool scene = false, nearSeen = false, nearCopied = false, uiCopied = false,
       history = false, paused = false, generatedOnly = false, disabled = false;

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

  explicit State(IDirect3DDevice9 *d)
      : device(d), generatedOnly(config().generatedOnly) {
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
             quote(generatedOnly ? "generated" : "paired") +
             ",\"auto_capture\":" +
             std::string(config().autoCapture ? "true" : "false") +
             ",\"pacing_budget_ms\":" + std::to_string(config().pacingMs));
  }
  ~State() {
    gpu.reset();
    if (pacingTimer) {
      CancelWaitableTimer(pacingTimer);
      CloseHandle(pacingTimer);
    }
    emit("summary", 0,
         "\"logical_presents\":" + std::to_string(logical) +
             ",\"generated_presents\":" + std::to_string(generated));
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
  void invalidate(const char *reason, uint32_t uid, bool release = false) {
    history = false;
    clearFrame();

    if (release)
      gpu.reset();
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
               ",\"mode\":" + quote(generatedOnly ? "generated" : "paired"));
    }
    if (b && !f9) {
      generatedOnly = !generatedOnly;
      captureLimit = std::min(16u, captures + config().captures);
      if (config().autoCapture)
        scheduleCapture();
      emit("mode", uid,
           "\"enabled\":" + std::string(paused ? "false" : "true") +
               ",\"mode\":" + quote(generatedOnly ? "generated" : "paired"));
    }
    f8 = a;
    f9 = b;
  }
  void transfer(IDirect3DSurface9 *surface, ResidentFg::Slot slot,
                bool write = false) {
    Com<ID3D9VkInteropTexture> texture;
    auto info = imageInfo(surface, texture);
    Com<ID3D9VkInteropDevice> interop;
    hr(device->QueryInterface(__uuidof(ID3D9VkInteropDevice),
                              reinterpret_cast<void **>(interop.put())),
       "FG interop");

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
    } restore{interop.get(), texture.get(), range, target, info.layout};

    interop->TransitionTextureLayout(texture.get(), &range, info.layout,
                                     target);
    interop->FlushRenderingCommands();

    if (!interop->WaitForResource(surface, 0))
      throw std::runtime_error("FG source wait");

    if (write)
      gpu->writeBack(info, slot);
    else
      gpu->copyInto(info, slot,
                    slot < ResidentFg::CurrentUi ? VK_IMAGE_ASPECT_DEPTH_BIT
                                                 : VK_IMAGE_ASPECT_COLOR_BIT);
    restore.apply();

    if (!interop->WaitForResource(surface, 0))
      throw std::runtime_error("FG restore wait");
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
      Com<ID3D9VkInteropDevice> interop;
      hr(device->QueryInterface(__uuidof(ID3D9VkInteropDevice),
                                reinterpret_cast<void **>(interop.put())),
         "FG device interop");
      gpu = std::make_unique<ResidentFg>(interop.get());
      gpu->initialize(b.create.extent.width, b.create.extent.height,
                      d.create.format, b.create.format);
      if (config().autoCapture)
        scheduleCapture();
      emit("gpu_initialized", 0,
           "\"width\":" + std::to_string(gpu->width) +
               ",\"height\":" + std::to_string(gpu->height) +
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
    if (!scene || !gpu || nearInvalid)
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
    transfer(rt.get(), ResidentFg::CurrentNearBefore);
    nearTarget = id(rt.get());
    nearBeforeCopied = true;
  }
  void nearCopy(uint32_t uid) {
    if (!scene || !nearSeen || !gpu || !nearDraws ||
        nearCapturedDraws == nearDraws)
      return;
    Com<IDirect3DSurface9> rt, ds;
    if (nearBeforeCopied && !nearInvalid &&
        SUCCEEDED(device->GetRenderTarget(0, rt.put())) && rt.get() &&
        id(rt.get()) == nearTarget) {
      transfer(rt.get(), ResidentFg::CurrentNearAfter);
      nearColourCopied = true;
    }
    if (SUCCEEDED(device->GetDepthStencilSurface(ds.put())) && ds.get() &&
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
      nearSeen = scene;
      nearBegin();
    } else if (nearSeen)
      nearCopy(uid);

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

    transfer(back.get(), ResidentFg::CurrentUi);
    uiCopied = true;

    if (frame < 16)
      emit("ui_boundary", uid,
           "\"classification\":\"screen_alpha_on_backbuffer_candidate\"");
  }
  void afterDraw(uint32_t uid, HRESULT result) {
    if (disabled || paused || FAILED(result))
      return;
    D3DVIEWPORT9 observed{};
    if (scene && nearSeen && SUCCEEDED(device->GetViewport(&observed)) &&
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
    bool captured = false, generatedFrame = false, qualityRejected = false;
    ResidentFg::Quality quality{};
    double captureMs = 0, generatedPresentMs = 0, logicalPresentMs = 0;
    int64_t generatedStart = 0;
    bool wrote = false, ready = false;
    HRESULT extra = S_OK;
    Com<IDirect3DSurface9> back;

    try {
      Com<IDirect3DSwapChain9> primary;
      hr(device->GetSwapChain(0, primary.put()), "FG primary swapchain");
      bool primaryChain = !chain || chain == primary.get();

      if (!disabled && !paused && !partial && primaryChain && scene &&
          !candidate.conflict &&
          (!nearSeen || (nearColourCopied && !nearInvalid)) && gpu) {
        hr(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, back.put()),
           "FG final backbuffer");
        transfer(back.get(), ResidentFg::CurrentFinal);
        ready = true;

        if (history && previousColor == sceneColor &&
            previousDepth == sceneDepth) {
          ResidentFg::Params params{};

          if (ldb_camera::frameTransforms(previousMatrix, currentMatrix,
                                          params.rows)) {
            uint32_t flags =
                (nearCopied ? 1u : 0u) | (uiCopied ? 2u : 0u) |
                (gpu->finalFormat == VK_FORMAT_B8G8R8A8_UNORM ? 4u : 0u) |
                (nearColourCopied ? 8u : 0u);
            params.flags = {gpu->width, gpu->height, flags, previousFlags};
            bool captureDue =
                config().autoCapture
                    ? (captures < 32 && burstRemaining && now() >= nextCapture)
                    : captures < captureLimit;
            gpu->generate(params, !captureDue);

            if (captureDue) {
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
              captured = true;
              captureMs = ms(now() - captureStart);
              emit("capture", uid,
                   "\"index\":" + std::to_string(captures) + ",\"mode\":" +
                       quote(generatedOnly ? "generated" : "paired"));
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
                quality.reject(uint64_t(gpu->width) * gpu->height);
            if (qualityRejected) {
              transfer(back.get(), ResidentFg::CurrentFinal, true);
              wrote = false;
              generatedFrame = false;
              emit(
                  "frame_rejected", uid,
                  "\"reason\":" +
                      quote(quality.rejectReason(uint64_t(gpu->width) *
                                                 gpu->height)) +
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
      previousFlags = (nearCopied ? 1u : 0u) | (uiCopied ? 2u : 0u) |
                      (nearColourCopied ? 8u : 0u);
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
              ",\"near_draws\":" + std::to_string(nearDraws) +
              ",\"ui_boundary_observed\":" +
              std::string(uiCopied ? "true" : "false"));

    auto metrics = gpu ? gpu->takeCounters() : ResidentFg::Counters{};
    emit(
        "frame_metrics", uid,
        "\"enabled\":" + std::string(!paused && !disabled ? "true" : "false") +
            ",\"mode\":" + quote(generatedOnly ? "generated" : "paired") +
            ",\"generated\":" + std::string(generatedFrame ? "true" : "false") +
            ",\"captured\":" + std::string(captured ? "true" : "false") +
            ",\"quality_rejected\":" +
            std::string(qualityRejected ? "true" : "false") +
            ",\"quality_total_pixels\":" + std::to_string(quality.total) +
            ",\"foreground_pixels\":" + std::to_string(quality.foreground) +
            ",\"near_colour_pixels\":" + std::to_string(quality.nearColour) +
            ",\"near_depth_pixels\":" + std::to_string(quality.nearDepth) +
            ",\"ui_pixels\":" + std::to_string(quality.ui) +
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
inline void destroy(IDirect3DDevice9 *) noexcept {}
template <class F>
inline HRESULT present(IDirect3DDevice9 *, uint32_t, F &&backend, bool = false,
                       IDirect3DSwapChain9 * = nullptr) {
  return backend();
}
} // namespace ldb_fg
#endif

// Standalone x64 diagnostic for the pinned custom Remix runtime. This does not
// load L4D2 or prove Source threading, game geometry conversion, or performance.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#define REMIX_WINAPI_NO_LIBRARY_LOADER
#include <windows.h>
#include <d3d9.h>
#include <dbghelp.h>
#include <remix/remix_c.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <algorithm>
#include <array>

namespace fs = std::filesystem;
constexpr unsigned kWidth = 640, kHeight = 360;
constexpr uint32_t kObject = 0x4c445254;

struct PickResult {
  uint32_t expected = kObject;
  std::atomic<bool> called{false}, hit{false};
  std::atomic<uint32_t> values{0}, callbacks{0};
};
void REMIXAPI_CALL picked(const uint32_t* values, uint32_t count, void* user) {
  auto& result = *static_cast<PickResult*>(user);
  for (uint32_t i = 0; i < count; ++i) { if (values[i] == result.expected) { result.hit.store(true); } }
  result.values.store(count); result.callbacks.fetch_add(1); result.called.store(true);
}

struct Log;
struct Runtime {
  HMODULE module = nullptr;
  remixapi_Interface api{};
  IDirect3D9Ex* d3d = nullptr;
  IDirect3DDevice9Ex* device = nullptr;
  bool registered = false;
  HWND window = nullptr;
  void cleanup(Log& log);
  // The standalone process keeps the DLL mapped through process exit. Window
  // hooks and asynchronous runtime callbacks must never execute unmapped code.
};
template<class T> struct ComPtr {
  T* value = nullptr;
  ~ComPtr() { if (value) { value->Release(); } }
};

struct Log {
  std::ofstream output;
  const char* stage = "prepare";
  explicit Log(const fs::path& path) : output(path) { if (!output) { throw std::runtime_error("open log"); } }
  void result(const char* name, uint32_t code) {
    stage = name;
    output << "{\"stage\":\"" << name << "\",\"code\":" << code << "}\n" << std::flush;
    if (code) { throw std::runtime_error(name); }
  }
  void hr(const char* name, HRESULT code) { result(name, SUCCEEDED(code) ? 0 : uint32_t(code)); }
};

void Runtime::cleanup(Log& log) {
  if (window && IsWindow(window)) {
    log.stage = "destroy_window";
    log.result("destroy_window", DestroyWindow(window) ? 0 : GetLastError());
  }
  window = nullptr;
  if (registered) {
    log.result("shutdown_runtime", uint32_t(api.Shutdown()));
    registered = false; device = nullptr; d3d = nullptr;
  } else {
    if (device) { device->Release(); device = nullptr; }
    if (d3d) { d3d->Release(); d3d = nullptr; }
  }
  log.result("cleanup_complete", 0);
}

std::string jsonString(const std::string& text) {
  std::string result = "\"";
  for (const unsigned char c : text) {
    if (c == '"' || c == '\\') { result += '\\'; result += char(c); }
    else if (c == '\n') { result += "\\n"; }
    else if (c == '\r') { result += "\\r"; }
    else if (c == '\t') { result += "\\t"; }
    else if (c < 32) { result += '?'; }
    else { result += char(c); }
  }
  return result + '"';
}

std::wstring crashPath, dumpPath;
LONG WINAPI crashReport(EXCEPTION_POINTERS* pointers) {
  const auto code = pointers && pointers->ExceptionRecord ? pointers->ExceptionRecord->ExceptionCode : 0;
  HANDLE dump = CreateFileW(dumpPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  bool saved = false;
  if (dump != INVALID_HANDLE_VALUE) {
    MINIDUMP_EXCEPTION_INFORMATION info{GetCurrentThreadId(), pointers, FALSE};
    saved = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), dump,
      MINIDUMP_TYPE(MiniDumpNormal | MiniDumpWithThreadInfo), pointers ? &info : nullptr, nullptr, nullptr) != FALSE;
    CloseHandle(dump);
  }
  HANDLE output = CreateFileW(crashPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (output != INVALID_HANDLE_VALUE) {
    char data[180]{};
    const int size = std::snprintf(data, sizeof(data), "{\"exception_code\":%lu,\"minidump_saved\":%s}\n",
      static_cast<unsigned long>(code), saved ? "true" : "false");
    DWORD written = 0;
    if (size > 0) { WriteFile(output, data, DWORD(size), &written, nullptr); FlushFileBuffers(output); }
    CloseHandle(output);
  }
  return EXCEPTION_EXECUTE_HANDLER;
}

struct ImageStats {
  unsigned minimum = 255, maximum = 0, nonblack = 0;
  unsigned center[3]{}, warmPixels = 0, centerWarm = 0, backgroundWarm = 0;
  bool hasSignal() const {
    // A first filter only. The Python coordinator independently checks the
    // projected silhouette, two stable images, and a translated-object control.
    return warmPixels >= 5000 && centerWarm >= 700 && backgroundWarm <= 100;
  }
};

ImageStats writeImage(const D3DLOCKED_RECT& locked, const fs::path& destination) {
  ImageStats stats;
  std::ofstream file(destination, std::ios::binary);
  file << "P6\n" << kWidth << ' ' << kHeight << "\n255\n";
  for (unsigned y = 0; y < kHeight; ++y) {
    const auto* row = static_cast<const uint8_t*>(locked.pBits) + ptrdiff_t(y) * locked.Pitch;
    for (unsigned x = 0; x < kWidth; ++x) {
      const unsigned rgb[] = {row[4 * x + 2], row[4 * x + 1], row[4 * x]};
      const bool warm = rgb[0] >= 32 && rgb[0] >= rgb[1] + 12 && rgb[1] >= rgb[2] + 3;
      if (warm) {
        ++stats.warmPixels;
        if (x >= 304 && x < 336 && y >= 164 && y < 196) { ++stats.centerWarm; }
        if ((x < 80 || x >= 560) && (y < 60 || y >= 300)) { ++stats.backgroundWarm; }
      }
      if (rgb[0] + rgb[1] + rgb[2] > 4) { ++stats.nonblack; }
      for (unsigned i = 0; i < 3; ++i) {
        stats.minimum = std::min(stats.minimum, rgb[i]); stats.maximum = std::max(stats.maximum, rgb[i]);
        if (x == kWidth / 2 && y == kHeight / 2) { stats.center[i] = rgb[i]; }
        file.put(char(rgb[i]));
      }
    }
  }
  file.close(); if (!file) { throw std::runtime_error("save final color"); }
  return stats;
}

ImageStats screenshot(Runtime& runtime, const fs::path& destination, Log& log) {
  ComPtr<IDirect3DSurface9> gpu, cpu;
  // FINAL_COLOR in the pinned API copies this same backbuffer, but leaks its
  // acquired surface reference. Read it directly and release every reference.
  log.hr("get_final_color_backbuffer", runtime.device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &gpu.value));
  log.hr("create_capture_readback", runtime.device->CreateOffscreenPlainSurface(kWidth, kHeight,
    D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &cpu.value, nullptr));
  log.hr("read_final_color", runtime.device->GetRenderTargetData(gpu.value, cpu.value));
  D3DLOCKED_RECT locked{};
  log.hr("lock_final_color", cpu.value->LockRect(&locked, nullptr, D3DLOCK_READONLY));
  struct Unlock { IDirect3DSurface9* surface; ~Unlock() { surface->UnlockRect(); } } unlock{cpu.value};
  return writeImage(locked, destination);
}

struct Outcome {
  unsigned frames = 0, captures = 0, captureAttempts = 0;
  bool signal = false, passed = false, stableSignal = false;
  const char* failedAt = "";
  std::string error;
  void save(const fs::path& file, const PickResult& pick, const char* cleanup) const {
    std::ofstream out(file);
    out << "{\"schema\":3,\"result\":\"" << (passed ? "static_visible_triangle_readback" : "fail")
        << "\",\"frames\":" << frames << ",\"pick_callback_count\":" << pick.callbacks.load()
        << ",\"pick_hit\":" << (pick.hit.load() ? "true" : "false") << ",\"pick_values\":" << pick.values.load()
        << ",\"capture_attempts\":" << captureAttempts << ",\"readback_successes\":" << captures
        << ",\"stable_image_signal\":" << (stableSignal ? "true" : "false")
        << ",\"image_has_signal\":" << (signal ? "true" : "false") << ",\"cleanup\":" << jsonString(cleanup)
        << ",\"failed_at\":" << jsonString(failedAt) << ",\"error\":" << jsonString(error)
        << ",\"game_rt\":\"unrun\",\"source_threading\":\"unrun\",\"performance\":\"unrun\"}\n";
    out.close(); if (!out) { throw std::runtime_error("save diagnostic status"); }
  }
};

struct Case {
  const char* name;
  bool blend, fixedExposure, highEmission;
  const char* debugView;
  float offset;
  bool started = false;
  PickResult pick{};
  Outcome outcome{};
};

void configure(Runtime& runtime, Log& log, const Case& test) {
  const std::pair<const char*, const char*> options[] = {
    {"rtx.tonemappingMode", test.fixedExposure ? "0" : "1"},
    {"rtx.autoExposure.enabled", test.fixedExposure ? "False" : "True"},
    {"rtx.tonemap.exposureBias", "0"}, {"rtx.tonemap.tonemappingEnabled", "True"},
    {"rtx.tonemap.colorGradingEnabled", "False"},
    {"rtx.debugView.debugViewIdx", test.debugView},
    {"rtx.debugView.composite.compositeViewIdx", "0"},
    {"rtx.debugView.enableGammaCorrection", "True"},
    {"rtx.debugView.replaceCompositeOutput", "False"},
    {"rtx.debugView.overlayOnTopOfRenderOutput", "False"}
  };
  for (const auto& option : options) {
    log.result(option.first, uint32_t(runtime.api.SetConfigVariable(option.first, option.second)));
  }
}

int wmain(int argc, wchar_t** argv) {
  if (argc != 3) { std::cerr << "usage: rt-runtime-smoke.exe <absolute-runtime-dll> <new-output-directory>\n"; return 2; }
  const fs::path dll(argv[1]), directory(argv[2]);
  try {
    if (!dll.is_absolute() || !fs::is_regular_file(dll) || !fs::create_directory(directory)) {
      throw std::runtime_error("require existing absolute DLL and new output directory");
    }
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 2; }
  crashPath = (directory / "crash.json").wstring(); dumpPath = (directory / "crash.dmp").wstring();
  SetUnhandledExceptionFilter(crashReport);
  PickResult pick; // Outlives explicit shutdown and its asynchronous callbacks.
  Log log(directory / "events.jsonl");
  Outcome outcome;
  std::array<Case, 7> cases{{
    {"baseline", false, false, false, "0", 0},
    {"explicit_blend", true, false, false, "0", 0},
    {"fixed_exposure", true, true, false, "0", 0},
    {"high_emission", true, true, true, "0", 0},
    {"albedo", true, true, false, "32", 0},
    {"emission", true, true, false, "30", 0},
    {"translated", false, false, false, "0", 1.0f}
  }};
  unsigned selected = 0;
  bool normalPassed = false;
  Runtime runtime; // Remains alive across the catch and window destruction.
  HWND& window = runtime.window;
  std::array<remixapi_MaterialHandle, 2> materialHandles{};
  std::array<remixapi_MeshHandle, 2> meshHandles{};
  try {
    runtime.module = LoadLibraryExW(dll.c_str(), nullptr,
      LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    log.result("load_runtime", runtime.module ? 0 : GetLastError());
    const auto procedure = GetProcAddress(runtime.module, "remixapi_InitializeLibrary");
    PFN_remixapi_InitializeLibrary initialize = nullptr;
    static_assert(sizeof(initialize) == sizeof(procedure), "Windows function pointer ABI");
    std::memcpy(&initialize, &procedure, sizeof(initialize));
    log.result("find_api", initialize ? 0 : ERROR_PROC_NOT_FOUND);
    remixapi_InitializeLibraryInfo init{};
    init.sType = REMIXAPI_STRUCT_TYPE_INITIALIZE_LIBRARY_INFO;
    init.version = REMIXAPI_VERSION_MAKE(0, 6, 4);
    log.result("initialize_api_0_6_4", uint32_t(initialize(&init, &runtime.api)));
    if (!runtime.api.Shutdown || !runtime.api.CreateMaterial || !runtime.api.DestroyMaterial ||
        !runtime.api.CreateMesh || !runtime.api.DestroyMesh || !runtime.api.SetupCamera ||
        !runtime.api.DrawInstance || !runtime.api.SetConfigVariable || !runtime.api.dxvk_CreateD3D9 ||
        !runtime.api.dxvk_RegisterD3D9Device || !runtime.api.dxvk_CopyRenderingOutput ||
        !runtime.api.pick_RequestObjectPicking || !runtime.api.Present) {
      throw std::runtime_error("incomplete API function table");
    }
    for (const auto& option : {std::pair<const char*, const char*>{"rtx.enableRaytracing", "True"},
         {"rtx.upscalerType", "0"}, {"rtx.enableRayReconstruction", "False"}, {"rtx.dlssPreset", "0"}}) {
      log.result(option.first, uint32_t(runtime.api.SetConfigVariable(option.first, option.second)));
    }
    WNDCLASSW wc{}; wc.lpfnWndProc = DefWindowProcW; wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"LdbRtRuntimeSmoke";
    if (!RegisterClassW(&wc)) { throw std::runtime_error("register window"); }
    RECT bounds{0, 0, kWidth, kHeight}; AdjustWindowRect(&bounds, WS_OVERLAPPEDWINDOW, FALSE);
    window = CreateWindowW(wc.lpszClassName, L"LDB static RT diagnostic", WS_OVERLAPPEDWINDOW,
      CW_USEDEFAULT, CW_USEDEFAULT, bounds.right - bounds.left, bounds.bottom - bounds.top,
      nullptr, nullptr, wc.hInstance, nullptr);
    if (!window) { throw std::runtime_error("create window"); }
    ShowWindow(window, SW_SHOW);
    log.result("create_remix_d3d9", uint32_t(runtime.api.dxvk_CreateD3D9(FALSE, &runtime.d3d)));
    if (!runtime.d3d) { throw std::runtime_error("null d3d9"); }
    D3DADAPTER_IDENTIFIER9 adapter{};
    log.hr("adapter", runtime.d3d->GetAdapterIdentifier(0, 0, &adapter));
    log.output << "{\"vendor_id\":" << adapter.VendorId << ",\"device_id\":" << adapter.DeviceId << "}\n" << std::flush;
    D3DPRESENT_PARAMETERS present{};
    present.BackBufferWidth = kWidth; present.BackBufferHeight = kHeight;
    present.BackBufferFormat = D3DFMT_A8R8G8B8; present.BackBufferCount = 1;
    present.SwapEffect = D3DSWAPEFFECT_DISCARD; present.hDeviceWindow = window;
    present.Windowed = TRUE; present.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    log.hr("create_device", runtime.d3d->CreateDeviceEx(0, D3DDEVTYPE_HAL, window,
      D3DCREATE_HARDWARE_VERTEXPROCESSING, &present, nullptr, &runtime.device));
    if (!runtime.device) { throw std::runtime_error("null device"); }
    log.result("register_device", uint32_t(runtime.api.dxvk_RegisterD3D9Device(runtime.device)));
    runtime.registered = true;

    for (unsigned variant = 0; variant < 2; ++variant) {
      remixapi_MaterialInfoOpaqueEXT opaque{};
      opaque.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
      opaque.albedoConstant = {0.8f, 0.2f, 0.05f}; opaque.opacityConstant = 1; opaque.roughnessConstant = 1;
      remixapi_MaterialInfo material{};
      material.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO; material.pNext = &opaque; material.hash = 0x4c445201 + variant * 16;
      material.emissiveIntensity = variant ? 64.0f : 4.0f; material.emissiveColorConstant = {1, 0.2f, 0.05f};
      log.result("create_material", uint32_t(runtime.api.CreateMaterial(&material, &materialHandles[variant])));
      remixapi_HardcodedVertex vertices[3]{};
      const float positions[3][3] = {{-2, -1, 3}, {0, 2, 3}, {2, -1, 3}};
      for (unsigned i = 0; i < 3; ++i) {
        for (unsigned j = 0; j < 3; ++j) { vertices[i].position[j] = positions[i][j]; }
        vertices[i].normal[2] = -1; vertices[i].color = 0xffffffff;
      }
      const uint32_t indices[] = {0, 1, 2};
      remixapi_MeshInfoSurfaceTriangles surface{};
      surface.vertices_values = vertices; surface.vertices_count = 3;
      surface.indices_values = indices; surface.indices_count = 3; surface.material = materialHandles[variant];
      remixapi_MeshInfo mesh{}; mesh.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO; mesh.hash = 0x4c445202 + variant * 16;
      mesh.surfaces_values = &surface; mesh.surfaces_count = 1;
      log.result("create_mesh", uint32_t(runtime.api.CreateMesh(&mesh, &meshHandles[variant])));
    }
    remixapi_CameraInfoParameterizedEXT parameters{};
    parameters.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO_PARAMETERIZED_EXT;
    parameters.forward = {0, 0, 1}; parameters.up = {0, 1, 0}; parameters.right = {1, 0, 0};
    parameters.fovYInDegrees = 60; parameters.aspect = float(kWidth) / kHeight;
    parameters.nearPlane = 0.1f; parameters.farPlane = 100;
    remixapi_CameraInfo camera{}; camera.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO;
    camera.pNext = &parameters; camera.type = REMIXAPI_CAMERA_TYPE_WORLD;
    // These are the pinned runtime's compact texture enums, not D3DTOP/D3DTA.
    // surface_shared.h: Texture=1, SelectArg1=1. Vulkan: ONE=1, ZERO=0,
    // ADD=0, ALWAYS=7, color component mask RGBA=15.
    remixapi_InstanceInfoBlendEXT blend{};
    blend.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_BLEND_EXT;
    blend.alphaTestCompareOp = 7; blend.srcColorBlendFactor = 1; blend.srcAlphaBlendFactor = 1;
    blend.textureColorArg1Source = 1; blend.textureColorOperation = 1;
    blend.textureAlphaArg1Source = 1; blend.textureAlphaOperation = 1;
    blend.tFactor = 0xffffffff; blend.writeMask = 15;
    const auto started = std::chrono::steady_clock::now();
    double lastProgress = -2;
    for (unsigned index = 0; index < cases.size(); ++index) {
      auto& test = cases[index];
      if (index == 6) {
        // Repeat a normal final-color profile, never a debug view, with just
        // the object's world X changed. Keep its material and exposure fixed.
        test.blend = cases[selected].blend; test.fixedExposure = cases[selected].fixedExposure;
        test.highEmission = cases[selected].highEmission;
      }
      test.started = true; test.pick.expected = kObject + index;
      const auto output = directory / test.name; fs::create_directory(output);
      log.output << "{\"stage\":\"case_begin\",\"case\":" << jsonString(test.name)
                 << ",\"object_id\":" << test.pick.expected << ",\"offset_x\":" << test.offset
                 << ",\"profile\":" << jsonString(index == 6 ? cases[selected].name : test.name) << "}\n" << std::flush;
      std::cout << "RT case " << index + 1 << "/7: " << test.name << std::endl;
      configure(runtime, log, test);
      remixapi_InstanceInfoObjectPickingEXT object{};
      object.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_OBJECT_PICKING_EXT;
      object.objectPickingValue = test.pick.expected; object.pNext = test.blend ? &blend : nullptr;
      remixapi_InstanceInfo instance{}; instance.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
      instance.pNext = &object; instance.mesh = meshHandles[test.highEmission ? 1 : 0]; instance.doubleSided = TRUE;
      for (unsigned i = 0; i < 3; ++i) { instance.transform.matrix[i][i] = 1; }
      instance.transform.matrix[0][3] = test.offset;
      const remixapi_Rect2D center{int(kWidth / 2), int(kHeight / 2), int(kWidth / 2 + 1), int(kHeight / 2 + 1)};
      bool requested = false;
      bool firstSignal = false;
      const unsigned endFrame = index == 0 ? 128 : 80;
      auto capture = [&](const char* name) {
        ++test.outcome.captureAttempts;
        try {
          const auto stats = screenshot(runtime, output / name, log);
          ++test.outcome.captures; test.outcome.signal = stats.hasSignal();
          log.output << "{\"stage\":\"image_stats\",\"case\":" << jsonString(test.name)
                     << ",\"file\":" << jsonString(name) << ",\"frame\":" << outcome.frames
                     << ",\"minimum\":" << stats.minimum << ",\"maximum\":" << stats.maximum
                     << ",\"warm_pixels\":" << stats.warmPixels << ",\"center_warm\":" << stats.centerWarm
                     << ",\"background_warm\":" << stats.backgroundWarm << ",\"center_rgb\":["
                     << stats.center[0] << ',' << stats.center[1] << ',' << stats.center[2] << "]}\n" << std::flush;
          return stats.hasSignal();
        } catch (const std::exception& error) {
          log.output << "{\"stage\":\"capture_failed\",\"case\":" << jsonString(test.name)
                     << ",\"failed_at\":" << jsonString(log.stage) << ",\"error\":" << jsonString(error.what()) << "}\n" << std::flush;
          return false;
        }
      };
      for (unsigned frame = 0; frame < endFrame; ++frame) {
        if (std::chrono::steady_clock::now() - started > std::chrono::seconds(240)) {
          log.stage = "matrix_budget"; throw std::runtime_error("240-second total diagnostic budget exhausted");
        }
        MSG message;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
        if (!IsWindow(window)) { throw std::runtime_error("diagnostic window closed"); }
        log.result("setup_camera", uint32_t(runtime.api.SetupCamera(&camera)));
        log.result("draw_instance", uint32_t(runtime.api.DrawInstance(&instance)));
        if (frame >= 24 && (!requested || (test.pick.called.load() && !test.pick.hit.load() && frame % 15 == 0))) {
          test.pick.called.store(false);
          log.result("request_center_hit", uint32_t(runtime.api.pick_RequestObjectPicking(&center, picked, &test.pick)));
          requested = true;
        }
        // Avoid the pinned API Present wrapper's leaked GetSwapChain reference.
        log.hr("present_device", runtime.device->PresentEx(nullptr, nullptr, nullptr, nullptr, 0));
        ++outcome.frames; ++test.outcome.frames;
        if (frame == endFrame - 17) { firstSignal = capture("early-color.ppm"); }
        if (frame == endFrame - 1) { test.outcome.stableSignal = capture("final-color.ppm") && firstSignal; }
        const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        if (elapsed - lastProgress >= 2) {
          std::ofstream progress(directory / "progress.json");
          progress << "{\"case\":" << jsonString(test.name) << ",\"frames\":" << outcome.frames
                   << ",\"elapsed_seconds\":" << elapsed << ",\"pick_callback_count\":" << test.pick.callbacks.load()
                   << ",\"readback_successes\":" << outcome.captures + test.outcome.captures << "}\n";
          std::cout << "RT " << test.name << ": frame " << frame << ", total " << unsigned(elapsed)
                    << "s, pick callbacks=" << test.pick.callbacks.load() << std::endl;
          lastProgress = elapsed;
        }
        MsgWaitForMultipleObjectsEx(0, nullptr, 16, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
      }
      test.outcome.passed = test.pick.hit.load() && test.outcome.stableSignal;
      if (!test.outcome.passed) {
        test.outcome.failedAt = "verify_visible_triangle";
        test.outcome.error = !test.pick.hit.load() ? "expected object picking hit missing" : "expected orange triangle missing from stable final color";
      }
      outcome.captures += test.outcome.captures; outcome.captureAttempts += test.outcome.captureAttempts;
      if (index < 4 && test.outcome.passed && !normalPassed) { selected = index; normalPassed = true; }
      test.outcome.save(output / "status.json", test.pick, "pending");
    }
    outcome.signal = normalPassed; outcome.stableSignal = normalPassed && cases[6].outcome.stableSignal;
    outcome.passed = normalPassed && cases[6].outcome.passed;
    log.stage = "save_matrix";
    fs::copy_file(directory / cases[selected].name / "final-color.ppm", directory / "final-color.ppm");
    std::ofstream matrix(directory / "matrix.json");
    matrix << "{\"schema\":1,\"selected_profile\":" << jsonString(cases[selected].name)
           << ",\"normal_profile_visible\":" << (normalPassed ? "true" : "false")
           << ",\"translated_offset_x\":1.0,\"debug_views_count_as_acceptance\":false}\n";
    matrix.close(); if (!matrix) { throw std::runtime_error("save case matrix"); }
    if (!outcome.passed) { log.stage = "verify_visible_triangle"; throw std::runtime_error("normal final color and translated control did not both show the expected triangle"); }
  } catch (const std::exception& error) {
    outcome.failedAt = log.stage; outcome.error = error.what();
    log.output << "{\"failed_at\":" << jsonString(log.stage) << ",\"error\":" << jsonString(error.what()) << "}\n" << std::flush;
    std::cerr << error.what() << '\n';
  }
  // Persist the measurement before touching window/device lifetime. A cleanup
  // crash must not erase the test's real failure or be mistaken for GPU loss.
  auto& finalPick = cases[0].started ? cases[selected].pick : pick;
  outcome.save(directory / "status.json", finalPick, "pending");
  try {
    for (const auto mesh : meshHandles) { if (mesh) { log.result("destroy_mesh", uint32_t(runtime.api.DestroyMesh(mesh))); } }
    for (const auto material : materialHandles) { if (material) { log.result("destroy_material", uint32_t(runtime.api.DestroyMaterial(material))); } }
    runtime.cleanup(log);
  } catch (const std::exception& error) {
    log.output << "{\"cleanup_failed_at\":" << jsonString(log.stage) << ",\"error\":" << jsonString(error.what()) << "}\n" << std::flush;
    if (outcome.error.empty()) { outcome.failedAt = log.stage; outcome.error = error.what(); }
    outcome.passed = false; outcome.save(directory / "status.json", finalPick, "fail");
    return 1;
  }
  for (auto& test : cases) {
    if (test.started) { test.outcome.save(directory / test.name / "status.json", test.pick, "complete"); }
  }
  outcome.save(directory / "status.json", finalPick, "complete");
  return outcome.passed ? 0 : 1;
}

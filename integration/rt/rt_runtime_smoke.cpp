// Standalone x64 diagnostic for the pinned custom Remix runtime. This does not
// load L4D2 or prove Source threading, game geometry conversion, or performance.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#define REMIX_WINAPI_NO_LIBRARY_LOADER
#include <windows.h>
#include <d3d9.h>
#include <remix/remix_c.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;
constexpr unsigned kWidth = 640, kHeight = 360;
constexpr uint32_t kObject = 0x4c445254;

struct PickResult {
  std::atomic<bool> called{false}, hit{false};
  std::atomic<uint32_t> values{0};
};
void REMIXAPI_CALL picked(const uint32_t* values, uint32_t count, void* user) {
  auto& result = *static_cast<PickResult*>(user);
  for (uint32_t i = 0; i < count; ++i) { if (values[i] == kObject) { result.hit.store(true); } }
  result.values.store(count); result.called.store(true);
}

struct Runtime {
  HMODULE module = nullptr;
  remixapi_Interface api{};
  IDirect3D9Ex* d3d = nullptr;
  IDirect3DDevice9Ex* device = nullptr;
  bool registered = false;
  ~Runtime() {
    // The pinned runtime's Shutdown drains its D3D reference counts, including
    // those returned by CreateD3D9/CreateDeviceEx. Do not release stale pointers.
    if (registered) { api.Shutdown(); }
    else { if (device) { device->Release(); } if (d3d) { d3d->Release(); } }
    if (module) { FreeLibrary(module); }
  }
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

void screenshot(Runtime& runtime, const fs::path& destination, Log& log) {
  ComPtr<IDirect3DSurface9> gpu, cpu;
  log.hr("create_capture_target", runtime.device->CreateRenderTarget(kWidth, kHeight, D3DFMT_A8R8G8B8,
    D3DMULTISAMPLE_NONE, 0, FALSE, &gpu.value, nullptr));
  log.hr("create_capture_readback", runtime.device->CreateOffscreenPlainSurface(kWidth, kHeight,
    D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &cpu.value, nullptr));
  log.result("copy_final_color", uint32_t(runtime.api.dxvk_CopyRenderingOutput(gpu.value,
    REMIXAPI_DXVK_COPY_RENDERING_OUTPUT_TYPE_FINAL_COLOR)));
  log.hr("read_final_color", runtime.device->GetRenderTargetData(gpu.value, cpu.value));
  D3DLOCKED_RECT locked{};
  log.hr("lock_final_color", cpu.value->LockRect(&locked, nullptr, D3DLOCK_READONLY));
  struct Unlock { IDirect3DSurface9* surface; ~Unlock() { surface->UnlockRect(); } } unlock{cpu.value};
  std::ofstream file(destination, std::ios::binary);
  file << "P6\n" << kWidth << ' ' << kHeight << "\n255\n";
  for (unsigned y = 0; y < kHeight; ++y) {
    const auto* row = static_cast<const uint8_t*>(locked.pBits) + ptrdiff_t(y) * locked.Pitch;
    for (unsigned x = 0; x < kWidth; ++x) {
      const char rgb[] = {char(row[4 * x + 2]), char(row[4 * x + 1]), char(row[4 * x])};
      file.write(rgb, 3);
    }
  }
  file.close(); if (!file) { throw std::runtime_error("save final color"); }
}

int wmain(int argc, wchar_t** argv) {
  if (argc != 3) { std::cerr << "usage: rt-runtime-smoke.exe <absolute-runtime-dll> <new-output-directory>\n"; return 2; }
  const fs::path dll(argv[1]), directory(argv[2]);
  try {
    if (!dll.is_absolute() || !fs::is_regular_file(dll) || !fs::create_directory(directory)) {
      throw std::runtime_error("require existing absolute DLL and new output directory");
    }
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 2; }
  PickResult pick; // Outlives Runtime shutdown and its asynchronous callbacks.
  Log log(directory / "events.jsonl");
  unsigned frames = 0;
  bool image = false;
  HWND window = nullptr;
  try {
    Runtime runtime;
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

    remixapi_MaterialInfoOpaqueEXT opaque{};
    opaque.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
    opaque.albedoConstant = {0.8f, 0.2f, 0.05f}; opaque.opacityConstant = 1; opaque.roughnessConstant = 1;
    remixapi_MaterialInfo material{};
    material.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO; material.pNext = &opaque; material.hash = 0x4c445201;
    material.emissiveIntensity = 4; material.emissiveColorConstant = {1, 0.2f, 0.05f};
    remixapi_MaterialHandle materialHandle = nullptr;
    log.result("create_material", uint32_t(runtime.api.CreateMaterial(&material, &materialHandle)));
    remixapi_HardcodedVertex vertices[3]{};
    const float positions[3][3] = {{-2, -1, 3}, {0, 2, 3}, {2, -1, 3}};
    for (unsigned i = 0; i < 3; ++i) {
      for (unsigned j = 0; j < 3; ++j) { vertices[i].position[j] = positions[i][j]; }
      vertices[i].normal[2] = -1; vertices[i].color = 0xffffffff;
    }
    const uint32_t indices[] = {0, 1, 2};
    remixapi_MeshInfoSurfaceTriangles surface{};
    surface.vertices_values = vertices; surface.vertices_count = 3;
    surface.indices_values = indices; surface.indices_count = 3; surface.material = materialHandle;
    remixapi_MeshInfo mesh{}; mesh.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO; mesh.hash = 0x4c445202;
    mesh.surfaces_values = &surface; mesh.surfaces_count = 1;
    remixapi_MeshHandle meshHandle = nullptr;
    log.result("create_mesh", uint32_t(runtime.api.CreateMesh(&mesh, &meshHandle)));
    remixapi_CameraInfoParameterizedEXT parameters{};
    parameters.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO_PARAMETERIZED_EXT;
    parameters.forward = {0, 0, 1}; parameters.up = {0, 1, 0}; parameters.right = {1, 0, 0};
    parameters.fovYInDegrees = 60; parameters.aspect = float(kWidth) / kHeight;
    parameters.nearPlane = 0.1f; parameters.farPlane = 100;
    remixapi_CameraInfo camera{}; camera.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO;
    camera.pNext = &parameters; camera.type = REMIXAPI_CAMERA_TYPE_WORLD;
    remixapi_InstanceInfoObjectPickingEXT object{};
    object.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_OBJECT_PICKING_EXT; object.objectPickingValue = kObject;
    remixapi_InstanceInfo instance{}; instance.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
    instance.pNext = &object; instance.mesh = meshHandle; instance.doubleSided = TRUE;
    for (unsigned i = 0; i < 3; ++i) { instance.transform.matrix[i][i] = 1; }
    const remixapi_Rect2D center{int(kWidth / 2), int(kHeight / 2), int(kWidth / 2 + 1), int(kHeight / 2 + 1)};
    bool requested = false;
    const auto started = std::chrono::steady_clock::now();
    for (; frames < 600 && std::chrono::steady_clock::now() - started < std::chrono::seconds(60); ++frames) {
      MSG message;
      while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
      if (!IsWindow(window)) { throw std::runtime_error("diagnostic window closed"); }
      log.result("setup_camera", uint32_t(runtime.api.SetupCamera(&camera)));
      log.result("draw_instance", uint32_t(runtime.api.DrawInstance(&instance)));
      if (frames >= 30 && (!requested || (pick.called.load() && !pick.hit.load() && frames % 15 == 0))) {
        pick.called.store(false);
        log.result("request_center_hit", uint32_t(runtime.api.pick_RequestObjectPicking(&center, picked, &pick)));
        requested = true;
      }
      log.result("present", uint32_t(runtime.api.Present(nullptr)));
      if (frames >= 60 && pick.hit.load()) { screenshot(runtime, directory / "final-color.ppm", log); image = true; ++frames; break; }
      MsgWaitForMultipleObjectsEx(0, nullptr, 16, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }
    log.result("destroy_mesh", uint32_t(runtime.api.DestroyMesh(meshHandle)));
    log.result("destroy_material", uint32_t(runtime.api.DestroyMaterial(materialHandle)));
    if (!pick.hit.load() || !image) { throw std::runtime_error("no expected center hit and final-color readback"); }
    // Shutdown completes before the callback storage or DLL code is released.
  } catch (const std::exception& error) {
    log.output << "{\"failed_at\":\"" << log.stage << "\"}\n" << std::flush;
    std::cerr << error.what() << '\n';
    if (window && IsWindow(window)) { DestroyWindow(window); }
    std::ofstream(directory / "status.json") << "{\"schema\":1,\"result\":\"fail\",\"game_rt\":\"unrun\"}\n";
    return 1;
  }
  if (window && IsWindow(window)) { DestroyWindow(window); }
  std::ofstream(directory / "status.json") << "{\"schema\":1,\"result\":\"static_hit_and_color_readback\",\"frames\":" << frames
    << ",\"object_id\":" << kObject << ",\"pick_values\":" << pick.values.load()
    << ",\"game_rt\":\"unrun\",\"source_threading\":\"unrun\",\"performance\":\"unrun\"}\n";
  return 0;
}

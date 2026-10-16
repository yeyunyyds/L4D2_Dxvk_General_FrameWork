#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "rt_world_runtime.h"
#include "rt_file_hash.h"
#include "rt_draw_packet.h"
#include "rt_world_diagnostics.h"
// A separate TU prevents mixing the Bridge's old API with the pinned 0.6.4 ABI.
#define REMIX_WINAPI_NO_LIBRARY_LOADER
#include "rt_world_remix_c.h"
#include <cmath>
#include <cstring>
#include <iomanip>
#include <set>

namespace ldb_rt::world_runtime {
namespace {
remixapi_Interface api{};
draw_observe::Request launch;
std::ofstream events;
IDirect3DDevice9* registered = nullptr;
bool initialized = false;
bool raytracingRequested = false;
uint64_t physicalDraws = 0;
world_diagnostics::Comparison comparison;
std::set<std::array<uint64_t, 3>> audited;
std::array<unsigned, 4> auditCounts{};
std::array<float, 16> lastView{}, lastProjection{};
bool haveWorldCamera = false;
const std::pair<const char*, const char*> options[] = {
  {"rtx.enableRaytracing", "False"}, {"rtx.useVertexCapture", "False"},
  {"rtx.upscalerType", "0"}, {"rtx.enableRayReconstruction", "False"},
  {"rtx.sceneScale", "0.3937007874"},
  {"rtx.fallbackLightMode", "2"}, {"rtx.fallbackLightType", "1"},
  {"rtx.fallbackLightRadiance", "30,30,30"}, {"rtx.fallbackLightRadius", "5"},
  {"rtx.fallbackLightPositionOffset", "0,0,0"}, {"rtx.enableFallbackLightShaping", "False"},
  {"rtx.fallbackLightDirection", "0,0,-1"},
  {"rtx.autoExposure.enabled", "True"}, {"rtx.autoExposure.evMinValue", "-2"},
  {"rtx.autoExposure.evMaxValue", "5"}, {"rtx.tonemappingMode", "1"},
  {"rtx.localtonemap.exposure", "0.75"},
  {"rtx.bloom.enable", "False"}, {"rtx.postfx.enable", "False"}
};
void event(const char* name, uint32_t code, const char* value = nullptr) {
  if (events) {
    events << "{\"event\":\"" << name << "\",\"code\":" << code;
    if (value) { events << ",\"value\":\"" << value << '"'; }
    events << "}\n" << std::flush;
  }
}
bool configure() {
  for (const auto& option : options) {
    const auto code = api.SetConfigVariable(option.first, option.second); event(option.first, uint32_t(code), option.second);
    if (code != REMIXAPI_ERROR_CODE_SUCCESS) { return false; }
  }
  return true;
}
void bind(IDirect3DDevice9* device) noexcept {
  try {
    if (!initialized || registered == device || registered) { return; }
    const auto result = api.dxvk_RegisterD3D9Device(static_cast<IDirect3DDevice9Ex*>(device));
    event("register_game_device", uint32_t(result));
    if (result == REMIXAPI_ERROR_CODE_SUCCESS) { registered = device; }
  } catch (...) { }
}
bool raytracing(bool enabled) noexcept {
  try {
    if (!initialized || (enabled && !registered)) { return false; }
    if (enabled == raytracingRequested) { return true; }
    const auto result = api.SetConfigVariable("rtx.enableRaytracing", enabled ? "True" : "False");
    // The pinned API queues option changes; the runtime applies them at a
    // frame boundary. This records a request, never proof of effective RT.
    event(enabled ? "raytracing_requested_on" : "raytracing_requested_off", uint32_t(result));
    if (result != REMIXAPI_ERROR_CODE_SUCCESS) { return false; }
    raytracingRequested = enabled; return true;
  } catch (...) { return false; }
}
void draw(IDirect3DDevice9* device) noexcept {
  bind(device);
  if (!physicalDraws++) { event("first_game_draw", 0, "startup_raster_until_selected_world"); }
}
template<class T> struct Ref {
  T* value = nullptr;
  ~Ref() { if (value) { value->Release(); } }
};
void audit(IDirect3DDevice9* device, const draw_observe::Snapshot& snapshot) noexcept {
  try {
    if (!events || device != registered) { return; }
    lastView = snapshot.view; lastProjection = snapshot.projection; haveWorldCamera = true;
    const auto phase = comparison.phase();
    const auto index = size_t(phase);
    const std::array<uint64_t, 3> key{{uint64_t(phase), snapshot.material, snapshot.materialTable}};
    if (phase == world_diagnostics::Phase::Raster || auditCounts[index] >= 64 || !audited.insert(key).second) { return; }
    ++auditCounts[index];
    Ref<IDirect3DPixelShader9> shader;
    const auto ps = device->GetPixelShader(&shader.value);
    events << "{\"event\":\"world_material_sample\",\"phase\":\"" << world_diagnostics::name(phase)
      << "\",\"material\":" << snapshot.material << ",\"table\":" << snapshot.materialTable
      << ",\"ordinal\":" << snapshot.ordinal << ",\"pixel_shader_hresult\":" << uint32_t(ps)
      << ",\"pixel_shader_bound\":" << (shader.value ? "true" : "false") << ",\"stages\":[";
    const D3DTEXTURESTAGESTATETYPE names[] = {D3DTSS_COLOROP, D3DTSS_COLORARG1, D3DTSS_COLORARG2,
      D3DTSS_ALPHAOP, D3DTSS_ALPHAARG1, D3DTSS_ALPHAARG2, D3DTSS_TEXCOORDINDEX, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTSS_RESULTARG};
    for (DWORD stage = 0; stage < 8; ++stage) {
      if (stage) { events << ','; }
      events << "{\"stage\":" << stage << ",\"texture_stage_values\":[";
      uint32_t failures = 0;
      for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        DWORD value = 0;
        if (FAILED(device->GetTextureStageState(stage, names[i], &value))) { failures |= 1u << i; }
        if (i) { events << ','; } events << value;
      }
      Ref<IDirect3DBaseTexture9> texture;
      const auto fetched = device->GetTexture(stage, &texture.value);
      const auto type = texture.value ? texture.value->GetType() : D3DRESOURCETYPE(0);
      D3DSURFACE_DESC desc{}; HRESULT described = S_FALSE;
      if (type == D3DRTYPE_TEXTURE) { described = static_cast<IDirect3DTexture9*>(texture.value)->GetLevelDesc(0, &desc); }
      DWORD srgb = 0;
      const auto sampler = device->GetSamplerState(stage, D3DSAMP_SRGBTEXTURE, &srgb);
      events << "],\"query_failure_bits\":" << failures << ",\"texture_hresult\":" << uint32_t(fetched)
        << ",\"texture_bound\":" << (texture.value ? "true" : "false") << ",\"texture_type\":" << uint32_t(type)
        << ",\"description_hresult\":" << uint32_t(described) << ",\"width\":" << desc.Width
        << ",\"height\":" << desc.Height << ",\"format\":" << uint32_t(desc.Format)
        << ",\"srgb_hresult\":" << uint32_t(sampler) << ",\"srgb\":" << srgb << '}';
    }
    events << "]}\n" << std::flush;
  } catch (...) { event("material_sample_exception", uint32_t(E_FAIL)); }
}
void capture(IDirect3DDevice9* device, uint64_t frame) noexcept {
  try {
    if (device != registered || !events) { return; }
    // Explicitly reduced diagnostic readback, not full-resolution quality data.
    constexpr UINT width = 640, height = 400;
    for (unsigned kind = 0; kind < 3; ++kind) {
      const std::string stem = "world-" + std::to_string(frame) + "-" + std::to_string(kind);
      Ref<IDirect3DSurface9> target, staging;
      HRESULT hr = device->CreateRenderTarget(width, height, D3DFMT_A32B32G32R32F, D3DMULTISAMPLE_NONE, 0, FALSE, &target.value, nullptr);
      if (SUCCEEDED(hr)) { hr = device->ColorFill(target.value, nullptr, D3DCOLOR_ARGB(0, 0, 0, 0)); }
      const auto copied = SUCCEEDED(hr) ? api.dxvk_CopyRenderingOutput(target.value, remixapi_dxvk_CopyRenderingOutputType(kind)) : REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
      if (copied == REMIXAPI_ERROR_CODE_SUCCESS && SUCCEEDED(hr)) {
        hr = device->CreateOffscreenPlainSurface(width, height, D3DFMT_A32B32G32R32F, D3DPOOL_SYSTEMMEM, &staging.value, nullptr);
      } else if (SUCCEEDED(hr)) { hr = E_FAIL; }
      if (SUCCEEDED(hr)) { hr = device->GetRenderTargetData(target.value, staging.value); }
      D3DLOCKED_RECT locked{};
      if (SUCCEEDED(hr)) { hr = staging.value->LockRect(&locked, nullptr, D3DLOCK_READONLY); }
      bool written = false;
      if (SUCCEEDED(hr)) {
        if (locked.pBits && locked.Pitch >= int(width * 16)) {
          std::ofstream output(launch.output / (stem + ".rgba32f"), std::ios::binary);
          for (UINT row = 0; row < height; ++row) {
            output.write(static_cast<const char*>(locked.pBits) + size_t(row) * size_t(locked.Pitch), width * 16);
          }
          output.close(); written = bool(output);
        }
        const HRESULT unlock = staging.value->UnlockRect(); if (FAILED(unlock)) { hr = unlock; }
      }
      events << "{\"event\":\"world_readback\",\"phase\":\"" << world_diagnostics::name(comparison.phase())
        << "\",\"frame\":" << frame << ",\"kind\":" << kind
        << ",\"width\":" << width << ",\"height\":" << height << ",\"copy_code\":" << uint32_t(copied)
        << ",\"hresult\":" << uint32_t(hr) << ",\"written\":" << (written ? "true" : "false")
        << ",\"file\":\"" << stem << ".rgba32f\",\"initialized_to_zero\":true,\"sampling\":\"runtime_nearest_blit\"}\n" << std::flush;
    }
    events << "{\"event\":\"world_camera_sample\",\"frame\":" << frame
      << ",\"available\":" << (haveWorldCamera ? "true" : "false") << ",\"view\":[";
    for (size_t i = 0; i < lastView.size(); ++i) { if (i) { events << ','; } events << lastView[i]; }
    events << "],\"projection\":[";
    for (size_t i = 0; i < lastProjection.size(); ++i) { if (i) { events << ','; } events << lastProjection[i]; }
    events << "]}\n" << std::flush;
    const bool advanced = comparison.afterCapture(frame, [](const char* key, const char* value) {
      const auto code = api.SetConfigVariable(key, value); event(key, uint32_t(code), value);
      return code == REMIXAPI_ERROR_CODE_SUCCESS;
    });
    events << "{\"event\":\"lighting_phase_requested\",\"code\":" << (advanced ? 0 : uint32_t(E_FAIL))
      << ",\"after_present\":" << frame << ",\"phase\":\"" << world_diagnostics::name(comparison.phase()) << "\"}\n" << std::flush;
    if (!advanced) { (void)raytracing(false); state().active = false; }
  } catch (...) {
    event("readback_exception", uint32_t(E_FAIL)); (void)raytracing(false); state().active = false;
  }
}
}
HMODULE load(const draw_observe::Request& request) noexcept {
  try {
    state() = {};
    initialized = false; registered = nullptr; raytracingRequested = false; physicalDraws = 0;
    comparison = {}; audited.clear(); auditCounts = {}; haveWorldCamera = false; lastView = {}; lastProjection = {};
    launch = request;
    events.open(request.output / "world-runtime.jsonl", std::ios::binary);
    if (!events || !request.world || sizeof(void*) != 8) { return nullptr; }
    events << std::setprecision(9);
    const auto path = request.runtime / "d3d9.dll";
    if (ldb_rt_file::fileSha256(path) != "21c826bac9fe6a27b28d0b1300f838efa21b010b24216c44f4728b266d8a8ec7") {
      event("runtime_sha_mismatch", ERROR_INVALID_DATA); return nullptr;
    }
    for (const auto* name : {L"DXVK_LOG_PATH", L"DXVK_METRICS_PATH"}) { SetEnvironmentVariableW(name, request.output.c_str()); }
    SetEnvironmentVariableW(L"DXVK_CONFIG_FILE", (request.output / "world.conf").c_str());
    SetEnvironmentVariableW(L"DXVK_RTX_CONFIG_FILE", (request.output / "world.conf").c_str());
    HMODULE module = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module) { event("load_runtime", GetLastError()); return nullptr; }
    wchar_t loaded[32768]{};
    if (!GetModuleFileNameW(module, loaded, 32768) || !std::filesystem::equivalent(path, loaded)) {
      event("loaded_runtime_path_mismatch", ERROR_INVALID_DATA); return nullptr;
    }
    event("loaded_pinned_runtime", 0);
    const auto address = GetProcAddress(module, "remixapi_InitializeLibrary");
    PFN_remixapi_InitializeLibrary initialize = nullptr;
    static_assert(sizeof(initialize) == sizeof(address), "Windows function pointer ABI");
    std::memcpy(&initialize, &address, sizeof(initialize));
    if (!initialize) { event("find_api", ERROR_PROC_NOT_FOUND); return nullptr; }
    remixapi_InitializeLibraryInfo info{}; info.sType = REMIXAPI_STRUCT_TYPE_INITIALIZE_LIBRARY_INFO;
    info.version = REMIXAPI_VERSION_MAKE(0, 6, 4);
    const auto result = initialize(&info, &api); event("initialize_api_0_6_4", uint32_t(result));
    if (result != REMIXAPI_ERROR_CODE_SUCCESS || !api.SetConfigVariable || !api.Shutdown ||
        !api.dxvk_RegisterD3D9Device || !api.dxvk_CopyRenderingOutput) { return nullptr; }
    initialized = true;
    if (!configure()) { return nullptr; }
    event("startup_raster_policy", 0, "RT_off_until_valid_converted_draw_after_control");
    event("lighting_comparison_policy", 0, "v5_sphere30_sphere3000_distant3_readbacks30_120_180_240");
    event("material_audit_policy", 0, "first_restored_state_per_material_phase_max64_COLOROP_COLORARG1_COLORARG2_ALPHAOP_ALPHAARG1_ALPHAARG2_TEXCOORDINDEX_TEXTURETRANSFORMFLAGS_RESULTARG");
    state() = {true, &bind, &capture, &raytracing, &draw, &audit}; return module;
  } catch (...) { event("runtime_initialization_exception", uint32_t(E_FAIL)); return nullptr; }
}
bool configureRaster() noexcept {
  try {
    // API setters can accept a request before the runtime creates its option
    // layers. Reapply after standard Direct3DCreate9 initializes those layers;
    // this also overrides an old user.conf through the actual user layer.
    if (!initialized || !configure()) { return false; }
    raytracingRequested = false; event("post_d3d9_startup_raster_request", 0); return true;
  } catch (...) { event("post_d3d9_configuration_exception", uint32_t(E_FAIL)); return false; }
}
void shutdown() noexcept {
  // The game owns its device. Do not call the standalone API Shutdown path:
  // it force-releases the device and D3D9 object to zero. The Bridge owns both.
  (void)raytracing(false);
  state() = {}; registered = nullptr; initialized = false;
  if (events) { event("physical_draw_count", 0, std::to_string(physicalDraws).c_str()); }
  if (events) { event("host_cleanup", 0); events.close(); }
}
} // namespace ldb_rt::world_runtime

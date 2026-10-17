#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "rt_world_runtime.h"
#include "rt_file_hash.h"
#include "rt_draw_packet.h"
#include "rt_world_diagnostics.h"
#include "rt_overlay.h"
#include "rt_scene_input.h"
#include "rt_bulk_static.h"
#include <map>
#include <sstream>
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
world_diagnostics::CoverageComparison comparison;
std::set<std::array<uint64_t, 4>> audited;
std::array<unsigned, 4> auditCounts{};
std::array<float, 16> lastView{}, lastProjection{};
bool haveWorldCamera = false;
const std::pair<const char*, const char*> options[] = {
  {"rtx.enableRaytracing", "False"}, {"rtx.useVertexCapture", "False"},
  {"rtx.upscalerType", "0"}, {"rtx.enableRayReconstruction", "False"},
  {"rtx.sceneScale", "0.3937007874"},
  {"rtx.fallbackLightMode", "2"}, {"rtx.fallbackLightType", "1"},
  {"rtx.fallbackLightRadiance", "3000,3000,3000"}, {"rtx.fallbackLightRadius", "5"},
  {"rtx.fallbackLightPositionOffset", "0,0,0"}, {"rtx.enableFallbackLightShaping", "False"},
  {"rtx.fallbackLightDirection", "0,0,-1"},
  {"rtx.autoExposure.enabled", "True"}, {"rtx.autoExposure.evMinValue", "-2"},
  {"rtx.autoExposure.evMaxValue", "5"}, {"rtx.tonemappingMode", "1"},
  {"rtx.localtonemap.exposure", "0.75"},
  {"rtx.orthographicIsUI", "True"}, {"rtx.skipDrawCallsPostRTXInjection", "False"},
  {"rtx.enableAlphaBlend", "True"}, {"rtx.enableAlphaTest", "True"},
  {"rtx.fogIgnoreSky", "True"},
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
    const std::array<uint64_t, 4> key{{uint64_t(phase), snapshot.material, snapshot.materialTable,snapshot.materialInfo.format}};
    if (phase == world_diagnostics::CoveragePhase::Raster || auditCounts[index] >= 64 || !audited.insert(key).second) { return; }
    ++auditCounts[index];
    Ref<IDirect3DPixelShader9> shader;
    const auto ps = device->GetPixelShader(&shader.value);
    DWORD cull=0;const auto cullResult=device->GetRenderState(D3DRS_CULLMODE,&cull);
    DWORD category=0;const auto categoryResult=device->GetRenderState(static_cast<D3DRENDERSTATETYPE>(42),&category);
    events << "{\"event\":\"world_material_sample\",\"phase\":\"" << world_diagnostics::name(phase)
      << "\",\"material\":" << snapshot.material << ",\"table\":" << snapshot.materialTable
      << ",\"ordinal\":" << snapshot.ordinal << ",\"vertex_format\":" << snapshot.materialInfo.format
      << ",\"shader_name\":\"" << snapshot.materialInfo.shader.data() << "\",\"cull_mode\":" << cull
      << ",\"restored_category\":" << category << ",\"category_hresult\":" << uint32_t(categoryResult)
      << ",\"cull_hresult\":" << uint32_t(cullResult) << ",\"pixel_shader_hresult\":" << uint32_t(ps)
      << ",\"pixel_shader_bound\":" << (shader.value ? "true" : "false") << ",\"render_state_values\":[";
    const D3DRENDERSTATETYPE renderNames[] = {D3DRS_TEXTUREFACTOR, D3DRS_LIGHTING, D3DRS_COLORVERTEX,
      D3DRS_DIFFUSEMATERIALSOURCE, D3DRS_AMBIENTMATERIALSOURCE, D3DRS_AMBIENT, D3DRS_FOGENABLE, D3DRS_SRGBWRITEENABLE};
    uint32_t renderFailures = 0;
    for (unsigned i = 0; i < sizeof(renderNames) / sizeof(renderNames[0]); ++i) {
      DWORD value = 0;
      if (FAILED(device->GetRenderState(renderNames[i], &value))) { renderFailures |= 1u << i; }
      if (i) { events << ','; } events << value;
    }
    events << "],\"render_state_query_failure_bits\":" << renderFailures << ",\"stages\":[";
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
struct SceneCounts { uint64_t attempts=0,ready=0,reconstructed=0,fallback=0;std::map<std::string,uint64_t> reasons; };
std::array<SceneCounts,4> sceneCounts{};
std::array<unsigned,4> sceneSampleCounts{};
std::array<std::array<unsigned,3>,4> sceneFamilyCounts{};
std::set<std::array<uint64_t,4>> sceneSampled;
std::map<uint32_t,bool> skyReadiness;
void inspect(IDirect3DDevice9* device,const draw_observe::Snapshot& s,uint32_t kind,D3DPRIMITIVETYPE type,const std::array<uint32_t,6>& args) noexcept {
  try {
    if (!events || device!=registered) { return; }
    const bool sky=scene_draw::candidate(s,true,false)==scene_draw::Mode::Sky;
    if (!sky && !bulk_static::candidate(s)) { return; }
    const auto phase=comparison.phase();const auto index=size_t(phase);
    const unsigned group=sky?0u:bulk_static::family(s.materialInfo.shader.data())==1?1u:2u;
    constexpr unsigned limits[]={6,8,10};
    const std::array<uint64_t,4> key{{uint64_t(phase),s.material,s.materialTable,s.materialInfo.format}};
    if (sceneFamilyCounts[index][group]>=limits[group] || !sceneSampled.insert(key).second) { return; }
    ++sceneFamilyCounts[index][group];++sceneSampleCounts[index];
    auto input=scene_input::capture(device,kind,type,args);
    const char* replay="not_sky";
    if (sky) {
      auto warmed=scene_input::prepare(device,s,input);replay=warmed.reason;
      if (phase==world_diagnostics::CoveragePhase::ExpandedWorld) { skyReadiness[s.material]=warmed.ready; }
    }
    std::ostringstream log;log << std::setprecision(9);
    const std::string prefix="scene-"+std::to_string(s.ordinal);
    log << "{\"event\":\"scene_input_sample\",\"phase\":\"" << world_diagnostics::name(phase) << "\",\"ordinal\":" << s.ordinal
      << ",\"material\":" << s.material << ",\"table\":" << s.materialTable << ",\"shader\":\"" << s.materialInfo.shader.data()
      << "\",\"format\":" << s.materialInfo.format << ",\"kind\":" << kind << ",\"topology\":" << uint32_t(type) << ",\"args\":[";
    for (unsigned i=0;i<6;++i) { if (i) { log << ','; }log << args[i]; }
    log << "],\"world\":[";for (unsigned i=0;i<16;++i) { if (i) { log << ','; }log << s.world[i]; }
    log << "],\"view\":[";for (unsigned i=0;i<16;++i) { if (i) { log << ','; }log << s.view[i]; }
    log << "],\"projection\":[";for (unsigned i=0;i<16;++i) { if (i) { log << ','; }log << s.projection[i]; }
    log << "],\"geometry_hresult\":" << uint32_t(input.geometryResult) << ",\"shader_hresult\":" << uint32_t(input.shaderResult)
      << ",\"constants_hresult\":" << uint32_t(input.constantsResult) << ",\"geometry_status\":\"" << input.reason
      << "\",\"replay_status\":\"" << replay << "\",\"first_absolute_vertex\":" << input.first << ",\"vertex_count\":" << input.count << ",\"stride\":" << input.stride
      << ",\"stream_offset\":" << input.offset << ",\"index_bytes\":" << input.indexBytes << ",";
    // Prefix-only auxiliary streams are evidence, never claimed as complete vertices.
    log << "\"auxiliary\":[";
    for (unsigned i=0;i<3;++i) { const auto& a=input.auxiliary[i];if (i) { log << ','; }
      log << "{\"stream\":" << a.stream << ",\"offset\":" << a.offset << ",\"stride\":" << a.stride << ",\"frequency\":" << a.frequency << ",\"hresult\":" << uint32_t(a.result) << ",\"prefix_bytes\":" << a.bytes.size() << '}'; }
    log << "],\"integer_constants_hresult\":" << uint32_t(input.integerResult) << ",\"bool_constants_hresult\":" << uint32_t(input.boolResult) << ",\"pixel_constants_hresult\":" << uint32_t(input.pixelResult) << ",\"files\":[";
    bool comma=false;
    const auto save=[&](const char* suffix,const void* bytes,size_t size) {
      if (!size) { return; }const std::string name=prefix+suffix;
      std::ofstream file(launch.output/name,std::ios::binary);file.write(static_cast<const char*>(bytes),std::streamsize(size));file.close();
      if (!file) { return; }if (comma) { log << ','; }comma=true;
      log << "{\"file\":\"" << name << "\",\"size\":" << size << ",\"sha256\":\"" << ldb_rt_file::fileSha256(launch.output/name) << "\"}";
    };
    save("-vertices.bin",input.vertices.data(),input.vertices.size());save("-indices.bin",input.indices.data(),input.indices.size());
    save("-vs.bin",input.vs.data(),input.vs.size()*4);save("-ps.bin",input.ps.data(),input.ps.size()*4);
    save("-declaration.bin",input.declaration.data(),input.declaration.size()*sizeof(D3DVERTEXELEMENT9));
    if (SUCCEEDED(input.constantsResult)) { save("-constants.bin",input.constants.data(),sizeof(input.constants)); }
    if (SUCCEEDED(input.integerResult)) { save("-vs-integers.bin",input.integerConstants.data(),sizeof(input.integerConstants)); }
    if (SUCCEEDED(input.boolResult)) { save("-vs-bools.bin",input.boolConstants.data(),sizeof(input.boolConstants)); }
    if (SUCCEEDED(input.pixelResult)) { save("-ps-constants.bin",input.pixelConstants.data(),sizeof(input.pixelConstants)); }
    for (unsigned i=0;i<3;++i) { const std::string name="-stream-"+std::to_string(i+1)+".bin";save(name.c_str(),input.auxiliary[i].bytes.data(),input.auxiliary[i].bytes.size()); }

    log << "]}\n";events << log.str() << std::flush;
  } catch (...) { event("scene_input_exception",uint32_t(E_FAIL)); }
}
bool scene(IDirect3DDevice9* device,const draw_observe::Snapshot& s,uint32_t kind,D3DPRIMITIVETYPE type,
    const std::array<uint32_t,6>& args,static_draw::Report& report,bool& called,HRESULT& physical,HRESULT& result) noexcept {
  try {
    if (device!=registered || (kind!=1 && kind!=2)) { return false; }
    auto& counts=sceneCounts[size_t(comparison.phase())];++counts.attempts;
    const bool completeSky=skyReadiness.size()==6 && std::all_of(skyReadiness.begin(),skyReadiness.end(),[](const auto& p) { return p.second; });
    auto prepared=[&] {
      if (!completeSky) { scene_input::Prepared fallback;fallback.reason="sky_set_not_fully_supported";return fallback; }
      auto input=scene_input::capture(device,kind,type,args,false);return scene_input::prepare(device,s,input);
    }();
    ++counts.reasons[prepared.reason];if (prepared.ready) { ++counts.ready; }
    result=scene_input::execute(device,s,prepared,type,[&] {
      called=true;physical=kind==1?device->DrawPrimitive(type,args[0],args[1]):device->DrawIndexedPrimitive(type,INT(int32_t(args[0])),args[1],args[2],args[3],args[4]);return physical;
    },report);
    if (prepared.ready && report.converted && SUCCEEDED(report.restore) && SUCCEEDED(result)) { ++counts.reconstructed; }else { ++counts.fallback; }return true;
  } catch (...) { event("sky_rebuild_exception",uint32_t(E_FAIL));physical=E_FAIL;result=E_FAIL;report.capture=E_FAIL;report.code=static_draw::Code::CaptureFailed;return true; }
}
void sceneSummary(uint64_t frame) {
  const auto phase=comparison.phase();const auto& c=sceneCounts[size_t(phase)];
  events << "{\"event\":\"sky_rebuild_counts\",\"frame\":" << frame << ",\"phase\":\"" << world_diagnostics::name(phase)
    << "\",\"attempts\":" << c.attempts << ",\"ready\":" << c.ready << ",\"reconstructed\":" << c.reconstructed << ",\"fallback\":" << c.fallback << ",\"input_samples\":" << sceneSampleCounts[size_t(phase)] << ",\"reasons\":{";
  bool comma=false;for (const auto& pair:c.reasons) { if (comma) { events << ','; }comma=true;events << '"' << pair.first << "\":" << pair.second; }events << "}}\n" << std::flush;
}

bool overlayDraw(IDirect3DDevice9* device, const draw_observe::Snapshot& snapshot,
    D3DPRIMITIVETYPE type, bool userMemory, static_draw::Report& report, HRESULT& physical) noexcept {
  if (device!=registered || !raytracingRequested || !overlay::candidate(snapshot,type,userMemory)) { return false; }
  const auto result=overlay::inject(device,snapshot);report=result.report;
  physical=FAILED(report.restore)?report.restore:result.draw;
  events << "{\"event\":\"hud_injection\",\"phase\":\"" << world_diagnostics::name(comparison.phase())
    << "\",\"ordinal\":" << snapshot.ordinal
    << ",\"capture_hresult\":" << uint32_t(report.capture) << ",\"apply_hresult\":" << uint32_t(report.apply)
    << ",\"draw_hresult\":" << uint32_t(result.draw) << ",\"restore_hresult\":" << uint32_t(report.restore)
    << ",\"scope_code\":" << uint32_t(report.code) << ",\"replaced_post\":" << (result.replaced?"true":"false") << "}\n" << std::flush;
  return result.replaced || FAILED(report.restore);
}
void capture(IDirect3DDevice9* device, uint64_t frame) noexcept {
  try {
    if (device != registered || !events) { return; }
    sceneSummary(frame);
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
    const bool advanced = comparison.afterCapture(frame);
    events << "{\"event\":\"coverage_phase_requested\",\"code\":" << (advanced ? 0 : uint32_t(E_FAIL))
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
    event("coverage_comparison_policy", 0, "v9_world_then_v8_sky_after120_then_guarded_original_vs_sky_and_rigid_after180");
    event("world_coverage_revision", 0, "10");
    event("material_audit_policy", 0, "first_restored_state_per_material_phase_max64_COLOROP_COLORARG1_COLORARG2_ALPHAOP_ALPHAARG1_ALPHAARG2_TEXCOORDINDEX_TEXTURETRANSFORMFLAGS_RESULTARG");
    state() = {true, &bind, &capture, &raytracing, &draw, &audit, &inspect, &scene, &overlayDraw}; return module;
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
  if (events) { event("host_draw_request_count", 0, std::to_string(physicalDraws).c_str()); }
  if (events) { event("host_cleanup", 0); events.close(); }
}
} // namespace ldb_rt::world_runtime

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
#include "rt_model_input.h"
#include <map>
#include <sstream>
// A separate TU prevents mixing the Bridge's old API with the pinned 0.6.4 ABI.
#define REMIX_WINAPI_NO_LIBRARY_LOADER
#include "rt_world_remix_c.h"
#include "rt_live_mesh.h"
#include "rt_model_batch.h"
#include "rt_infected_material.h"
#include <deque>
#include <memory>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <set>

namespace ldb_rt::world_runtime {
namespace {
remixapi_Interface api{};
live_mesh::Scene::UpdateMesh updateMesh=nullptr;
draw_observe::Request launch;
std::ofstream events;
IDirect3DDevice9* registered = nullptr;
bool initialized = false;
bool raytracingRequested = false;
bool performanceProfileConfigured = false;
uint64_t physicalDraws = 0;
world_diagnostics::LiveCoverageComparison comparison;
live_mesh::Scene apiMeshes(&api);
model_texture::Cache modelTextures;
infected_material::Cache infectedMaterials;
uint64_t targetBytes=0,targetRows=0,targetFrame=0;unsigned targetFrameRows=0;
std::map<std::string,uint64_t> liveShaderSubmissions,liveShaderFallbacks;
std::set<std::pair<uintptr_t,uintptr_t>> unknownPrograms;
uint64_t unknownBytes=0,unknownFrame=0;unsigned unknownRows=0,unknownFrameRows=0;
uint64_t previewStarted=0,previewRecoverAt=0,previewFrames=0,previewRasterFrames=0,previewStartSubmitted=0,previewStartCreated=0,previewStartReused=0,previewStartUpdated=0,previewStartOmitted=0,previewStartSkinnedSubmitted=0,previewStartSkinnedCreated=0,previewStartSkinnedReused=0,previewFirstRead=0;
bool previewTaa=false;
std::map<std::string,uint64_t> sourceShaderCensus;uint64_t sourceCensusOmitted=0;
unsigned previewReadbacks=0,previewBindingRows=0,previewBindingOmitted=0;bool previewRecover=false,previewResourcesRecovered=false,previewMaintenanceFailed=false,previewReadFailed=false;
world_diagnostics::ModelStage currentModels() {
  return state().preview?(previewRecover?world_diagnostics::ModelStage::Recovery:world_diagnostics::ModelStage::Preview):comparison.models();
}
uint64_t texturedSubmissions=0,textureFallbackSubmissions=0;
struct MeshFence { static_draw::Ref<IDirect3DQuery9> query;uint64_t frame=0; };
std::deque<std::unique_ptr<MeshFence>> meshFences;
uint64_t completedMeshFrame=0,apiCameraFrame=0,apiMaintenanceFrame=0;
unsigned previousModelDraws=0;
uint64_t gpuSkinDensityThrottleFrames=0;
std::unique_ptr<model_batch::Pool> modelWorkers;
uint64_t batchFrame=0,batchFlushes=0,batchSourceDraws=0,batchSubmitted=0,batchFailures=0,batchInputFallbacks=0;
uint64_t batchFallbackFrame=0,batchFrameInputFallbacks=0;
std::vector<std::future<model_batch::Result>> batchPending;
std::array<float,16> apiView{},apiProjection{};
std::array<uint64_t,world_diagnostics::kModelStages> apiPayloadBytes{};
std::set<std::array<uint64_t, 4>> audited;
std::array<unsigned, 4> auditCounts{};
std::array<float, 16> lastView{}, lastProjection{};
bool haveWorldCamera = false;
const std::pair<const char*, const char*> options[] = {
  {"rtx.enableRaytracing", "False"}, {"rtx.useVertexCapture", "False"},
  {"rtx.useBuffersDirectly", "False"}, {"rtx.enablePreservePath", "False"},
  {"rtx.resolutionScale", "1.0"}, {"rtx.upscalerType", "0"}, {"rtx.enableRayReconstruction", "False"},
  {"rtx.sceneScale", "0.3937007874"},
  {"rtx.fallbackLightMode", "2"}, {"rtx.fallbackLightType", "1"},
  {"rtx.fallbackLightRadiance", "30,30,30"}, {"rtx.fallbackLightRadius", "5"},
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
  if (launch.performance) {
    // Keep this fixed for the complete run. Changing these options while RT is
    // active reallocates render targets and makes a device-loss result
    // ambiguous. TAAU at 0.75 traces 56.25% of the native-resolution pixels.
    const std::pair<const char*, const char*> profile[] = {
      // v35 completed the 128-frame load while Remix selected ReSTIR GI. v36
      // proved that forcing importance-sampled integration on the same Arc
      // adapter loses the Vulkan device. Keep Custom so every requested value
      // is explicit, and select the mode with surviving device evidence.
      {"rtx.graphicsPreset", "4"}, {"rtx.upscalerType", "3"}, {"rtx.resolutionScale", "0.75"},
      {"rtx.enableDirectLighting", "True"}, {"rtx.enableSecondaryBounces", "False"},
      {"rtx.useRTXDI", "False"}, {"rtx.integrateIndirectMode", "1"},
      // Source's complex programmable model shaders produced corrupt geometry
      // and 0.31 FPS when globally captured in v38. All accepted model parts
      // use the audited explicit API path in v39.
      {"rtx.useVertexCapture", "False"}
    };
    for (const auto& option : profile) {
      const auto code=api.SetConfigVariable(option.first,option.second);event(option.first,uint32_t(code),option.second);
      if(code!=REMIXAPI_ERROR_CODE_SUCCESS) { return false; }
    }
    performanceProfileConfigured=true;
  }
  return true;
}
void bind(IDirect3DDevice9* device) noexcept {
  try {
    if (!initialized || registered == device || registered) { return; }
    const auto result = api.dxvk_RegisterD3D9Device(static_cast<IDirect3DDevice9Ex*>(device));
    event("register_game_device", uint32_t(result));
    if (result == REMIXAPI_ERROR_CODE_SUCCESS) { registered = device;
      if (!configure()) { state().active=false;return; }
      raytracingRequested=false;event("post_device_configuration_requested",0,"startup_RT_off_after_layers_and_device");
    }
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
  if(state().preview) { return; }
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
std::array<std::array<unsigned,5>,4> sceneFamilyCounts{};
std::set<std::array<uint64_t,16>> sceneSampled;
constexpr unsigned sceneLimits[]={6,48,96,48,24};
constexpr uint64_t sceneByteLimits[]={3u*1024*1024,3u*1024*1024,3u*1024*1024,16u*1024*1024};
std::array<uint64_t,4> sceneBytes{};
constexpr uint64_t sceneFamilyByteLimits[][5]={{128u*1024,640u*1024,512u*1024,768u*1024,512u*1024},{128u*1024,640u*1024,512u*1024,768u*1024,512u*1024},{128u*1024,640u*1024,512u*1024,768u*1024,512u*1024},{256u*1024,4096u*1024,1792u*1024,6144u*1024,4096u*1024}};
std::array<std::array<uint64_t,5>,4> sceneFamilyBytes{};
std::array<std::array<unsigned,5>,4> sceneQuotaOmitted{},scenePayloadOmitted{};
std::array<unsigned,4> sceneIdentityFailures{};
std::map<uint32_t,bool> skyReadiness;
void inspect(IDirect3DDevice9* device,const draw_observe::Snapshot& s,uint32_t kind,D3DPRIMITIVETYPE type,const std::array<uint32_t,6>& args) noexcept {
  try {
    if (!events || device!=registered) { return; }
    if(sourceShaderCensus.size()<128||sourceShaderCensus.count(s.materialInfo.shader.data())) { ++sourceShaderCensus[s.materialInfo.shader.data()]; }else { ++sourceCensusOmitted; }
    if(state().preview) { return; }
    const bool sky=scene_draw::candidate(s,true,false)==scene_draw::Mode::Sky;
    const bool infected=!std::strcmp(s.materialInfo.shader.data(),"Infected");
    if (!sky && !infected && !bulk_static::candidate(s)) { return; }
    const auto phase=comparison.phase();const auto index=size_t(phase);
    const unsigned group=infected?4u:sky?0u:bulk_static::customModel(s)?3u:bulk_static::family(s.materialInfo.shader.data())==1?1u:2u;
    static_draw::Ref<IDirect3DVertexShader9> identityShader;static_draw::Ref<IDirect3DVertexBuffer9> identityBuffer;UINT identityOffset=0,identityStride=0;
    const auto shaderIdentity=device->GetVertexShader(&identityShader.value),bufferIdentity=device->GetStreamSource(0,&identityBuffer.value,&identityOffset,&identityStride);
    if (FAILED(shaderIdentity) || FAILED(bufferIdentity)) {
      if (!sceneIdentityFailures[index]++) { event("scene_input_identity_failed",uint32_t(FAILED(shaderIdentity)?shaderIdentity:bufferIdentity)); }return;
    }
    std::array<uint64_t,16> key{{uint64_t(phase),s.material,s.materialTable,s.materialInfo.format,
      uint64_t(reinterpret_cast<uintptr_t>(identityShader.value)),uint64_t(reinterpret_cast<uintptr_t>(identityBuffer.value)),identityOffset,identityStride,kind,uint32_t(type)}};
    for (unsigned i=0;i<6;++i) { key[10+i]=args[i]; }
    if (!sceneSampled.insert(key).second) { return; }
    if (sceneFamilyCounts[index][group]>=sceneLimits[group]) { ++sceneQuotaOmitted[index][group];return; }
    auto input=scene_input::capture(device,kind,type,args,true,group==1?scene_input::kModelLimits:group>=3?scene_input::kAuditLimits:scene_input::Limits{},true);
    const char* replay=group>=3?"model_input_audit":"not_sky";
    if (sky) {
      auto warmed=scene_input::prepare(device,s,input);replay=warmed.reason;
      if (phase==world_diagnostics::CoveragePhase::BaselineWorld) { skyReadiness[s.material]=warmed.ready; }
    }
    const uint64_t bytes=input.vertices.size()+input.indices.size()+(input.vs.size()+input.ps.size())*4+input.declaration.size()*sizeof(D3DVERTEXELEMENT9)+
      (SUCCEEDED(input.constantsResult)?sizeof(input.constants):0)+(SUCCEEDED(input.integerResult)?sizeof(input.integerConstants):0)+
      (SUCCEEDED(input.boolResult)?sizeof(input.boolConstants):0)+(SUCCEEDED(input.pixelResult)?sizeof(input.pixelConstants):0)+
      input.auxiliary[0].bytes.size()+input.auxiliary[1].bytes.size()+input.auxiliary[2].bytes.size()+input.morph.positions.size()*sizeof(sky_shader::Vec);
    if (bytes>sceneByteLimits[index]-sceneBytes[index] || bytes>sceneFamilyByteLimits[index][group]-sceneFamilyBytes[index][group]) { ++scenePayloadOmitted[index][group];return; }
    ++sceneFamilyCounts[index][group];++sceneSampleCounts[index];
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
      << ",\"geometry_window\":true,\"source_vertex_buffer_bytes\":" << input.vertexBufferBytes << ",\"stream_offset\":" << input.offset << ",\"index_bytes\":" << input.indexBytes << ",";
    log << "\"morph_position\":{\"hresult\":" << uint32_t(input.morph.result) << ",\"stream\":" << input.morph.stream
      << ",\"offset\":" << input.morph.offset << ",\"stride\":" << input.morph.stride << ",\"frequency\":" << input.morph.frequency
      << ",\"source_buffer_bytes\":" << input.morph.bufferBytes << ",\"first_absolute_vertex\":" << input.first
      << ",\"vertices\":" << input.morph.positions.size() << "},";
    // Prefix-only auxiliary streams are evidence, never claimed as complete vertices.
    log << "\"auxiliary\":[";
    for (unsigned i=0;i<3;++i) { const auto& a=input.auxiliary[i];if (i) { log << ','; }
      log << "{\"stream\":" << a.stream << ",\"offset\":" << a.offset << ",\"stride\":" << a.stride << ",\"frequency\":" << a.frequency << ",\"hresult\":" << uint32_t(a.result) << ",\"prefix_bytes\":" << a.bytes.size() << '}'; }
    log << "],\"render_states\":[";
    const D3DRENDERSTATETYPE states[]={D3DRS_ZENABLE,D3DRS_ZWRITEENABLE,D3DRS_ALPHATESTENABLE,D3DRS_ALPHABLENDENABLE,D3DRS_SRCBLEND,D3DRS_DESTBLEND,D3DRS_BLENDOP,D3DRS_SEPARATEALPHABLENDENABLE,D3DRS_STENCILENABLE,D3DRS_CLIPPLANEENABLE,D3DRS_COLORWRITEENABLE,D3DRS_ZFUNC};
    bool stateComma=false;for(const auto state:states) { DWORD value=0;const auto hr=device->GetRenderState(state,&value);if(stateComma) { log<<','; }stateComma=true;
      log<<"{\"state\":"<<unsigned(state)<<",\"value\":"<<value<<",\"hresult\":"<<uint32_t(hr)<<'}'; }
    log << "],\"integer_constants_hresult\":" << uint32_t(input.integerResult) << ",\"bool_constants_hresult\":" << uint32_t(input.boolResult) << ",\"pixel_constants_hresult\":" << uint32_t(input.pixelResult) << ",\"files\":[";
    bool comma=false;
    const auto save=[&](const char* suffix,const void* bytes,size_t size) {
      if (!size) { return; }const std::string name=prefix+suffix;
      std::ofstream file(launch.output/name,std::ios::binary);file.write(static_cast<const char*>(bytes),std::streamsize(size));file.close();
      if (!file) { return; }sceneBytes[index]+=size;sceneFamilyBytes[index][group]+=size;if (comma) { log << ','; }comma=true;
      log << "{\"file\":\"" << name << "\",\"size\":" << size << ",\"sha256\":\"" << ldb_rt_file::fileSha256(launch.output/name) << "\"}";
    };
    save("-vertices.bin",input.vertices.data(),input.vertices.size());save("-indices.bin",input.indices.data(),input.indices.size());
    save("-vs.bin",input.vs.data(),input.vs.size()*4);save("-ps.bin",input.ps.data(),input.ps.size()*4);
    save("-declaration.bin",input.declaration.data(),input.declaration.size()*sizeof(D3DVERTEXELEMENT9));
    if (SUCCEEDED(input.constantsResult)) { save("-constants.bin",input.constants.data(),sizeof(input.constants)); }
    if (SUCCEEDED(input.integerResult)) { save("-vs-integers.bin",input.integerConstants.data(),sizeof(input.integerConstants)); }
    if (SUCCEEDED(input.boolResult)) { save("-vs-bools.bin",input.boolConstants.data(),sizeof(input.boolConstants)); }
    if (SUCCEEDED(input.pixelResult)) { save("-ps-constants.bin",input.pixelConstants.data(),sizeof(input.pixelConstants)); }
    if (!input.morph.positions.empty()) { save("-morph-positions.bin",input.morph.positions.data(),input.morph.positions.size()*sizeof(sky_shader::Vec)); }
    for (unsigned i=0;i<3;++i) { const std::string name="-stream-"+std::to_string(i+1)+".bin";save(name.c_str(),input.auxiliary[i].bytes.data(),input.auxiliary[i].bytes.size()); }

    log << "]}\n";events << log.str() << std::flush;
  } catch (...) { event("scene_input_exception",uint32_t(E_FAIL)); }
}
bool scene(IDirect3DDevice9* device,const draw_observe::Snapshot& s,uint32_t kind,D3DPRIMITIVETYPE type,
    const std::array<uint32_t,6>& args,static_draw::Report& report,bool& called,HRESULT& physical,HRESULT& result) noexcept {
  try {
    runtime_profile::Scope timing(runtime_profile::Stage::Scene);
    if (device!=registered || (kind!=1 && kind!=2)) { return false; }
    auto& counts=sceneCounts[size_t(comparison.phase())];++counts.attempts;
    const bool completeSky=skyReadiness.size()==6 && std::all_of(skyReadiness.begin(),skyReadiness.end(),[](const auto& p) { return p.second; });
    auto prepared=[&] {
      if (!completeSky) { scene_input::Prepared fallback;fallback.reason="sky_set_not_fully_supported";return fallback; }
      auto input=scene_input::capture(device,kind,type,args,false);return scene_input::prepare(device,s,input);
    }();
    ++counts.reasons[prepared.reason];if (prepared.ready) { ++counts.ready; }
    result=scene_input::execute(device,s,prepared,type,[&] {
      { runtime_profile::Scope originalTiming(runtime_profile::Stage::OriginalDraw);called=true;physical=kind==1?device->DrawPrimitive(type,args[0],args[1]):device->DrawIndexedPrimitive(type,INT(int32_t(args[0])),args[1],args[2],args[3],args[4]); }return physical;
    },report);
    if (prepared.ready && report.converted && SUCCEEDED(report.restore) && SUCCEEDED(result)) { ++counts.reconstructed; }else { ++counts.fallback; }return true;
  } catch (...) { event("sky_rebuild_exception",uint32_t(E_FAIL));physical=E_FAIL;result=E_FAIL;report.capture=E_FAIL;report.code=static_draw::Code::CaptureFailed;return true; }
}
std::array<SceneCounts,4> modelCounts{};
constexpr uint64_t kBetaPreviewDurationMs=5ull*60*1000;
constexpr uint64_t kPerformancePreviewFrames=world_diagnostics::kGpuSkinPerformanceFrames;
const char* performanceSegment(uint64_t completedFrames) {
  static constexpr const char* names[]={"explicit_oracle_80","frame_batch_warm","persistent_batch_update","gpu_lifecycle_raster"};
  return names[world_diagnostics::performanceGpuSkinPhase(completedFrames)];
}
std::array<unsigned,world_diagnostics::kModelStages> modelSampleCounts{},modelStateCounts{},modelSampleOmitted{};
std::array<unsigned,world_diagnostics::kModelStages> modelPreviousSampleCounts{},modelPreviousSampleOmitted{};
void modelState(IDirect3DDevice9* device,const draw_observe::Snapshot& s) {
  const auto stage=unsigned(currentModels());
  if (modelStateCounts[stage]>=world_diagnostics::kModelSampleLimit) { return; }++modelStateCounts[stage];
  const unsigned names[]={7,14,15,23,24,25,27,28,171,206,22,42,52,137,149,150,152,168,169,174,175,177,195,196,197,206,210,211,212,213,214,215,216,217,218,219,220};
  events << "{\"event\":\"model_state_sample\",\"ordinal\":" << s.ordinal << ",\"material\":" << s.material
    << ",\"frame\":" << state().presents+1 << ",\"model_stage\":\"" << world_diagnostics::texturedName(currentModels()) << "\",\"shader\":\"" << s.materialInfo.shader.data() << "\",\"states\":[";
  for (unsigned i=0;i<sizeof(names)/sizeof(names[0]);++i) {
    DWORD value=0;const auto hr=device->GetRenderState(D3DRENDERSTATETYPE(names[i]),&value);
    if (i) { events << ','; }events << "{\"state\":" << names[i] << ",\"value\":" << value << ",\"hresult\":" << uint32_t(hr) << '}';
  }
  events << "]}\n" << std::flush;
}
live_mesh::Material liveMaterial(world_diagnostics::ModelStage stage) {
  // v23 uses neutral opaque base materials throughout. Synthetic metallic,
  // translucent and emissive probes obscured the actual clothing comparison.
  (void)stage;return live_mesh::Material::Neutral;
}

void apiPayload(const draw_observe::Snapshot& s,const scene_input::Prepared& p,uint32_t kind,D3DPRIMITIVETYPE topology,DWORD cull,
                const live_mesh::Submission& submission,DWORD ignored) {
  const uint64_t frame=state().presents+1;const bool previous=frame==239||frame==299;
  if(!previous&&!world_diagnostics::readback(frame)) { return; }
  const auto stage=size_t(currentModels());auto& count=(previous?modelPreviousSampleCounts:modelSampleCounts)[stage];
  if(count>=world_diagnostics::kModelSampleLimit) { ++(previous?modelPreviousSampleOmitted:modelSampleOmitted)[stage];return; }
  ++count;const auto vh=ldb_rt_file::geometrySha256(p.bytes.data(),p.bytes.size()),ih=ldb_rt_file::geometrySha256(p.indices.data(),p.indices.size());
  const bool save=(previous||frame==240||frame==300) && p.bytes.size()+p.indices.size()<=(previous?8ull:16ull)*1024*1024-apiPayloadBytes[stage];
  events << "{\"event\":\"" << (previous?"api_model_previous_sample":"api_model_sample") << "\",\"frame\":" << frame << ",\"ordinal\":" << s.ordinal
    << ",\"stage\":\"" << world_diagnostics::texturedName(currentModels()) << "\",\"shader\":\"" << s.materialInfo.shader.data()
    << "\",\"material\":\"" << (std::strcmp(s.materialInfo.shader.data(),"Infected")?live_mesh::name(liveMaterial(currentModels())):"neutral") << "\",\"kind\":" << kind << ",\"topology\":" << uint32_t(topology)
    << ",\"source_material\":" << s.material << ",\"source_vs_reason\":\"" << p.reason << "\",\"vertices\":" << p.bytes.size()/32 << ",\"triangles\":" << submission.triangles << ",\"index_bytes\":" << p.indexBytes
    << ",\"mesh\":" << submission.handle << ",\"created\":" << (submission.created?"true":"false") << ",\"skinned\":" << (submission.skinned?"true":"false")
    << ",\"bones_per_vertex\":" << p.skinning.bonesPerVertex << ",\"bone_transforms\":" << p.skinning.transforms.size()
    << ",\"textured\":" << (submission.textured?"true":"false") << ",\"material_handle\":" << submission.materialHandle << ",\"cull\":" << cull
    << ",\"instance_cull\":" << ((!std::strcmp(s.materialInfo.shader.data(),"Infected")&&currentModels()==world_diagnostics::ModelStage::Combined)?DWORD(D3DCULL_NONE):cull)
    << ",\"ignored_category_at_original_draw\":" << ignored << ",\"vertices_sha256\":\"" << vh << "\",\"indices_sha256\":\"" << ih
    << "\",\"gpu_upload\":\"pinned_CreateMesh_synchronous_copy_queued_owned_buffers\",\"world\":[";
  for(unsigned i=0;i<16;++i) { if(i) { events<<','; }events<<p.world[i]; }events<<"],\"view\":[";
  for(unsigned i=0;i<16;++i) { if(i) { events<<','; }events<<s.view[i]; }events<<"],\"projection\":[";
  for(unsigned i=0;i<16;++i) { if(i) { events<<','; }events<<s.projection[i]; }events<<"],\"files\":[";
  bool comma=false;
  const auto write=[&](const char* suffix,const std::vector<uint8_t>& bytes,const std::string& digest) {
    if(!save||bytes.empty()) { return; }const auto name="api-"+std::to_string(s.ordinal)+suffix;
    std::ofstream output(launch.output/name,std::ios::binary);output.write(reinterpret_cast<const char*>(bytes.data()),std::streamsize(bytes.size()));output.close();
    if(!output) { throw std::runtime_error("write API model payload"); }apiPayloadBytes[stage]+=bytes.size();
    if(comma) { events<<','; }comma=true;events<<"{\"file\":\""<<name<<"\",\"size\":"<<bytes.size()<<",\"sha256\":\""<<digest<<"\"}";
  };
  write("-vertices.bin",p.bytes,vh);write("-indices.bin",p.indices,ih);events<<"],\"payload_saved\":"<<(save?"true":"false")<<"}\n"<<std::flush;
}
void textureBinding(const model_texture::Source& source,const live_mesh::Submission& submission,uint64_t frame,const draw_observe::Snapshot& snapshot) {
  if(!submission.materialUpdated) { return; }
  if(state().preview&&previewBindingRows++>=2048) { ++previewBindingOmitted;return; }
  events << "{\"event\":\"model_texture_binding\",\"frame\":" << frame << ",\"material_handle\":" << submission.materialHandle
    << ",\"source_material\":" << snapshot.material << ",\"source_shader\":\"" << snapshot.materialInfo.shader.data() << "\",\"textured\":" << (source.textured()?"true":"false") << ",\"reason\":\"" << source.reason
    << "\",\"file\":\"" << source.file << "\",\"sha256\":\"" << source.digest << "\",\"width\":" << source.width
    << ",\"height\":" << source.height << ",\"format\":" << source.format << ",\"srgb\":" << (source.srgb?"true":"false")
    << ",\"evidence_saved\":" << (source.evidenceSaved?"true":"false") << ",\"mip_levels\":" << source.levels << ",\"full_mip_levels\":" << source.fullMipLevels
    << ",\"alpha_blend\":" << (source.alphaBlend?"true":"false") << ",\"alpha_test\":" << unsigned(source.alphaTest) << ",\"alpha_ref\":" << unsigned(source.alphaRef)
    << ",\"filter\":" << unsigned(source.filter) << ",\"wrap_u\":" << unsigned(source.wrapU) << ",\"wrap_v\":" << unsigned(source.wrapV)
    << ",\"material\":\"" << (source.reason.rfind("infected_",0)==0?"neutral":live_mesh::name(liveMaterial(currentModels()))) << "\"}\n" << std::flush;
}
bool pollMeshCompletion() {
    while(!meshFences.empty()) {
      const auto hr=meshFences.front()->query.value->GetData(nullptr,0,D3DGETDATA_FLUSH);
      if(hr==S_FALSE) { break; }if(FAILED(hr)) { event("api_mesh_fence_failed",uint32_t(hr));(void)raytracing(false);state().active=false;return false; }
      completedMeshFrame=meshFences.front()->frame;meshFences.pop_front();
    }
    return true;
}
void submissionEvent(const draw_observe::Snapshot& s,const live_mesh::Submission& submission,uint64_t frame) {
  if(state().preview&&submission.deferred) { return; }
  events << "{\"event\":\"" << (submission.deferred?"api_model_budget_omitted":"api_model_submit_failed")
    << "\",\"code\":" << submission.code << ",\"frame\":" << frame << ",\"ordinal\":" << s.ordinal
    << ",\"operation\":\"" << submission.operation << "\",\"reason\":\"" << live_mesh::name(submission.budget)
    << "\",\"live_bytes\":" << apiMeshes.bytes << ",\"entries\":" << apiMeshes.entries()
    << ",\"frame_draws\":" << apiMeshes.frameDraws << ",\"frame_new_bytes\":" << apiMeshes.frameBytes
    << ",\"pending_fences\":" << meshFences.size() << "}\n" << std::flush;
}
bool frameBatchActive() noexcept {
  return launch.performance&&state().preview&&!previewRecover&&world_diagnostics::performanceFrameBatch(previewFrames);
}
bool flushModelBatch(uint64_t frame) {
  if(batchPending.empty()) { return true; }
  if(!modelWorkers||batchFrame!=frame) {
    event("frame_batch_identity_failed",ERROR_INVALID_DATA);++batchFailures;return false;
  }
  std::vector<model_batch::Result> results;results.reserve(batchPending.size());
  {
    runtime_profile::Scope timing(runtime_profile::Stage::BatchWait);
    for(auto& future:batchPending) { results.push_back(future.get()); }
  }
  batchPending.clear();
  for(const auto& result:results) {
    if(!result.success) {
      events<<"{\"event\":\"frame_batch_job_failed\",\"frame\":"<<frame<<",\"ordinal\":"<<result.ordinal
        <<",\"shader\":\""<<result.shader<<"\",\"reason\":\""<<result.reason<<"\"}\n"<<std::flush;
      ++batchFailures;return false;
    }
  }
  model_batch::Assembly assembly;
  { runtime_profile::Scope timing(runtime_profile::Stage::BatchAssemble);assembly=model_batch::assemble(std::move(results)); }
  uint64_t submittedDraws=0;size_t submittedBatches=0,persistentCreated=0,persistentUpdated=0;
  {
    runtime_profile::Scope timing(runtime_profile::Stage::BatchSubmit);
    for(const auto& batch:assembly.batches) {
      const auto persistent=world_diagnostics::performancePersistentBatch(previewFrames)&&batch.persistent;
      const auto submission=apiMeshes.drawBatch(batch.geometry,batch.world,batch.cull,batch.variant,frame,meshFences.size(),batch.material,
        completedMeshFrame,persistent?batch.persistentKey:std::string{});
      if(!submission.success) {
        events<<"{\"event\":\"frame_batch_submit_failed\",\"frame\":"<<frame<<",\"code\":"<<submission.code
          <<",\"operation\":\""<<submission.operation<<"\",\"reason\":\""<<live_mesh::name(submission.budget)<<"\"}\n"<<std::flush;
        ++batchFailures;return false;
      }
      submittedDraws+=batch.sourceDraws;++submittedBatches;
      if(persistent&&submission.created) { ++persistentCreated; }
      if(persistent&&submission.updated) { ++persistentUpdated; }
      if(batch.material.textured()) { texturedSubmissions+=batch.sourceDraws; }else { textureFallbackSubmissions+=batch.sourceDraws; }
    }
  }
  const auto stats=modelWorkers->statistics();LARGE_INTEGER frequency{};QueryPerformanceFrequency(&frequency);
  ++batchFlushes;batchSourceDraws+=assembly.sourceDraws;batchSubmitted+=submittedDraws;
  events<<"{\"event\":\"frame_batch_flush\",\"frame\":"<<frame<<",\"source_draws\":"<<assembly.sourceDraws
    <<",\"source_vertices\":"<<assembly.sourceVertices<<",\"source_indices\":"<<assembly.sourceIndices
    <<",\"batches\":"<<submittedBatches<<",\"submitted_source_draws\":"<<submittedDraws
    <<",\"persistent_phase\":"<<(world_diagnostics::performancePersistentBatch(previewFrames)?"true":"false")
    <<",\"persistent_created\":"<<persistentCreated<<",\"persistent_updated\":"<<persistentUpdated
    <<",\"input_fallbacks\":"<<(batchFallbackFrame==frame?batchFrameInputFallbacks:0)
    <<",\"worker_submitted\":"<<stats.submitted<<",\"worker_completed\":"<<stats.completed<<",\"worker_failed\":"<<stats.failed
    <<",\"worker_threads\":"<<stats.workers<<",\"worker_max_active\":"<<stats.maxActive
    <<",\"worker_cpu_elapsed_ms\":"<<(frequency.QuadPart?double(stats.workerTicks)*1000.0/double(frequency.QuadPart):0.0)<<"}\n"<<std::flush;
  batchFrame=0;batchFallbackFrame=0;batchFrameInputFallbacks=0;
  return assembly.sourceDraws&&submittedDraws==assembly.sourceDraws;
}
void presented(IDirect3DDevice9* device,uint64_t frame) noexcept {
  try {
    if(!batchPending.empty()) {
      event("frame_batch_missed_injection",ERROR_INVALID_DATA);++batchFailures;previewMaintenanceFailed=true;(void)raytracing(false);state().active=false;return;
    }
    const auto gpuSkinDensityInput=previousModelDraws;
    if(device!=registered||!pollMeshCompletion()) { if(state().preview) { previewMaintenanceFailed=true; }return; }
    const auto retired=apiMeshes.retire(completedMeshFrame,frame,currentModels()==world_diagnostics::ModelStage::Recovery);
    if(retired) { event("api_mesh_retire_failed",retired);previewMaintenanceFailed=true;(void)raytracing(false);state().active=false;return; }
    if(apiMeshes.frame==frame&&apiMeshes.frameDraws) {
      if(meshFences.size()>=8) { event("api_mesh_fence_budget",ERROR_TIMEOUT);previewMaintenanceFailed=true;(void)raytracing(false);state().active=false;return; }
      auto fence=std::make_unique<MeshFence>();fence->frame=frame;
      auto hr=device->CreateQuery(D3DQUERYTYPE_EVENT,&fence->query.value);
      if(SUCCEEDED(hr)&&fence->query.value) { hr=fence->query.value->Issue(D3DISSUE_END); }else if(SUCCEEDED(hr)) { hr=E_FAIL; }
      if(FAILED(hr)) { event("api_mesh_fence_failed",uint32_t(hr));previewMaintenanceFailed=true;(void)raytracing(false);state().active=false;return; }
      meshFences.push_back(std::move(fence));
    }
    if(currentModels()==world_diagnostics::ModelStage::Recovery&&!apiMeshes.entries()) { modelTextures.clear(); }
    const bool reportFrame=launch.performance&&state().preview?world_diagnostics::performanceGpuSkinReport(previewFrames):frame%60==0;
    if(reportFrame) {
      runtime_profile::emit(events,frame,launch.performance&&state().preview?performanceSegment(previewFrames-1):state().preview?(previewTaa?"preview_native_taa":"preview_no_aa"):"diagnostic");
      events<<"{\"event\":\"source_shader_census\",\"frame\":"<<frame<<",\"omitted_draws\":"<<sourceCensusOmitted<<",\"counts\":{";
      bool comma=false;for(const auto& row:sourceShaderCensus) {
        // Shader names come from decoded fixed fields; retain only JSON-safe tokens.
        if(row.first.empty()||row.first.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-")!=std::string::npos) { continue; }
        if(comma) { events<<','; }comma=true;events<<'"'<<row.first<<"\":"<<row.second;
      }events<<"}}\n";
    }
    const auto currentModelDraws=apiMeshes.frame==frame?apiMeshes.frameDraws:0;
    previousModelDraws=currentModelDraws;
    if(state().preview&&!reportFrame) { return; }
    const auto effectiveGpuSkinLimit=launch.performance?world_diagnostics::performanceGpuSkinLimit(previewFrames?previewFrames-1:0):world_diagnostics::persistentGpuSkinLimit(gpuSkinDensityInput);
    events << "{\"event\":\"api_mesh_frame\",\"frame\":" << frame << ",\"stage\":\"" << world_diagnostics::texturedName(currentModels())
      << "\",\"submitted\":" << apiMeshes.submitted << ",\"created\":" << apiMeshes.created << ",\"reused\":" << apiMeshes.reused
      << ",\"skinned_submitted\":" << apiMeshes.skinnedSubmitted << ",\"skinned_created\":" << apiMeshes.skinnedCreated << ",\"skinned_reused\":" << apiMeshes.skinnedReused
      << ",\"preview_submitted\":" << (launch.performance?apiMeshes.submitted-previewStartSubmitted:0)
      << ",\"preview_skinned_submitted\":" << (launch.performance?apiMeshes.skinnedSubmitted-previewStartSkinnedSubmitted:0)
      << ",\"preview_skinned_created\":" << (launch.performance?apiMeshes.skinnedCreated-previewStartSkinnedCreated:0)
      << ",\"preview_skinned_reused\":" << (launch.performance?apiMeshes.skinnedReused-previewStartSkinnedReused:0)
      << ",\"skinned_frame_submitted\":" << apiMeshes.frameSkinnedDraws << ",\"skinned_frame_limit\":" << effectiveGpuSkinLimit
      << ",\"skinned_frame_unique_meshes\":" << apiMeshes.frameSkinnedUniqueMeshes << ",\"skinned_frame_repeat_draws\":" << apiMeshes.frameSkinnedRepeatDraws
      << ",\"skinned_frame_max\":" << (launch.performance?world_diagnostics::kGpuSkinPerformanceLimit:world_diagnostics::kGpuSkinSubmissionsPerFrame)
      << ",\"gpu_skin_performance_phase\":" << (launch.performance?world_diagnostics::performanceGpuSkinPhase(previewFrames?previewFrames-1:0):-1)
      << ",\"gpu_skin_previous_frame_draws\":" << gpuSkinDensityInput << ",\"gpu_skin_density_limit\":" << world_diagnostics::kGpuSkinModelDensityLimit
      << ",\"gpu_skin_high_density_limit\":" << world_diagnostics::kGpuSkinHighDensitySubmissionsPerFrame
      << ",\"gpu_skin_only_omitted\":0"
      << ",\"preview_cpu_pose_submitted\":" << (launch.performance?(apiMeshes.submitted-previewStartSubmitted)-(apiMeshes.skinnedSubmitted-previewStartSkinnedSubmitted):0)
      << ",\"preview_cpu_pose_created\":" << (launch.performance?(apiMeshes.created-previewStartCreated)-(apiMeshes.skinnedCreated-previewStartSkinnedCreated):0)
      << ",\"preview_cpu_pose_reused\":" << (launch.performance?(apiMeshes.reused-previewStartReused)-(apiMeshes.skinnedReused-previewStartSkinnedReused):0)
      << ",\"gpu_skin_density_throttle_frames\":" << gpuSkinDensityThrottleFrames
      << ",\"destroyed\":" << apiMeshes.destroyed << ",\"entries\":" << apiMeshes.entries() << ",\"bytes\":" << apiMeshes.bytes
      << ",\"material_updates\":" << apiMeshes.materialUpdates << ",\"textured_submissions\":" << texturedSubmissions << ",\"texture_fallback_submissions\":" << textureFallbackSubmissions << ",\"texture_exports\":" << modelTextures.exported << ",\"texture_bytes\":" << modelTextures.bytes << ",\"texture_held\":" << modelTextures.held() << ",\"texture_read_failures\":" << modelTextures.failed << ",\"texture_budget_deferred\":" << modelTextures.deferred
      << ",\"infected_bakes\":" << infectedMaterials.baked << ",\"infected_bake_deferred\":" << infectedMaterials.deferred << ",\"infected_bake_failures\":" << infectedMaterials.failed
      << ",\"texture_rejected_entries\":"<<modelTextures.rejectedCount()<<",\"texture_retry_suppressed\":"<<modelTextures.retrySuppressed
      << ",\"texture_backing_entries\":"<<modelTextures.backingCount()<<",\"texture_backing_hits\":"<<modelTextures.backingHits<<",\"texture_source_reads\":"<<modelTextures.sourceReads<<",\"texture_invalidated\":"<<modelTextures.invalidated
      << ",\"texture_resident_bytes\":" << modelTextures.residentBytes << ",\"texture_evidence_bytes\":" << modelTextures.evidenceBytes << ",\"texture_evidence_files\":" << modelTextures.evidenceFiles << ",\"texture_evicted\":" << modelTextures.evicted
      << ",\"peak_bytes\":" << apiMeshes.peakBytes << ",\"completed_through\":" << completedMeshFrame << ",\"pending_fences\":" << meshFences.size()
      << ",\"frame_draws\":" << (apiMeshes.frame==frame?apiMeshes.frameDraws:0)
      << ",\"frame_new_bytes\":" << (apiMeshes.frame==frame?apiMeshes.frameBytes:0)
      << ",\"frame_omitted\":" << (apiMeshes.frame==frame?apiMeshes.frameOmitted:0) << ",\"omitted_total\":" << apiMeshes.omitted << ",\"omission_reasons\":{";
    for(unsigned i=1;i<unsigned(live_mesh::Budget::Count);++i) {
      if(i>1) { events<<','; }events << '"' << live_mesh::name(static_cast<live_mesh::Budget>(i)) << "\":" << apiMeshes.omissionTotals[i];
    }
    events << "}}\n" << std::flush;
  } catch(...) { event("api_mesh_present_exception",uint32_t(E_FAIL));previewMaintenanceFailed=true;(void)raytracing(false);state().active=false; }
}
void unknownEvidence(IDirect3DDevice9* device,const draw_observe::Snapshot& s,uint32_t kind,D3DPRIMITIVETYPE type,const std::array<uint32_t,6>& args,const char* reason,uint64_t frame) {
  if(frame%60||unknownRows>=32||unknownPrograms.size()>=256||!std::strcmp(reason,"api_model_stage_excluded")) { return; }
  if(unknownFrame!=frame) { unknownFrame=frame;unknownFrameRows=0; }if(unknownFrameRows>=4) { return; }
  static_draw::Ref<IDirect3DVertexShader9> vs;static_draw::Ref<IDirect3DPixelShader9> ps;
  if(FAILED(device->GetVertexShader(&vs.value))||FAILED(device->GetPixelShader(&ps.value))) { return; }
  if(!unknownPrograms.emplace(reinterpret_cast<uintptr_t>(vs.value),reinterpret_cast<uintptr_t>(ps.value)).second) { return; }
  ++unknownFrameRows;const auto phase=comparison.phase();
  const auto input=scene_input::capture(device,kind,type,args,true,scene_input::kAuditLimits,true);
    const uint64_t bytes=input.vertices.size()+input.indices.size()+(input.vs.size()+input.ps.size())*4+input.declaration.size()*sizeof(D3DVERTEXELEMENT9)+
      (SUCCEEDED(input.constantsResult)?sizeof(input.constants):0)+(SUCCEEDED(input.integerResult)?sizeof(input.integerConstants):0)+
      (SUCCEEDED(input.boolResult)?sizeof(input.boolConstants):0)+(SUCCEEDED(input.pixelResult)?sizeof(input.pixelConstants):0)+
      input.auxiliary[0].bytes.size()+input.auxiliary[1].bytes.size()+input.auxiliary[2].bytes.size()+input.morph.positions.size()*sizeof(sky_shader::Vec);
    if(bytes>16ull*1024*1024-unknownBytes) { return; }
    ++unknownRows;
    std::ostringstream log;log << std::setprecision(9);
    const std::string prefix="unknown-"+std::to_string(s.ordinal);
    log << "{\"event\":\"unknown_model_input\",\"phase\":\"" << world_diagnostics::name(phase) << "\",\"ordinal\":" << s.ordinal
      << ",\"material\":" << s.material << ",\"table\":" << s.materialTable << ",\"shader\":\"" << s.materialInfo.shader.data()
      << "\",\"format\":" << s.materialInfo.format << ",\"kind\":" << kind << ",\"topology\":" << uint32_t(type) << ",\"args\":[";
    for (unsigned i=0;i<6;++i) { if (i) { log << ','; }log << args[i]; }
    log << "],\"world\":[";for (unsigned i=0;i<16;++i) { if (i) { log << ','; }log << s.world[i]; }
    log << "],\"view\":[";for (unsigned i=0;i<16;++i) { if (i) { log << ','; }log << s.view[i]; }
    log << "],\"projection\":[";for (unsigned i=0;i<16;++i) { if (i) { log << ','; }log << s.projection[i]; }
    log << "],\"geometry_hresult\":" << uint32_t(input.geometryResult) << ",\"shader_hresult\":" << uint32_t(input.shaderResult)
      << ",\"constants_hresult\":" << uint32_t(input.constantsResult) << ",\"geometry_status\":\"" << input.reason
      << "\",\"replay_status\":\"" << reason << "\",\"first_absolute_vertex\":" << input.first << ",\"vertex_count\":" << input.count << ",\"stride\":" << input.stride
      << ",\"geometry_window\":true,\"source_vertex_buffer_bytes\":" << input.vertexBufferBytes << ",\"stream_offset\":" << input.offset << ",\"index_bytes\":" << input.indexBytes << ",";
    log << "\"morph_position\":{\"hresult\":" << uint32_t(input.morph.result) << ",\"stream\":" << input.morph.stream
      << ",\"offset\":" << input.morph.offset << ",\"stride\":" << input.morph.stride << ",\"frequency\":" << input.morph.frequency
      << ",\"source_buffer_bytes\":" << input.morph.bufferBytes << ",\"first_absolute_vertex\":" << input.first
      << ",\"vertices\":" << input.morph.positions.size() << "},";
    // Prefix-only auxiliary streams are evidence, never claimed as complete vertices.
    log << "\"auxiliary\":[";
    for (unsigned i=0;i<3;++i) { const auto& a=input.auxiliary[i];if (i) { log << ','; }
      log << "{\"stream\":" << a.stream << ",\"offset\":" << a.offset << ",\"stride\":" << a.stride << ",\"frequency\":" << a.frequency << ",\"hresult\":" << uint32_t(a.result) << ",\"prefix_bytes\":" << a.bytes.size() << '}'; }
    log << "],\"render_states\":[";
    const D3DRENDERSTATETYPE states[]={D3DRS_ZENABLE,D3DRS_ZWRITEENABLE,D3DRS_ALPHATESTENABLE,D3DRS_ALPHABLENDENABLE,D3DRS_SRCBLEND,D3DRS_DESTBLEND,D3DRS_BLENDOP,D3DRS_SEPARATEALPHABLENDENABLE,D3DRS_STENCILENABLE,D3DRS_CLIPPLANEENABLE,D3DRS_COLORWRITEENABLE,D3DRS_ZFUNC};
    bool stateComma=false;for(const auto state:states) { DWORD value=0;const auto hr=device->GetRenderState(state,&value);if(stateComma) { log<<','; }stateComma=true;
      log<<"{\"state\":"<<unsigned(state)<<",\"value\":"<<value<<",\"hresult\":"<<uint32_t(hr)<<'}'; }
    log << "],\"integer_constants_hresult\":" << uint32_t(input.integerResult) << ",\"bool_constants_hresult\":" << uint32_t(input.boolResult) << ",\"pixel_constants_hresult\":" << uint32_t(input.pixelResult) << ",\"files\":[";
    bool comma=false;
    const auto save=[&](const char* suffix,const void* bytes,size_t size) {
      if (!size) { return; }const std::string name=prefix+suffix;
      std::ofstream file(launch.output/name,std::ios::binary);file.write(static_cast<const char*>(bytes),std::streamsize(size));file.close();
      if (!file) { return; }unknownBytes+=size;if (comma) { log << ','; }comma=true;
      log << "{\"file\":\"" << name << "\",\"size\":" << size << ",\"sha256\":\"" << ldb_rt_file::fileSha256(launch.output/name) << "\"}";
    };
    save("-vertices.bin",input.vertices.data(),input.vertices.size());save("-indices.bin",input.indices.data(),input.indices.size());
    save("-vs.bin",input.vs.data(),input.vs.size()*4);save("-ps.bin",input.ps.data(),input.ps.size()*4);
    save("-declaration.bin",input.declaration.data(),input.declaration.size()*sizeof(D3DVERTEXELEMENT9));
    if (SUCCEEDED(input.constantsResult)) { save("-constants.bin",input.constants.data(),sizeof(input.constants)); }
    if (SUCCEEDED(input.integerResult)) { save("-vs-integers.bin",input.integerConstants.data(),sizeof(input.integerConstants)); }
    if (SUCCEEDED(input.boolResult)) { save("-vs-bools.bin",input.boolConstants.data(),sizeof(input.boolConstants)); }
    if (SUCCEEDED(input.pixelResult)) { save("-ps-constants.bin",input.pixelConstants.data(),sizeof(input.pixelConstants)); }
    if (!input.morph.positions.empty()) { save("-morph-positions.bin",input.morph.positions.data(),input.morph.positions.size()*sizeof(sky_shader::Vec)); }
    for (unsigned i=0;i<3;++i) { const std::string name="-stream-"+std::to_string(i+1)+".bin";save(name.c_str(),input.auxiliary[i].bytes.data(),input.auxiliary[i].bytes.size()); }

    log << "]}\n";std::ofstream output(launch.output/"unknown-model-inputs.jsonl",std::ios::app);output << log.str();output.close();if(!output) { throw std::runtime_error("unknown shader journal"); }
}
void targetEvidence(const draw_observe::Snapshot& s,const scene_input::Prepared& p,const live_mesh::Submission& submitted,uint64_t frame) {
  if((std::strcmp(s.materialInfo.shader.data(),"Infected")&&std::strcmp(s.materialInfo.shader.data(),"VertexLitGeneric"))||frame%60||targetRows>=256||!std::strcmp(p.reason,"api_model_stage_excluded")) { return; }
  // The first nearby Draws were often tiny rigid props/weapon parts. Prefer
  // substantial skinned candidates; this remains a heuristic, not entity identity.
  if(!p.ready) { return; }
  if(!std::strcmp(s.materialInfo.shader.data(),"VertexLitGeneric")&&
     ((std::strcmp(p.reason,"original_vs_three_weight")&&std::strcmp(p.reason,"original_vs_gpu_three_weight"))||p.bytes.size()/32<512)) { return; }
  if(targetFrame!=frame) { targetFrame=frame;targetFrameRows=0; }if(targetFrameRows>=4) { return; }
  double distance=-1;std::array<double,3> center{};std::array<double,4> clip{};
  if(p.ready&&!p.bytes.empty()) {
    for(size_t at=0;at<p.bytes.size();at+=32) { float v[3]{};std::memcpy(v,p.bytes.data()+at,12);
      if(p.skinning.enabled) {
        float skinned[3]{};const size_t vertex=at/32,base=vertex*p.skinning.bonesPerVertex;
        for(uint32_t k=0;k<p.skinning.bonesPerVertex;++k) { const auto& m=p.skinning.transforms[p.skinning.indices[base+k]];const float w=p.skinning.weights[base+k];
          for(unsigned row=0;row<3;++row) { skinned[row]+=(v[0]*m[row*4]+v[1]*m[row*4+1]+v[2]*m[row*4+2]+m[row*4+3])*w; } }
        std::memcpy(v,skinned,sizeof(v));
      }
      for(unsigned j=0;j<3;++j) { center[j]+=double(v[0])*p.world[j]+double(v[1])*p.world[4+j]+double(v[2])*p.world[8+j]+p.world[12+j]; }
    }
    for(auto& v:center) { v/=double(p.bytes.size()/32); }
    std::array<double,4> camera{};for(unsigned j=0;j<4;++j) { camera[j]=s.view[12+j];for(unsigned k=0;k<3;++k) { camera[j]+=center[k]*s.view[k*4+j]; } }
    distance=std::sqrt(camera[0]*camera[0]+camera[1]*camera[1]+camera[2]*camera[2]);
    if(distance>1024) { return; }for(unsigned j=0;j<4;++j) { for(unsigned k=0;k<4;++k) { clip[j]+=camera[k]*s.projection[k*4+j]; } }
  }
  if(clip[3]<=0||std::abs(clip[0])>1.1*clip[3]||std::abs(clip[1])>1.1*clip[3]||clip[2]<0||clip[2]>clip[3]) { return; }
  ++targetRows;++targetFrameRows;std::string vertexFile,indexFile,weightFile,boneIndexFile,transformFile;
  const size_t skinBytes=p.skinning.weights.size()*sizeof(float)+p.skinning.indices.size()*sizeof(uint32_t)+p.skinning.transforms.size()*sizeof(std::array<float,12>);
  if(p.ready&&p.bytes.size()+p.indices.size()+skinBytes<=16ull*1024*1024-targetBytes) {
    const auto write=[&](const char* suffix,const void* data,size_t bytes) {
      if(!bytes) { return std::string{}; }const auto file="target-"+std::to_string(s.ordinal)+suffix;
      std::ofstream f(launch.output/file,std::ios::binary);f.write(static_cast<const char*>(data),std::streamsize(bytes));f.close();if(!f) { throw std::runtime_error("write near target"); }targetBytes+=bytes;return file;
    };vertexFile=write("-vertices.bin",p.bytes.data(),p.bytes.size());indexFile=write("-indices.bin",p.indices.data(),p.indices.size());
    weightFile=write("-skin-weights.bin",p.skinning.weights.data(),p.skinning.weights.size()*sizeof(float));
    boneIndexFile=write("-skin-indices.bin",p.skinning.indices.data(),p.skinning.indices.size()*sizeof(uint32_t));
    transformFile=write("-bone-transforms.bin",p.skinning.transforms.data(),p.skinning.transforms.size()*sizeof(std::array<float,12>));
  }
  events<<"{\"event\":\"infected_near_target\",\"frame\":"<<frame<<",\"ordinal\":"<<s.ordinal<<",\"preview\":"<<(state().preview?"true":"false")
    <<",\"material_handle\":"<<submitted.materialHandle<<",\"textured\":"<<(submitted.textured?"true":"false")
    <<",\"shader\":\""<<s.materialInfo.shader.data()<<"\",\"source_material\":"<<s.material<<",\"ready\":"<<(p.ready?"true":"false")<<",\"submitted\":"<<(submitted.success?"true":"false")<<",\"reason\":\""<<p.reason
    <<"\",\"distance_source_units\":"<<distance<<",\"world_center\":["<<center[0]<<','<<center[1]<<','<<center[2]<<"],\"center_clip\":["<<clip[0]<<','<<clip[1]<<','<<clip[2]<<','<<clip[3]
    <<"],\"vertex_file\":\""<<vertexFile<<"\",\"index_file\":\""<<indexFile<<"\",\"skin_weight_file\":\""<<weightFile<<"\",\"skin_index_file\":\""<<boneIndexFile<<"\",\"bone_transform_file\":\""<<transformFile
    <<"\",\"skinned\":"<<(p.skinning.enabled?"true":"false")<<",\"skin_weights_sha256\":\""<<ldb_rt_file::geometrySha256(p.skinning.weights.data(),p.skinning.weights.size()*sizeof(float))
    <<"\",\"skin_indices_sha256\":\""<<ldb_rt_file::geometrySha256(p.skinning.indices.data(),p.skinning.indices.size()*sizeof(uint32_t))
    <<"\",\"bone_transforms_sha256\":\""<<ldb_rt_file::geometrySha256(p.skinning.transforms.data(),p.skinning.transforms.size()*sizeof(std::array<float,12>))
    <<"\",\"vertices_sha256\":\""<<ldb_rt_file::geometrySha256(p.bytes.data(),p.bytes.size())
    <<"\",\"indices_sha256\":\""<<ldb_rt_file::geometrySha256(p.indices.data(),p.indices.size())<<"\",\"payload_bytes\":"<<targetBytes<<"}\n";
}
bool model(IDirect3DDevice9* device,const draw_observe::Snapshot& s,uint32_t kind,D3DPRIMITIVETYPE type,const std::array<uint32_t,6>& args,
    static_draw::Report& report,bool& called,HRESULT& physical,HRESULT& result) noexcept {
  runtime_profile::Scope modelTiming(runtime_profile::Stage::Model);
  const char* operation="target_and_retirement";
  const auto failed=[&](const char* message) {
    if(events) {
      events << "{\"event\":\"api_model_exception_context\",\"frame\":" << state().presents+1 << ",\"ordinal\":" << s.ordinal
        << ",\"source_material\":" << s.material << ",\"operation\":\"" << operation << "\",\"message\":\"";
      static const char hex[]="0123456789abcdef";
      for(size_t i=0;message[i]&&i<512;++i) {
        const auto c=static_cast<unsigned char>(message[i]);
        if(c=='"'||c=='\\') { events << '\\' << char(c); }
        else if(c<32) { events << "\\u00" << hex[c>>4] << hex[c&15]; }
        else { events << char(c); }
      }
      events << "\"}\n" << std::flush;
    }
    event("api_model_exception",uint32_t(E_FAIL));(void)raytracing(false);state().active=false;
    if(!called) { called=true;physical=kind==1?device->DrawPrimitive(type,args[0],args[1]):device->DrawIndexedPrimitive(type,INT(int32_t(args[0])),args[1],args[2],args[3],args[4]); }
    result=physical;report.capture=E_FAIL;report.code=static_draw::Code::CaptureFailed;return true;
  };
  try {
    if(device!=registered||!raytracingRequested) { return false; }
    Ref<IDirect3DSurface9> target;D3DSURFACE_DESC desc{};D3DVIEWPORT9 vp{};
    if(FAILED(device->GetRenderTarget(0,&target.value))||!target.value||FAILED(target.value->GetDesc(&desc))||
       FAILED(device->GetViewport(&vp))||desc.Format!=D3DFMT_A16B16G16R16F||desc.Width!=s.viewport[2]||desc.Height!=s.viewport[3]||
       vp.X||vp.Y||vp.Width!=desc.Width||vp.Height!=desc.Height||vp.MinZ!=0||vp.MaxZ!=1) { return false; }
    auto& counts=modelCounts[size_t(comparison.phase())];++counts.attempts;
    const uint64_t frame=state().presents+1;const bool included=world_diagnostics::liveInclude(currentModels(),bulk_static::customModel(s));
    modelTextures.completedThrough(completedMeshFrame);
    if(apiMaintenanceFrame!=frame) {
      if(!pollMeshCompletion()) { --counts.attempts;return false; }
      const auto retired=apiMeshes.begin(completedMeshFrame,frame);
      if(retired) { event("api_mesh_retire_failed",retired);(void)raytracing(false);state().active=false;--counts.attempts;return false; }
      if(!launch.performance&&previousModelDraws>world_diagnostics::kGpuSkinModelDensityLimit) { ++gpuSkinDensityThrottleFrames; }
      apiMaintenanceFrame=frame;
    }
    if(!launch.performance&&world_diagnostics::readback(frame)) { modelState(device,s); }
    if(frameBatchActive()&&included) {
      operation="capture_frame_batch_model";
      if(!modelWorkers||batchPending.size()>=2048||(batchFrame&&batchFrame!=frame)) { return failed("frame_batch_queue_state"); }
      auto captured=model_input::capture(device,s,kind,type,args,false);
      const auto disposition=model_batch::captureDisposition(captured);
      if(disposition!=model_batch::CaptureDisposition::Queue) {
        ++counts.reasons[captured.reason];++counts.fallback;
        ++liveShaderFallbacks[std::string(s.materialInfo.shader.data())+":"+captured.reason];
        if(disposition==model_batch::CaptureDisposition::Fatal) { return failed(captured.reason); }
        if(batchFallbackFrame!=frame) { batchFallbackFrame=frame;batchFrameInputFallbacks=0; }
        ++batchFrameInputFallbacks;++batchInputFallbacks;
        // The caller performs the untouched physical Draw.  This mirrors the
        // synchronous path and, critically, does not disable RT or discard the
        // rest of the frame because one model pass is intentionally excluded.
        return false;
      }
      DWORD cull=0;auto hr=device->GetRenderState(D3DRS_CULLMODE,&cull);
      if(SUCCEEDED(hr)&&apiCameraFrame!=frame) {
        remixapi_CameraInfo camera{};camera.sType=REMIXAPI_STRUCT_TYPE_CAMERA_INFO;camera.type=REMIXAPI_CAMERA_TYPE_WORLD;
        std::memcpy(camera.view,s.view.data(),64);std::memcpy(camera.projection,s.projection.data(),64);
        const auto code=api.SetupCamera(&camera);if(code!=REMIXAPI_ERROR_CODE_SUCCESS) { hr=E_FAIL; }
        else { apiCameraFrame=frame;apiView=s.view;apiProjection=s.projection; }
      }
      bool sameCamera=true;for(unsigned i=0;i<16;++i) { sameCamera=sameCamera&&std::abs(apiView[i]-s.view[i])<=1e-4f&&std::abs(apiProjection[i]-s.projection[i])<=1e-4f; }
      if(FAILED(hr)||!sameCamera) { return failed("frame_batch_camera_or_cull"); }
      operation="acquire_frame_batch_texture";
      const bool infected=!std::strcmp(s.materialInfo.shader.data(),"Infected");
      auto source=[&] { runtime_profile::Scope timing(runtime_profile::Stage::Texture);return infected?infectedMaterials.acquire(device,modelTextures,launch.output,frame,s.material):modelTextures.acquire(device,launch.output,frame,s.material); }();
      model_batch::Job job;job.frame=frame;job.snapshot=s;job.captured=std::move(captured);job.kind=kind;job.topology=type;job.args=args;
      job.cull=cull;job.variant=infected?live_mesh::Material::Neutral:liveMaterial(currentModels());job.material=std::move(source);
      operation="enqueue_frame_batch_model";batchFrame=frame;batchPending.push_back(modelWorkers->submit(std::move(job)));
      ++counts.reasons["frame_batch_queued"];++counts.ready;
      std::unique_ptr<live_mesh::IgnoreScope<IDirect3DDevice9>> scope=std::make_unique<live_mesh::IgnoreScope<IDirect3DDevice9>>(device);
      report.capture=scope->capture;report.apply=scope->apply;
      if(!scope->ready()) { return failed("frame_batch_ignore_failed"); }
      operation="frame_batch_original_draw_and_restore";
      { runtime_profile::Scope originalTiming(runtime_profile::Stage::OriginalDraw);
        called=true;physical=kind==1?device->DrawPrimitive(type,args[0],args[1]):device->DrawIndexedPrimitive(type,INT(int32_t(args[0])),args[1],args[2],args[3],args[4]); }
      scope->finish();report.restore=scope->restore;
      if(FAILED(report.restore)) { return failed("frame_batch_ignore_restore_failed"); }
      report.converted=SUCCEEDED(report.apply)&&SUCCEEDED(physical);report.code=report.converted?static_draw::Code::Applied:static_draw::Code::ApplyFailed;
      result=physical;if(report.converted) { ++counts.reconstructed;++liveShaderSubmissions[s.materialInfo.shader.data()]; }else { ++counts.fallback; }
      return true;
    }
    operation="prepare_model";
    const auto gpuSkinLimit=launch.performance?world_diagnostics::performanceGpuSkinLimit(previewFrames):world_diagnostics::persistentGpuSkinLimit(previousModelDraws);
    const bool allowGpuSkinCanary=included&&apiMeshes.frameSkinnedDraws<gpuSkinLimit;
    auto prepared=[&] {
      // Performance mode now keeps visual completeness: the GPU limit chooses
      // persistent skinning, while excess/unsupported-for-GPU accepted inputs
      // retain the proven explicit CPU-pose reconstruction.
      if(included) { return model_input::prepare(device,s,kind,type,args,nullptr,true,allowGpuSkinCanary,!launch.performance,false); }
      scene_input::Prepared disabled;disabled.reason="api_model_stage_excluded";return disabled;
    }();
    ++counts.reasons[prepared.reason];if(prepared.ready) { ++counts.ready; }
    live_mesh::Submission submitted;DWORD cull=0;
    if(prepared.ready) {
      auto hr=device->GetRenderState(D3DRS_CULLMODE,&cull);
      if(SUCCEEDED(hr)&&apiCameraFrame!=frame) {
        remixapi_CameraInfo camera{};camera.sType=REMIXAPI_STRUCT_TYPE_CAMERA_INFO;camera.type=REMIXAPI_CAMERA_TYPE_WORLD;
        std::memcpy(camera.view,s.view.data(),64);std::memcpy(camera.projection,s.projection.data(),64);
        const auto code=api.SetupCamera(&camera);if(code!=REMIXAPI_ERROR_CODE_SUCCESS) { hr=E_FAIL; }
        else { apiCameraFrame=frame;apiView=s.view;apiProjection=s.projection; }
      }
      bool sameCamera=true;for(unsigned i=0;i<16;++i) { sameCamera=sameCamera&&std::abs(apiView[i]-s.view[i])<=1e-4f&&std::abs(apiProjection[i]-s.projection[i])<=1e-4f; }
      if(SUCCEEDED(hr)&&sameCamera) {
        operation="acquire_model_texture";
        const bool infected=!std::strcmp(s.materialInfo.shader.data(),"Infected");
        const bool control=infected&&currentModels()==world_diagnostics::ModelStage::Combined;
        auto source=[&] { runtime_profile::Scope timing(runtime_profile::Stage::Texture);return infected?infectedMaterials.acquire(device,modelTextures,launch.output,frame,s.material):modelTextures.acquire(device,launch.output,frame,s.material); }();
        if(control) { source={};source.key="infected-bright-opaque-control";source.reason="infected_bright_opaque_double_sided_control"; }
        // Infected alpha is encoded palette data, never surface coverage.
        const auto variant=infected?live_mesh::Material::Neutral:liveMaterial(currentModels());
        operation="submit_api_mesh";
        submitted=apiMeshes.draw(prepared,kind,type,control?DWORD(D3DCULL_NONE):cull,variant,frame,meshFences.size(),source,completedMeshFrame,
          s.modelScope?s.modelResource:0,s.modelScope?s.modelSurface:0,s.modelScope?s.modelInstance:0);
        textureBinding(source,submitted,frame,s);
        if(submitted.success) { if(source.textured()) { ++texturedSubmissions; }else { ++textureFallbackSubmissions; } }
      }
      else { submitted.code=ERROR_INVALID_DATA;submitted.operation="camera_or_cull"; }
      if(submitted.deferred) { if(apiMeshes.frameOmitted<=8) { submissionEvent(s,submitted,frame); } }
      else if(!submitted.success) { submissionEvent(s,submitted,frame);(void)raytracing(false);state().active=false; }
    }
    // Hide only draws with an explicit replacement. Unsupported programmable
    // draws remain absent from RT when vertex capture is disabled; every model
    // family proven by the earlier complete visual pilot is rebuilt above.
    std::unique_ptr<live_mesh::IgnoreScope<IDirect3DDevice9>> scope;
    if(submitted.success) {
      scope=std::make_unique<live_mesh::IgnoreScope<IDirect3DDevice9>>(device);
      report.capture=scope->capture;report.apply=scope->apply;
      if(!scope->ready()) { event("api_model_ignore_failed",uint32_t(FAILED(scope->capture)?scope->capture:scope->apply));(void)raytracing(false);state().active=false; }
    }
    DWORD ignored=0;if(scope&&scope->ready()) { const auto q=device->GetRenderState(live_mesh::kCategory,&ignored);
      if(FAILED(q)||!(ignored&live_mesh::kIgnore)) { event("api_model_ignore_binding_failed",uint32_t(E_FAIL));(void)raytracing(false);state().active=false;report.apply=E_FAIL; } }
    operation="original_draw_and_restore";
    { runtime_profile::Scope originalTiming(runtime_profile::Stage::OriginalDraw);
    called=true;physical=kind==1?device->DrawPrimitive(type,args[0],args[1]):device->DrawIndexedPrimitive(type,INT(int32_t(args[0])),args[1],args[2],args[3],args[4]);
    }
    if(scope) { scope->finish();report.restore=scope->restore; }
    if(FAILED(report.restore)) { event("api_model_ignore_restore_failed",uint32_t(report.restore));(void)raytracing(false);state().active=false; }
    report.converted=submitted.success&&SUCCEEDED(report.restore)&&SUCCEEDED(report.apply);
    report.code=FAILED(report.capture)?static_draw::Code::CaptureFailed:FAILED(report.apply)?static_draw::Code::ApplyFailed:
      FAILED(report.restore)?static_draw::Code::RestoreFailed:submitted.success?static_draw::Code::Applied:static_draw::Code::Layout;
    result=FAILED(report.restore)?report.restore:physical;
    operation="save_model_evidence";
    if(report.converted&&SUCCEEDED(result)) { ++liveShaderSubmissions[s.materialInfo.shader.data()];++counts.reconstructed;if(!launch.performance) { apiPayload(s,prepared,kind,type,cull,submitted,ignored); } }
    else { ++counts.fallback;++liveShaderFallbacks[std::string(s.materialInfo.shader.data())+":"+prepared.reason]; }
    if(!launch.performance) {
      if(!prepared.ready) { unknownEvidence(device,s,kind,type,args,prepared.reason,frame); }
      targetEvidence(s,prepared,submitted,frame);
    }return true;
  } catch(const std::exception& error) { return failed(error.what()); }
    catch(...) { return failed("non_standard_exception"); }
}
void sceneSummary(uint64_t frame) {
  const auto index=size_t(comparison.phase());
  if (frame<=180 || frame==world_diagnostics::kCoveragePresents) {
  events << "{\"event\":\"scene_input_counts\",\"frame\":" << frame << ",\"phase\":\"" << world_diagnostics::name(comparison.phase())
    << "\",\"payload_bytes\":" << sceneBytes[index] << ",\"captured\":" << sceneSampleCounts[index] << ",\"identity_failures\":" << sceneIdentityFailures[index];
  const auto counts=[&](const char* name,const auto& values) {
    events << ",\"" << name << "\":[";for (unsigned i=0;i<values.size();++i) { if (i) { events << ','; }events << values[i]; }events << ']';
  };
  counts("family_payload_bytes",sceneFamilyBytes[index]);counts("family_captured",sceneFamilyCounts[index]);counts("family_quota_omitted",sceneQuotaOmitted[index]);counts("family_payload_omitted",scenePayloadOmitted[index]);events << "}\n" << std::flush;
  }
  const auto phase=comparison.phase();const auto& m=modelCounts[size_t(phase)];
  events << "{\"event\":\"model_rebuild_counts\",\"frame\":" << frame << ",\"phase\":\"" << world_diagnostics::name(phase)
    << "\",\"model_stage\":\"" << world_diagnostics::texturedName(currentModels()) << "\",\"attempts\":" << m.attempts << ",\"ready\":" << m.ready << ",\"reconstructed\":" << m.reconstructed << ",\"fallback\":" << m.fallback << ",\"reasons\":{";
  bool modelComma=false;for (const auto& pair:m.reasons) { if (modelComma) { events << ','; }modelComma=true;events << '"' << pair.first << "\":" << pair.second; }events << "}}\n" << std::flush;
  if (frame>180 && frame!=world_diagnostics::kCoveragePresents) { return; }
  const auto& c=sceneCounts[size_t(phase)];
  events << "{\"event\":\"sky_rebuild_counts\",\"frame\":" << frame << ",\"phase\":\"" << world_diagnostics::name(phase)
    << "\",\"attempts\":" << c.attempts << ",\"ready\":" << c.ready << ",\"reconstructed\":" << c.reconstructed << ",\"fallback\":" << c.fallback << ",\"input_samples\":" << sceneSampleCounts[size_t(phase)] << ",\"reasons\":{";
  bool comma=false;for (const auto& pair:c.reasons) { if (comma) { events << ','; }comma=true;events << '"' << pair.first << "\":" << pair.second; }events << "}}\n" << std::flush;
}

bool overlayDraw(IDirect3DDevice9* device, const draw_observe::Snapshot& snapshot,
    D3DPRIMITIVETYPE type, bool userMemory, static_draw::Report& report, HRESULT& physical) noexcept {
  if (device!=registered || !raytracingRequested || !overlay::candidate(snapshot,type,userMemory)) { return false; }
  if(frameBatchActive()&&!flushModelBatch(state().presents+1)) {
    event("frame_batch_flush_failed",ERROR_INVALID_DATA);previewMaintenanceFailed=true;(void)raytracing(false);state().active=false;
    report.capture=E_FAIL;report.code=static_draw::Code::CaptureFailed;physical=E_FAIL;return true;
  }
  const auto result=overlay::inject(device,snapshot);report=result.report;
  physical=FAILED(report.restore)?report.restore:result.draw;
  if(!state().preview) { events << "{\"event\":\"hud_injection\",\"phase\":\"" << world_diagnostics::name(comparison.phase())
    << "\",\"ordinal\":" << snapshot.ordinal
    << ",\"capture_hresult\":" << uint32_t(report.capture) << ",\"apply_hresult\":" << uint32_t(report.apply)
    << ",\"draw_hresult\":" << uint32_t(result.draw) << ",\"restore_hresult\":" << uint32_t(report.restore)
    << ",\"scope_code\":" << uint32_t(report.code) << ",\"replaced_post\":" << (result.replaced?"true":"false") << "}\n" << std::flush;
  }
  return result.replaced || FAILED(report.restore);
}
void capture(IDirect3DDevice9* device, uint64_t frame) noexcept {
  try {
    if (device != registered || !events) { return; }
    runtime_profile::Scope timing(runtime_profile::Stage::Readback);
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
    events << "{\"event\":\"model_stage_readback\",\"frame\":" << frame << ",\"model_stage\":\"" << world_diagnostics::texturedName(currentModels()) << "\"}\n" << std::flush;
    events << "{\"event\":\"api_model_path_readback\",\"frame\":" << frame
      << ",\"stage\":\"" << world_diagnostics::texturedName(currentModels()) << "\",\"material\":\"" << live_mesh::name(liveMaterial(currentModels()))
      << "\",\"submission_samples\":" << modelSampleCounts[unsigned(currentModels())]
      << ",\"submission_sample_omitted\":" << modelSampleOmitted[unsigned(currentModels())]
      << ",\"previous_samples\":" << modelPreviousSampleCounts[unsigned(currentModels())]
      << ",\"previous_sample_omitted\":" << modelPreviousSampleOmitted[unsigned(currentModels())]
      << ",\"omitted_total\":" << apiMeshes.omitted << ",\"submitted_total\":" << apiMeshes.submitted << ",\"live_entries\":" << apiMeshes.entries() << "}\n" << std::flush;
    bool advanced = comparison.afterCapture(frame,[&](const char* key,const char* value) {
      const auto code=api.SetConfigVariable(key,value);
      events << "{\"event\":\"model_path_config_requested\",\"after_present\":" << frame << ",\"key\":\"" << key
        << "\",\"value\":\"" << value << "\",\"code\":" << uint32_t(code) << "}\n" << std::flush;
      return code==REMIXAPI_ERROR_CODE_SUCCESS;
    });
    if(advanced&&(frame==300||frame==360||frame==540)) {
      const char* value=frame==300?"3,3,3":"30,30,30";
      const auto code=api.SetConfigVariable("rtx.fallbackLightRadiance",value);
      events << "{\"event\":\"model_light_config_requested\",\"after_present\":" << frame
        << ",\"value\":\"" << value << "\",\"code\":" << uint32_t(code) << "}\n" << std::flush;
      advanced=code==REMIXAPI_ERROR_CODE_SUCCESS;
    }
    events << "{\"event\":\"coverage_phase_requested\",\"code\":" << (advanced ? 0 : uint32_t(E_FAIL))
      << ",\"after_present\":" << frame << ",\"phase\":\"" << world_diagnostics::name(comparison.phase()) << "\"}\n" << std::flush;
    if (!advanced) { (void)raytracing(false); state().active = false; }
  } catch (...) {
    event("readback_exception", uint32_t(E_FAIL)); (void)raytracing(false); state().active = false;
  }
}

void previewColor(IDirect3DDevice9* device) {
  runtime_profile::Scope timing(runtime_profile::Stage::Readback);
  constexpr UINT width=640,height=400;Ref<IDirect3DSurface9> target,staging;
  HRESULT hr=device->CreateRenderTarget(width,height,D3DFMT_A32B32G32R32F,D3DMULTISAMPLE_NONE,0,FALSE,&target.value,nullptr);
  if(SUCCEEDED(hr)) { hr=device->ColorFill(target.value,nullptr,0); }
  const auto code=SUCCEEDED(hr)?api.dxvk_CopyRenderingOutput(target.value,remixapi_dxvk_CopyRenderingOutputType(0)):REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
  if(code!=REMIXAPI_ERROR_CODE_SUCCESS) { hr=E_FAIL; }
  if(SUCCEEDED(hr)) { hr=device->CreateOffscreenPlainSurface(width,height,D3DFMT_A32B32G32R32F,D3DPOOL_SYSTEMMEM,&staging.value,nullptr); }
  if(SUCCEEDED(hr)) { hr=device->GetRenderTargetData(target.value,staging.value); }
  D3DLOCKED_RECT lock{};bool written=false;const auto file="preview-"+std::to_string(previewReadbacks)+".rgba32f";
  if(SUCCEEDED(hr)) { hr=staging.value->LockRect(&lock,nullptr,D3DLOCK_READONLY);
    if(SUCCEEDED(hr)) {
      if(lock.pBits&&lock.Pitch>=int(width*16)) {
        std::ofstream output(launch.output/file,std::ios::binary);
        for(UINT y=0;y<height;++y) { output.write(static_cast<const char*>(lock.pBits)+size_t(y)*size_t(lock.Pitch),width*16); }
        output.close();written=bool(output);
      }
      const auto unlocked=staging.value->UnlockRect();if(FAILED(unlocked)) { hr=unlocked; }
    }
  }
  events<<"{\"event\":\"preview_readback\",\"frame\":"<<state().presents<<",\"elapsed_ms\":"<<GetTickCount64()-previewStarted
    <<",\"file\":\""<<file<<"\",\"width\":640,\"height\":400,\"copy_code\":"<<uint32_t(code)<<",\"hresult\":"<<uint32_t(hr)<<",\"written\":"<<(written?"true":"false")<<"}\n"<<std::flush;
  ++previewReadbacks;if(!written||FAILED(hr)) { previewReadFailed=true; }
}
void nativeColor(IDirect3DDevice9* device,const char* mode) {
  runtime_profile::Scope timing(runtime_profile::Stage::Readback);
  Ref<IDirect3DSurface9> back,target,staging;D3DSURFACE_DESC desc{};
  HRESULT hr=device->GetBackBuffer(0,0,D3DBACKBUFFER_TYPE_MONO,&back.value);
  if(SUCCEEDED(hr)&&back.value) { hr=back.value->GetDesc(&desc); }else if(SUCCEEDED(hr)) { hr=E_FAIL; }
  if(SUCCEEDED(hr)&&(!desc.Width||!desc.Height||desc.Width>4096||desc.Height>2160)) { hr=E_INVALIDARG; }
  const UINT width=uint64_t(desc.Width)*desc.Height*4<=16ull*1024*1024?desc.Width:std::min(1024u,desc.Width);
  const UINT height=uint64_t(desc.Width)*desc.Height*4<=16ull*1024*1024?desc.Height:std::min(768u,desc.Height);
  const UINT x=(desc.Width-width)/2,y=(desc.Height-height)/2;
  if(SUCCEEDED(hr)) { hr=device->CreateRenderTarget(desc.Width,desc.Height,D3DFMT_A8R8G8B8,D3DMULTISAMPLE_NONE,0,FALSE,&target.value,nullptr); }
  auto code=REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
  if(SUCCEEDED(hr)) { hr=device->ColorFill(target.value,nullptr,0); }
  if(SUCCEEDED(hr)) { code=api.dxvk_CopyRenderingOutput(target.value,remixapi_dxvk_CopyRenderingOutputType(0));if(code!=REMIXAPI_ERROR_CODE_SUCCESS) { hr=E_FAIL; } }
  if(SUCCEEDED(hr)) { hr=device->CreateOffscreenPlainSurface(desc.Width,desc.Height,D3DFMT_A8R8G8B8,D3DPOOL_SYSTEMMEM,&staging.value,nullptr); }
  if(SUCCEEDED(hr)) { hr=device->GetRenderTargetData(target.value,staging.value); }
  const std::string file=std::string("native-")+mode+".bgra8";bool written=false;D3DLOCKED_RECT lock{};
  if(SUCCEEDED(hr)) {
    hr=staging.value->LockRect(&lock,nullptr,D3DLOCK_READONLY);
    if(SUCCEEDED(hr)) {
      if(lock.pBits&&lock.Pitch>=int(desc.Width*4)) {
        std::ofstream output(launch.output/file,std::ios::binary);
        for(UINT row=0;row<height;++row) { output.write(static_cast<const char*>(lock.pBits)+size_t(y+row)*size_t(lock.Pitch)+size_t(x)*4,width*4); }
        output.close();written=bool(output);
      }
      const auto unlock=staging.value->UnlockRect();if(FAILED(unlock)) { hr=unlock; }
    }
  }
  events<<"{\"event\":\"native_edge_readback\",\"frame\":"<<state().presents<<",\"elapsed_ms\":"<<GetTickCount64()-previewStarted<<",\"mode\":\""<<mode<<"\",\"file\":\""<<file
    <<"\",\"source_width\":"<<desc.Width<<",\"source_height\":"<<desc.Height<<",\"x\":"<<x<<",\"y\":"<<y<<",\"width\":"<<width<<",\"height\":"<<height
    <<",\"copy_code\":"<<uint32_t(code)<<",\"hresult\":"<<uint32_t(hr)<<",\"written\":"<<(written?"true":"false")<<",\"resampled\":false}\n"<<std::flush;
}
void shaderSummary() {
  events<<"{\"event\":\"live_shader_summary\",\"infected_bakes\":"<<infectedMaterials.baked<<",\"infected_deferred\":"<<infectedMaterials.deferred
    <<",\"infected_failed\":"<<infectedMaterials.failed<<",\"near_target_rows\":"<<targetRows<<",\"near_target_payload_bytes\":"<<targetBytes<<",\"submissions\":{";
  bool comma=false;for(const auto& row:liveShaderSubmissions) { if(comma) { events<<','; }comma=true;events<<'"'<<row.first<<"\":"<<row.second; }events<<"},\"fallbacks\":{";
  comma=false;for(const auto& row:liveShaderFallbacks) { if(comma) { events<<','; }comma=true;events<<'"'<<row.first<<"\":"<<row.second; }events<<"}}\n";
}
void endPreview(bool clean,const char* reason) noexcept {
  try {
    (void)raytracing(false);state().active=false;
    const bool lifecycleComplete=!launch.performance||(previewResourcesRecovered&&
      previewRasterFrames>=world_diagnostics::kGpuSkinPerformanceRasterFrames&&!previewMaintenanceFailed);
    const bool complete=clean&&lifecycleComplete&&!previewReadFailed&&previewReadbacks==(launch.performance?0u:3u)&&
      apiMeshes.entries()==0&&meshFences.empty()&&batchPending.empty()&&!batchFailures;
    if(complete) { modelTextures.clear(); }
    runtime_profile::emit(events,state().presents,launch.performance?"gpu_lifecycle_raster":previewTaa?"preview_native_taa_final":"preview_no_aa_final");
    shaderSummary();
    const auto workerStats=modelWorkers?modelWorkers->statistics():model_batch::Statistics{};
    events<<"{\"event\":\"preview_end\",\"complete\":"<<(complete?"true":"false")<<",\"reason\":\""<<reason
      <<"\",\"elapsed_ms\":"<<GetTickCount64()-previewStarted<<",\"frames\":"<<previewFrames
      <<",\"rt_frames\":"<<(std::min)(previewFrames,uint64_t(world_diagnostics::kGpuSkinPerformanceRtFrames))
      <<",\"raster_survival_frames\":"<<previewRasterFrames<<",\"resources_recovered\":"<<(previewResourcesRecovered?"true":"false")
      <<",\"texture_exports\":"<<modelTextures.exported<<",\"texture_bytes\":"<<modelTextures.bytes<<",\"texture_resident_bytes\":"<<modelTextures.residentBytes<<",\"texture_evicted\":"<<modelTextures.evicted
      <<",\"submitted\":"<<apiMeshes.submitted-previewStartSubmitted<<",\"skinned_submitted\":"<<apiMeshes.skinnedSubmitted-previewStartSkinnedSubmitted
      <<",\"skinned_created\":"<<apiMeshes.skinnedCreated-previewStartSkinnedCreated<<",\"skinned_reused\":"<<apiMeshes.skinnedReused-previewStartSkinnedReused
      <<",\"gpu_skin_only_omitted\":0"
      <<",\"cpu_pose_submitted\":"<<(apiMeshes.submitted-previewStartSubmitted)-(apiMeshes.skinnedSubmitted-previewStartSkinnedSubmitted)
      <<",\"cpu_pose_created\":"<<(apiMeshes.created-previewStartCreated)-(apiMeshes.skinnedCreated-previewStartSkinnedCreated)
      <<",\"cpu_pose_reused\":"<<(apiMeshes.reused-previewStartReused)-(apiMeshes.skinnedReused-previewStartSkinnedReused)
      <<",\"budget_omitted\":"<<apiMeshes.omitted-previewStartOmitted
      <<",\"gpu_skin_density_throttle_frames\":"<<gpuSkinDensityThrottleFrames
      <<",\"frame_batch_flushes\":"<<batchFlushes<<",\"frame_batch_source_draws\":"<<batchSourceDraws
      <<",\"frame_batch_submitted_source_draws\":"<<batchSubmitted<<",\"frame_batch_failures\":"<<batchFailures
      <<",\"frame_batch_input_fallbacks\":"<<batchInputFallbacks
      <<",\"frame_batch_worker_submitted\":"<<workerStats.submitted<<",\"frame_batch_worker_completed\":"<<workerStats.completed
      <<",\"frame_batch_worker_failed\":"<<workerStats.failed<<",\"frame_batch_worker_threads\":"<<workerStats.workers
      <<",\"frame_batch_worker_max_active\":"<<workerStats.maxActive
      <<",\"persistent_mesh_updates\":"<<apiMeshes.updated-previewStartUpdated
      <<",\"entries\":"<<apiMeshes.entries()<<",\"pending_fences\":"<<meshFences.size()
      <<",\"binding_rows_omitted\":"<<previewBindingOmitted<<",\"held\":"<<modelTextures.held()<<",\"readbacks\":"<<previewReadbacks<<"}\n"<<std::flush;
    std::ofstream done(draw_observe::previewDoneFile(launch));done<<"{\"session\":"<<launch.session<<",\"complete\":"<<(complete?"true":"false")<<"}\n";done.close();
    if(modelWorkers) { modelWorkers->shutdown();modelWorkers.reset(); }batchPending.clear();batchFrame=0;state().preview=false;
  } catch(...) {
    state().active=false;state().preview=false;
    if(modelWorkers) { modelWorkers->shutdown();modelWorkers.reset(); }
    batchPending.clear();batchFrame=0;
  }
}
void startPreview(IDirect3DDevice9* device) noexcept {
  try {
    if(device!=registered||apiMeshes.entries()||!meshFences.empty()) { return; }
    events.close();events.open(launch.output/"preview-runtime.jsonl",std::ios::binary);events<<std::setprecision(9);
    if(!events) { return; }
    std::ofstream marker(draw_observe::previewRunningFile(launch));marker<<"{\"session\":"<<launch.session<<",\"duration_ms\":"<<(launch.performance?0ull:launch.beta?kBetaPreviewDurationMs:60000ull)<<",\"target_frames\":"<<(launch.performance?kPerformancePreviewFrames:0ull)<<"}\n";marker.close();
    if(!marker) { return; }
    runtime_profile::reset();sourceShaderCensus.clear();sourceCensusOmitted=0;previewTaa=false;
    if(modelWorkers) { modelWorkers->shutdown(); }modelWorkers=launch.performance?std::make_unique<model_batch::Pool>(4):nullptr;
    batchPending.clear();batchFrame=0;batchFlushes=0;batchSourceDraws=0;batchSubmitted=0;batchFailures=0;batchInputFallbacks=0;batchFallbackFrame=0;batchFrameInputFallbacks=0;
    state().preview=true;state().active=true;previewStarted=GetTickCount64();previewRecoverAt=0;previewFrames=0;previewRasterFrames=0;
    previewRecover=false;previewResourcesRecovered=false;previewMaintenanceFailed=false;previewReadFailed=false;previewReadbacks=0;previewBindingRows=0;previewBindingOmitted=0;previewFirstRead=0;previewStartSubmitted=apiMeshes.submitted;previewStartCreated=apiMeshes.created;previewStartReused=apiMeshes.reused;previewStartUpdated=apiMeshes.updated;previewStartOmitted=apiMeshes.omitted;
    previousModelDraws=0;gpuSkinDensityThrottleFrames=0;
    previewStartSkinnedSubmitted=apiMeshes.skinnedSubmitted;previewStartSkinnedCreated=apiMeshes.skinnedCreated;previewStartSkinnedReused=apiMeshes.skinnedReused;
    event("world_coverage_revision",0,"23");
    event("pilot_delivery_revision",0,"46");
    event("preview_begin",0,launch.performance?"Arc_persistent_mesh_update;vertex_capture_off;30_RT_explicit_oracle_GPU80;30_RT_CPU_worker_batch_control;30_RT_persistent_handle_update;RT_off;resources_zero;60_raster_Presents;custom_preset;ReSTIR_GI;TAAU_0.75;one_F9":launch.beta?"five_minute_beta;30_seconds_no_AA_then_native_TAA;one_F9;not_controlled_performance_benchmark":"30_seconds_no_AA_then_30_seconds_native_TAA;one_F9;not_controlled_performance_benchmark");
    if(launch.performance) {
      event("performance_lifecycle_begin",performanceProfileConfigured?0:ERROR_INVALID_DATA,
        "explicit_GPU80_oracle_then_CPU_worker_frame_batch_control_then_persistent_mesh_update_windows_of_30;vertex_capture_off;RT_off;resources_zero;raster_survival_60;graphics_preset_custom;TAAU_0.75;direct_light;secondary_off;RTXDI_off;ReSTIR_GI");
      if(!performanceProfileConfigured) { endPreview(false,"performance_profile_failed");return; }
    }
    if(!raytracing(true)) { endPreview(false,"rt_enable_failed"); }
  } catch(...) { endPreview(false,"start_exception"); }
}
void previewStep(IDirect3DDevice9* device,bool successful) noexcept {
  try {
    if(!state().preview) { return; }
    if(!successful||(!launch.performance&&!state().active)||(launch.performance&&!previewRecover&&!state().active)) {
      endPreview(false,"present_or_runtime_failed");return;
    }
    ++previewFrames;presented(device,state().presents);
    if(launch.performance) {
      if(previewMaintenanceFailed||(!previewRecover&&!state().active)) { endPreview(false,"maintenance_failed");return; }
      if(!previewRecover&&previewFrames>=world_diagnostics::kGpuSkinPerformanceRtFrames) {
        previewRecover=true;previewRecoverAt=GetTickCount64();
        const bool disabled=raytracing(false);state().active=false;
        event("performance_rt_recovery_requested",disabled?0:ERROR_INVALID_DATA,"RT_off_after_90_successful_RT_Presents");
        if(!disabled) { endPreview(false,"rt_disable_failed"); }
      } else if(previewRecover&&!previewResourcesRecovered) {
        if(apiMeshes.entries()==0&&meshFences.empty()) {
          previewResourcesRecovered=true;previewRasterFrames=0;
          event("performance_resources_recovered",0,"API_meshes_0;pending_fences_0;begin_60_raster_Presents");
        } else if(GetTickCount64()-previewRecoverAt>15000) { endPreview(false,"recovery_timeout"); }
      } else if(previewResourcesRecovered) {
        ++previewRasterFrames;
        if(previewRasterFrames>=world_diagnostics::kGpuSkinPerformanceRasterFrames) {
          event("performance_raster_survival_complete",0,"60_successful_Presents_after_RT_resource_recovery");
          endPreview(true,"performance_lifecycle_complete");
        }
      }
      return;
    }
    if(!state().active) { endPreview(false,"maintenance_failed");return; }
    const auto elapsed=GetTickCount64()-previewStarted;
    if(!previewRecover) {
      const auto duration=launch.beta?kBetaPreviewDurationMs:60000ull;
      if((previewReadbacks==0&&previewFrames>=60)||(previewReadbacks==1&&elapsed>=30000)||(previewReadbacks==2&&elapsed>=duration)) { previewColor(device); }
      if(elapsed>=30000&&!previewTaa) {
        nativeColor(device,"no-aa");runtime_profile::emit(events,state().presents,"preview_no_aa_final");
        const auto scale=api.SetConfigVariable("rtx.resolutionScale","1.0"),taa=api.SetConfigVariable("rtx.upscalerType","3");
        events<<"{\"event\":\"native_taa_requested\",\"frame\":"<<state().presents<<",\"elapsed_ms\":"<<elapsed
          <<",\"scale_code\":"<<uint32_t(scale)<<",\"taa_code\":"<<uint32_t(taa)<<",\"resolution_scale\":1,\"upscaler_type\":3}\n"<<std::flush;
        if(scale!=REMIXAPI_ERROR_CODE_SUCCESS||taa!=REMIXAPI_ERROR_CODE_SUCCESS) { endPreview(false,"taa_configuration_failed");return; }previewTaa=true;
      }
      if(elapsed>=duration) { nativeColor(device,"taa");previewRecover=true;previewRecoverAt=GetTickCount64();event("preview_recovery_requested",0); }
    } else {
      if(apiMeshes.entries()==0&&meshFences.empty()) { endPreview(true,"duration_complete"); }
      else if(GetTickCount64()-previewRecoverAt>5000) { endPreview(false,"recovery_timeout"); }
    }
  } catch(...) { endPreview(false,"step_exception"); }
}
}
HMODULE load(const draw_observe::Request& request) noexcept {
  try {
    if(modelWorkers) { modelWorkers->shutdown();modelWorkers.reset(); }batchPending.clear();batchFrame=0;
    state() = {};runtime_profile::reset();sourceShaderCensus.clear();sourceCensusOmitted=0;
    initialized = false; registered = nullptr; updateMesh=nullptr;raytracingRequested = false; performanceProfileConfigured=false; physicalDraws = 0;
    comparison = {}; audited.clear(); auditCounts = {}; modelCounts={};apiPayloadBytes={};modelSampleCounts={};modelStateCounts={};modelSampleOmitted={};modelPreviousSampleCounts={};modelPreviousSampleOmitted={}; haveWorldCamera = false; lastView = {}; lastProjection = {};previousModelDraws=0;gpuSkinDensityThrottleFrames=0;
    launch = request;
    events.open(request.output / "world-runtime.jsonl", std::ios::binary);
    if (!events || !request.world || sizeof(void*) != 8) { return nullptr; }
    events << std::setprecision(9);
    const auto path = request.runtime / "d3d9.dll";
    if (ldb_rt_file::fileSha256(path) != "0ecebaf16bb4f89575354f49cc6a23ccc4689e20aa3f59fe9e2844871aa5ef9b") {
      event("runtime_sha_mismatch", ERROR_INVALID_DATA); return nullptr;
    }
    for (const auto* name : {L"DXVK_LOG_PATH", L"DXVK_METRICS_PATH"}) { SetEnvironmentVariableW(name, request.output.c_str()); }
    SetEnvironmentVariableW(L"DXVK_ENABLE_RAYTRACING",nullptr);
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
    const auto updateAddress = GetProcAddress(module, "remixapi_LdbUpdateMesh");
    PFN_remixapi_InitializeLibrary initialize = nullptr;
    static_assert(sizeof(initialize) == sizeof(address), "Windows function pointer ABI");
    std::memcpy(&initialize, &address, sizeof(initialize));
    static_assert(sizeof(updateMesh)==sizeof(updateAddress),"Windows function pointer ABI");
    std::memcpy(&updateMesh,&updateAddress,sizeof(updateMesh));
    if (!initialize||!updateMesh) { event(!initialize?"find_api":"find_ldb_update_mesh", ERROR_PROC_NOT_FOUND); return nullptr; }
    remixapi_InitializeLibraryInfo info{}; info.sType = REMIXAPI_STRUCT_TYPE_INITIALIZE_LIBRARY_INFO;
    info.version = REMIXAPI_VERSION_MAKE(0, 6, 4);
    const auto result = initialize(&info, &api); event("initialize_api_0_6_4", uint32_t(result));
    if (result != REMIXAPI_ERROR_CODE_SUCCESS || !api.SetConfigVariable || !api.Shutdown ||
        !api.dxvk_RegisterD3D9Device || !api.dxvk_CopyRenderingOutput || !api.CreateMaterial || !api.DestroyMaterial ||
        !api.CreateMesh || !api.DestroyMesh || !api.DrawInstance || !api.SetupCamera) { return nullptr; }
    apiMeshes.bind(&api,updateMesh);event("find_ldb_update_mesh",0);
    initialized = true;
    if (!configure()) { return nullptr; }
    event("startup_raster_policy", 0, "RT_off_until_valid_converted_draw_after_control");
    event("coverage_comparison_policy",0,"v23_display_4GiB_priority_spool_no_rejected_rereads_standard_alpha_models");
    event("world_coverage_revision",0,"23");
    event("pilot_delivery_revision",0,"46");
    event("runtime_timing_policy",0,"QPC_elapsed_nested_scopes_plus_device_thread_kernel_user_CPU;not_GPU_duration;source_geometry_skin_texture_hash_API_original_Draw_Present_readback");
    event("api_mesh_policy",0,"v46_persistent_topology_identity;runtime_first_frame_upsert;stable_topology_hashes;BVH_update_refit;recoverable_batch_input_fallback;30_frame_explicit_GPU80_oracle_then_30_frame_CPU_worker_batch_control_then_30_frame_persistent_update;dynamic_CPU_pose_batches_only;vertex_capture_off;mesh_entries_3072_bytes_512MiB;RT_off_resource_zero_then60_raster_Presents;TAAU_0.75_secondary_off_RTXDI_off;four_workers;flush_before_HUD_injection");
    event("model_texture_policy",0,"sampler0_source_mips_128MiB_working_4GiB_spool_256MiB_infected_reserve_64MiB_evidence_256_refs_16_reads_per_frame");
    event("scene_input_policy",0,"skinned_materials_v2");
    event("material_audit_policy", 0, "first_restored_state_per_material_phase_max64_COLOROP_COLORARG1_COLORARG2_ALPHAOP_ALPHAARG1_ALPHAARG2_TEXCOORDINDEX_TEXTURETRANSFORMFLAGS_RESULTARG");
    state() = {true, &bind, &capture, &raytracing, &draw, &audit, &inspect, &model, &scene, &overlayDraw};
    state().presented=&presented;state().startPreview=&startPreview;state().previewStep=&previewStep;state().release=&beforeDeviceDestroy;return module;
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
void invalidateTexture(IDirect3DBaseTexture9* texture) noexcept {
  if(initialized&&texture) { modelTextures.invalidate(texture); }
}
void invalidateSurface(IDirect3DSurface9* surface) noexcept {
  if(!initialized||!surface) { return; }
  constexpr GUID textureGuid{0x85c31227,0x3de5,0x4f00,{0x9b,0x3a,0xf1,0x1a,0xc3,0x8c,0x18,0xb5}};
  static_draw::Ref<IDirect3DTexture9> texture;
  if(SUCCEEDED(surface->GetContainer(textureGuid,reinterpret_cast<void**>(&texture.value)))) { invalidateTexture(texture.value); }
}
void beforeDeviceDestroy(IDirect3DDevice9* device) noexcept {
  if(device&&device==registered) { event("before_device_destroy",0);shutdown(); }
}
void shutdown() noexcept {
  if(!initialized&&!registered&&!events.is_open()) { return; }
  // The game owns its device. Do not call the standalone API Shutdown path:
  // it force-releases the device and D3D9 object to zero. The Bridge owns both.
  if(state().preview) { endPreview(false,"game_shutdown"); }
  if(modelWorkers) { modelWorkers->shutdown();modelWorkers.reset(); }batchPending.clear();batchFrame=0;
  (void)raytracing(false);
  if(registered) {
    // Include commands from an aborted partial frame, not only prior Presents.
    // This runs from the device-destroy hook while the backend is still alive.
    if(apiMeshes.entries()) {
      auto fence=std::make_unique<MeshFence>();fence->frame=state().presents+1;
      auto hr=registered->CreateQuery(D3DQUERYTYPE_EVENT,&fence->query.value);
      if(SUCCEEDED(hr)&&fence->query.value) { hr=fence->query.value->Issue(D3DISSUE_END); }else if(SUCCEEDED(hr)) { hr=E_FAIL; }
      event("device_release_final_fence",uint32_t(hr));if(SUCCEEDED(hr)) { meshFences.push_back(std::move(fence)); }
    }
    const ULONGLONG deadline=GetTickCount64()+1500;
    while(!meshFences.empty()&&GetTickCount64()<deadline) {
      const auto hr=meshFences.front()->query.value->GetData(nullptr,0,D3DGETDATA_FLUSH);
      if(hr==S_OK) { completedMeshFrame=meshFences.front()->frame;meshFences.pop_front(); }
      else if(FAILED(hr)) { break; }else { SwitchToThread(); }
    }
    const auto code=apiMeshes.retire(completedMeshFrame,state().presents,true);event("api_mesh_shutdown_retire",code);
    event("api_mesh_shutdown_remaining",0,std::to_string(apiMeshes.entries()).c_str());meshFences.clear();modelTextures.clear();event("model_texture_shutdown_held",0,std::to_string(modelTextures.held()).c_str());
  }
  runtime_profile::window().enabled=false;
  state() = {}; registered = nullptr; initialized = false;
  if (events) { event("host_draw_request_count", 0, std::to_string(physicalDraws).c_str()); }
  if (events) { event("host_cleanup", 0); events.close(); }
}
} // namespace ldb_rt::world_runtime

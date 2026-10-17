#pragma once
#include "rt_scene_draw.h"
#include "rt_sky_shader.h"

namespace ldb_rt::scene_input {
constexpr uint32_t kMaxVertices=4096,kMaxIndices=16384;
struct Auxiliary { UINT stream=0,offset=0,stride=0,frequency=0;HRESULT result=E_FAIL;std::vector<uint8_t> bytes; };
struct Data {
  HRESULT result=S_OK;
  const char* reason="not_captured";
  uint32_t first=0,count=0,stride=0,offset=0,indexBytes=0;
  std::vector<uint8_t> vertices,indices;
  std::vector<uint32_t> vs,ps;
  std::vector<D3DVERTEXELEMENT9> declaration;
  sky_shader::Constants constants{};
  std::array<Auxiliary,3> auxiliary{};
  std::array<std::array<int,4>,16> integerConstants{};std::array<BOOL,16> boolConstants{};
  std::array<sky_shader::Vec,32> pixelConstants{};
  HRESULT integerResult=E_FAIL,boolResult=E_FAIL,pixelResult=E_FAIL;
  HRESULT geometryResult=E_FAIL,shaderResult=E_FAIL,constantsResult=E_FAIL;
};
template<class Shader> HRESULT shaderBytes(Shader* shader,std::vector<uint32_t>& bytes) {
  if (!shader) { return E_FAIL; }UINT size=0;HRESULT hr=shader->GetFunction(nullptr,&size);
  if (FAILED(hr) || !size || size%4 || size>65536) { return FAILED(hr)?hr:E_INVALIDARG; }
  bytes.resize(size/4);const UINT expected=size;hr=shader->GetFunction(bytes.data(),&size);
  if (FAILED(hr) || size!=expected) { bytes.clear();return FAILED(hr)?hr:E_FAIL; }return S_OK;
}
// Bounds are computed in 64 bits before Lock. All source locks are READONLY;
// a failed read never retries with write flags or modifies a game buffer.
inline HRESULT geometry(IDirect3DDevice9* d,uint32_t kind,D3DPRIMITIVETYPE topology,const std::array<uint32_t,6>& args,Data& out) {
  if ((kind!=1 && kind!=2) || (topology!=D3DPT_TRIANGLELIST && topology!=D3DPT_TRIANGLESTRIP)) { out.reason="unsupported_draw";return E_INVALIDARG; }
  static_draw::Ref<IDirect3DVertexDeclaration9> declaration;auto hr=d->GetVertexDeclaration(&declaration.value);
  if (FAILED(hr) || !declaration.value) { out.reason="declaration_unavailable";return FAILED(hr)?hr:E_FAIL; }
  D3DVERTEXELEMENT9 elements[MAXD3DDECLLENGTH+1]{};UINT n=MAXD3DDECLLENGTH+1;hr=declaration.value->GetDeclaration(elements,&n);
  if (FAILED(hr) || !n || n>MAXD3DDECLLENGTH+1) { out.reason="invalid_declaration";return FAILED(hr)?hr:E_INVALIDARG; }out.declaration.assign(elements,elements+n);
  const uint64_t primitive=kind==2?args[4]:args[1],indexCount=topology==D3DPT_TRIANGLELIST?primitive*3:primitive+2;
  if (!primitive || indexCount>kMaxIndices) { out.reason="index_budget";return E_INVALIDARG; }
  uint64_t first=0,count=0;
  if (kind==2) {
    if (!args[2] || args[2]>kMaxVertices || uint64_t(args[1])+args[2]>uint64_t(UINT32_MAX)) { out.reason="vertex_budget";return E_INVALIDARG; }
    const int64_t begin=int64_t(int32_t(args[0]))+args[1];
    if (begin<0 || uint64_t(begin)+args[2]>kMaxVertices) { out.reason="absolute_vertex_budget";return E_INVALIDARG; }first=uint64_t(begin);count=args[2];
    static_draw::Ref<IDirect3DIndexBuffer9> ib;hr=d->GetIndices(&ib.value);D3DINDEXBUFFER_DESC desc{};
    if (FAILED(hr)||!ib.value) { out.reason="indices_unavailable";return FAILED(hr)?hr:E_FAIL; }
    hr=ib.value->GetDesc(&desc);if (FAILED(hr)) { out.reason="index_description";return hr; }
    out.indexBytes=desc.Format==D3DFMT_INDEX16?2u:desc.Format==D3DFMT_INDEX32?4u:0u;
    const uint64_t start=uint64_t(args[3])*out.indexBytes,bytes=indexCount*out.indexBytes;
    if (!out.indexBytes || start+bytes>desc.Size) { out.reason="index_range";return E_INVALIDARG; }
    out.indices.resize(size_t(bytes));
    void* memory=nullptr;hr=ib.value->Lock(UINT(start),UINT(bytes),&memory,D3DLOCK_READONLY);
    if (FAILED(hr)) { out.indices.clear();out.reason="index_readonly_lock";return hr; }
    if (memory) { std::memcpy(out.indices.data(),memory,size_t(bytes)); }
    hr=ib.value->Unlock();if (FAILED(hr)||!memory) { out.reason="index_unlock_or_null";return FAILED(hr)?hr:E_FAIL; }
    for (size_t i=0;i<size_t(indexCount);++i) {
      uint32_t value=0;if (out.indexBytes==2) { uint16_t v=0;std::memcpy(&v,out.indices.data()+i*2,2);value=v; }else { std::memcpy(&value,out.indices.data()+i*4,4); }
      if (value<args[1] || uint64_t(value)>=uint64_t(args[1])+args[2]) { out.reason="index_outside_declared_vertices";return E_INVALIDARG; }
    }
  } else {
    first=args[0];count=indexCount;
    if (first+count>kMaxVertices) { out.reason="absolute_vertex_budget";return E_INVALIDARG; }
  }
  static_draw::Ref<IDirect3DVertexBuffer9> vb;UINT offset=0,stride=0,frequency=0;
  hr=d->GetStreamSource(0,&vb.value,&offset,&stride);
  if (FAILED(hr)||!vb.value) { out.reason="stream_unavailable";return FAILED(hr)?hr:E_FAIL; }
  hr=d->GetStreamSourceFreq(0,&frequency);D3DVERTEXBUFFER_DESC desc{};
  if (FAILED(hr)||frequency!=1 || !stride || stride>128) { out.reason="stream_frequency_or_stride";return FAILED(hr)?hr:E_INVALIDARG; }
  hr=vb.value->GetDesc(&desc);if (FAILED(hr)) { out.reason="vertex_description";return hr; }
  const uint64_t start=uint64_t(offset)+first*stride,bytes=count*stride;
  if (!bytes || start+bytes>desc.Size || bytes>kMaxVertices*128u) { out.reason="vertex_range";return E_INVALIDARG; }
  out.vertices.resize(size_t(bytes));
  void* memory=nullptr;hr=vb.value->Lock(UINT(start),UINT(bytes),&memory,D3DLOCK_READONLY);
  if (FAILED(hr)) { out.vertices.clear();out.reason="vertex_readonly_lock";return hr; }
  if (memory) { std::memcpy(out.vertices.data(),memory,size_t(bytes)); }
  hr=vb.value->Unlock();if (FAILED(hr)||!memory) { out.reason="vertex_unlock_or_null";return FAILED(hr)?hr:E_FAIL; }
  out.first=uint32_t(first);out.count=uint32_t(count);out.offset=offset;out.stride=stride;out.reason="complete";return S_OK;
}
inline Data capture(IDirect3DDevice9* d,uint32_t kind,D3DPRIMITIVETYPE topology,const std::array<uint32_t,6>& args,bool extended=true) {
  Data result;result.geometryResult=geometry(d,kind,topology,args,result);
  static_draw::Ref<IDirect3DVertexShader9> vs;static_draw::Ref<IDirect3DPixelShader9> ps;
  auto hr=d->GetVertexShader(&vs.value);if (SUCCEEDED(hr)) { hr=shaderBytes(vs.value,result.vs); }
  auto pixel=d->GetPixelShader(&ps.value);if (SUCCEEDED(pixel)) { pixel=shaderBytes(ps.value,result.ps); }
  result.shaderResult=FAILED(hr)?hr:pixel;
  result.constantsResult=d->GetVertexShaderConstantF(0,result.constants[0].data(),256);
  if (extended) {
  result.integerResult=d->GetVertexShaderConstantI(0,result.integerConstants[0].data(),16);
  result.boolResult=d->GetVertexShaderConstantB(0,result.boolConstants.data(),16);
  result.pixelResult=d->GetPixelShaderConstantF(0,result.pixelConstants[0].data(),32);
  for (UINT i=0;i<3;++i) {
    auto& a=result.auxiliary[i];a.stream=i+1;static_draw::Ref<IDirect3DVertexBuffer9> buffer;
    a.result=d->GetStreamSource(a.stream,&buffer.value,&a.offset,&a.stride);
    if (FAILED(a.result)||!buffer.value) { if (SUCCEEDED(a.result)) { a.result=S_FALSE; }continue; }
    a.result=d->GetStreamSourceFreq(a.stream,&a.frequency);if (FAILED(a.result)) { continue; }
    D3DVERTEXBUFFER_DESC desc{};a.result=buffer.value->GetDesc(&desc);if (FAILED(a.result)) { continue; }
    if (a.offset>=desc.Size) { a.result=E_INVALIDARG;continue; }
    const UINT bytes=std::min(256u,desc.Size-a.offset);a.bytes.resize(bytes);void* pointer=nullptr;
    a.result=buffer.value->Lock(a.offset,bytes,&pointer,D3DLOCK_READONLY);if (FAILED(a.result)) { a.bytes.clear();continue; }
    if (pointer) { std::memcpy(a.bytes.data(),pointer,bytes); }a.result=buffer.value->Unlock();
    if (!pointer || FAILED(a.result)) { a.bytes.clear();if (!pointer && SUCCEEDED(a.result)) { a.result=E_FAIL; } }
  }

  }
  result.result=FAILED(result.geometryResult)?result.geometryResult:FAILED(result.shaderResult)?result.shaderResult:result.constantsResult;
  if (SUCCEEDED(result.geometryResult) && FAILED(result.shaderResult)) { result.reason="shader_bytecode_unavailable"; }
  else if (SUCCEEDED(result.geometryResult) && FAILED(result.constantsResult)) { result.reason="shader_constants_unavailable"; }
  return result;
}
struct Prepared {
  Prepared()=default;Prepared(const Prepared&)=delete;Prepared& operator=(const Prepared&)=delete;
  Prepared(Prepared&& other) noexcept:ready(other.ready),reason(other.reason),result(other.result),bytes(std::move(other.bytes)) { buffer.value=other.buffer.value;other.buffer.value=nullptr; }
  static_draw::Ref<IDirect3DVertexBuffer9> buffer;
  bool ready=false;const char* reason="unprepared";HRESULT result=S_OK;
  std::vector<uint8_t> bytes;
};
inline Prepared prepare(IDirect3DDevice9* d,const draw_observe::Snapshot& snapshot,const Data& input) {
  Prepared out;
  if (FAILED(input.result)) { out.reason=input.reason;out.result=input.result;return out; }
  if (input.stride!=32 || !input.count || uint64_t(input.first)+input.count>kMaxVertices || input.vertices.size()!=size_t(input.count)*32) { out.reason="unexpected_sky_vertex_range";return out; }
  const auto program=sky_shader::parse(input.vs);
  if (!program.valid) { out.reason=program.reason;return out; }
  if (!sky_shader::pixelInputs(input.ps)) { out.reason="unsupported_pixel_inputs";return out; }
  std::array<double,16> inverse{};
  if (!sky_shader::inverseViewProjection(snapshot.view,snapshot.projection,inverse)) { out.reason="singular_view_projection";return out; }
  out.bytes.resize(size_t(input.first+input.count)*32);double firstW=0;
  for (uint32_t i=0;i<input.count;++i) {
    sky_shader::Vec position{0,0,0,1},uv{0,0,0,1},clip{},outputUV{},world{};
    const auto* src=input.vertices.data()+size_t(i)*32;std::memcpy(position.data(),src,12);std::memcpy(uv.data(),src+12,8);
    if (!sky_shader::evaluate(program,input.constants,position,uv,clip,outputUV)) { out.reason="undefined_or_nonfinite_shader_output";out.bytes.clear();return out; }
    double w=0;if (!sky_shader::unproject(clip,inverse,world,w)) { out.reason="invalid_unprojected_position";out.bytes.clear();return out; }
    if (!i) { firstW=w; }
    if (std::abs(w-firstW)>1e-4*std::max(1.0,std::abs(firstW))) { out.reason="varying_homogeneous_scale";out.bytes.clear();return out; }
    auto* dst=out.bytes.data()+size_t(input.first+i)*32;std::memcpy(dst,world.data(),12);std::memcpy(dst+12,outputUV.data(),8);
  }
  out.result=d->CreateVertexBuffer(UINT(out.bytes.size()),D3DUSAGE_WRITEONLY,0,D3DPOOL_DEFAULT,&out.buffer.value,nullptr);
  if (FAILED(out.result)||!out.buffer.value) { out.reason="rebuilt_buffer_create";if (SUCCEEDED(out.result)) { out.result=E_FAIL; }return out; }
  void* memory=nullptr;out.result=out.buffer.value->Lock(0,UINT(out.bytes.size()),&memory,0);
  if (FAILED(out.result)) { out.reason="rebuilt_buffer_lock";return out; }
  if (memory) { std::memcpy(memory,out.bytes.data(),out.bytes.size()); }
  out.result=out.buffer.value->Unlock();if (!memory || FAILED(out.result)) { out.reason="rebuilt_buffer_unlock_or_null";if (SUCCEEDED(out.result)) { out.result=E_FAIL; }return out; }
  out.ready=true;out.reason="reconstructed_original_vs_position_uv";return out;
}
template<class Device,class Draw> HRESULT execute(Device* d,const draw_observe::Snapshot& s,const Prepared& prepared,D3DPRIMITIVETYPE topology,Draw&& draw,static_draw::Report& report) {
  if (!prepared.ready) { return scene_draw::execute(d,s,scene_draw::Mode::Sky,topology,false,draw,report); }
  struct Binding {
    Device* d;static_draw::Ref<IDirect3DVertexBuffer9> old;UINT offset=0,stride=0;bool dirty=false;HRESULT capture=S_OK,restore=S_OK;
    explicit Binding(Device* device):d(device) { capture=d->GetStreamSource(0,&old.value,&offset,&stride);if (SUCCEEDED(capture)&&!old.value) { capture=E_FAIL; } }
    void finish() { if (dirty) { dirty=false;restore=d->SetStreamSource(0,old.value,offset,stride); } }
    ~Binding() { finish(); }
  } binding(d);
  if (FAILED(binding.capture)) { report.code=static_draw::Code::CaptureFailed;report.capture=binding.capture;return draw(); }
  scene_draw::Scope<Device> scope(d,s,scene_draw::Mode::Sky,topology,false);
  if (FAILED(scope.report().restore)) { report=scope.report();return report.restore; }
  if (!scope.report().converted) { report=scope.report();return draw(); }
  binding.dirty=true;const auto applied=d->SetStreamSource(0,prepared.buffer.value,0,32);
  if (FAILED(applied)) {
    binding.finish();scope.finish();report=scope.report();report.converted=false;report.apply=applied;report.code=static_draw::Code::ApplyFailed;
    if (FAILED(binding.restore)) { report.restore=binding.restore;report.code=static_draw::Code::RestoreFailed; }
    return FAILED(report.restore)?report.restore:draw();
  }
  const auto physical=draw();binding.finish();scope.finish();report=scope.report();
  if (FAILED(binding.restore)) { report.restore=binding.restore;report.code=static_draw::Code::RestoreFailed; }
  return FAILED(report.restore)?report.restore:physical;
}
} // namespace ldb_rt::scene_input

#pragma once
#include "rt_draw_packet.h"
#include "rt_static_draw.h"

namespace ldb_rt::scene_draw {
enum class Mode : uint32_t { None, Sky, Rigid };
constexpr auto kCategoryState=static_cast<D3DRENDERSTATETYPE>(42);
constexpr DWORD kUnusedCategory=0xfefefefe, kSkyCategory=1u<<2;
inline bool rigid(const std::array<float,16>& m) noexcept {
  for (float x:m) { if (!std::isfinite(x)) { return false; } }
  if (std::abs(m[3])+std::abs(m[7])+std::abs(m[11])+std::abs(m[15]-1)>1e-5f) { return false; }
  for (unsigned i=0;i<3;++i) { for (unsigned j=0;j<3;++j) {
    double dot=0;for (unsigned k=0;k<3;++k) { dot+=double(m[i*4+k])*m[j*4+k]; }
    if (std::abs(dot-double(i==j))>.01) { return false; }
  } }
  const double determinant=double(m[0])*(double(m[5])*m[10]-double(m[6])*m[9])-
    double(m[1])*(double(m[4])*m[10]-double(m[6])*m[8])+double(m[2])*(double(m[4])*m[9]-double(m[5])*m[8]);
  return std::abs(determinant-1)<.02;
}
inline Mode candidate(const draw_observe::Snapshot& s,bool sky,bool props) noexcept {
  if (s.status!=draw_observe::Status::Valid || !s.material || !s.materialTable || s.materialInfo.flags!=3 ||
      !rigid(s.world) || !rigid(s.view) || !invertible(s.projection) || s.minZ!=0 || s.maxZ!=1 ||
      s.viewport[0] || s.viewport[1] || !s.viewport[2] || !s.viewport[3] ||
      std::abs(s.projection[11]+1)>1e-5f || std::abs(s.projection[15])>1e-5f ||
      s.projection[0]<=0 || s.projection[5]<=0 || s.projection[10]>=-1) { return Mode::None; }
  const float nearPlane=s.projection[14]/s.projection[10];
  if (!std::isfinite(nearPlane) || nearPlane<4 || nearPlane>32) { return Mode::None; }
  if (sky && s.materialInfo.format==0x80001 && !std::strcmp(s.materialInfo.shader.data(),"NekoSky")) {
    for (unsigned i=0;i<16;++i) { if (std::abs(s.world[i]-float(i%5==0))>1e-6f) { return Mode::None; } }
    return Mode::Sky;
  }
  if (props && (s.materialInfo.format==0x80503 || s.materialInfo.format==0xa0403) &&
      !std::strcmp(s.materialInfo.shader.data(),"VertexLitGeneric")) { return Mode::Rigid; }
  return Mode::None;
}
template<class Device> class Scope final {
  Device* d;
  Mode mode;
  static_draw::Ref<IDirect3DVertexShader9> vs;
  static_draw::Ref<IDirect3DPixelShader9> ps;
  static_draw::Ref<IDirect3DVertexDeclaration9> original,canonical;
  static_draw::Ref<IDirect3DVertexBuffer9> vb;
  D3DMATRIX oldWorld{},oldView{},oldProjection{};
  DWORD category=0,fog=0,lighting=0;
  bool dirty=false;
  static_draw::Report result;
  bool get(HRESULT hr) { if (FAILED(hr)) { result.capture=hr;result.code=static_draw::Code::CaptureFailed;return false; }return true; }
  bool set(HRESULT hr) { if (FAILED(hr)) { result.apply=hr;result.code=static_draw::Code::ApplyFailed;finish();return false; }return true; }
public:
  Scope(Device* device,const draw_observe::Snapshot& s,Mode selected,D3DPRIMITIVETYPE topology,bool up):d(device),mode(selected) {
    if (!d || selected==Mode::None || candidate(s,selected==Mode::Sky,selected==Mode::Rigid)!=selected) { result.code=static_draw::Code::Unlabelled;return; }
    if (up || (topology!=D3DPT_TRIANGLELIST && topology!=D3DPT_TRIANGLESTRIP)) { result.code=static_draw::Code::Topology;return; }
    static_draw::Ref<IDirect3DSurface9> target;D3DSURFACE_DESC desc{};D3DVIEWPORT9 vp{};
    if (!get(d->GetRenderTarget(0,&target.value))) { return; }
    if (!target.value) { result.code=static_draw::Code::Pipeline;return; }
    if (!get(target.value->GetDesc(&desc)) || !get(d->GetViewport(&vp))) { return; }
    if (desc.Format!=D3DFMT_A16B16G16R16F || desc.Width!=s.viewport[2] || desc.Height!=s.viewport[3] ||
        vp.X || vp.Y || vp.Width!=desc.Width || vp.Height!=desc.Height || vp.MinZ!=0 || vp.MaxZ!=1) { result.code=static_draw::Code::Pipeline;return; }
    DWORD alpha=0,test=0,depth=0,write=0;
    if (!get(d->GetRenderState(D3DRS_ALPHABLENDENABLE,&alpha)) || !get(d->GetRenderState(D3DRS_ALPHATESTENABLE,&test)) ||
        !get(d->GetRenderState(D3DRS_ZENABLE,&depth)) || !get(d->GetRenderState(D3DRS_ZWRITEENABLE,&write))) { return; }
    const DWORD expectedDepth=selected==Mode::Sky?0u:1u;
    if (alpha || test || depth!=expectedDepth || write!=expectedDepth) { result.code=static_draw::Code::Pipeline;return; }
    if (!get(d->GetVertexDeclaration(&original.value))) { return; }
    if (!original.value) { result.code=static_draw::Code::Layout;return; }
    D3DVERTEXELEMENT9 elements[MAXD3DDECLLENGTH+1]{};UINT count=MAXD3DDECLLENGTH+1;
    if (!get(original.value->GetDeclaration(elements,&count))) { return; }
    const D3DVERTEXELEMENT9 skyDecl[]={
      {0,0,D3DDECLTYPE_FLOAT3,0,D3DDECLUSAGE_POSITION,0},{0,12,D3DDECLTYPE_FLOAT2,0,D3DDECLUSAGE_TEXCOORD,0},
      {2,0,D3DDECLTYPE_FLOAT4,0,D3DDECLUSAGE_POSITION,1},{2,16,D3DDECLTYPE_FLOAT3,0,D3DDECLUSAGE_NORMAL,1},D3DDECL_END()};
    const D3DVERTEXELEMENT9 propDecl[]={
      {0,0,D3DDECLTYPE_FLOAT3,0,D3DDECLUSAGE_POSITION,0},{0,12,D3DDECLTYPE_UBYTE4,0,D3DDECLUSAGE_NORMAL,0},
      {0,16,D3DDECLTYPE_FLOAT2,0,D3DDECLUSAGE_TEXCOORD,0},{0,12,D3DDECLTYPE_UBYTE4,0,D3DDECLUSAGE_TANGENT,0},
      {1,0,D3DDECLTYPE_D3DCOLOR,0,D3DDECLUSAGE_COLOR,1},{2,0,D3DDECLTYPE_FLOAT4,0,D3DDECLUSAGE_POSITION,1},
      {2,16,D3DDECLTYPE_UBYTE4,0,D3DDECLUSAGE_NORMAL,1},{3,0,D3DDECLTYPE_FLOAT1,0,D3DDECLUSAGE_POSITION,2},D3DDECL_END()};
    const D3DVERTEXELEMENT9 propAlt[]={propDecl[0],propDecl[1],propDecl[2],propDecl[3],propDecl[5],propDecl[6],propDecl[7],D3DDECL_END()};
    const bool layout=selected==Mode::Sky ? count==5 && !std::memcmp(elements,skyDecl,sizeof(skyDecl)) :
      s.materialInfo.format==0x80503 ? count==9 && !std::memcmp(elements,propDecl,sizeof(propDecl)) :
      count==8 && !std::memcmp(elements,propAlt,sizeof(propAlt));
    UINT offset=0,stride=0,frequency=0;
    if (!layout) { result.code=static_draw::Code::Layout;return; }
    if (!get(d->GetStreamSource(0,&vb.value,&offset,&stride)) || !get(d->GetStreamSourceFreq(0,&frequency))) { return; }
    if (!vb.value || stride!=32 || offset%4 || frequency!=1) { result.code=static_draw::Code::Layout;return; }
    if (!get(d->GetVertexShader(&vs.value)) || (selected==Mode::Rigid && !get(d->GetPixelShader(&ps.value))) ||
        !get(d->GetTransform(D3DTS_WORLD,&oldWorld)) || !get(d->GetTransform(D3DTS_VIEW,&oldView)) ||
        !get(d->GetTransform(D3DTS_PROJECTION,&oldProjection)) || !get(d->GetRenderState(D3DRS_LIGHTING,&lighting))) { return; }
    if (selected==Mode::Sky && (!get(d->GetRenderState(kCategoryState,&category)) || !get(d->GetRenderState(D3DRS_FOGENABLE,&fog)))) { return; }
    // Omit Source's packed normal/tangent and all bone/morph/auxiliary input.
    // The pinned ray-hit shader uses geometric triangle normals when absent.
    const D3DVERTEXELEMENT9 simple[]={skyDecl[0],{0,WORD(selected==Mode::Sky?12:16),D3DDECLTYPE_FLOAT2,0,D3DDECLUSAGE_TEXCOORD,0},D3DDECL_END()};
    if (!get(d->CreateVertexDeclaration(simple,&canonical.value))) { return; }
    if (!canonical.value) { result.code=static_draw::Code::CaptureFailed;result.capture=E_FAIL;return; }
    D3DMATRIX world{},view{},projection{};std::memcpy(&world,s.world.data(),sizeof(world));
    std::memcpy(&view,s.view.data(),sizeof(view));std::memcpy(&projection,s.projection.data(),sizeof(projection));
    dirty=true;
    if (!set(d->SetVertexShader(nullptr)) || (selected==Mode::Rigid && !set(d->SetPixelShader(nullptr))) ||
        !set(d->SetVertexDeclaration(canonical.value)) || !set(d->SetTransform(D3DTS_WORLD,&world)) ||
        !set(d->SetTransform(D3DTS_VIEW,&view)) || !set(d->SetTransform(D3DTS_PROJECTION,&projection)) ||
        !set(d->SetRenderState(D3DRS_LIGHTING,FALSE))) { return; }
    if (selected==Mode::Sky && (!set(d->SetRenderState(kCategoryState,(category==kUnusedCategory?0:category)|kSkyCategory)) ||
        !set(d->SetRenderState(D3DRS_FOGENABLE,FALSE)))) { return; }
    result.code=static_draw::Code::Applied;result.converted=true;
  }
  Scope(const Scope&)=delete;Scope& operator=(const Scope&)=delete;
  ~Scope() { finish(); }
  void finish() noexcept {
    if (!dirty) { return; }dirty=false;
    const auto restore=[&](HRESULT hr) { if (FAILED(hr) && SUCCEEDED(result.restore)) { result.restore=hr; } };
    restore(d->SetTransform(D3DTS_WORLD,&oldWorld));restore(d->SetTransform(D3DTS_VIEW,&oldView));restore(d->SetTransform(D3DTS_PROJECTION,&oldProjection));
    restore(d->SetVertexDeclaration(original.value));restore(d->SetVertexShader(vs.value));
    if (mode==Mode::Rigid) { restore(d->SetPixelShader(ps.value)); }
    restore(d->SetRenderState(D3DRS_LIGHTING,lighting));
    if (mode==Mode::Sky) { restore(d->SetRenderState(kCategoryState,category));restore(d->SetRenderState(D3DRS_FOGENABLE,fog)); }
    if (FAILED(result.restore)) { result.code=static_draw::Code::RestoreFailed; }
  }
  const static_draw::Report& report() const noexcept { return result; }
};
template<class Device,class Draw> HRESULT execute(Device* d,const draw_observe::Snapshot& s,Mode mode,
    D3DPRIMITIVETYPE topology,bool up,Draw&& draw,static_draw::Report& report) {
  Scope<Device> scope(d,s,mode,topology,up);
  if (FAILED(scope.report().restore)) { report=scope.report();return report.restore; }
  const auto hr=draw();scope.finish();report=scope.report();return FAILED(report.restore)?report.restore:hr;
}
} // namespace ldb_rt::scene_draw

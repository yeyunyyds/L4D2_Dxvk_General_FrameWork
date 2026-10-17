#pragma once
#include "rt_draw_packet.h"
#include "rt_static_draw.h"

namespace ldb_rt::overlay {
inline bool identity(const std::array<float,16>& m) noexcept {
  for (size_t i=0;i<m.size();++i) {
    if (!std::isfinite(m[i]) || std::abs(m[i]-float(i%5==0))>1e-6f) { return false; }
  }
  return true;
}
inline bool candidate(const draw_observe::Snapshot& s, D3DPRIMITIVETYPE type, bool userMemory) noexcept {
  return !userMemory && type==D3DPT_TRIANGLELIST && s.status==draw_observe::Status::Valid &&
    s.material && s.materialTable && s.materialInfo.flags==3 && s.materialInfo.format==0x80001 &&
    !std::strcmp(s.materialInfo.shader.data(),"Neko_Engine_Post") && identity(s.world) && identity(s.view) &&
    identity(s.projection) && s.minZ==0 && s.maxZ==1 && s.viewport[0]==0 && s.viewport[1]==0 && s.viewport[2] && s.viewport[3];
}
struct Result {
  static_draw::Report report;
  HRESULT draw = E_ABORT;
  bool replaced = false;
};
// One owner-thread transparent UI draw triggers the pinned runtime's native
// orthographic/pretransformed injection path. D3DSBT_ALL also restores stream 0,
// which DrawPrimitiveUP clears. No Source calls, fade-color reads or globals.
template<class Device> Result inject(Device* device, const draw_observe::Snapshot& s) noexcept {
  Result result;
  auto& r=result.report;
  auto capture=[&](HRESULT hr) {
    if (FAILED(hr)) { r.capture=hr;r.code=static_draw::Code::CaptureFailed;return false; }
    return true;
  };
  static_draw::Ref<IDirect3DSurface9> target,backbuffer;
  D3DSURFACE_DESC desc{};D3DVIEWPORT9 vp{};
  if (!capture(device->GetRenderTarget(0,&target.value)) || !target.value ||
      !capture(device->GetBackBuffer(0,0,D3DBACKBUFFER_TYPE_MONO,&backbuffer.value)) || !backbuffer.value ||
      !capture(target.value->GetDesc(&desc)) || !capture(device->GetViewport(&vp))) { return result; }
  r.code=static_draw::Code::Pipeline;
  if (target.value!=backbuffer.value || desc.Format!=D3DFMT_A8R8G8B8 || desc.Width!=s.viewport[2] || desc.Height!=s.viewport[3] ||
      vp.X || vp.Y || vp.Width!=desc.Width || vp.Height!=desc.Height || vp.MinZ!=0 || vp.MaxZ!=1) { return result; }
  for (const auto name : {D3DRS_ALPHABLENDENABLE,D3DRS_ALPHATESTENABLE,D3DRS_ZENABLE,D3DRS_ZWRITEENABLE}) {
    DWORD value=1;if (!capture(device->GetRenderState(name,&value)) || value) { return result; }
  }
  static_draw::Ref<IDirect3DVertexDeclaration9> declaration;
  static_draw::Ref<IDirect3DVertexBuffer9> buffer;
  D3DVERTEXELEMENT9 elements[MAXD3DDECLLENGTH+1]{};UINT count=MAXD3DDECLLENGTH+1,offset=0,stride=0,freq=0;
  if (!capture(device->GetVertexDeclaration(&declaration.value)) || !declaration.value ||
      !capture(declaration.value->GetDeclaration(elements,&count)) ||
      !capture(device->GetStreamSource(0,&buffer.value,&offset,&stride)) ||
      !capture(device->GetStreamSourceFreq(0,&freq))) { return result; }
  const D3DVERTEXELEMENT9 expected[]={{0,0,D3DDECLTYPE_FLOAT3,0,D3DDECLUSAGE_POSITION,0},
    {0,12,D3DDECLTYPE_FLOAT2,0,D3DDECLUSAGE_TEXCOORD,0},
    {2,0,D3DDECLTYPE_FLOAT4,0,D3DDECLUSAGE_POSITION,1},
    {2,16,D3DDECLTYPE_FLOAT3,0,D3DDECLUSAGE_NORMAL,1},D3DDECL_END()};
  r.code=static_draw::Code::Layout;
  if (!buffer.value || count!=5 || std::memcmp(elements,expected,sizeof(expected)) || stride!=32 || freq!=1 || offset%4) { return result; }
  D3DMATRIX saved[3]{};const D3DTRANSFORMSTATETYPE transforms[]={D3DTS_WORLD,D3DTS_VIEW,D3DTS_PROJECTION};
  for (unsigned i=0;i<3;++i) { if (!capture(device->GetTransform(transforms[i],&saved[i]))) { return result; } }
  static_draw::Ref<IDirect3DVertexShader9> vs;static_draw::Ref<IDirect3DPixelShader9> ps;
  if (!capture(device->GetVertexShader(&vs.value)) || !capture(device->GetPixelShader(&ps.value))) { return result; }
  const std::pair<D3DRENDERSTATETYPE,DWORD> states[]={{D3DRS_ZENABLE,FALSE},{D3DRS_ZWRITEENABLE,FALSE},
    {D3DRS_ALPHABLENDENABLE,TRUE},{D3DRS_ALPHATESTENABLE,FALSE},{D3DRS_SRCBLEND,D3DBLEND_SRCALPHA},
    {D3DRS_DESTBLEND,D3DBLEND_INVSRCALPHA},{D3DRS_BLENDOP,D3DBLENDOP_ADD},{D3DRS_SEPARATEALPHABLENDENABLE,FALSE},
    {D3DRS_FOGENABLE,FALSE},{D3DRS_CULLMODE,D3DCULL_NONE},{D3DRS_STENCILENABLE,FALSE},
    {D3DRS_SCISSORTESTENABLE,FALSE},{D3DRS_COLORWRITEENABLE,7}};
  const std::pair<D3DTEXTURESTAGESTATETYPE,DWORD> stage[]={{D3DTSS_COLOROP,D3DTOP_SELECTARG1},
    {D3DTSS_COLORARG1,D3DTA_DIFFUSE},{D3DTSS_ALPHAOP,D3DTOP_SELECTARG1},{D3DTSS_ALPHAARG1,D3DTA_DIFFUSE}};
  constexpr size_t stateCount=sizeof(states)/sizeof(states[0]),stageCount=sizeof(stage)/sizeof(stage[0]);
  DWORD oldStates[stateCount]{},oldStage0[stageCount]{},oldStage1[2]{};
  static_draw::Ref<IDirect3DBaseTexture9> texture;
  for (size_t i=0;i<stateCount;++i) { if (!capture(device->GetRenderState(states[i].first,&oldStates[i]))) { return result; } }
  for (size_t i=0;i<stageCount;++i) { if (!capture(device->GetTextureStageState(0,stage[i].first,&oldStage0[i]))) { return result; } }
  if (!capture(device->GetTextureStageState(1,D3DTSS_COLOROP,&oldStage1[0])) ||
      !capture(device->GetTextureStageState(1,D3DTSS_ALPHAOP,&oldStage1[1])) ||
      !capture(device->GetTexture(0,&texture.value))) { return result; }
  static_draw::Ref<IDirect3DStateBlock9> block;
  if (!capture(device->CreateStateBlock(D3DSBT_ALL,&block.value)) || !block.value || !capture(block.value->Capture())) { return result; }
  auto set=[&](HRESULT hr) {
    if (FAILED(hr)) { r.apply=hr;r.code=static_draw::Code::ApplyFailed;return false; }
    return true;
  };
  bool ok=set(device->SetVertexShader(nullptr)) && set(device->SetPixelShader(nullptr)) &&
    set(device->SetFVF(D3DFVF_XYZRHW|D3DFVF_DIFFUSE));
  for (const auto& state:states) { if (ok) { ok=set(device->SetRenderState(state.first,state.second)); } }
  if (ok) { ok=set(device->SetTexture(0,nullptr)); }
  for (const auto& state:stage) { if (ok) { ok=set(device->SetTextureStageState(0,state.first,state.second)); } }
  if (ok) { ok=set(device->SetTextureStageState(1,D3DTSS_COLOROP,D3DTOP_DISABLE)) && set(device->SetTextureStageState(1,D3DTSS_ALPHAOP,D3DTOP_DISABLE)); }
  D3DMATRIX matrix{};for (unsigned i=0;i<4;++i) { matrix.m[i][i]=1; }
  for (auto transform:{D3DTS_WORLD,D3DTS_VIEW,D3DTS_PROJECTION}) { if (ok) { ok=set(device->SetTransform(transform,&matrix)); } }
  if (ok) {
    struct Vertex { float x,y,z,rhw;DWORD color; };
    const Vertex vertices[]={{-.5f,-.5f,0,1,0},{float(desc.Width)-.5f,-.5f,0,1,0},{-.5f,float(desc.Height)-.5f,0,1,0}};
    result.draw=device->DrawPrimitiveUP(D3DPT_TRIANGLELIST,1,vertices,sizeof(Vertex));
    r.code=SUCCEEDED(result.draw)?static_draw::Code::Applied:static_draw::Code::ApplyFailed;
    if (FAILED(result.draw)) { r.apply=result.draw; }
  }
  r.restore=block.value->Apply();
  // Match the established D3D9 UI backend workaround: some system drivers do
  // not restore all requested fields reliably from D3DSBT_ALL. Independently
  // restore every public field touched by this scope, checking every result.
  auto restore=[&](HRESULT hr) { if (FAILED(hr) && SUCCEEDED(r.restore)) { r.restore=hr; } };
  for (unsigned i=0;i<3;++i) { restore(device->SetTransform(transforms[i],&saved[i])); }
  restore(device->SetStreamSource(0,buffer.value,offset,stride));
  restore(device->SetVertexDeclaration(declaration.value));
  restore(device->SetVertexShader(vs.value));restore(device->SetPixelShader(ps.value));
  for (size_t i=0;i<stateCount;++i) { restore(device->SetRenderState(states[i].first,oldStates[i])); }
  restore(device->SetTexture(0,texture.value));
  for (size_t i=0;i<stageCount;++i) { restore(device->SetTextureStageState(0,stage[i].first,oldStage0[i])); }
  restore(device->SetTextureStageState(1,D3DTSS_COLOROP,oldStage1[0]));
  restore(device->SetTextureStageState(1,D3DTSS_ALPHAOP,oldStage1[1]));
  if (FAILED(r.restore)) { r.code=static_draw::Code::RestoreFailed; }
  result.replaced=ok && SUCCEEDED(result.draw) && SUCCEEDED(r.restore);
  return result;
}
} // namespace ldb_rt::overlay

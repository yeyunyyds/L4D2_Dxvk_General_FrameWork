#pragma once
#include "rt_scene_input.h"

namespace ldb_rt::bulk_static {
inline unsigned family(const char* shader) noexcept {
  if (!std::strcmp(shader,"VertexLitGeneric")) { return 1; }
  if (!std::strcmp(shader,"UnlitGeneric")) { return 2; }
  if (!std::strcmp(shader,"LightmappedGeneric") || !std::strcmp(shader,"LightmappedGeneric_DX9")) { return 3; }
  if (!std::strcmp(shader,"WorldVertexTransition") || !std::strcmp(shader,"WorldVertexTransition_DX9")) { return 4; }
  if (!std::strcmp(shader,"WorldTwoTextureBlend")) { return 5; }
  if (!std::strcmp(shader,"NekoToon")) { return 6; }
  if (!std::strcmp(shader,"Infected")) { return 7; }
  return 0;
}
inline bool affine(const std::array<float,16>& m) noexcept {
  for (float v:m) { if (!std::isfinite(v) || std::abs(v)>1e7f) { return false; } }
  if (std::abs(m[3])+std::abs(m[7])+std::abs(m[11])+std::abs(m[15]-1)>1e-5f) { return false; }
  const double det=double(m[0])*(double(m[5])*m[10]-double(m[6])*m[9])-
    double(m[1])*(double(m[4])*m[10]-double(m[6])*m[8])+double(m[2])*(double(m[4])*m[9]-double(m[5])*m[8]);
  return det>1e-8 && det<1e12;
}
inline bool mainCamera(const draw_observe::Snapshot& s) noexcept {
  if (s.status!=draw_observe::Status::Valid || !s.material || !s.materialTable || s.materialInfo.flags!=3 ||
      !affine(s.world) || !scene_draw::rigid(s.view) || !invertible(s.projection) ||
      s.viewport[0] || s.viewport[1] || !s.viewport[2] || !s.viewport[3] || s.minZ!=0 || s.maxZ!=1) { return false; }
  const auto& p=s.projection;
  return std::abs(p[11]+1)<=1e-5f && std::abs(p[15])<=1e-5f && p[0]>0 && p[5]>0 && p[10]<-1 && p[14]/p[10]>=4 && p[14]/p[10]<=32;
}
inline bool candidate(const draw_observe::Snapshot& s) noexcept { return family(s.materialInfo.shader.data()) && mainCamera(s); }
// This family always goes through the full VS/PS identity and model input gate.
inline bool customModel(const draw_observe::Snapshot& s) noexcept { return family(s.materialInfo.shader.data())==6; }
inline unsigned elementBytes(BYTE type) noexcept {
  switch (type) {
    case D3DDECLTYPE_FLOAT1:return 4;case D3DDECLTYPE_FLOAT2:return 8;
    case D3DDECLTYPE_FLOAT3:return 12;case D3DDECLTYPE_FLOAT4:return 16;
    case D3DDECLTYPE_D3DCOLOR:case D3DDECLTYPE_UBYTE4:case D3DDECLTYPE_UBYTE4N:
    case D3DDECLTYPE_SHORT2:case D3DDECLTYPE_SHORT2N:case D3DDECLTYPE_USHORT2N:
    case D3DDECLTYPE_UDEC3:case D3DDECLTYPE_DEC3N:case D3DDECLTYPE_FLOAT16_2:return 4;
    case D3DDECLTYPE_SHORT4:case D3DDECLTYPE_SHORT4N:case D3DDECLTYPE_USHORT4N:case D3DDECLTYPE_FLOAT16_4:return 8;
    default:return 0;
  }
}
// Semantic checks allow whole layout families, not a stride-only guess. Source
// auxiliary morph declarations are recorded but not executed by this static path.
inline bool layout(const D3DVERTEXELEMENT9* e,UINT count,UINT stride,std::vector<D3DVERTEXELEMENT9>& canonical) {
  canonical.clear();bool position=false,uv=false;
  const D3DVERTEXELEMENT9 end=D3DDECL_END();
  if (count<3 || count>MAXD3DDECLLENGTH+1 || !stride || stride>128 || stride%4 || std::memcmp(e+count-1,&end,sizeof(end))) { return false; }
  for (UINT i=0;i+1<count;++i) {
    const auto& x=e[i];const auto bytes=elementBytes(x.Type);
    if (x.Method!=D3DDECLMETHOD_DEFAULT || !bytes || x.Usage==D3DDECLUSAGE_BLENDWEIGHT ||
        x.Usage==D3DDECLUSAGE_BLENDINDICES || x.Usage==D3DDECLUSAGE_POSITIONT) { return false; }
    if (x.Stream) {
      const bool known=(x.Stream==1 && x.Usage==D3DDECLUSAGE_COLOR && x.UsageIndex==1 && x.Offset==0 && x.Type==D3DDECLTYPE_D3DCOLOR) ||
        (x.Stream==2 && ((x.Usage==D3DDECLUSAGE_POSITION && x.UsageIndex==1 && x.Offset==0 && x.Type==D3DDECLTYPE_FLOAT4) ||
        (x.Usage==D3DDECLUSAGE_NORMAL && x.UsageIndex==1 && x.Offset==16 && (x.Type==D3DDECLTYPE_FLOAT3 || x.Type==D3DDECLTYPE_UBYTE4)))) ||
        (x.Stream==3 && x.Usage==D3DDECLUSAGE_POSITION && x.UsageIndex==2 && x.Offset==0 && x.Type==D3DDECLTYPE_FLOAT1);
      if (!known) { return false; }continue;
    }
    if (x.Offset%4 || unsigned(x.Offset)+bytes>stride) { return false; }
    if (x.Usage==D3DDECLUSAGE_POSITION) {
      if (position || x.UsageIndex || x.Type!=D3DDECLTYPE_FLOAT3) { return false; }position=true;canonical.insert(canonical.begin(),x);
    } else if (x.Usage==D3DDECLUSAGE_TEXCOORD) {
      if (x.UsageIndex>7 || (x.UsageIndex?x.Type>D3DDECLTYPE_FLOAT4:x.Type!=D3DDECLTYPE_FLOAT2)) { return false; }
      if (!x.UsageIndex) { if (uv) { return false; }uv=true;canonical.push_back(x); }
    } else if ((x.Usage!=D3DDECLUSAGE_NORMAL && x.Usage!=D3DDECLUSAGE_TANGENT && x.Usage!=D3DDECLUSAGE_BINORMAL && x.Usage!=D3DDECLUSAGE_COLOR) || x.UsageIndex) { return false; }
  }
  if (!position || !uv) { return false; }canonical.push_back(end);return true;
}
struct Stage { DWORD stage;D3DTEXTURESTAGESTATETYPE name;DWORD value; };
constexpr Stage stages[]={{0,D3DTSS_COLOROP,D3DTOP_SELECTARG1},{0,D3DTSS_COLORARG1,D3DTA_TEXTURE},
  {0,D3DTSS_ALPHAOP,D3DTOP_SELECTARG1},{0,D3DTSS_ALPHAARG1,D3DTA_TEXTURE},{0,D3DTSS_TEXCOORDINDEX,0},
  {0,D3DTSS_TEXTURETRANSFORMFLAGS,D3DTTFF_DISABLE},{0,D3DTSS_RESULTARG,D3DTA_CURRENT},
  {1,D3DTSS_COLOROP,D3DTOP_DISABLE},{1,D3DTSS_ALPHAOP,D3DTOP_DISABLE}};
template<class Device> class Scope final {
  Device* d;static_draw::Report result;bool dirty=false,textureChanged=false,neutralMaterial=false;
  static_draw::Ref<IDirect3DVertexShader9> vs;static_draw::Ref<IDirect3DPixelShader9> ps;
  static_draw::Ref<IDirect3DVertexDeclaration9> decl,generated;static_draw::Ref<IDirect3DVertexBuffer9> vb;
  static_draw::Ref<IDirect3DIndexBuffer9> ib;
  static_draw::Ref<IDirect3DBaseTexture9> originalTexture,sourceTexture;
  D3DMATRIX old[3]{};DWORD lighting=0,blend=0,indexed=0,tss[9]{},textureFactor=0;
  UINT oldOffset=0,oldStride=0;bool streamChanged=false,indexChanged=false;
  bool get(HRESULT hr) { if (FAILED(hr)) { result.capture=hr;result.code=static_draw::Code::CaptureFailed;return false; }return true; }
  bool set(HRESULT hr) { if (FAILED(hr)) { result.apply=hr;result.code=static_draw::Code::ApplyFailed;finish();return false; }return true; }
public:
  Scope(Device* device,const draw_observe::Snapshot& s,D3DPRIMITIVETYPE type,bool up,const scene_input::Prepared* rebuilt=nullptr,bool neutral=false):d(device) {
    neutralMaterial=neutral && rebuilt;
    if (!d || !candidate(s)) { result.code=static_draw::Code::Unlabelled;return; }
    if (up || (type!=D3DPT_TRIANGLELIST && type!=D3DPT_TRIANGLESTRIP)) { result.code=static_draw::Code::Topology;return; }
    static_draw::Ref<IDirect3DSurface9> target;D3DSURFACE_DESC surface{};D3DVIEWPORT9 viewport{};DWORD alpha=0,z=0,write=0;
    if (!get(d->GetRenderTarget(0,&target.value)) || !get(d->GetViewport(&viewport))) { return; }
    if (!target.value) { result.code=static_draw::Code::Pipeline;return; }
    if (!get(target.value->GetDesc(&surface)) || !get(d->GetRenderState(D3DRS_ALPHABLENDENABLE,&alpha)) ||
        !get(d->GetRenderState(D3DRS_ZENABLE,&z)) || !get(d->GetRenderState(D3DRS_ZWRITEENABLE,&write))) { return; }
    if (alpha || z!=1 || write!=1 || surface.Format!=D3DFMT_A16B16G16R16F || surface.Width!=s.viewport[2] || surface.Height!=s.viewport[3] ||
        viewport.X || viewport.Y || viewport.Width!=surface.Width || viewport.Height!=surface.Height || viewport.MinZ!=0 || viewport.MaxZ!=1) { result.code=static_draw::Code::Pipeline;return; }
    if (!get(d->GetVertexDeclaration(&decl.value))) { return; }
    if (!decl.value) { result.code=static_draw::Code::Layout;return; }
    D3DVERTEXELEMENT9 e[MAXD3DDECLLENGTH+1]{};UINT n=MAXD3DDECLLENGTH+1,offset=0,stride=0,frequency=0;
    if (!get(decl.value->GetDeclaration(e,&n)) || !get(d->GetStreamSource(0,&vb.value,&offset,&stride)) || !get(d->GetStreamSourceFreq(0,&frequency))) { return; }
    std::vector<D3DVERTEXELEMENT9> simple;
    if (!vb.value || offset%4 || frequency!=1) { result.code=static_draw::Code::Layout;return; }
    if ((customModel(s) || family(s.materialInfo.shader.data())==7) && !rebuilt) { result.code=static_draw::Code::Layout;return; }
    if (rebuilt) {
      if (!rebuilt->ready || !rebuilt->buffer.value || !affine(rebuilt->world) || stride!=32 || n!=rebuilt->sourceDeclaration.size() ||
          std::memcmp(e,rebuilt->sourceDeclaration.data(),n*sizeof(*e))) { result.code=static_draw::Code::Layout;return; }
      simple=rebuilt->elements;oldOffset=offset;oldStride=stride;streamChanged=true;
      indexChanged=rebuilt->ownedDraw && !rebuilt->indices.empty();
      if (rebuilt->ownedDraw && (!rebuilt->uploadVerified || (indexChanged && !rebuilt->indexBuffer.value))) { result.code=static_draw::Code::Layout;return; }
      if (indexChanged && !get(d->GetIndices(&ib.value))) { return; }
    } else if (!layout(e,n,stride,simple)) { result.code=static_draw::Code::Layout;return; }
    if (!get(d->GetTexture(0,&originalTexture.value))) { return; }
    textureChanged=family(s.materialInfo.shader.data())==4;
    auto* texture=originalTexture.value;
    if (textureChanged) { if (!get(d->GetTexture(7,&sourceTexture.value))) { return; }texture=sourceTexture.value; }
    if (!texture || texture->GetType()!=D3DRTYPE_TEXTURE) { result.code=static_draw::Code::Pipeline;return; }
    if (!get(d->GetVertexShader(&vs.value)) || !get(d->GetPixelShader(&ps.value)) || !get(d->GetRenderState(D3DRS_LIGHTING,&lighting)) ||
        !get(d->GetRenderState(D3DRS_VERTEXBLEND,&blend)) || !get(d->GetRenderState(D3DRS_INDEXEDVERTEXBLENDENABLE,&indexed))) { return; }
    if (neutralMaterial && !get(d->GetRenderState(D3DRS_TEXTUREFACTOR,&textureFactor))) { return; }
    const D3DTRANSFORMSTATETYPE transforms[]={D3DTS_WORLD,D3DTS_VIEW,D3DTS_PROJECTION};
    for (unsigned i=0;i<3;++i) { if (!get(d->GetTransform(transforms[i],old+i))) { return; } }
    for (unsigned i=0;i<9;++i) { if (!get(d->GetTextureStageState(stages[i].stage,stages[i].name,tss+i))) { return; } }
    if (!get(d->CreateVertexDeclaration(simple.data(),&generated.value))) { return; }
    if (!generated.value) { result.capture=E_FAIL;result.code=static_draw::Code::CaptureFailed;return; }
    dirty=true;
    if (!set(d->SetVertexShader(nullptr)) || !set(d->SetPixelShader(nullptr)) || !set(d->SetVertexDeclaration(generated.value)) || !set(d->SetRenderState(D3DRS_LIGHTING,FALSE)) ||
        !set(d->SetRenderState(D3DRS_VERTEXBLEND,D3DVBF_DISABLE)) || !set(d->SetRenderState(D3DRS_INDEXEDVERTEXBLENDENABLE,FALSE))) { return; }
    if (streamChanged && !set(d->SetStreamSource(0,rebuilt->buffer.value,0,rebuilt->stride))) { return; }
    if (indexChanged && !set(d->SetIndices(rebuilt->indexBuffer.value))) { return; }
    const std::array<float,16>* matrices[]={rebuilt?&rebuilt->world:&s.world,&s.view,&s.projection};
    for (unsigned i=0;i<3;++i) { D3DMATRIX m{};std::memcpy(&m,matrices[i]->data(),sizeof(m));if (!set(d->SetTransform(transforms[i],&m))) { return; } }
    if (textureChanged && !set(d->SetTexture(0,sourceTexture.value))) { return; }
    if (neutralMaterial && !set(d->SetRenderState(D3DRS_TEXTUREFACTOR,0xff808080u))) { return; }
    for (const auto& x:stages) {
      const DWORD value=neutralMaterial && !x.stage && x.name==D3DTSS_COLORARG1?D3DTA_TFACTOR:x.value;
      if (!set(d->SetTextureStageState(x.stage,x.name,value))) { return; }
    }
    if (rebuilt && rebuilt->ownedDraw) {
      // Check what the public device actually bound, after all overrides and
      // before Draw. These mapped uploads are not a GPU/BLAS readback claim.
      static_draw::Ref<IDirect3DVertexBuffer9> boundVB;static_draw::Ref<IDirect3DIndexBuffer9> boundIB;
      static_draw::Ref<IDirect3DVertexDeclaration9> boundDecl;
      static_draw::Ref<IDirect3DVertexShader9> boundVS;static_draw::Ref<IDirect3DPixelShader9> boundPS;
      UINT boundOffset=0,boundStride=0,frequencyValue=0;D3DMATRIX boundWorld{};
      const HRESULT queries[]={d->GetStreamSource(0,&boundVB.value,&boundOffset,&boundStride),d->GetIndices(&boundIB.value),
        d->GetVertexDeclaration(&boundDecl.value),d->GetVertexShader(&boundVS.value),d->GetPixelShader(&boundPS.value),
        d->GetStreamSourceFreq(0,&frequencyValue),d->GetTransform(D3DTS_WORLD,&boundWorld)};
      bool valid=boundVB.value==rebuilt->buffer.value && !boundOffset && boundStride==rebuilt->stride && frequencyValue==1 &&
        (!indexChanged || boundIB.value==rebuilt->indexBuffer.value) && boundDecl.value==generated.value && !boundVS.value && !boundPS.value &&
        !std::memcmp(&boundWorld,rebuilt->world.data(),sizeof(boundWorld));
      for (auto hr:queries) { valid=valid && SUCCEEDED(hr); }
      if (!valid) { result.apply=E_FAIL;result.code=static_draw::Code::ApplyFailed;finish();return; }
    }
    // ALPHATESTENABLE/FUNC/REF, alpha blending, cull, depth, samplers and buffers
    // remain exact original state. The first diffuse layer is a diagnostic material.
    result.code=static_draw::Code::Applied;result.converted=true;
  }
  Scope(const Scope&)=delete;Scope& operator=(const Scope&)=delete;
  ~Scope() { finish(); }
  void finish() noexcept {
    if (!dirty) { return; }dirty=false;
    const auto restore=[&](HRESULT hr) { if (FAILED(hr) && SUCCEEDED(result.restore)) { result.restore=hr; } };
    const D3DTRANSFORMSTATETYPE transforms[]={D3DTS_WORLD,D3DTS_VIEW,D3DTS_PROJECTION};
    for (unsigned i=0;i<3;++i) { restore(d->SetTransform(transforms[i],old+i)); }
    restore(d->SetVertexDeclaration(decl.value));restore(d->SetVertexShader(vs.value));restore(d->SetPixelShader(ps.value));restore(d->SetRenderState(D3DRS_LIGHTING,lighting));
    restore(d->SetRenderState(D3DRS_VERTEXBLEND,blend));restore(d->SetRenderState(D3DRS_INDEXEDVERTEXBLENDENABLE,indexed));
    if (streamChanged) { restore(d->SetStreamSource(0,vb.value,oldOffset,oldStride)); }
    if (indexChanged) { restore(d->SetIndices(ib.value)); }
    if (textureChanged) { restore(d->SetTexture(0,originalTexture.value)); }
    if (neutralMaterial) { restore(d->SetRenderState(D3DRS_TEXTUREFACTOR,textureFactor)); }
    for (unsigned i=0;i<9;++i) { restore(d->SetTextureStageState(stages[i].stage,stages[i].name,tss[i])); }
    if (FAILED(result.restore)) { result.code=static_draw::Code::RestoreFailed; }
  }
  const static_draw::Report& report() const noexcept { return result; }
};
template<class Device,class Draw> HRESULT execute(Device* d,const draw_observe::Snapshot& s,D3DPRIMITIVETYPE type,bool up,Draw&& draw,static_draw::Report& report,const scene_input::Prepared* rebuilt=nullptr,bool neutral=false) {
  Scope<Device> scope(d,s,type,up,rebuilt,neutral);if (FAILED(scope.report().restore)) { report=scope.report();return report.restore; }
  const auto hr=[&] { if constexpr (std::is_invocable<Draw,bool>::value) { return draw(scope.report().converted); }else { return draw(); } }();
  scope.finish();report=scope.report();return FAILED(report.restore)?report.restore:hr;
}
} // namespace ldb_rt::bulk_static

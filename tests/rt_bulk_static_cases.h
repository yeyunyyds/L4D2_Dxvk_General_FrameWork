#pragma once
#include "rt_bulk_static.h"
#include "rt_scene_cases.h"
#include <iostream>
#include <string>

namespace rt_bulk_static_test {
using rt_scene_test::check;
struct Proxy {
  IDirect3DDevice9* d;unsigned gets=0,sets=0,failGet=0,failSet=0,extra=0;
  HRESULT get(HRESULT h) { return ++gets==failGet?E_FAIL:h; }
  HRESULT set(HRESULT h) { ++sets;return sets==failSet || sets==extra?E_FAIL:h; }
#define GET(name) template<class... A> HRESULT name(A... a) { return get(d->name(a...)); }
#define SET(name) template<class... A> HRESULT name(A... a) { return set(d->name(a...)); }
  GET(GetRenderTarget) GET(GetViewport) GET(GetRenderState) GET(GetVertexDeclaration)
  GET(GetStreamSource) GET(GetStreamSourceFreq) GET(GetTexture) GET(GetVertexShader)
  GET(GetPixelShader) GET(GetTransform) GET(GetTextureStageState) GET(CreateVertexDeclaration)
  SET(SetVertexShader) SET(SetPixelShader) SET(SetVertexDeclaration) SET(SetRenderState)
  SET(SetTransform) SET(SetTexture) SET(SetTextureStageState)
#undef GET
#undef SET
};
inline std::vector<uint64_t> state(IDirect3DDevice9* d) {
  std::vector<uint64_t> r;
  for (auto name:{D3DRS_ALPHABLENDENABLE,D3DRS_ALPHATESTENABLE,D3DRS_ALPHAFUNC,D3DRS_ALPHAREF,D3DRS_CULLMODE,D3DRS_ZENABLE,D3DRS_ZWRITEENABLE,D3DRS_LIGHTING,D3DRS_VERTEXBLEND,D3DRS_INDEXEDVERTEXBLENDENABLE}) {
    DWORD v=0;check(SUCCEEDED(d->GetRenderState(name,&v)),"bulk read render state");r.push_back(v);
  }
  for (const auto& x:ldb_rt::bulk_static::stages) { DWORD v=0;check(SUCCEEDED(d->GetTextureStageState(x.stage,x.name,&v)),"bulk read TSS");r.push_back(v); }
  for (auto name:{D3DTS_WORLD,D3DTS_VIEW,D3DTS_PROJECTION}) {
    D3DMATRIX m{};check(SUCCEEDED(d->GetTransform(name,&m)),"bulk read matrix");
    for (const auto& row:m.m) { for (float v:row) { uint32_t bits=0;std::memcpy(&bits,&v,4);r.push_back(bits); } }
  }
  ldb_rt::static_draw::Ref<IDirect3DVertexBuffer9> vb;UINT offset=0,stride=0;
  check(SUCCEEDED(d->GetStreamSource(0,&vb.value,&offset,&stride)),"bulk read stream");r.push_back(uint64_t(reinterpret_cast<uintptr_t>(vb.value)));r.push_back(offset);r.push_back(stride);
  UINT frequency=0;check(SUCCEEDED(d->GetStreamSourceFreq(0,&frequency)),"bulk read frequency");r.push_back(frequency);
  ldb_rt::static_draw::Ref<IDirect3DVertexDeclaration9> decl;ldb_rt::static_draw::Ref<IDirect3DVertexShader9> vs;ldb_rt::static_draw::Ref<IDirect3DPixelShader9> ps;
  check(SUCCEEDED(d->GetVertexDeclaration(&decl.value)) && SUCCEEDED(d->GetVertexShader(&vs.value)) && SUCCEEDED(d->GetPixelShader(&ps.value)),"bulk read shaders/decl");
  r.push_back(uint64_t(reinterpret_cast<uintptr_t>(decl.value)));r.push_back(uint64_t(reinterpret_cast<uintptr_t>(vs.value)));r.push_back(uint64_t(reinterpret_cast<uintptr_t>(ps.value)));
  for (unsigned i:{0u,7u}) { ldb_rt::static_draw::Ref<IDirect3DBaseTexture9> t;check(SUCCEEDED(d->GetTexture(i,&t.value)),"bulk read texture");r.push_back(uint64_t(reinterpret_cast<uintptr_t>(t.value))); }
  return r;
}
inline void systemCases(IDirect3DDevice9* d,IDirect3DVertexShader9* vs,IDirect3DPixelShader9* ps) {
  using namespace ldb_rt;
  const auto ok=[](unsigned line,HRESULT h) { if (FAILED(h)) { throw std::runtime_error("bulk system API failure line="+std::to_string(line)+" HRESULT="+std::to_string(uint32_t(h))); } };
  static_draw::Ref<IDirect3DSurface9> hdr,depth;ok(__LINE__,d->CreateRenderTarget(320,240,D3DFMT_A16B16G16R16F,D3DMULTISAMPLE_NONE,0,FALSE,&hdr.value,nullptr));ok(__LINE__,d->SetRenderTarget(0,hdr.value));
  ok(__LINE__,d->CreateDepthStencilSurface(320,240,D3DFMT_D16,D3DMULTISAMPLE_NONE,0,TRUE,&depth.value,nullptr));ok(__LINE__,d->SetDepthStencilSurface(depth.value));
  static_draw::Ref<IDirect3DTexture9> texture,second;
  ok(__LINE__,d->CreateTexture(2,2,1,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,&texture.value,nullptr));ok(__LINE__,d->CreateTexture(2,2,1,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,&second.value,nullptr));
  static_draw::Ref<IDirect3DVertexBuffer9> vb;ok(__LINE__,d->CreateVertexBuffer(400,0,0,D3DPOOL_MANAGED,&vb.value,nullptr));
  const char* names[]={"VertexLitGeneric","UnlitGeneric","LightmappedGeneric","LightmappedGeneric_DX9","WorldVertexTransition","WorldVertexTransition_DX9","WorldTwoTextureBlend"};
  for (unsigned kind=0;kind<7;++kind) {
    auto s=rt_scene_test::snapshot(1);s.viewport={0,0,320,240};s.world={2,0,0,0,0,1.5f,0,0,0,0,.5f,0,123,10,-20,1};
    s.materialInfo.shader.fill(0);std::memcpy(s.materialInfo.shader.data(),names[kind],std::strlen(names[kind]));
    s.materialInfo.format=kind==0?0xa0103:kind==1?0x80007:0x480007;
    const UINT stride=kind==0?32u:kind==1?48u:80u;
    std::vector<D3DVERTEXELEMENT9> elements={{0,4,D3DDECLTYPE_FLOAT3,0,D3DDECLUSAGE_POSITION,0},{0,20,D3DDECLTYPE_FLOAT2,0,D3DDECLUSAGE_TEXCOORD,0},D3DDECL_END()};
    static_draw::Ref<IDirect3DVertexDeclaration9> declaration;ok(__LINE__,d->CreateVertexDeclaration(elements.data(),&declaration.value));
    const auto setup=[&] {
      ok(__LINE__,d->SetVertexDeclaration(declaration.value));ok(__LINE__,d->SetStreamSource(0,vb.value,16,stride));ok(__LINE__,d->SetStreamSourceFreq(0,1));ok(__LINE__,d->SetVertexShader(vs));ok(__LINE__,d->SetPixelShader(ps));
      const D3DVIEWPORT9 viewport{0,0,320,240,0,1};ok(__LINE__,d->SetViewport(&viewport));
      ok(__LINE__,d->SetRenderState(D3DRS_ALPHABLENDENABLE,FALSE));ok(__LINE__,d->SetRenderState(D3DRS_ALPHATESTENABLE,TRUE));ok(__LINE__,d->SetRenderState(D3DRS_ALPHAFUNC,D3DCMP_GREATER));ok(__LINE__,d->SetRenderState(D3DRS_ALPHAREF,137));
      ok(__LINE__,d->SetRenderState(D3DRS_ZENABLE,TRUE));ok(__LINE__,d->SetRenderState(D3DRS_ZWRITEENABLE,TRUE));ok(__LINE__,d->SetRenderState(D3DRS_LIGHTING,TRUE));ok(__LINE__,d->SetRenderState(D3DRS_CULLMODE,D3DCULL_CCW));
      ok(__LINE__,d->SetRenderState(D3DRS_VERTEXBLEND,D3DVBF_1WEIGHTS));ok(__LINE__,d->SetRenderState(D3DRS_INDEXEDVERTEXBLENDENABLE,TRUE));
      ok(__LINE__,d->SetTexture(0,texture.value));ok(__LINE__,d->SetTexture(7,second.value));
      for (const auto& x:bulk_static::stages) { const DWORD value=x.name==D3DTSS_COLOROP || x.name==D3DTSS_ALPHAOP?DWORD(D3DTOP_MODULATE):x.name==D3DTSS_TEXCOORDINDEX?1u:x.name==D3DTSS_TEXTURETRANSFORMFLAGS?DWORD(D3DTTFF_COUNT2):x.name==D3DTSS_RESULTARG?DWORD(D3DTA_TEMP):DWORD(D3DTA_DIFFUSE);ok(__LINE__,d->SetTextureStageState(x.stage,x.name,value)); }
      for (auto name:{D3DTS_WORLD,D3DTS_VIEW,D3DTS_PROJECTION}) { const auto m=rt_test::matrix(99);ok(__LINE__,d->SetTransform(name,&m)); }
    };
    void* memory=nullptr;ok(__LINE__,vb.value->Lock(0,400,&memory,0));std::memset(memory,0,400);
    for (unsigned i=0;i<3;++i) { const float p[]={float(i)-1,i==1?1.f:-1.f,-10};std::memcpy(static_cast<uint8_t*>(memory)+16+i*stride+4,p,sizeof(p)); }ok(__LINE__,vb.value->Unlock());
    setup();const auto before=state(d);unsigned gets=0,sets=0;
    const auto run=[&](Proxy& proxy,unsigned& calls,bool physical) {
      static_draw::Report report;ok(__LINE__,d->BeginScene());
      const auto hr=bulk_static::execute(&proxy,s,D3DPT_TRIANGLELIST,false,[&] {
        ++calls;if (!physical) { return S_OK; }
        static_draw::Ref<IDirect3DVertexShader9> v;static_draw::Ref<IDirect3DPixelShader9> p;static_draw::Ref<IDirect3DBaseTexture9> t;
        ok(__LINE__,d->GetVertexShader(&v.value));ok(__LINE__,d->GetPixelShader(&p.value));ok(__LINE__,d->GetTexture(0,&t.value));
        check(!v.value && !p.value && t.value==(kind==4 || kind==5?second.value:texture.value),"bulk fixed shader/base layer wrong");
        DWORD a=0;ok(__LINE__,d->GetRenderState(D3DRS_ALPHAREF,&a));check(a==137,"bulk alpha reference changed");
        ok(__LINE__,d->GetRenderState(D3DRS_ALPHAFUNC,&a));check(a==D3DCMP_GREATER,"bulk alpha function changed");
        ok(__LINE__,d->GetTextureStageState(0,D3DTSS_ALPHAOP,&a));check(a==D3DTOP_SELECTARG1,"bulk texture alpha missing");
        ok(__LINE__,d->GetTextureStageState(0,D3DTSS_RESULTARG,&a));check(a==D3DTA_CURRENT,"bulk texture result routed to stale TEMP");
        ok(__LINE__,d->GetRenderState(D3DRS_VERTEXBLEND,&a));check(a==D3DVBF_DISABLE,"bulk inherited vertex blending");
        ok(__LINE__,d->GetRenderState(D3DRS_INDEXEDVERTEXBLENDENABLE,&a));check(!a,"bulk inherited indexed blending");
        D3DMATRIX m{};ok(__LINE__,d->GetTransform(D3DTS_WORLD,&m));check(!std::memcmp(&m,s.world.data(),sizeof(m)),"bulk affine transform lost");
        return d->DrawPrimitive(D3DPT_TRIANGLELIST,0,1);
      },report);ok(__LINE__,d->EndScene());return std::make_pair(hr,report);
    };
    { Proxy proxy{d};unsigned calls=0;const auto r=run(proxy,calls,true);check(SUCCEEDED(r.first) && r.second.converted && calls==1 && state(d)==before,"bulk success/restoration");gets=proxy.gets;sets=proxy.sets/2; }
    // Every getter and every mutation-then-failure setter, on actual COM state.
    for (unsigned i=1;i<=gets;++i) { Proxy proxy{d};proxy.failGet=i;unsigned calls=0;const auto r=run(proxy,calls,false);check(SUCCEEDED(r.first) && !r.second.converted && !proxy.sets && calls==1 && state(d)==before,"bulk failed getter mutation/leak"); }
    for (unsigned i=1;i<=2*sets;++i) { Proxy proxy{d};proxy.failSet=i;unsigned calls=0;const auto r=run(proxy,calls,false);check(calls==1 && state(d)==before && (i<=sets?SUCCEEDED(r.first) && !r.second.converted:FAILED(r.first)) ,"bulk setter/restore failure hidden"); }
    { Proxy proxy{d};proxy.failSet=1;proxy.extra=2;unsigned calls=0;const auto r=run(proxy,calls,false);check(!calls && FAILED(r.first) && state(d)==before,"bulk drew after rollback failure"); }
    for (unsigned fault=0;fault<5;++fault) {
      setup();if (fault==0) { ok(__LINE__,d->SetRenderState(D3DRS_ALPHABLENDENABLE,TRUE)); }if (fault==1) { ok(__LINE__,d->SetRenderState(D3DRS_ZWRITEENABLE,FALSE)); }
      if (fault==2) { ok(__LINE__,d->SetTexture(kind==4 || kind==5?7:0,nullptr)); }if (fault==3) { ok(__LINE__,d->SetStreamSource(0,vb.value,16,132)); }
      if (fault==4) { s.world[0]=-2; }
      const auto rejected=state(d);Proxy proxy{d};unsigned calls=0;const auto r=run(proxy,calls,false);
      check(!r.second.converted && !proxy.sets && calls==1 && state(d)==rejected,"bulk unsupported input accepted");s.world[0]=2;
    }
    std::vector<D3DVERTEXELEMENT9> canonical;auto bad=elements;bad.insert(bad.end()-1,{0,28,D3DDECLTYPE_UBYTE4,0,D3DDECLUSAGE_BLENDINDICES,0});
    check(!bulk_static::layout(bad.data(),UINT(bad.size()),stride,canonical),"bulk bones accepted");bad=elements;bad[1].Offset=WORD(stride-4);
    check(!bulk_static::layout(bad.data(),UINT(bad.size()),stride,canonical),"bulk out-of-stride UV accepted");
    auto camera=s;camera.maxZ=.1f;check(!bulk_static::candidate(camera),"bulk viewmodel accepted");camera=s;camera.materialInfo.shader[0]='X';check(!bulk_static::candidate(camera),"bulk unknown shader accepted");
    std::cout << "RT bulk family=" << names[kind] << " getters=" << gets << " apply=" << sets << " restore=" << sets << '\n';
  }
  std::cout << "RT bulk static families/affine/cutout/base-layer/rollback/system NULLREF: PASS; actual Remix/B580 unrun\n";
}
} // namespace rt_bulk_static_test

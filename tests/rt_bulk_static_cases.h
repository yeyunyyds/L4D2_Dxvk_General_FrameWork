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
  SET(SetTransform) SET(SetTexture) SET(SetTextureStageState) SET(SetStreamSource)
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
inline void modelGeometryBounds(IDirect3DDevice9* d) {
  using namespace ldb_rt;
  const auto ok=[](HRESULT h) { check(SUCCEEDED(h),"larger model geometry fixture API"); };
  static_draw::Ref<IDirect3DVertexBuffer9> oldVB,vb;static_draw::Ref<IDirect3DIndexBuffer9> oldIB,ib;
  static_draw::Ref<IDirect3DVertexDeclaration9> oldDecl,decl;UINT oldOffset=0,oldStride=0;
  ok(d->GetStreamSource(0,&oldVB.value,&oldOffset,&oldStride));ok(d->GetIndices(&oldIB.value));ok(d->GetVertexDeclaration(&oldDecl.value));
  const D3DVERTEXELEMENT9 elements[]={{0,0,D3DDECLTYPE_FLOAT3,0,D3DDECLUSAGE_POSITION,0},{0,16,D3DDECLTYPE_FLOAT2,0,D3DDECLUSAGE_TEXCOORD,0},D3DDECL_END()};
  ok(d->CreateVertexDeclaration(elements,&decl.value));ok(d->SetVertexDeclaration(decl.value));
  constexpr UINT vertices=16977,sourceVertices=32768,indices=16919*3,offset=16,start=7;
  std::vector<uint8_t> source(offset+sourceVertices*32,0x5a);
  for (UINT i=0;i<sourceVertices;++i) { const float p[]={float(i%3),1,-10};const float uv[]={.2f,.3f};std::memcpy(source.data()+offset+i*32,p,12);std::memcpy(source.data()+offset+i*32+16,uv,8); }
  ok(d->CreateVertexBuffer(UINT(source.size()),0,0,D3DPOOL_MANAGED,&vb.value,nullptr));void* memory=nullptr;
  ok(vb.value->Lock(0,UINT(source.size()),&memory,0));check(memory!=nullptr,"large VB null");std::memcpy(memory,source.data(),source.size());ok(vb.value->Unlock());
  std::vector<uint16_t> sourceIndices(start+indices,0x5555);
  for (UINT i=0;i<indices;++i) { sourceIndices[start+i]=uint16_t(100+i%vertices); }
  ok(d->CreateIndexBuffer(UINT(sourceIndices.size()*2),0,D3DFMT_INDEX16,D3DPOOL_MANAGED,&ib.value,nullptr));
  ok(ib.value->Lock(0,UINT(sourceIndices.size()*2),&memory,0));check(memory!=nullptr,"large IB null");std::memcpy(memory,sourceIndices.data(),sourceIndices.size()*2);ok(ib.value->Unlock());
  ok(d->SetStreamSource(0,vb.value,offset,32));ok(d->SetIndices(ib.value));
  const std::array<uint32_t,6> args{{uint32_t(-100),100,vertices,start,16919,0}};scene_input::Data legacy,larger;
  check(FAILED(scene_input::geometry(d,2,D3DPT_TRIANGLELIST,args,legacy)),"legacy scene geometry budget unexpectedly changed");
  ok(scene_input::geometry(d,2,D3DPT_TRIANGLELIST,args,larger,scene_input::kModelLimits));
  check(larger.first==0 && larger.count==vertices && larger.vertices.size()==vertices*32 && larger.indices.size()==indices*2,"signed base/min/start range changed");
  check(!std::memcmp(larger.vertices.data(),source.data()+offset,larger.vertices.size()) && !std::memcmp(larger.indices.data(),sourceIndices.data()+start,larger.indices.size()),"original model payload differs");
  auto shiftedArgs=args;shiftedArgs[0]=0;scene_input::Data shifted;
  ok(scene_input::geometry(d,2,D3DPT_TRIANGLELIST,shiftedArgs,shifted,scene_input::kModelLimits));
  check(shifted.first==100 && shifted.count==vertices && !std::memcmp(shifted.vertices.data(),source.data()+offset+100*32,shifted.vertices.size()),"nonzero declared vertex base rejected or read incorrectly");
  auto invalid=args;invalid[2]=scene_input::kModelLimits.vertices+1;scene_input::Data rejected;
  check(FAILED(scene_input::geometry(d,2,D3DPT_TRIANGLELIST,invalid,rejected,scene_input::kModelLimits)) && rejected.vertices.empty(),"model vertex cap absent");
  invalid=args;invalid[4]=scene_input::kModelLimits.indices/3+1;
  check(FAILED(scene_input::geometry(d,2,D3DPT_TRIANGLELIST,invalid,rejected,scene_input::kModelLimits)),"model index cap absent");
  invalid=args;invalid[0]=uint32_t(-101);
  check(FAILED(scene_input::geometry(d,2,D3DPT_TRIANGLELIST,invalid,rejected,scene_input::kModelLimits)),"negative absolute vertex accepted");
  ok(vb.value->Lock(0,UINT(source.size()),&memory,D3DLOCK_READONLY));check(memory && !std::memcmp(memory,source.data(),source.size()),"capture modified original VB");ok(vb.value->Unlock());
  ok(ib.value->Lock(0,UINT(sourceIndices.size()*2),&memory,D3DLOCK_READONLY));check(memory && !std::memcmp(memory,sourceIndices.data(),sourceIndices.size()*2),"capture modified original IB");ok(ib.value->Unlock());
  // Large Neko meshes and standard models now share bounded geometry limits;
  // shader identity and pipeline checks still decide conversion separately.
  constexpr UINT customTriangles=27456;std::vector<uint16_t> auditIndices(start+customTriangles*3);
  for (size_t i=start;i<auditIndices.size();++i) { auditIndices[i]=uint16_t(100+(i-start)%sourceVertices); }
  static_draw::Ref<IDirect3DIndexBuffer9> auditIB;
  ok(d->CreateIndexBuffer(UINT(auditIndices.size()*2),0,D3DFMT_INDEX16,D3DPOOL_MANAGED,&auditIB.value,nullptr));
  ok(auditIB.value->Lock(0,UINT(auditIndices.size()*2),&memory,0));check(memory!=nullptr,"audit IB null");std::memcpy(memory,auditIndices.data(),auditIndices.size()*2);ok(auditIB.value->Unlock());ok(d->SetIndices(auditIB.value));
  const std::array<uint32_t,6> customArgs{{uint32_t(-100),100,sourceVertices,start,customTriangles,0}};scene_input::Data custom;
  ok(scene_input::geometry(d,2,D3DPT_TRIANGLELIST,customArgs,custom,scene_input::kModelLimits));
  ok(scene_input::geometry(d,2,D3DPT_TRIANGLELIST,customArgs,custom,scene_input::kAuditLimits));
  check(custom.count==sourceVertices && custom.indices.size()==customTriangles*6 && !std::memcmp(custom.vertices.data(),source.data()+offset,custom.vertices.size()),"large custom material audit incomplete");
  uint16_t selected[]={uint16_t(100+sourceVertices-3),uint16_t(100+sourceVertices-2),uint16_t(100+sourceVertices-1)};
  ok(auditIB.value->Lock(start*2,sizeof(selected),&memory,0));std::memcpy(memory,selected,sizeof(selected));ok(auditIB.value->Unlock());
  auto windowArgs=customArgs;windowArgs[4]=1;scene_input::Data window;
  ok(scene_input::geometry(d,2,D3DPT_TRIANGLELIST,windowArgs,window,{},true));
  check(window.first==sourceVertices-3 && window.count==3 && window.vertices.size()==96 && window.vertexBufferBytes==source.size() && !std::memcmp(window.vertices.data(),source.data()+offset+window.first*32,96),"indexed audit window lost absolute vertex offset");
  scene_input::Data nonindexed;std::array<uint32_t,6> directArgs{{sourceVertices-3,1,0,0,0,0}};
  check(FAILED(scene_input::geometry(d,1,D3DPT_TRIANGLELIST,directArgs,nonindexed)),"legacy nonindexed absolute cap changed");
  ok(scene_input::geometry(d,1,D3DPT_TRIANGLELIST,directArgs,nonindexed,{},true));check(nonindexed.first==sourceVertices-3 && nonindexed.count==3 && nonindexed.vertices.size()==96,"nonindexed audit window failed");
  selected[2]=uint16_t(100+sourceVertices);ok(auditIB.value->Lock(start*2,sizeof(selected),&memory,0));std::memcpy(memory,selected,sizeof(selected));ok(auditIB.value->Unlock());
  check(FAILED(scene_input::geometry(d,2,D3DPT_TRIANGLELIST,windowArgs,window,{},true)),"audit window accepted index outside original declared range");
  std::cout << "RT indexed audit window/large shared VB/range rejection/system NULLREF: PASS\n";
  std::cout << "RT custom shader audit large geometry independent cap/system NULLREF: PASS\n";
  ok(d->SetStreamSource(0,oldVB.value,oldOffset,oldStride));ok(d->SetIndices(oldIB.value));ok(d->SetVertexDeclaration(oldDecl.value));
  std::cout << "RT larger model signed-base/index-window/read-only/caps/system NULLREF: PASS\n";
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
    scene_input::Prepared rebuilt;bool replacing=false;
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
        D3DMATRIX m{};ok(__LINE__,d->GetTransform(D3DTS_WORLD,&m));check(!std::memcmp(&m,(replacing?rebuilt.world:s.world).data(),sizeof(m)),"bulk affine transform lost");
        if (replacing) {
          static_draw::Ref<IDirect3DVertexBuffer9> bound;UINT offset=0,step=0;ok(__LINE__,d->GetStreamSource(0,&bound.value,&offset,&step));
          check(bound.value==rebuilt.buffer.value && !offset && step==32,"original model rebuilt stream not bound");
        }
        return d->DrawPrimitive(D3DPT_TRIANGLELIST,0,1);
      },report,replacing?&rebuilt:nullptr);ok(__LINE__,d->EndScene());return std::make_pair(hr,report);
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
    if (kind==0) {
      setup();rebuilt.world=s.world;rebuilt.world[12]+=33;rebuilt.stride=32;rebuilt.sourceDeclaration=elements;
      rebuilt.elements={{0,0,D3DDECLTYPE_FLOAT3,0,D3DDECLUSAGE_POSITION,0},{0,12,D3DDECLTYPE_FLOAT2,0,D3DDECLUSAGE_TEXCOORD,0},D3DDECL_END()};
      rebuilt.bytes.resize(96);
      for (unsigned i=0;i<3;++i) { const float v[]={float(i)-1,i==1?1.f:-1.f,-10,.2f,.3f};std::memcpy(rebuilt.bytes.data()+i*32,v,sizeof(v)); }
      check(scene_input::upload(d,rebuilt),"model fixture upload failed");rebuilt.ready=true;replacing=true;
      const auto originalState=state(d);
      { Proxy proxy{d};unsigned calls=0;const auto r=run(proxy,calls,true);check(SUCCEEDED(r.first) && r.second.converted && calls==1 && state(d)==originalState,"model override success/restoration");gets=proxy.gets;sets=proxy.sets/2; }
      for (unsigned i=1;i<=gets;++i) { Proxy proxy{d};proxy.failGet=i;unsigned calls=0;const auto r=run(proxy,calls,false);check(SUCCEEDED(r.first) && !r.second.converted && !proxy.sets && calls==1 && state(d)==originalState,"model getter failure mutated state"); }
      for (unsigned i=1;i<=2*sets;++i) { Proxy proxy{d};proxy.failSet=i;unsigned calls=0;const auto r=run(proxy,calls,false);check(calls==1 && state(d)==originalState && (i<=sets?SUCCEEDED(r.first) && !r.second.converted:FAILED(r.first)),"model stream/setter rollback failure hidden"); }
      { Proxy proxy{d};proxy.failSet=1;proxy.extra=2;unsigned calls=0;const auto r=run(proxy,calls,false);check(!calls && FAILED(r.first) && state(d)==originalState,"model drew after rollback failure"); }
      // Neko must never enter the generic no-shader-proof path. Its prepared
      // first-diffuse path shares all stream/state restoration failure cases.
      std::memcpy(s.materialInfo.shader.data(),"NekoToon",9);replacing=false;
      { Proxy proxy{d};unsigned calls=0;const auto r=run(proxy,calls,false);check(!r.second.converted && !proxy.sets && calls==1 && state(d)==originalState,"unverified Neko entered generic conversion"); }
      replacing=true;
      { Proxy proxy{d};unsigned calls=0;const auto r=run(proxy,calls,true);check(SUCCEEDED(r.first) && r.second.converted && calls==1 && state(d)==originalState,"prepared Neko state/stream restoration");gets=proxy.gets;sets=proxy.sets/2; }
      for (unsigned i=1;i<=gets;++i) { Proxy proxy{d};proxy.failGet=i;unsigned calls=0;const auto r=run(proxy,calls,false);check(!proxy.sets && calls==1 && !r.second.converted && state(d)==originalState,"Neko getter failure changed state"); }
      for (unsigned i=1;i<=2*sets;++i) { Proxy proxy{d};proxy.failSet=i;unsigned calls=0;const auto r=run(proxy,calls,false);check(calls==1 && state(d)==originalState && (i<=sets?SUCCEEDED(r.first) && !r.second.converted:FAILED(r.first)),"Neko setter/restore failure hidden"); }
      std::memcpy(s.materialInfo.shader.data(),names[kind],std::strlen(names[kind])+1);
      std::cout << "RT Neko prepared-only/first-diffuse/state rollback/system NULLREF: PASS\n";
      replacing=false;
      std::cout << "RT original model transform/rebuilt stream rollback/system NULLREF: PASS; synthetic prepared geometry, actual captured inputs checked separately\n";
    }
    std::vector<D3DVERTEXELEMENT9> canonical;auto bad=elements;bad.insert(bad.end()-1,{0,28,D3DDECLTYPE_UBYTE4,0,D3DDECLUSAGE_BLENDINDICES,0});
    check(!bulk_static::layout(bad.data(),UINT(bad.size()),stride,canonical),"bulk bones accepted");bad=elements;bad[1].Offset=WORD(stride-4);
    check(!bulk_static::layout(bad.data(),UINT(bad.size()),stride,canonical),"bulk out-of-stride UV accepted");
    auto camera=s;camera.maxZ=.1f;check(!bulk_static::candidate(camera),"bulk viewmodel accepted");camera=s;camera.materialInfo.shader[0]='X';check(!bulk_static::candidate(camera),"bulk unknown shader accepted");
    std::cout << "RT bulk family=" << names[kind] << " getters=" << gets << " apply=" << sets << " restore=" << sets << '\n';
  }
  modelGeometryBounds(d);
  std::cout << "RT bulk static families/affine/cutout/base-layer/rollback/system NULLREF: PASS; actual Remix/B580 unrun\n";
}
} // namespace rt_bulk_static_test

#pragma once
#include "rt_scene_input.h"
#include "rt_scene_cases.h"
#include <d3dcompiler.h>
#include <string>

namespace rt_sky_input_test {
using rt_scene_test::check;
inline void systemCases(IDirect3DDevice9* d,decltype(&D3DCompile) compile) {
  using namespace ldb_rt;
  static_draw::Ref<ID3DBlob> vsCode,psCode,errors;
  const auto ok=[&](unsigned line,HRESULT hr) { if (FAILED(hr)) { if (errors.value) { std::cerr << static_cast<const char*>(errors.value->GetBufferPointer()) << '\n'; }throw std::runtime_error("sky replay system API failure line="+std::to_string(line)+" HRESULT="+std::to_string(uint32_t(hr))); } };
  const char vsText[]="float4 A:register(c0);float4 B:register(c1);float4 C:register(c2);float4 D:register(c3);float4 S:register(c4);float4 T:register(c5);struct I{float3 p:POSITION;float2 uv:TEXCOORD0;};struct O{float4 p:POSITION;float2 uv:TEXCOORD0;};O main(I i){O o;float4 p=float4(i.p,1);o.p=float4(dot(p,A),dot(p,B),dot(p,C),dot(p,D));o.uv=i.uv*S.xy+T.xy;return o;}";
  const char psText[]="sampler2D image:register(s0);float4 main(float2 uv:TEXCOORD0):COLOR0{return tex2D(image,uv);}";
  ok(__LINE__,compile(vsText,sizeof(vsText)-1,nullptr,nullptr,nullptr,"main","vs_2_0",D3DCOMPILE_ENABLE_STRICTNESS,0,&vsCode.value,&errors.value));
  if (errors.value) { errors.value->Release();errors.value=nullptr; }
  // SM2 sampler2D is legacy HLSL; strict mode rejects that syntax itself.
  ok(__LINE__,compile(psText,sizeof(psText)-1,nullptr,nullptr,nullptr,"main","ps_2_0",D3DCOMPILE_ENABLE_BACKWARDS_COMPATIBILITY,0,&psCode.value,&errors.value));
  static_draw::Ref<IDirect3DVertexShader9> vs;static_draw::Ref<IDirect3DPixelShader9> ps;
  ok(__LINE__,d->CreateVertexShader(static_cast<const DWORD*>(vsCode.value->GetBufferPointer()),&vs.value));ok(__LINE__,d->CreatePixelShader(static_cast<const DWORD*>(psCode.value->GetBufferPointer()),&ps.value));
  auto s=rt_scene_test::snapshot(0);s.viewport={0,0,320,240};s.projection[5]=2;
  auto layout=rt_scene_test::declaration(0);static_draw::Ref<IDirect3DVertexDeclaration9> decl;
  ok(__LINE__,d->CreateVertexDeclaration(layout.data(),&decl.value));ok(__LINE__,d->SetVertexDeclaration(decl.value));
  static_draw::Ref<IDirect3DVertexBuffer9> source;ok(__LINE__,d->CreateVertexBuffer(112,0,0,D3DPOOL_MANAGED,&source.value,nullptr));
  void* memory=nullptr;ok(__LINE__,source.value->Lock(0,112,&memory,0));std::memset(memory,0,112);
  for (unsigned i=0;i<3;++i) { const float v[5]={float(i)-1,i==1?1.f:-1.f,-10,.2f,.3f};std::memcpy(static_cast<uint8_t*>(memory)+16+i*32,v,sizeof(v)); }ok(__LINE__,source.value->Unlock());
  ok(__LINE__,d->SetStreamSource(0,source.value,16,32));ok(__LINE__,d->SetStreamSourceFreq(0,1));ok(__LINE__,d->SetVertexShader(vs.value));ok(__LINE__,d->SetPixelShader(ps.value));
  sky_shader::Constants constants{};constants[0]={2,0,0,1};constants[1]={0,2,0,-3};constants[2]={0,0,s.projection[10],s.projection[14]};constants[3]={0,0,-1,0};constants[4]={2,-1,0,0};constants[5]={.25f,1,0,0};
  ok(__LINE__,d->SetVertexShaderConstantF(0,constants[0].data(),256));
  for (auto name:{D3DRS_ALPHABLENDENABLE,D3DRS_ALPHATESTENABLE,D3DRS_ZENABLE,D3DRS_ZWRITEENABLE}) { ok(__LINE__,d->SetRenderState(name,0)); }
  const auto input=scene_input::capture(d,1,D3DPT_TRIANGLELIST,{0,1,0,0,0,0});check(SUCCEEDED(input.result) && input.count==3 && input.first==0 && input.vertices.size()==96,"original input capture failed");
  auto prepared=scene_input::prepare(d,s,input);check(prepared.ready,"compiled original shader not replayed");
  float first[5]{};std::memcpy(first,prepared.bytes.data(),sizeof(first));
  check(std::abs(first[0]+1)<.001 && std::abs(first[1]+2.5f)<.001 && std::abs(first[2]+10)<.001 && std::abs(first[3]-.65f)<1e-6 && std::abs(first[4]-.7f)<1e-6,"compiled shader position/UV not preserved");
  // Indexed capture: nonzero IB offset and negative base vertex; validate range.
  static_draw::Ref<IDirect3DIndexBuffer9> ib;ok(__LINE__,d->CreateIndexBuffer(8,0,D3DFMT_INDEX16,D3DPOOL_MANAGED,&ib.value,nullptr));
  ok(__LINE__,ib.value->Lock(0,8,&memory,0));const uint16_t indices[]={0,2,3,4};std::memcpy(memory,indices,sizeof(indices));ok(__LINE__,ib.value->Unlock());ok(__LINE__,d->SetIndices(ib.value));
  scene_input::Data indexed;ok(__LINE__,scene_input::geometry(d,2,D3DPT_TRIANGLELIST,{uint32_t(-2),2,3,1,1,0},indexed));check(indexed.first==0 && indexed.count==3 && indexed.indices.size()==6,"base/index/stream offset range wrong");
  scene_input::Data invalid;check(FAILED(scene_input::geometry(d,2,D3DPT_TRIANGLELIST,{uint32_t(-2),2,3,2,1,0},invalid)),"out of bounds indices accepted");
  invalid={};check(FAILED(scene_input::geometry(d,2,D3DPT_TRIANGLELIST,{uint32_t(-3),2,3,1,1,0},invalid)),"negative absolute vertex accepted");
  invalid={};check(FAILED(scene_input::geometry(d,1,D3DPT_TRIANGLELIST,{4095,1,0,0,0,0},invalid)),"oversize absolute vertex accepted");
  static_draw::Ref<IDirect3DIndexBuffer9> ib32;ok(__LINE__,d->CreateIndexBuffer(12,0,D3DFMT_INDEX32,D3DPOOL_MANAGED,&ib32.value,nullptr));
  ok(__LINE__,ib32.value->Lock(0,12,&memory,0));const uint32_t wide[]={0,1,2};std::memcpy(memory,wide,sizeof(wide));ok(__LINE__,ib32.value->Unlock());ok(__LINE__,d->SetIndices(ib32.value));
  scene_input::Data wideData;ok(__LINE__,scene_input::geometry(d,2,D3DPT_TRIANGLELIST,{0,0,3,0,1,0},wideData));check(wideData.indexBytes==4 && wideData.indices.size()==12,"32 bit index capture wrong");
  ok(__LINE__,d->SetIndices(nullptr));
  struct Adapter:rt_scene_test::System {
    unsigned calls=0,fail=0,extra=0,gets=0;bool failGet=false;
    HRESULT GetStreamSource(UINT n,IDirect3DVertexBuffer9** b,UINT* o,UINT* s) { const auto hr=d->GetStreamSource(n,b,o,s);return failGet && ++gets==1?E_FAIL:hr; }
    HRESULT SetStreamSource(UINT n,IDirect3DVertexBuffer9* b,UINT offset,UINT stride) { const auto hr=d->SetStreamSource(n,b,offset,stride);++calls;return calls==fail || calls==extra?E_FAIL:hr; }
  };
  for (unsigned fail=0;fail<5;++fail) {
    Adapter adapter;adapter.d=d;adapter.fail=fail==3?1:fail;adapter.extra=fail==3?2:0;adapter.failGet=fail==4;static_draw::Report report;unsigned calls=0;
    ok(__LINE__,d->BeginScene());const auto hr=scene_input::execute(&adapter,s,prepared,D3DPT_TRIANGLELIST,[&] {
      ++calls;static_draw::Ref<IDirect3DVertexBuffer9> actual;UINT offset=0,stride=0;ok(__LINE__,d->GetStreamSource(0,&actual.value,&offset,&stride));
      check(actual.value==(fail==1 || fail==4?source.value:prepared.buffer.value) && offset==(fail==1 || fail==4?16u:0u),"sky replay/fallback stream wrong at Draw");
      if (fail==1 || fail==4) { return S_OK; }return d->DrawPrimitive(D3DPT_TRIANGLELIST,0,1);
    },report);ok(__LINE__,d->EndScene());
    check(calls==(fail==3?0u:1u) && (fail==2 || fail==3?FAILED(hr):SUCCEEDED(hr)) && (fail?report.code==(fail==4?static_draw::Code::CaptureFailed:fail==1?static_draw::Code::ApplyFailed:static_draw::Code::RestoreFailed):report.converted),"sky binding failure hidden");
    static_draw::Ref<IDirect3DVertexBuffer9> actual;UINT offset=0,stride=0;ok(__LINE__,d->GetStreamSource(0,&actual.value,&offset,&stride));
    check(actual.value==source.value && offset==16 && stride==32,"source stream not restored");
    static_draw::Ref<IDirect3DVertexShader9> actualVS;ok(__LINE__,d->GetVertexShader(&actualVS.value));check(actualVS.value==vs.value,"original VS lost");
  }
  std::cout << "RT sky original VS reconstruction/geometry bounds/binding rollback/system NULLREF: PASS; actual Remix/B580 unrun\n";
}
} // namespace rt_sky_input_test

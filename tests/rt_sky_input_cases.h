#pragma once
#include "rt_scene_input.h"
#include "rt_scene_cases.h"
#include <d3dcompiler.h>

namespace rt_sky_input_test {
using rt_scene_test::check;
inline void systemCases(IDirect3DDevice9* d,decltype(&D3DCompile) compile) {
  using namespace ldb_rt;
  const auto ok=[](HRESULT hr) { check(SUCCEEDED(hr),"sky replay system API failure"); };
  const char vsText[]="float4 A:register(c0);float4 B:register(c1);float4 C:register(c2);float4 D:register(c3);float4 S:register(c4);float4 T:register(c5);struct I{float3 p:POSITION;float2 uv:TEXCOORD0;};struct O{float4 p:POSITION;float2 uv:TEXCOORD0;};O main(I i){O o;float4 p=float4(i.p,1);o.p=float4(dot(p,A),dot(p,B),dot(p,C),dot(p,D));o.uv=i.uv*S.xy+T.xy;return o;}";
  const char psText[]="sampler2D image:register(s0);float4 main(float2 uv:TEXCOORD0):COLOR0{return tex2D(image,uv);}";
  static_draw::Ref<ID3DBlob> vsCode,psCode,errors;
  ok(compile(vsText,sizeof(vsText)-1,nullptr,nullptr,nullptr,"main","vs_2_0",D3DCOMPILE_ENABLE_STRICTNESS,0,&vsCode.value,&errors.value));
  if (errors.value) { errors.value->Release();errors.value=nullptr; }
  ok(compile(psText,sizeof(psText)-1,nullptr,nullptr,nullptr,"main","ps_2_0",D3DCOMPILE_ENABLE_STRICTNESS,0,&psCode.value,&errors.value));
  static_draw::Ref<IDirect3DVertexShader9> vs;static_draw::Ref<IDirect3DPixelShader9> ps;
  ok(d->CreateVertexShader(static_cast<const DWORD*>(vsCode.value->GetBufferPointer()),&vs.value));ok(d->CreatePixelShader(static_cast<const DWORD*>(psCode.value->GetBufferPointer()),&ps.value));
  auto s=rt_scene_test::snapshot(0);s.viewport={0,0,320,240};s.projection[5]=2;
  auto layout=rt_scene_test::declaration(0);static_draw::Ref<IDirect3DVertexDeclaration9> decl;
  ok(d->CreateVertexDeclaration(layout.data(),&decl.value));ok(d->SetVertexDeclaration(decl.value));
  static_draw::Ref<IDirect3DVertexBuffer9> source;ok(d->CreateVertexBuffer(112,0,0,D3DPOOL_MANAGED,&source.value,nullptr));
  void* memory=nullptr;ok(source.value->Lock(0,112,&memory,0));std::memset(memory,0,112);
  for (unsigned i=0;i<3;++i) { const float v[5]={float(i)-1,i==1?1.f:-1.f,-10,.2f,.3f};std::memcpy(static_cast<uint8_t*>(memory)+16+i*32,v,sizeof(v)); }ok(source.value->Unlock());
  ok(d->SetStreamSource(0,source.value,16,32));ok(d->SetStreamSourceFreq(0,1));ok(d->SetVertexShader(vs.value));ok(d->SetPixelShader(ps.value));
  sky_shader::Constants constants{};constants[0]={2,0,0,1};constants[1]={0,2,0,-3};constants[2]={0,0,s.projection[10],s.projection[14]};constants[3]={0,0,-1,0};constants[4]={2,-1,0,0};constants[5]={.25f,1,0,0};
  ok(d->SetVertexShaderConstantF(0,constants[0].data(),256));
  for (auto name:{D3DRS_ALPHABLENDENABLE,D3DRS_ALPHATESTENABLE,D3DRS_ZENABLE,D3DRS_ZWRITEENABLE}) { ok(d->SetRenderState(name,0)); }
  const auto input=scene_input::capture(d,1,D3DPT_TRIANGLELIST,{0,1,0,0,0,0});check(SUCCEEDED(input.result) && input.count==3 && input.first==0 && input.vertices.size()==96,"original input capture failed");
  auto prepared=scene_input::prepare(d,s,input);check(prepared.ready,"compiled original shader not replayed");
  float first[5]{};std::memcpy(first,prepared.bytes.data(),sizeof(first));
  check(std::abs(first[0]+1)<.001 && std::abs(first[1]+2.5f)<.001 && std::abs(first[2]+10)<.001 && std::abs(first[3]-.65f)<1e-6 && std::abs(first[4]-.7f)<1e-6,"compiled shader position/UV not preserved");
  // Indexed capture: nonzero IB offset and negative base vertex; validate range.
  static_draw::Ref<IDirect3DIndexBuffer9> ib;ok(d->CreateIndexBuffer(8,0,D3DFMT_INDEX16,D3DPOOL_MANAGED,&ib.value,nullptr));
  ok(ib.value->Lock(0,8,&memory,0));const uint16_t indices[]={0,2,3,4};std::memcpy(memory,indices,sizeof(indices));ok(ib.value->Unlock());ok(d->SetIndices(ib.value));
  scene_input::Data indexed;ok(scene_input::geometry(d,2,D3DPT_TRIANGLELIST,{uint32_t(-2),2,3,1,1,0},indexed));check(indexed.first==0 && indexed.count==3 && indexed.indices.size()==6,"base/index/stream offset range wrong");
  scene_input::Data invalid;check(FAILED(scene_input::geometry(d,2,D3DPT_TRIANGLELIST,{uint32_t(-2),2,3,2,1,0},invalid)),"out of bounds indices accepted");
  invalid={};check(FAILED(scene_input::geometry(d,2,D3DPT_TRIANGLELIST,{uint32_t(-3),2,3,1,1,0},invalid)),"negative absolute vertex accepted");
  invalid={};check(FAILED(scene_input::geometry(d,1,D3DPT_TRIANGLELIST,{4095,1,0,0,0,0},invalid)),"oversize absolute vertex accepted");
  static_draw::Ref<IDirect3DIndexBuffer9> ib32;ok(d->CreateIndexBuffer(12,0,D3DFMT_INDEX32,D3DPOOL_MANAGED,&ib32.value,nullptr));
  ok(ib32.value->Lock(0,12,&memory,0));const uint32_t wide[]={0,1,2};std::memcpy(memory,wide,sizeof(wide));ok(ib32.value->Unlock());ok(d->SetIndices(ib32.value));
  scene_input::Data wideData;ok(scene_input::geometry(d,2,D3DPT_TRIANGLELIST,{0,0,3,0,1,0},wideData));check(wideData.indexBytes==4 && wideData.indices.size()==12,"32 bit index capture wrong");
  ok(d->SetIndices(nullptr));
  struct Adapter:rt_scene_test::System {
    unsigned calls=0,fail=0,extra=0,gets=0;bool failGet=false;
    HRESULT GetStreamSource(UINT n,IDirect3DVertexBuffer9** b,UINT* o,UINT* s) { const auto hr=d->GetStreamSource(n,b,o,s);return failGet && ++gets==1?E_FAIL:hr; }
    HRESULT SetStreamSource(UINT n,IDirect3DVertexBuffer9* b,UINT offset,UINT stride) { const auto hr=d->SetStreamSource(n,b,offset,stride);++calls;return calls==fail || calls==extra?E_FAIL:hr; }
  };
  for (unsigned fail=0;fail<5;++fail) {
    Adapter adapter;adapter.d=d;adapter.fail=fail==3?1:fail;adapter.extra=fail==3?2:0;adapter.failGet=fail==4;static_draw::Report report;unsigned calls=0;
    ok(d->BeginScene());const auto hr=scene_input::execute(&adapter,s,prepared,D3DPT_TRIANGLELIST,[&] {
      ++calls;static_draw::Ref<IDirect3DVertexBuffer9> actual;UINT offset=0,stride=0;ok(d->GetStreamSource(0,&actual.value,&offset,&stride));
      check(actual.value==(fail==1 || fail==4?source.value:prepared.buffer.value) && offset==(fail==1 || fail==4?16u:0u),"sky replay/fallback stream wrong at Draw");
      if (fail==1 || fail==4) { return S_OK; }return d->DrawPrimitive(D3DPT_TRIANGLELIST,0,1);
    },report);ok(d->EndScene());
    check(calls==(fail==3?0u:1u) && (fail==2 || fail==3?FAILED(hr):SUCCEEDED(hr)) && (fail?report.code==(fail==4?static_draw::Code::CaptureFailed:fail==1?static_draw::Code::ApplyFailed:static_draw::Code::RestoreFailed):report.converted),"sky binding failure hidden");
    static_draw::Ref<IDirect3DVertexBuffer9> actual;UINT offset=0,stride=0;ok(d->GetStreamSource(0,&actual.value,&offset,&stride));
    check(actual.value==source.value && offset==16 && stride==32,"source stream not restored");
    static_draw::Ref<IDirect3DVertexShader9> actualVS;ok(d->GetVertexShader(&actualVS.value));check(actualVS.value==vs.value,"original VS lost");
  }
  std::cout << "RT sky original VS reconstruction/geometry bounds/binding rollback/system NULLREF: PASS; actual Remix/B580 unrun\n";
}
} // namespace rt_sky_input_test

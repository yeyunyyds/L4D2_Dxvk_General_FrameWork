#pragma once
#include "rt_scene_input.h"
#include "rt_scene_cases.h"

namespace rt_morph_input_test {
using rt_scene_test::check;
// Real NULLREF VB operations with failure injection at its read boundaries.
struct Buffer final:IDirect3DVertexBuffer9 {
  IDirect3DVertexBuffer9* inner;ULONG refs=1;unsigned fault=0,locks=0,unlocks=0;
  explicit Buffer(IDirect3DVertexBuffer9* b):inner(b) { }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID,void** p) override { *p=nullptr;return E_NOINTERFACE; }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
  ULONG STDMETHODCALLTYPE Release() override { return --refs; }
  HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9**) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID,const void*,DWORD,DWORD) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID,void*,DWORD*) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID) override { return E_NOTIMPL; }
  DWORD STDMETHODCALLTYPE SetPriority(DWORD) override { return 0; }
  DWORD STDMETHODCALLTYPE GetPriority() override { return 0; }
  void STDMETHODCALLTYPE PreLoad() override { }
  D3DRESOURCETYPE STDMETHODCALLTYPE GetType() override { return D3DRTYPE_VERTEXBUFFER; }
  HRESULT STDMETHODCALLTYPE GetDesc(D3DVERTEXBUFFER_DESC* p) override { return fault==3?E_FAIL:inner->GetDesc(p); }
  HRESULT STDMETHODCALLTYPE Lock(UINT offset,UINT size,void** p,DWORD flags) override {
    check(flags==D3DLOCK_READONLY,"morph source lock attempted write");++locks;
    if (fault==4) { return E_FAIL; }
    const auto hr=inner->Lock(offset,size,p,flags);if (fault==6) { *p=nullptr; }return hr;
  }
  HRESULT STDMETHODCALLTYPE Unlock() override { ++unlocks;const auto hr=inner->Unlock();return fault==5?E_FAIL:hr; }
};
struct Device {
  IDirect3DDevice9* d;Buffer* buffer;unsigned fault=0;
  HRESULT GetStreamSource(UINT stream,IDirect3DVertexBuffer9** b,UINT* o,UINT* s) {
    ldb_rt::static_draw::Ref<IDirect3DVertexBuffer9> actual;
    const auto hr=d->GetStreamSource(stream,&actual.value,o,s);
    if (FAILED(hr)) { return hr; }check(actual.value==buffer->inner && stream==2,"morph stream identity");
    *b=buffer;buffer->AddRef();return fault==1?E_FAIL:S_OK;
  }
  HRESULT GetStreamSourceFreq(UINT stream,UINT* frequency) {
    const auto hr=d->GetStreamSourceFreq(stream,frequency);return fault==2?E_FAIL:hr;
  }
};
inline void systemCases(IDirect3DDevice9* d) {
  using namespace ldb_rt;
  const auto ok=[](HRESULT h) { check(SUCCEEDED(h),"morph NULLREF fixture API"); };
  static_draw::Ref<IDirect3DVertexBuffer9> previous,source;UINT previousOffset=0,previousStride=0,previousFrequency=0;
  ok(d->GetStreamSource(2,&previous.value,&previousOffset,&previousStride));ok(d->GetStreamSourceFreq(2,&previousFrequency));
  constexpr UINT offset=16,stride=28,first=7,count=40;
  std::vector<uint8_t> bytes(offset+(first+count)*stride,0x5a);
  for (unsigned i=0;i<first+count;++i) { const sky_shader::Vec value{float(i),float(i*2),-float(i*3),999};std::memcpy(bytes.data()+offset+i*stride,value.data(),16); }
  ok(d->CreateVertexBuffer(UINT(bytes.size()),0,0,D3DPOOL_MANAGED,&source.value,nullptr));void* pointer=nullptr;
  ok(source.value->Lock(0,UINT(bytes.size()),&pointer,0));check(pointer!=nullptr,"morph fixture null VB");std::memcpy(pointer,bytes.data(),bytes.size());ok(source.value->Unlock());
  ok(d->SetStreamSource(2,source.value,offset,stride));ok(d->SetStreamSourceFreq(2,1));
  scene_input::Data input;input.count=count;input.first=first;input.constants[13][0]=1;
  input.declaration={{2,0,D3DDECLTYPE_FLOAT4,0,D3DDECLUSAGE_POSITION,1},D3DDECL_END()};
  Buffer buffer(source.value);
  for (unsigned fault=0;fault<=6;++fault) {
    buffer.fault=fault;buffer.locks=buffer.unlocks=0;Device device{d,&buffer,fault};
    const auto hr=scene_input::morphPositions(&device,input);
    if (!fault) {
      check(hr==S_OK && input.morph.positions.size()==count && input.morph.positions.front()[0]==first &&
        input.morph.positions.back()[2]==-float((first+count-1)*3),"full morph absolute range/prefix/last vertex wrong");
    } else { check(FAILED(hr) && input.morph.positions.empty(),"morph getter/description/lock/unlock/null failure hidden"); }
    check(buffer.refs==1 && buffer.unlocks==(fault==0 || fault>=5?1u:0u),"morph source lease/lock leaked");
    ok(source.value->Lock(0,UINT(bytes.size()),&pointer,D3DLOCK_READONLY));check(pointer && !std::memcmp(pointer,bytes.data(),bytes.size()),"morph capture modified source or left lock held");ok(source.value->Unlock());
  }
  buffer.fault=0;Device device{d,&buffer,0};
  for (unsigned fault=0;fault<5;++fault) {
    input.first=first;input.count=count;input.declaration[0].Offset=0;input.declaration[0].Type=D3DDECLTYPE_FLOAT4;
    ok(d->SetStreamSource(2,source.value,offset,stride));
    if (fault==0) { ++input.count; }
    if (fault==1) { input.first=UINT32_MAX; }
    if (fault==2) { input.declaration[0].Offset=4; }
    if (fault==3) { input.declaration[0].Type=D3DDECLTYPE_FLOAT3; }
    if (fault==4) { ok(d->SetStreamSource(2,source.value,offset,12)); }
    const auto locks=buffer.locks;
    check(scene_input::morphPositions(&device,input)==S_FALSE && input.morph.positions.empty() && buffer.locks==locks,"unsupported morph range/declaration reached Lock");
  }
  ok(d->SetStreamSource(2,previous.value,previousOffset,previousStride));ok(d->SetStreamSourceFreq(2,previousFrequency));
  sky_shader::Vec psValue{.1f,.2f,.3f,.4f};ok(d->SetPixelShaderConstantF(184,psValue.data(),1));
  scene_input::Data constants;ok(d->GetPixelShaderConstantF(0,constants.pixelConstants[0].data(),UINT(constants.pixelConstants.size())));
  check(constants.pixelConstants.size()==224 && constants.pixelConstants[184]==psValue,"full PS material constant range missing");
  std::cout<<"RT full morph read-only range/last vertex/failures/PS224/system NULLREF: PASS\n";
}
} // namespace rt_morph_input_test

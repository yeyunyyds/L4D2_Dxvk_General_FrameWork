#pragma once
#include "rt_consumer_world.h"
#include "rt_static_device.h"
#include <iostream>

namespace rt_tangent_test {
inline void require(bool ok,const char* why) { if (!ok) { throw std::runtime_error(why); } }
inline std::vector<D3DVERTEXELEMENT9> declaration(unsigned stride) {
  // Exact bytes from the v6 actual-game trace, independent of the production
  // declaration builder. Both include the original auxiliary stream2 fields.
  const char* text=stride==64 ?
    "000000000200000000000c0002000300000018000100050000002000010005010000280002000600000034000200070002000000030000010200100002000301ff00000011000000" :
    "000000000200000000000c0002000300000018000100050000002000010005010000280001000502000030000200060000003c000200070002000000030000010200100002000301ff00000011000000";
  const auto bytes=std::strlen(text)/2;
  std::vector<D3DVERTEXELEMENT9> result(bytes/sizeof(D3DVERTEXELEMENT9));
  auto* out=reinterpret_cast<uint8_t*>(result.data());
  const auto nibble=[](char c) { return uint8_t(c>='a'?c-'a'+10:c-'0'); };
  for (size_t i=0;i<bytes;++i) { out[i]=uint8_t(nibble(text[2*i])*16+nibble(text[2*i+1])); }
  return result;
}
inline ldb_rt::static_draw::Layout layout(unsigned stride) {
  return stride==64?ldb_rt::static_draw::Layout::Tangent2UV:ldb_rt::static_draw::Layout::Tangent3UV;
}
template<class Device> auto input(unsigned stride,uint32_t width=1920,uint32_t height=1080) {
  typename ldb_rt::static_draw::Scope<Device>::ConsumerInput result;
  result.world=rt_test::matrix(0);
  result.view[0]=result.view[5]=result.view[10]=result.view[15]=1;
  result.projection={1,0,0,0,0,1,0,0,0,0,-1.0001f,-1,0,0,-7.0007f,0};
  result.width=width;result.height=height;result.layout=layout(stride);return result;
}
inline void mockCases() {
  using namespace ldb_rt;
  for (unsigned stride:{64u,80u}) {
    const auto expected=declaration(stride);
    auto prepare=[&](rt_test::Graphics& d) { d.stride=stride;d.offset=16;d.declaration.elements=expected; };
    const auto requested=input<rt_test::Graphics>(stride);
    draw_observe::Snapshot s;s.status=draw_observe::Status::Valid;s.material=1;s.materialTable=2;s.materialInfo.flags=3;
    s.materialInfo.format=stride==64?0x480033:0x2480033;
    std::memcpy(s.materialInfo.shader.data(),"LightmappedGeneric",19);
    std::memcpy(s.world.data(),&requested.world,sizeof(requested.world));s.view=requested.view;s.projection=requested.projection;
    s.viewport={0,0,1920,1080};s.maxZ=1;
    require(consumer_world::classify(s,false)==consumer_world::Decision::Material &&
      consumer_world::classify(s,true)==consumer_world::Decision::Selected,"tangent expansion escaped phase gate");
    auto unsafe=s;unsafe.materialInfo.format|=0x400;
    require(consumer_world::classify(unsafe,true)==consumer_world::Decision::Material,"compressed format accepted");
    unsafe=s;unsafe.world[12]=10;
    require(consumer_world::classify(unsafe,true)==consumer_world::Decision::Transform,"translated candidate accepted");
    {
      rt_test::Graphics d;prepare(d);
      { static_draw::Scope<rt_test::Graphics> scope(&d,requested,D3DPT_TRIANGLELIST,false);
        require(scope.report().converted && !d.currentShader && !d.currentPixel &&
          d.currentFvf==(D3DFVF_XYZ|D3DFVF_NORMAL|D3DFVF_TEX2) && d.stride==stride && d.offset==16,
          "tangent prefix conversion changed source stride/offset or retained PS"); }
      require(d.restored() && d.leasesReleased() && !d.foreign,"tangent successful scope state/ref/owner leak");
    }
    for (unsigned fault=0;fault<8;++fault) {
      rt_test::Graphics d;prepare(d);auto changed=requested;
      if (fault==0) { d.declaration.elements[1].Type=D3DDECLTYPE_UBYTE4; }
      if (fault==1) { d.declaration.elements[4].Offset+=4; }
      if (fault==2) { d.declaration.elements[d.declaration.elements.size()-2].UsageIndex=0; }
      if (fault==3) { d.stride=stride==64?80:64; }
      if (fault==4) { d.offset=2; }
      if (fault==5) { d.frequency=D3DSTREAMSOURCE_INDEXEDDATA|2; }
      if (fault==6) { changed.layout=static_draw::Layout::World; }
      if (fault==7) { changed.layout=layout(stride==64?80:64); }
      { static_draw::Scope<rt_test::Graphics> scope(&d,changed,D3DPT_TRIANGLELIST,false);
        require(!scope.report().converted && scope.report().code==static_draw::Code::Layout && !d.sets,"unsafe tangent layout accepted or mutated"); }
      require(d.restored() && d.leasesReleased(),"rejected tangent leaked state/ref");
    }
    for (unsigned fault=1;fault<=13;++fault) {
      rt_test::Graphics d;prepare(d);d.failGet=fault;
      { static_draw::Scope<rt_test::Graphics> scope(&d,requested,D3DPT_TRIANGLELIST,false);
        require(scope.report().code==static_draw::Code::CaptureFailed && !d.sets,"tangent failed getter mutated state"); }
      require(d.restored() && d.leasesReleased(),"tangent failed getter ref leak");
    }
    for (unsigned fault=1;fault<=12;++fault) {
      rt_test::Graphics d;prepare(d);d.failSet={fault};
      { static_draw::Scope<rt_test::Graphics> scope(&d,requested,D3DPT_TRIANGLELIST,false);scope.finish();
        require(scope.report().code==(fault<=6?static_draw::Code::ApplyFailed:static_draw::Code::RestoreFailed),"tangent failed setter accepted"); }
      require(d.restored() && d.leasesReleased(),"tangent mutate-then-fail rollback skipped state/ref");
    }
  }
}
inline void systemCases(IDirect3DDevice9* d,IDirect3DVertexShader9* vs,IDirect3DPixelShader9* ps) {
  using namespace ldb_rt;
  require(vs && ps,"real original tangent shaders required");
  const auto check=[](HRESULT hr) { require(SUCCEEDED(hr),"system tangent setup/draw failed"); };
  static_draw::Ref<IDirect3DSurface9> depth;
  check(d->CreateDepthStencilSurface(320,240,D3DFMT_D16,D3DMULTISAMPLE_NONE,0,TRUE,&depth.value,nullptr));
  check(d->SetDepthStencilSurface(depth.value));
  for (unsigned stride:{64u,80u}) {
    auto elements=declaration(stride);
    static_draw::Ref<IDirect3DVertexDeclaration9> decl;
    check(d->CreateVertexDeclaration(elements.data(),&decl.value));
    static_draw::Ref<IDirect3DVertexBuffer9> vb,aux;
    check(d->CreateVertexBuffer(16+stride*3,0,0,D3DPOOL_MANAGED,&vb.value,nullptr));
    check(d->CreateVertexBuffer(84,0,0,D3DPOOL_MANAGED,&aux.value,nullptr));
    void* bytes=nullptr;check(vb.value->Lock(0,16+stride*3,&bytes,0));require(bytes!=nullptr,"tangent VB lock returned null");
    std::memset(bytes,0,16+stride*3);
    for (unsigned i=0;i<3;++i) {
      auto* f=reinterpret_cast<float*>(static_cast<uint8_t*>(bytes)+16+i*stride);
      f[0]=float(i)-1;f[1]=i==1?1.f:-1.f;f[2]=-3;f[5]=1;
    }
    check(vb.value->Unlock());
    check(d->SetVertexDeclaration(decl.value));check(d->SetStreamSource(0,vb.value,16,stride));
    check(d->SetStreamSource(2,aux.value,0,28));check(d->SetStreamSourceFreq(0,1));
    check(d->SetVertexShader(vs));check(d->SetPixelShader(ps));
    const D3DVIEWPORT9 viewport{0,0,320,240,0,1};check(d->SetViewport(&viewport));
    check(d->SetRenderState(D3DRS_ALPHABLENDENABLE,FALSE));check(d->SetRenderState(D3DRS_ALPHATESTENABLE,FALSE));
    check(d->SetRenderState(D3DRS_ZENABLE,D3DZB_TRUE));check(d->SetRenderState(D3DRS_ZWRITEENABLE,TRUE));
    std::array<D3DMATRIX,3> before{};
    for (unsigned i=0;i<3;++i) { before[i]=rt_test::matrix(float(100+i));check(d->SetTransform(i==0?D3DTS_WORLD:i==1?D3DTS_VIEW:D3DTS_PROJECTION,&before[i])); }
    const auto requested=input<IDirect3DDevice9>(stride,320,240);
    check(d->BeginScene());
    { static_draw::Scope<IDirect3DDevice9> scope(d,requested,D3DPT_TRIANGLELIST,false);
      require(scope.report().converted,"system tangent declaration rejected");
      static_draw::Ref<IDirect3DVertexShader9> appliedVS;static_draw::Ref<IDirect3DPixelShader9> appliedPS;
      check(d->GetVertexShader(&appliedVS.value));check(d->GetPixelShader(&appliedPS.value));
      require(!appliedVS.value && !appliedPS.value,"system tangent did not enter fixed shader path");
      check(d->DrawPrimitive(D3DPT_TRIANGLELIST,0,1));scope.finish();check(scope.report().restore); }
    check(d->EndScene());
    static_draw::Ref<IDirect3DVertexDeclaration9> actualDecl;static_draw::Ref<IDirect3DVertexShader9> actualVS;static_draw::Ref<IDirect3DPixelShader9> actualPS;
    check(d->GetVertexDeclaration(&actualDecl.value));check(d->GetVertexShader(&actualVS.value));check(d->GetPixelShader(&actualPS.value));
    require(actualDecl.value==decl.value && actualVS.value==vs && actualPS.value==ps,"system tangent original COM bindings lost");
    for (unsigned stream:{0u,2u}) {
      static_draw::Ref<IDirect3DVertexBuffer9> actual;UINT offset=0,step=0;
      check(d->GetStreamSource(stream,&actual.value,&offset,&step));
      require(actual.value==(stream?aux.value:vb.value) && offset==(stream?0u:16u) && step==(stream?28u:stride),"system tangent source/aux stream changed");
    }
    for (unsigned i=0;i<3;++i) { D3DMATRIX after{};check(d->GetTransform(i==0?D3DTS_WORLD:i==1?D3DTS_VIEW:D3DTS_PROJECTION,&after));
      require(!std::memcmp(&after,&before[i],sizeof(after)),"system tangent transform restore mismatch"); }
  }
  std::cout << "RT tangent64/80 exact inputs/phase/rollback/system NULLREF: PASS; GPU RT unrun\n";
}
} // namespace rt_tangent_test

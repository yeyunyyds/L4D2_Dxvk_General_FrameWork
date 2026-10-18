#pragma once
#include "rt_scene_draw.h"
#include "rt_static_device.h"
#include <iostream>

namespace rt_scene_test {
namespace scene_draw=ldb_rt::scene_draw;
inline void check(bool ok,const char* why) { if (!ok) { throw std::runtime_error(why); } }
struct Surface final:IDirect3DSurface9 {
  ULONG refs=1;
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID,void** out) override { *out=nullptr;return E_NOINTERFACE; }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
  ULONG STDMETHODCALLTYPE Release() override { return --refs; }
  HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9**) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID,const void*,DWORD,DWORD) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID,void*,DWORD*) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID) override { return E_NOTIMPL; }
  DWORD STDMETHODCALLTYPE SetPriority(DWORD) override { return 0; }
  DWORD STDMETHODCALLTYPE GetPriority() override { return 0; }
  void STDMETHODCALLTYPE PreLoad() override { }
  D3DRESOURCETYPE STDMETHODCALLTYPE GetType() override { return D3DRTYPE_SURFACE; }
  HRESULT STDMETHODCALLTYPE GetContainer(REFIID,void**) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetDesc(D3DSURFACE_DESC* out) override { *out={};out->Width=1920;out->Height=1080;out->Format=D3DFMT_A16B16G16R16F;return S_OK; }
  HRESULT STDMETHODCALLTYPE LockRect(D3DLOCKED_RECT*,const RECT*,DWORD) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE UnlockRect() override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetDC(HDC*) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE ReleaseDC(HDC) override { return E_NOTIMPL; }
};
struct Graphics:rt_test::Graphics {
  Surface surface;
  HRESULT GetRenderTarget(DWORD,IDirect3DSurface9** out) { surface.AddRef();*out=&surface;return operation(false); }
};
inline std::vector<D3DVERTEXELEMENT9> declaration(unsigned kind) {
  // Actual v7 bytes: NekoSky, bone-free VertexLitGeneric80503, and a0403.
  const char* text=kind==0?
    "000000000200000000000c000100050002000000030000010200100002000301ff00000011000000":kind==1?
    "000000000200000000000c0005000300000010000100050000000c00050006000100000004000a01020000000300000102001000050003010300000000000002ff00000011000000":
    "000000000200000000000c0005000300000010000100050000000c0005000600020000000300000102001000050003010300000000000002ff00000011000000";
  const auto size=std::strlen(text)/2;std::vector<D3DVERTEXELEMENT9> result(size/8);auto* bytes=reinterpret_cast<uint8_t*>(result.data());
  const auto nibble=[](char c) { return uint8_t(c>='a'?c-'a'+10:c-'0'); };
  for (size_t i=0;i<size;++i) { bytes[i]=uint8_t(nibble(text[2*i])*16+nibble(text[2*i+1])); }return result;
}
inline ldb_rt::draw_observe::Snapshot snapshot(unsigned kind) {
  ldb_rt::draw_observe::Snapshot s;s.status=ldb_rt::draw_observe::Status::Valid;s.material=1;s.materialTable=2;s.materialInfo.flags=3;
  s.materialInfo.format=kind==0?0x80001:kind==1?0x80503:0xa0403;
  const char* shader=kind==0?"NekoSky":"VertexLitGeneric";std::memcpy(s.materialInfo.shader.data(),shader,std::strlen(shader)+1);
  for (auto* m:{&s.world,&s.view}) { (*m)[0]=(*m)[5]=(*m)[10]=(*m)[15]=1; }
  s.projection={1,0,0,0,0,1,0,0,0,0,-1.0001f,-1,0,0,-7.0007f,0};s.viewport={0,0,1920,1080};s.maxZ=1;
  if (kind) { s.world[0]=0;s.world[1]=1;s.world[4]=-1;s.world[5]=0;s.world[12]=123; }
  return s;
}
inline void prepare(Graphics& d,unsigned kind) {
  d.stride=32;d.offset=16;d.depth=d.depthWrite=kind?1u:0u;d.fog=1;d.declaration.elements=declaration(kind);
}
inline bool restored(const Graphics& d) { return d.restored() && d.leasesReleased() && d.surface.refs==1 && d.lighting==1 && d.fog==1 && d.category==0xfefefefe; }
inline void mockCases() {
  using namespace ldb_rt;
  for (unsigned kind=0;kind<3;++kind) {
    const auto s=snapshot(kind);const auto mode=kind?scene_draw::Mode::Rigid:scene_draw::Mode::Sky;
    check(scene_draw::candidate(s,false,false)==scene_draw::Mode::None && scene_draw::candidate(s,kind==0,kind!=0)==mode,"scene stage gate");
    auto invalid=s;invalid.maxZ=.1f;check(scene_draw::candidate(invalid,true,true)==scene_draw::Mode::None,"viewmodel accepted");
    invalid=s;invalid.world[0]=2;check(scene_draw::candidate(invalid,true,true)==scene_draw::Mode::None,"scaled/reflected candidate accepted");
    unsigned getCount=0,setCount=0;
    {
      Graphics d;prepare(d,kind);static_draw::Report report;unsigned calls=0;
      const auto hr=scene_draw::execute(&d,s,mode,D3DPT_TRIANGLELIST,false,[&] {
        ++calls;check(!d.currentShader && (kind?!d.currentPixel:d.currentPixel==&d.pixel) && !d.lighting,"scene shader/lighting state");
        check(d.currentDeclaration==&d.generated,"scene canonical binding");
        const auto& elements=d.generated.elements;
        check(elements.size()==3 && elements[0].Usage==D3DDECLUSAGE_POSITION && elements[1].Usage==D3DDECLUSAGE_TEXCOORD &&
          elements[1].Offset==(kind?16:12),"packed normal/bone input leaked into canonical declaration");
        if (!kind) { check(d.category==scene_draw::kSkyCategory && !d.fog,"sky category/sentinel/fog not scoped"); }
        return S_OK;
      },report);
      getCount=d.gets;setCount=d.sets/2;
      check(SUCCEEDED(hr) && calls==1 && report.converted && restored(d),"scene success state/ref restore");
    }
    for (unsigned fail=1;fail<=getCount;++fail) {
      Graphics d;prepare(d,kind);d.failGet=fail;static_draw::Report report;unsigned calls=0;
      scene_draw::execute(&d,s,mode,D3DPT_TRIANGLELIST,false,[&] { ++calls;return S_OK; },report);
      check(calls==1 && !d.sets && report.code==static_draw::Code::CaptureFailed && restored(d),"scene failed getter not rejected safely");
    }
    for (unsigned fail=1;fail<=2*setCount;++fail) {
      Graphics d;prepare(d,kind);d.failSet={fail};static_draw::Report report;unsigned calls=0;
      const auto hr=scene_draw::execute(&d,s,mode,D3DPT_TRIANGLELIST,false,[&] { ++calls;return S_OK; },report);
      check(calls==1 && restored(d) && report.code==(fail<=setCount?static_draw::Code::ApplyFailed:static_draw::Code::RestoreFailed),"scene modify-then-fail rollback");
      check((fail<=setCount)==SUCCEEDED(hr),"scene restore error hidden");
    }
    {
      Graphics d;prepare(d,kind);d.failSet={1,2};static_draw::Report report;unsigned calls=0;
      const auto hr=scene_draw::execute(&d,s,mode,D3DPT_TRIANGLELIST,false,[&] { ++calls;return S_OK; },report);
      check(!calls && FAILED(hr) && restored(d),"scene drew after failed rollback");
    }
    for (unsigned fault=0;fault<5;++fault) {
      Graphics d;prepare(d,kind);
      if (fault==0) { d.declaration.elements[0].Offset=4; }
      if (fault==1) { d.declaration.elements[1].Type=D3DDECLTYPE_SHORT2; }
      if (fault==2) { d.stride=36; }
      if (fault==3) { d.frequency=D3DSTREAMSOURCE_INDEXEDDATA|2; }
      if (fault==4) { d.offset=2; }
      static_draw::Report report;unsigned calls=0;
      scene_draw::execute(&d,s,mode,D3DPT_TRIANGLELIST,false,[&] { ++calls;return S_OK; },report);
      check(calls==1 && !report.converted && !d.sets && restored(d),"unknown/skinned scene layout accepted");
    }
    std::cout << "RT scene scope kind=" << kind << " getters=" << getCount << " apply=" << setCount << " restore=" << setCount << '\n';
  }
}
// System D3D9 has no contract for reserved42. Only this adapter emulates that
// one pinned Remix extension. All standard state/COM/draw calls are real.
struct System {
  IDirect3DDevice9* d;DWORD category=scene_draw::kUnusedCategory;
#define GET(name) template<class... A> HRESULT name(A... a) { return d->name(a...); }
  GET(GetRenderTarget) GET(GetViewport) GET(GetVertexDeclaration) GET(GetStreamSource) GET(GetStreamSourceFreq)
  GET(GetVertexShader) GET(GetPixelShader) GET(GetTransform) GET(CreateVertexDeclaration)
  GET(SetVertexShader) GET(SetPixelShader) GET(SetVertexDeclaration) GET(SetTransform)
  GET(GetTextureStageState) GET(SetTextureStageState)
#undef GET
  HRESULT GetRenderState(D3DRENDERSTATETYPE name,DWORD* out) { if (name==scene_draw::kCategoryState) { *out=category;return S_OK; }return d->GetRenderState(name,out); }
  HRESULT SetRenderState(D3DRENDERSTATETYPE name,DWORD value) { if (name==scene_draw::kCategoryState) { category=value;return S_OK; }return d->SetRenderState(name,value); }
};
inline void systemCases(IDirect3DDevice9* d,IDirect3DVertexShader9* vs,IDirect3DPixelShader9* ps) {
  using namespace ldb_rt;
  const auto ok=[](HRESULT hr) { check(SUCCEEDED(hr),"system scene D3D9 API failed"); };
  static_draw::Ref<IDirect3DSurface9> hdr,depth;
  ok(d->CreateRenderTarget(320,240,D3DFMT_A16B16G16R16F,D3DMULTISAMPLE_NONE,0,FALSE,&hdr.value,nullptr));ok(d->SetRenderTarget(0,hdr.value));
  ok(d->CreateDepthStencilSurface(320,240,D3DFMT_D16,D3DMULTISAMPLE_NONE,0,TRUE,&depth.value,nullptr));ok(d->SetDepthStencilSurface(depth.value));
  for (unsigned kind=0;kind<3;++kind) {
    auto s=snapshot(kind);s.viewport={0,0,320,240};auto elements=declaration(kind);
    static_draw::Ref<IDirect3DVertexDeclaration9> decl;ok(d->CreateVertexDeclaration(elements.data(),&decl.value));ok(d->SetVertexDeclaration(decl.value));
    static_draw::Ref<IDirect3DVertexBuffer9> vb;ok(d->CreateVertexBuffer(112,0,0,D3DPOOL_MANAGED,&vb.value,nullptr));
    void* bytes=nullptr;ok(vb.value->Lock(0,112,&bytes,0));check(bytes!=nullptr,"scene VB null");std::memset(bytes,0,112);
    for (unsigned i=0;i<3;++i) { auto* p=reinterpret_cast<float*>(static_cast<uint8_t*>(bytes)+16+i*32);p[0]=float(i)-1;p[1]=i==1?1.f:-1.f;p[2]=-3; }ok(vb.value->Unlock());
    ok(d->SetStreamSource(0,vb.value,16,32));ok(d->SetStreamSourceFreq(0,1));ok(d->SetVertexShader(vs));ok(d->SetPixelShader(ps));
    const D3DVIEWPORT9 vp{0,0,320,240,0,1};ok(d->SetViewport(&vp));
    ok(d->SetRenderState(D3DRS_ALPHABLENDENABLE,FALSE));ok(d->SetRenderState(D3DRS_ALPHATESTENABLE,FALSE));
    ok(d->SetRenderState(D3DRS_ZENABLE,kind?1:0));ok(d->SetRenderState(D3DRS_ZWRITEENABLE,kind?1:0));
    ok(d->SetRenderState(D3DRS_LIGHTING,TRUE));ok(d->SetRenderState(D3DRS_FOGENABLE,TRUE));
    for (auto name:{D3DTS_WORLD,D3DTS_VIEW,D3DTS_PROJECTION}) { const auto m=rt_test::matrix(99);ok(d->SetTransform(name,&m)); }
    System device{d};static_draw::Report report;ok(d->BeginScene());
    const auto hr=scene_draw::execute(&device,s,kind?scene_draw::Mode::Rigid:scene_draw::Mode::Sky,D3DPT_TRIANGLELIST,false,[&] {
      static_draw::Ref<IDirect3DVertexShader9> v;static_draw::Ref<IDirect3DPixelShader9> p;ok(d->GetVertexShader(&v.value));ok(d->GetPixelShader(&p.value));
      check(!v.value && (kind?!p.value:p.value==ps),"system scene shader scope");
      return d->DrawPrimitive(D3DPT_TRIANGLELIST,0,1);
    },report);ok(d->EndScene());ok(hr);check(report.converted && SUCCEEDED(report.restore),"system scene not converted");
    static_draw::Ref<IDirect3DVertexDeclaration9> actual;static_draw::Ref<IDirect3DVertexShader9> actualVS;static_draw::Ref<IDirect3DPixelShader9> actualPS;
    ok(d->GetVertexDeclaration(&actual.value));ok(d->GetVertexShader(&actualVS.value));ok(d->GetPixelShader(&actualPS.value));
    check(actual.value==decl.value && actualVS.value==vs && actualPS.value==ps && device.category==scene_draw::kUnusedCategory,"system scene original bindings/category lost");
    for (auto name:{D3DRS_LIGHTING,D3DRS_FOGENABLE}) { DWORD value=0;ok(d->GetRenderState(name,&value));check(value==TRUE,"system scene render state restore"); }
    for (auto name:{D3DTS_WORLD,D3DTS_VIEW,D3DTS_PROJECTION}) { D3DMATRIX actualMatrix{};ok(d->GetTransform(name,&actualMatrix));const auto expected=rt_test::matrix(99);check(!std::memcmp(&actualMatrix,&expected,sizeof(expected)),"system scene matrix restore"); }
  }
  std::cout << "RT scene sky/rigid scope/rollback/system NULLREF: PASS; reserved42 adapter emulation; actual Remix/B580 unrun\n";
}
} // namespace rt_scene_test

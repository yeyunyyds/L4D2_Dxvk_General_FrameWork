#define NOMINMAX
#include "rt_overlay.h"
#include "rt_static_device.h"
#include "rt_tangent_cases.h"
#include "rt_scene_cases.h"
#include <d3dcompiler.h>
#include <iostream>
#include <vector>
using namespace ldb_rt;
void require(bool ok,const char* why) { if (!ok) { throw std::runtime_error(why); } }
struct Block final:IDirect3DStateBlock9 {
  ULONG refs=1;IDirect3DStateBlock9* inner;bool failCapture,failRestore;
  Block(IDirect3DStateBlock9* p,bool c,bool r):inner(p),failCapture(c),failRestore(r) { }
  ~Block() { inner->Release(); }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID,void** p) override { *p=nullptr;return E_NOINTERFACE; }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
  ULONG STDMETHODCALLTYPE Release() override { const auto n=--refs;if (!n) { delete this; }return n; }
  HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9** p) override { return inner->GetDevice(p); }
  HRESULT STDMETHODCALLTYPE Capture() override { return failCapture?E_FAIL:inner->Capture(); }
  HRESULT STDMETHODCALLTYPE Apply() override { const auto hr=inner->Apply();return failRestore?E_FAIL:hr; }
};
struct Proxy {
  IDirect3DDevice9* device;unsigned gets=0,sets=0,draws=0,failGet=0,failSet=0,beforeDrawSets=0;
  bool failCapture=false,failRestore=false,failDraw=false;
  IDirect3DSurface9* foreignBackbuffer=nullptr;
  HRESULT get(HRESULT hr) { return ++gets==failGet?E_FAIL:hr; }
  HRESULT set(HRESULT hr) { return ++sets==failSet?E_FAIL:hr; }
#define GET(name) template<class... A> HRESULT name(A... a) { return get(device->name(a...)); }
#define SET(name) template<class... A> HRESULT name(A... a) { return set(device->name(a...)); }
  GET(GetRenderTarget) GET(GetViewport) GET(GetRenderState)
  GET(GetVertexDeclaration) GET(GetStreamSource) GET(GetStreamSourceFreq)
  GET(GetTransform) GET(GetVertexShader) GET(GetPixelShader) GET(GetTextureStageState) GET(GetTexture)
  SET(SetVertexShader) SET(SetPixelShader) SET(SetFVF) SET(SetRenderState)
  SET(SetTexture) SET(SetTextureStageState) SET(SetTransform)
  SET(SetStreamSource) SET(SetVertexDeclaration)
#undef GET
#undef SET
  // NULLREF supplies no swapchain color target. Only this fixture aliases its
  // explicit offscreen RT as a backbuffer; production calls the real method.
  HRESULT GetBackBuffer(UINT,UINT,D3DBACKBUFFER_TYPE,IDirect3DSurface9** p) {
    if (foreignBackbuffer) { foreignBackbuffer->AddRef();*p=foreignBackbuffer;return get(S_OK); }
    return get(device->GetRenderTarget(0,p));
  }
  HRESULT CreateStateBlock(D3DSTATEBLOCKTYPE type,IDirect3DStateBlock9** p) {
    IDirect3DStateBlock9* inner=nullptr;const auto hr=device->CreateStateBlock(type,&inner);
    if (inner) { *p=new Block(inner,failCapture,failRestore); }return get(hr);
  }
  HRESULT DrawPrimitiveUP(D3DPRIMITIVETYPE type,UINT count,const void* vertices,UINT stride) {
    beforeDrawSets=sets;++draws;return failDraw?E_FAIL:device->DrawPrimitiveUP(type,count,vertices,stride);
  }
};
std::vector<uint64_t> state(IDirect3DDevice9* d) {
  std::vector<uint64_t> result;
  for (auto name:{D3DRS_ZENABLE,D3DRS_ZWRITEENABLE,D3DRS_ALPHABLENDENABLE,D3DRS_ALPHATESTENABLE,D3DRS_SRCBLEND,
      D3DRS_DESTBLEND,D3DRS_BLENDOP,D3DRS_SEPARATEALPHABLENDENABLE,D3DRS_FOGENABLE,D3DRS_CULLMODE,
      D3DRS_STENCILENABLE,D3DRS_SCISSORTESTENABLE,D3DRS_COLORWRITEENABLE}) {
    DWORD v=0;require(SUCCEEDED(d->GetRenderState(name,&v)),"read state");result.push_back(v);
  }
  for (DWORD stage=0;stage<2;++stage) {
    for (auto name:{D3DTSS_COLOROP,D3DTSS_COLORARG1,D3DTSS_ALPHAOP,D3DTSS_ALPHAARG1}) {
      DWORD v=0;require(SUCCEEDED(d->GetTextureStageState(stage,name,&v)),"read TSS");result.push_back(v);
    }
  }
  for (auto name:{D3DTS_WORLD,D3DTS_VIEW,D3DTS_PROJECTION}) {
    D3DMATRIX m{};require(SUCCEEDED(d->GetTransform(name,&m)),"read transform");
    for (const auto& row:m.m) { for (float v:row) { uint32_t bits=0;std::memcpy(&bits,&v,4);result.push_back(bits); } }
  }
  static_draw::Ref<IDirect3DVertexBuffer9> vb;UINT offset=0,stride=0;
  require(SUCCEEDED(d->GetStreamSource(0,&vb.value,&offset,&stride)),"read stream0");
  result.push_back(uint64_t(reinterpret_cast<uintptr_t>(vb.value)));result.push_back(offset);result.push_back(stride);
  static_draw::Ref<IDirect3DVertexDeclaration9> decl;require(SUCCEEDED(d->GetVertexDeclaration(&decl.value)),"read declaration");
  result.push_back(uint64_t(reinterpret_cast<uintptr_t>(decl.value)));
  static_draw::Ref<IDirect3DVertexShader9> vs;static_draw::Ref<IDirect3DPixelShader9> ps;static_draw::Ref<IDirect3DBaseTexture9> texture;
  require(SUCCEEDED(d->GetVertexShader(&vs.value)) && SUCCEEDED(d->GetPixelShader(&ps.value)) && SUCCEEDED(d->GetTexture(0,&texture.value)),"read shader/texture bindings");
  result.push_back(uint64_t(reinterpret_cast<uintptr_t>(vs.value)));result.push_back(uint64_t(reinterpret_cast<uintptr_t>(ps.value)));result.push_back(uint64_t(reinterpret_cast<uintptr_t>(texture.value)));
  return result;
}
void unlitCases() {
  rt_test::Graphics d;d.stride=32;
  const D3DVERTEXELEMENT9 layout[]={{0,0,D3DDECLTYPE_FLOAT3,0,D3DDECLUSAGE_POSITION,0},
    {0,12,D3DDECLTYPE_FLOAT3,0,D3DDECLUSAGE_NORMAL,0},{0,24,D3DDECLTYPE_FLOAT2,0,D3DDECLUSAGE_TEXCOORD,0},
    {1,0,D3DDECLTYPE_D3DCOLOR,0,D3DDECLUSAGE_COLOR,1},{2,0,D3DDECLTYPE_FLOAT4,0,D3DDECLUSAGE_POSITION,1},
    {2,16,D3DDECLTYPE_FLOAT3,0,D3DDECLUSAGE_NORMAL,1},{3,0,D3DDECLTYPE_FLOAT1,0,D3DDECLUSAGE_POSITION,2},D3DDECL_END()};
  d.declaration.elements.assign(std::begin(layout),std::end(layout));
  static_draw::Scope<rt_test::Graphics>::ConsumerInput input;
  input.world=rt_test::matrix(0);input.view[0]=input.view[5]=input.view[10]=input.view[15]=1;input.projection=input.view;
  input.width=1920;input.height=1080;input.layout=static_draw::Layout::Unlit;
  {
    static_draw::Scope<rt_test::Graphics> scope(&d,input,D3DPT_TRIANGLELIST,false);
    require(scope.report().converted && !d.currentShader && !d.currentPixel && d.currentFvf==(D3DFVF_XYZ|D3DFVF_NORMAL|D3DFVF_TEX1),"unlit prefix and fixed PS");
  }
  require(d.restored() && d.leasesReleased(),"unlit scope restoration");
  for (unsigned fail=1;fail<=6;++fail) {
    d.sets=0;d.failSet={fail};
    { static_draw::Scope<rt_test::Graphics> scope(&d,input,D3DPT_TRIANGLELIST,false);require(!scope.report().converted && FAILED(scope.report().apply),"failed unlit setter accepted"); }
    require(d.restored() && d.leasesReleased(),"unlit partial apply rollback");
  }
  d.failSet={};d.declaration.elements[1].Type=D3DDECLTYPE_UBYTE4;
  { static_draw::Scope<rt_test::Graphics> scope(&d,input,D3DPT_TRIANGLELIST,false);require(!scope.report().converted && scope.report().code==static_draw::Code::Layout,"compressed normal must be rejected"); }
}
int main() {
  try {
    unlitCases();
    rt_tangent_test::mockCases();
    rt_scene_test::mockCases();
    HMODULE library=LoadLibraryExW(L"d3d9.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);require(library!=nullptr,"system D3D9 required");
    using Create=IDirect3D9*(WINAPI*)(UINT);const auto address=GetProcAddress(library,"Direct3DCreate9");Create create=nullptr;std::memcpy(&create,&address,sizeof(create));require(create!=nullptr,"factory");
    static_draw::Ref<IDirect3D9> factory;factory.value=create(D3D_SDK_VERSION);require(factory.value!=nullptr,"D3D9");
    const HWND window=CreateWindowExW(0,L"STATIC",L"RT overlay scope",WS_OVERLAPPED,0,0,320,240,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);require(window!=nullptr,"window");
    D3DPRESENT_PARAMETERS pp{};pp.BackBufferWidth=320;pp.BackBufferHeight=240;pp.BackBufferFormat=D3DFMT_A8R8G8B8;pp.BackBufferCount=1;pp.SwapEffect=D3DSWAPEFFECT_DISCARD;pp.Windowed=TRUE;pp.hDeviceWindow=window;
    static_draw::Ref<IDirect3DDevice9> device;require(SUCCEEDED(factory.value->CreateDevice(0,D3DDEVTYPE_NULLREF,window,D3DCREATE_SOFTWARE_VERTEXPROCESSING,&pp,&device.value)),"NULLREF required");
    auto* d=device.value;
    static_draw::Ref<IDirect3DSurface9> offscreen;
    require(SUCCEEDED(d->CreateRenderTarget(320,240,D3DFMT_A8R8G8B8,D3DMULTISAMPLE_NONE,0,FALSE,&offscreen.value,nullptr)) &&
      SUCCEEDED(d->SetRenderTarget(0,offscreen.value)),"create explicit NULLREF offscreen target");
    const D3DVERTEXELEMENT9 layout[]={{0,0,D3DDECLTYPE_FLOAT3,0,D3DDECLUSAGE_POSITION,0},{0,12,D3DDECLTYPE_FLOAT2,0,D3DDECLUSAGE_TEXCOORD,0},{2,0,D3DDECLTYPE_FLOAT4,0,D3DDECLUSAGE_POSITION,1},{2,16,D3DDECLTYPE_FLOAT3,0,D3DDECLUSAGE_NORMAL,1},D3DDECL_END()};
    static_draw::Ref<IDirect3DVertexDeclaration9> declaration;require(SUCCEEDED(d->CreateVertexDeclaration(layout,&declaration.value)),"create decl");require(SUCCEEDED(d->SetVertexDeclaration(declaration.value)),"bind decl");
    static_draw::Ref<IDirect3DVertexBuffer9> vb;require(SUCCEEDED(d->CreateVertexBuffer(96,0,0,D3DPOOL_DEFAULT,&vb.value,nullptr)),"VB");require(SUCCEEDED(d->SetStreamSource(0,vb.value,0,32)),"bind stream0");
    require(SUCCEEDED(d->SetStreamSourceFreq(0,1)),"seed non-instanced stream0");
    const D3DVIEWPORT9 viewport{0,0,320,240,0,1};require(SUCCEEDED(d->SetViewport(&viewport)),"seed viewport");
    unsigned transformIndex=1;
    for (auto name:{D3DTS_WORLD,D3DTS_VIEW,D3DTS_PROJECTION}) {
      const auto matrix=rt_test::matrix(float(transformIndex++*100));require(SUCCEEDED(d->SetTransform(name,&matrix)),"seed original transform");
    }
    struct Compiler {
      HMODULE module=LoadLibraryExW(L"d3dcompiler_47.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
      ~Compiler() { if (module) { FreeLibrary(module); } }
    } compiler;
    require(compiler.module!=nullptr,"system HLSL compiler required for fixture");
    const auto compileAddress=GetProcAddress(compiler.module,"D3DCompile");decltype(&D3DCompile) compile=nullptr;
    std::memcpy(&compile,&compileAddress,sizeof(compile));require(compile!=nullptr,"HLSL compile export");
    const char vsSource[]="float4 main(float4 p : POSITION) : POSITION { return p; }";
    const char psSource[]="float4 main() : COLOR0 { return float4(1,0,0,1); }";
    static_draw::Ref<ID3DBlob> vsCode,psCode,errors;
    require(SUCCEEDED(compile(vsSource,sizeof(vsSource)-1,nullptr,nullptr,nullptr,"main","vs_2_0",D3DCOMPILE_ENABLE_STRICTNESS,0,&vsCode.value,&errors.value)),"compile fixture VS");
    if (errors.value) { errors.value->Release();errors.value=nullptr; }
    require(SUCCEEDED(compile(psSource,sizeof(psSource)-1,nullptr,nullptr,nullptr,"main","ps_2_0",D3DCOMPILE_ENABLE_STRICTNESS,0,&psCode.value,&errors.value)),"compile fixture PS");
    static_draw::Ref<IDirect3DVertexShader9> vs;static_draw::Ref<IDirect3DPixelShader9> ps;
    const auto vsResult=d->CreateVertexShader(static_cast<const DWORD*>(vsCode.value->GetBufferPointer()),&vs.value);
    const auto psResult=d->CreatePixelShader(static_cast<const DWORD*>(psCode.value->GetBufferPointer()),&ps.value);
    std::cout << "RT overlay fixture shader creation: vs_hr=" << uint32_t(vsResult) << " ps_hr=" << uint32_t(psResult) << '\n';
    require(SUCCEEDED(vsResult) && SUCCEEDED(d->SetVertexShader(vs.value)),"bind compiled original VS");
    require(SUCCEEDED(psResult) && SUCCEEDED(d->SetPixelShader(ps.value)),"bind compiled original PS");
    static_draw::Ref<IDirect3DTexture9> texture;require(SUCCEEDED(d->CreateTexture(1,1,1,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,&texture.value,nullptr)) && SUCCEEDED(d->SetTexture(0,texture.value)),"bind original texture");
    for (auto rs:{D3DRS_ZENABLE,D3DRS_ZWRITEENABLE,D3DRS_ALPHABLENDENABLE,D3DRS_ALPHATESTENABLE}) { require(SUCCEEDED(d->SetRenderState(rs,0)),"post state"); }
    draw_observe::Snapshot s;s.status=draw_observe::Status::Valid;s.material=1;s.materialTable=2;s.materialInfo.format=0x80001;s.materialInfo.flags=3;std::memcpy(s.materialInfo.shader.data(),"Neko_Engine_Post",17);s.viewport={0,0,320,240};s.maxZ=1;
    for (auto* m:{&s.world,&s.view,&s.projection}) { (*m)[0]=(*m)[5]=(*m)[10]=(*m)[15]=1; }
    require(overlay::candidate(s,D3DPT_TRIANGLELIST,false) && !overlay::candidate(s,D3DPT_TRIANGLELIST,true),"bounded post classifier");
    const auto baseline=state(d);require(SUCCEEDED(d->BeginScene()),"begin scene");
    D3DSURFACE_DESC targetDesc{};require(SUCCEEDED(offscreen.value->GetDesc(&targetDesc)),"fixture target description");
    std::cout << "RT overlay fixture target: explicit_offscreen_backbuffer_alias format=" << uint32_t(targetDesc.Format) << " width=" << targetDesc.Width << " height=" << targetDesc.Height << "; real swapchain/Remix unrun\n";
    static_draw::Ref<IDirect3DSurface9> foreign;
    require(SUCCEEDED(d->CreateRenderTarget(320,240,D3DFMT_A8R8G8B8,D3DMULTISAMPLE_NONE,0,FALSE,&foreign.value,nullptr)),"create foreign identity");
    Proxy rejected{d};rejected.foreignBackbuffer=foreign.value;
    require(!overlay::inject(&rejected,s).replaced && !rejected.sets && !rejected.draws && state(d)==baseline,"different target identity must reject without mutations");
    Proxy success{d};const auto good=overlay::inject(&success,s);
    const auto after=state(d);
    std::cout << "RT overlay scope: code=" << uint32_t(good.report.code) << " capture=" << uint32_t(good.report.capture)
      << " apply=" << uint32_t(good.report.apply) << " draw=" << uint32_t(good.draw) << " restore=" << uint32_t(good.report.restore)
      << " calls=" << success.draws << " apply_sets=" << success.beforeDrawSets << " total_sets=" << success.sets << '\n';
    for (size_t i=0;i<baseline.size();++i) { if (baseline[i]!=after[i]) { std::cout << "state_delta index=" << i << " before=" << baseline[i] << " after=" << after[i] << '\n'; } }
    require(good.replaced && success.draws==1 && after==baseline,"transparent injection must restore stream0 and all sampled state");
    for (unsigned at=1;at<=success.beforeDrawSets;++at) {
      Proxy bad{d};bad.failSet=at;const auto r=overlay::inject(&bad,s);
      require(!r.replaced && FAILED(r.report.apply) && !bad.draws && state(d)==baseline,"partial-mutating setter must roll back before fallback");
    }
    for (unsigned at=success.beforeDrawSets+1;at<=success.sets;++at) {
      Proxy bad{d};bad.failSet=at;const auto r=overlay::inject(&bad,s);
      require(!r.replaced && FAILED(r.report.restore) && bad.draws==1 && state(d)==baseline,"manual restore failure must not be accepted");
    }
    for (unsigned at=1;at<=success.gets;++at) {
      Proxy bad{d};bad.failGet=at;const auto r=overlay::inject(&bad,s);
      require(!r.replaced && FAILED(r.report.capture) && !bad.sets && !bad.draws && state(d)==baseline,"capture failure must not mutate or draw");
    }
    for (unsigned failure=0;failure<3;++failure) {
      Proxy bad{d};bad.failCapture=failure==0;bad.failDraw=failure==1;bad.failRestore=failure==2;const auto r=overlay::inject(&bad,s);
      require(!r.replaced && state(d)==baseline,"capture/draw/restore failure must be reported");
      if (failure==2) { require(FAILED(r.report.restore),"restore failure hidden"); }
    }
    require(SUCCEEDED(d->EndScene()),"end scene");
    s.materialInfo.shader[0]='X';require(!overlay::candidate(s,D3DPT_TRIANGLELIST,false),"unknown post shader accepted");
    rt_tangent_test::systemCases(d,vs.value,ps.value);
    rt_scene_test::systemCases(d,vs.value,ps.value);
    DestroyWindow(window);
    std::cout << "RT rigid-unlit/overlay rollback/system NULLREF: PASS; effective Remix/HUD output unrun\n";
    return 0;
  } catch (const std::exception& e) { std::cerr << e.what() << '\n';return 1; }
}

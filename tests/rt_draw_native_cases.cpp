#define NOMINMAX
#include "rt_draw_client.h"
#include "rt_draw_host.h"
#include "rt_host.h"
#include "rt_static_device.h"
#include "rt_scene_cases.h"
#include <iostream>
using namespace ldb_rt::draw_observe;
void require(bool value, const char* why) { if (!value) { throw std::runtime_error(why); } }
bool requested = false, rejectActivation = false;
unsigned requestsOn = 0, requestsOff = 0;
bool raytracingRequest(bool enabled) noexcept {
  if (enabled && rejectActivation) { return false; }
  if (enabled != requested) { enabled ? ++requestsOn : ++requestsOff; requested = enabled; }
  return true;
}
struct Surface : IDirect3DSurface9 {
  ULONG refs=1;
  D3DFORMAT format=D3DFMT_A8R8G8B8;
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
  D3DRESOURCETYPE STDMETHODCALLTYPE GetType() override { return D3DRTYPE_SURFACE; }
  HRESULT STDMETHODCALLTYPE GetContainer(REFIID,void**) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetDesc(D3DSURFACE_DESC* p) override { *p={};p->Width=1920;p->Height=1080;p->Format=format;return S_OK; }
  HRESULT STDMETHODCALLTYPE LockRect(D3DLOCKED_RECT*,const RECT*,DWORD) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE UnlockRect() override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetDC(HDC*) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE ReleaseDC(HDC) override { return E_NOTIMPL; }
};
struct Graphics : rt_test::Graphics {
  Surface surface;
  bool failCull=false;
  HRESULT GetRenderTarget(DWORD, IDirect3DSurface9** output) { surface.AddRef();*output=&surface;return S_OK; }
  HRESULT GetRenderState(D3DRENDERSTATETYPE type,DWORD* value) {
    const auto result=rt_test::Graphics::GetRenderState(type,value);
    return type==D3DRS_CULLMODE && failCull?E_FAIL:result;
  }
};
int main(int argc, char** argv) {
  try {
    require(argc == 2,"new native output required");
    const auto output = fs::absolute(fs::u8path(argv[1])); require(fs::create_directory(output),"preserve output");
    const auto request = requestFile(); require(!fs::exists(request),"preserve preexisting request");
    struct Cleanup { fs::path path; ~Cleanup() { std::error_code ec; fs::remove(path,ec); } } cleanup{request};
    { std::ofstream file(output/"abc",std::ios::binary); file << "abc"; }
    require(fileSha256(output/"abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad","real CNG SHA256");
    require(ldb_rt_file::bytesSha256("abc",3)==fileSha256(output/"abc"),"CNG bytecode SHA256 differs from file digest");
    { std::ofstream file(request,std::ios::binary); file << "LDB_RT_DRAW_1\n2a\n" << output.u8string() << '\n'; }
    Request parsed; require(readRequest(parsed) && parsed.session == 42 && parsed.output == output,"real request parser");
    Binding binding; Snapshot s; require(binding.capture(s) == Status::ModuleMissing,"fixture must never claim game binding");
    s.session=42; s.ordinal=0; s.thread=GetCurrentThreadId(); s.status=Status::Valid; s.viewport={0,0,1920,1080};
    for (auto* matrix : {&s.world,&s.view,&s.projection}) { (*matrix)[0]=(*matrix)[5]=(*matrix)[10]=(*matrix)[15]=1; }
    Graphics device;
    const auto shaderRefs = device.shader.refs, declarationRefs = device.declaration.refs, bufferRefs = device.buffer.refs;
    {
      Recorder recorder(7,42);
      for (unsigned frame=0;frame<120;++frame) {
        ++s.ordinal; LARGE_INTEGER tick{}; QueryPerformanceCounter(&tick); s.ticks=uint64_t(tick.QuadPart);
        recorder.draw(&device,frame*2+1,s,2,D3DPT_TRIANGLELIST,{0,0,30,0,10,0});
        recorder.result(frame*2+1,S_OK); recorder.present(frame*2+2,S_OK);
      }
    }
    require(device.sets == 0 && !device.foreign,"observation must not modify raster state");
    require(shaderRefs==device.shader.refs && declarationRefs==device.declaration.refs && bufferRefs==device.buffer.refs,"public getter COM references released");
    require(device.surface.refs==1,"render target reference released");
    device.failCull=true;
    const auto failedCull=pipeline(&device,true);ldb_rt::Reader cullReader{failedCull.data(),failedCull.size()};
    require((cullReader.u32()&512)!=0 && !device.sets && device.surface.refs==1,"v7 cull getter failure missing or changed draw state");
    device.failCull=false;
    require(fs::exists(doneFile(parsed)),"complete marker");
    const auto fakeRuntime=output/"fixture-runtime";fs::create_directory(fakeRuntime);
    { std::ofstream file(fakeRuntime/"d3d9.dll");file << "not a DLL; request parser fixture only"; }
    { std::ofstream file(request,std::ios::binary);file << "LDB_RT_BETA_1\n2b\n" << output.u8string() << '\n' << fakeRuntime.u8string() << '\n'; }
    require(readRequest(parsed) && parsed.world && parsed.beta && parsed.session==43,"beta launch request without private calls");
    { std::ofstream file(request,std::ios::binary);file << "LDB_RT_PERF_1\n2b\n" << output.u8string() << '\n' << fakeRuntime.u8string() << '\n'; }
    require(readRequest(parsed) && parsed.world && parsed.performance && !parsed.beta && parsed.session==43,"performance launch request without private calls");
    { std::ofstream file(request,std::ios::binary);file << "LDB_RT_WORLD_1\n2b\n" << output.u8string() << '\n' << fakeRuntime.u8string() << '\n'; }
    require(readRequest(parsed) && parsed.world && !parsed.beta && parsed.session==43,"world launch request without private calls");
    ldb_rt::world_runtime::state().active=true; // Simulated-device conversion, never a GPU/runtime claim.
    ldb_rt::world_runtime::state().raytracing=&raytracingRequest;
    device.surface.format=D3DFMT_A16B16G16R16F;device.stride=48;
    device.declaration.elements.pop_back();
    device.declaration.elements.push_back({2,0,D3DDECLTYPE_FLOAT4,D3DDECLMETHOD_DEFAULT,D3DDECLUSAGE_POSITION,1});
    device.declaration.elements.push_back({2,16,D3DDECLTYPE_FLOAT3,D3DDECLMETHOD_DEFAULT,D3DDECLUSAGE_NORMAL,1});
    device.declaration.elements.push_back(D3DDECL_END());
    s.session=43;s.ordinal=0;s.material=0x1000;s.materialTable=0x2000;s.projection={1,0,0,0,0,1,0,0,0,0,-1.0001f,-1,0,0,-7.0007f,0};
    s.materialInfo.format=0x480003;s.materialInfo.flags=3;std::memcpy(s.materialInfo.shader.data(),"LightmappedGeneric_DX9",22);
    {
      ldb_rt::consumer_world::Decision decision;
      ldb_rt::static_draw::Report report;
      device.stride=44;
      auto original=[&] { require(device.currentShader!=nullptr && !requested,"rejected path lost original raster shader"); return S_OK; };
      require(SUCCEEDED(ldb_rt::consumer_world::execute(&device,s,D3DPT_TRIANGLELIST,false,true,false,original,decision,report,&raytracingRequest)) &&
        !report.converted && report.code==ldb_rt::static_draw::Code::Layout && !requestsOn,"invalid layout must not request RT");
      device.stride=48;rejectActivation=true;
      require(SUCCEEDED(ldb_rt::consumer_world::execute(&device,s,D3DPT_TRIANGLELIST,false,true,false,original,decision,report,&raytracingRequest)) &&
        !report.converted && decision==ldb_rt::consumer_world::Decision::Disabled && device.restored() && device.leasesReleased() &&
        !requestsOn,"failed activation must restore before original raster Draw");
      rejectActivation=false;
    }
    uint32_t uid=1;
    for (unsigned frame=0;frame<ldb_rt::world_diagnostics::kCoveragePresents;++frame) {
      for (unsigned draw=0;draw<3;++draw) {
        ++s.ordinal; s.materialInfo.format=draw==1 ? 0x80407 : 0x480003;
        s.maxZ=draw==2 ? .1f : 1.f;device.viewport.MaxZ=s.maxZ;
        const auto bytes=pack(s);observe(8,uid,&device,bytes.data(),bytes.size(),2,D3DPT_TRIANGLELIST,{0,0,3,0,1,0});
        const auto result=ldb_rt_host::drawCommand(8,uid,&device,D3DPT_TRIANGLELIST,false,[&] {
          require((device.currentShader==nullptr)==(frame>=30 && draw==0),"world filter/control or shader state wrong at physical Draw");
          require(requested==(frame>=30),"RT request occurred before valid conversion or was lost");return S_OK;
        });
        require(SUCCEEDED(result) && device.restored() && device.leasesReleased(),"consumer conversion or restore failed");
        ldb_rt_host::drawResult(8,uid++,result);
      }
      if (frame==122 || frame==180 || frame==181) {
        const unsigned kind=frame==122?0:frame==180?1:2;
        auto scene=rt_scene_test::snapshot(kind);scene.session=43;scene.ordinal=++s.ordinal;scene.thread=s.thread;
        LARGE_INTEGER tick{};QueryPerformanceCounter(&tick);scene.ticks=uint64_t(tick.QuadPart);
        const auto originalDeclaration=device.declaration.elements;
        device.declaration.elements=rt_scene_test::declaration(kind);device.stride=32;device.viewport.MaxZ=1;
        device.depth=device.depthWrite=kind?1u:0u;
        const auto bytes=pack(scene);observe(8,uid,&device,bytes.data(),bytes.size(),2,D3DPT_TRIANGLELIST,{0,0,3,0,1,0});
        const auto result=ldb_rt_host::drawCommand(8,uid,&device,D3DPT_TRIANGLELIST,false,[&] {
          require(!device.currentShader && (kind?!device.currentPixel:device.currentPixel==&device.pixel) && !device.lighting,
            "Host scene routing did not apply scope");
          require(requested && (kind || device.category==ldb_rt::scene_draw::kSkyCategory),"Host scene category/activation");return S_OK;
        });
        require(SUCCEEDED(result) && device.restored() && device.leasesReleased() && device.lighting==1 &&
          device.category==ldb_rt::scene_draw::kUnusedCategory,"Host scene original state not restored");
        ldb_rt_host::drawResult(8,uid++,result);
        device.declaration.elements=originalDeclaration;device.stride=48;device.depth=device.depthWrite=1;
      }
      ldb_rt_host::present(8,uid++,S_OK);
      require(ldb_rt::world_runtime::state().presents==frame+1,"world submission/readback frame identity lost");
    }
    require(!requested && requestsOn==1 && requestsOff==1 && !ldb_rt::world_runtime::state().active,"capture end must request raster restoration exactly once");
    ++s.ordinal;s.materialInfo.format=0x480003;s.maxZ=1;device.viewport.MaxZ=1;
    const auto after=pack(s);observe(8,uid,&device,after.data(),after.size(),2,D3DPT_TRIANGLELIST,{0,0,3,0,1,0});
    require(SUCCEEDED(ldb_rt_host::drawCommand(8,uid,&device,D3DPT_TRIANGLELIST,false,[&] {
      require(device.currentShader!=nullptr && !requested,"post-capture Draw re-enabled RT");return S_OK;
    })) && requestsOn==1,"post-capture must stay raster");
    ldb_rt_host::destroy(8);
    require(!ldb_rt::world_runtime::state().active && fs::exists(doneFile(parsed)),"world closure");
    std::cout << "RT consumer world control/filter/layout/restore: PASS; game shader ABI and GPU RT unrun\n";
    std::cout << "RT world startup gate/activation failure/raster restoration: PASS; GPU unrun\n";
    std::cout << "RT Draw native request/CNG/Host observation: PASS; game ABI/queue/RT unrun\n"; return 0;
  } catch(const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}

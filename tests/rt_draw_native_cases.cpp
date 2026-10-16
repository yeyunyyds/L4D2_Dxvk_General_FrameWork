#define NOMINMAX
#include "rt_draw_client.h"
#include "rt_draw_host.h"
#include "rt_static_device.h"
#include <iostream>
using namespace ldb_rt::draw_observe;
void require(bool value, const char* why) { if (!value) { throw std::runtime_error(why); } }
struct Surface : IDirect3DSurface9 {
  ULONG refs=1;
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
  HRESULT STDMETHODCALLTYPE GetDesc(D3DSURFACE_DESC* p) override { *p={};p->Width=1920;p->Height=1080;p->Format=D3DFMT_A8R8G8B8;return S_OK; }
  HRESULT STDMETHODCALLTYPE LockRect(D3DLOCKED_RECT*,const RECT*,DWORD) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE UnlockRect() override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetDC(HDC*) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE ReleaseDC(HDC) override { return E_NOTIMPL; }
};
struct Graphics : rt_test::Graphics {
  Surface surface;
  HRESULT GetRenderTarget(DWORD, IDirect3DSurface9** output) { surface.AddRef();*output=&surface;return S_OK; }
};
int main(int argc, char** argv) {
  try {
    require(argc == 2,"new native output required");
    const auto output = fs::absolute(fs::u8path(argv[1])); require(fs::create_directory(output),"preserve output");
    const auto request = requestFile(); require(!fs::exists(request),"preserve preexisting request");
    struct Cleanup { fs::path path; ~Cleanup() { std::error_code ec; fs::remove(path,ec); } } cleanup{request};
    { std::ofstream file(output/"abc",std::ios::binary); file << "abc"; }
    require(fileSha256(output/"abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad","real CNG SHA256");
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
    require(fs::exists(doneFile(parsed)),"complete marker");
    std::cout << "RT Draw native request/CNG/Host observation: PASS; game ABI/queue/RT unrun\n"; return 0;
  } catch(const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}

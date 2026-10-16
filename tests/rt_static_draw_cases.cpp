#include "rt_static_device.h"
#include <functional>
#include <iostream>
#include <limits>

using namespace ldb_rt;
using namespace static_draw;
void require(bool value, const char* why) { if (!value) { throw std::runtime_error(why); } }

void conversionAndRestore() {
  rt_test::Graphics device;
  auto snapshot = rt_test::snapshot();
  snapshot.draw.transform = {0, -2, 0, 11, 3, 0, 0, 22, 0, 0, 4, 33};
  snapshot.camera.view[12] = 7; snapshot.camera.projection[0] = 2;
  Report report;
  unsigned draws = 0;
  const auto result = execute(&device, &snapshot, D3DPT_TRIANGLELIST, false, true, [&] {
    ++draws;
    const auto& w = device.transforms[0];
    require(!device.currentShader && device.currentFvf == (D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_TEX2), "not converted at Draw");
    require(w.m[0][1] == 3 && w.m[1][0] == -2 && w.m[2][2] == 4 &&
            w.m[3][0] == 11 && w.m[3][1] == 22 && w.m[3][2] == 33 && w.m[3][3] == 1, "Source to D3D transform convention");
    require(device.transforms[1].m[3][0] == 7 && device.transforms[2].m[0][0] == 2, "owned camera not applied");
    return S_OK;
  }, report);
  require(SUCCEEDED(result) && draws == 1 && report.code == Code::Applied && report.converted, "static draw failed");
  require(device.restored() && device.leasesReleased() && !device.foreign, "static state/ref leak");
  // Windows can derive a nonzero FVF for a manually created declaration. It
  // still needs that exact declaration back, rather than another FVF object.
  device.declaration.fvf = D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_TEX2;
  device.SetVertexDeclaration(&device.declaration);
  execute(&device, &snapshot, D3DPT_TRIANGLELIST, false, true, [] { return S_OK; }, report);
  require(device.restored() && device.leasesReleased(), "FVF-compatible manual declaration identity lost");
  // Restore FVF binding as an FVF binding, not an arbitrary declaration.
  device.SetFVF(D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_TEX2);
  const auto oldFvf = device.currentFvf;
  execute(&device, &snapshot, D3DPT_TRIANGLESTRIP, false, true, [] { return S_OK; }, report);
  require(device.currentFvf == oldFvf && device.currentDeclaration == &device.ffp, "original FVF binding lost");
}

void observedPaddedWorldLayout() {
  rt_test::Graphics device; auto snapshot = rt_test::snapshot(); device.stride = 48;
  device.declaration.elements.pop_back();
  device.declaration.elements.push_back({2,0,D3DDECLTYPE_FLOAT4,D3DDECLMETHOD_DEFAULT,D3DDECLUSAGE_POSITION,1});
  device.declaration.elements.push_back({2,16,D3DDECLTYPE_FLOAT3,D3DDECLMETHOD_DEFAULT,D3DDECLUSAGE_NORMAL,1});
  device.declaration.elements.push_back(D3DDECL_END());
  Report report; unsigned draws = 0;
  execute(&device,&snapshot,D3DPT_TRIANGLELIST,false,true,[&] {
    ++draws; require(device.stride==48 && device.currentShader==nullptr,"padded stride or shader changed incorrectly"); return S_OK;
  },report);
  require(draws==1 && report.converted && device.restored() && device.leasesReleased(),"actual world layout conversion/restore");
  device.declaration.elements[4].UsageIndex=0;
  execute(&device,&snapshot,D3DPT_TRIANGLELIST,false,true,[] {return S_OK;},report);
  require(!report.converted && report.code==Code::Layout && device.restored(),"unknown auxiliary layout accepted");
}

void rejectionPreservesDraw() {
  struct Case { Code code; std::function<void(rt_test::Graphics&, DrawSnapshot&)> change; };
  const Case cases[] = {
    {Code::NotStaticWorld, [](auto&, auto& s) { s.draw.categories = 0; }},
    {Code::NotStaticWorld, [](auto&, auto& s) { s.draw.modifiers = 1; }},
    {Code::NotStaticWorld, [](auto&, auto& s) { s.camera.kind = ViewKind::Hud; }},
    {Code::NotStaticWorld, [](auto&, auto& s) { s.camera.kind = ViewKind::Weapon; }},
    {Code::Layout, [](auto& d, auto&) { d.stride = 32; }},
    {Code::Layout, [](auto& d, auto&) { d.frequency = D3DSTREAMSOURCE_INDEXEDDATA | 2; }},
    {Code::Layout, [](auto& d, auto&) { d.declaration.elements[1].Type = D3DDECLTYPE_D3DCOLOR; }},
    {Code::Layout, [](auto& d, auto&) { d.declaration.elements[2].Stream = 1; }},
    {Code::Pipeline, [](auto& d, auto&) { d.alpha = 1; }},
    {Code::Pipeline, [](auto& d, auto&) { d.alphaTest = 1; }},
    {Code::Pipeline, [](auto& d, auto&) { d.depthWrite = 0; }},
    {Code::Pipeline, [](auto& d, auto&) { d.viewport.Width = 1024; }},
    {Code::Camera, [](auto&, auto& s) { s.camera.view = {}; }},
    {Code::Camera, [](auto&, auto& s) { s.draw.transform[0] = std::numeric_limits<float>::quiet_NaN(); }}
  };
  for (const auto& item : cases) {
    rt_test::Graphics device; auto snapshot = rt_test::snapshot(); item.change(device, snapshot);
    unsigned draws = 0; Report report;
    const auto result = execute(&device, &snapshot, D3DPT_TRIANGLELIST, false, true,
      [&] { ++draws; require(device.restored(), "rejected path changed state"); return S_OK; }, report);
    require(SUCCEEDED(result) && draws == 1 && report.code == item.code && !device.sets && device.leasesReleased(), "rejection hid a draw or leaked state");
  }
  for (unsigned mode = 0; mode < 4; ++mode) {
    rt_test::Graphics device; auto snapshot = rt_test::snapshot(); Report report; unsigned draws = 0;
    execute(&device, mode == 1 ? nullptr : &snapshot, mode == 2 ? D3DPT_LINELIST : D3DPT_TRIANGLELIST,
      mode == 3, mode != 0, [&] { ++draws; return S_OK; }, report);
    require(draws == 1 && !device.gets && !device.sets && !report.converted, "disabled/unlabelled/topology path touched state");
  }
}

void failuresRestoreAndRemainVisible() {
  for (unsigned at = 1; at <= 12; ++at) {
    rt_test::Graphics device; device.failGet = at; auto snapshot = rt_test::snapshot(); Report report;
    unsigned draws = 0;
    execute(&device, &snapshot, D3DPT_TRIANGLELIST, false, true, [&] { ++draws; return S_OK; }, report);
    require(draws == 1 && report.code == Code::CaptureFailed && !device.sets && device.restored() && device.leasesReleased(), "capture failure not retained safely");
  }
  for (unsigned at = 1; at <= 5; ++at) {
    rt_test::Graphics device; device.failSet = {at}; auto snapshot = rt_test::snapshot(); Report report;
    unsigned draws = 0;
    execute(&device, &snapshot, D3DPT_TRIANGLELIST, false, true, [&] {
      ++draws; require(device.restored(), "failed apply did not restore before fallback"); return S_OK;
    }, report);
    require(draws == 1 && report.code == Code::ApplyFailed && FAILED(report.apply) && SUCCEEDED(report.restore) && device.leasesReleased(), "failed apply hidden");
  }
  for (unsigned at = 6; at <= 10; ++at) {
    rt_test::Graphics device; device.failSet = {at}; auto snapshot = rt_test::snapshot(); Report report;
    unsigned draws = 0;
    const auto result = execute(&device, &snapshot, D3DPT_TRIANGLELIST, false, true, [&] { ++draws; return S_OK; }, report);
    require(FAILED(result) && draws == 1 && report.code == Code::RestoreFailed && FAILED(report.restore) && device.sets == 10, "restore failure was accepted or skipped later restores");
  }
  {
    rt_test::Graphics device; device.failSet = {1, 2}; auto snapshot = rt_test::snapshot(); Report report;
    unsigned draws = 0;
    const auto result = execute(&device, &snapshot, D3DPT_TRIANGLELIST, false, true, [&] { ++draws; return S_OK; }, report);
    require(FAILED(result) && !draws && report.code == Code::RestoreFailed, "unrestorable pipeline used for a draw");
  }
  {
    rt_test::Graphics device; auto snapshot = rt_test::snapshot(); Report report;
    const auto result = execute(&device, &snapshot, D3DPT_TRIANGLELIST, false, true, [] { return E_FAIL; }, report);
    require(FAILED(result) && device.restored() && device.leasesReleased(), "physical draw failure leaked state");
    // Type erasure keeps the deliberately throwing callback from making the
    // template's success path syntactically unreachable under MSVC /W4 /WX.
    std::function<HRESULT()> throwingDraw = []() -> HRESULT { throw std::runtime_error("draw"); };
    bool threw = false;
    try { execute(&device, &snapshot, D3DPT_TRIANGLELIST, false, true, throwingDraw, report); }
    catch (const std::runtime_error&) { threw = true; }
    require(threw, "missing test exception");
    require(device.restored() && device.leasesReleased(), "exception leaked state");
  }
}

// Actual Windows D3D9 COM/state roundtrip when NULLREF is present. It performs
// no rasterization or RT, and cannot substitute for Remix/B580 validation.
void systemD3D9State() {
  const auto unavailable = [](const char* stage, HRESULT result) {
    std::cout << "RT static-world system D3D9 NULLREF: UNAVAILABLE (" << stage << ", hr="
      << uint32_t(result) << ", GPU RT unrun, ptr_bits=" << 8 * sizeof(void*) << ")\n";
  };
  struct Library {
    HMODULE module = LoadLibraryExW(L"d3d9.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    ~Library() { if (module) { FreeLibrary(module); } }
  } library;
  if (!library.module) { unavailable("load", HRESULT_FROM_WIN32(GetLastError())); return; }
  const auto address = GetProcAddress(library.module, "Direct3DCreate9");
  using Create = IDirect3D9* (WINAPI*)(UINT);
  Create create = nullptr;
  static_assert(sizeof(create) == sizeof(address), "Windows function address");
  std::memcpy(&create, &address, sizeof(create));
  if (!create) { unavailable("export", E_NOINTERFACE); return; }
  Ref<IDirect3D9> d3d; d3d.value = create(D3D_SDK_VERSION);
  if (!d3d.value) { unavailable("create_d3d9", E_FAIL); return; }
  struct Window {
    HWND handle = CreateWindowExW(0, L"STATIC", L"LDB state check", WS_OVERLAPPED,
      0, 0, 320, 240, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    ~Window() { if (handle) { DestroyWindow(handle); } }
  } window;
  if (!window.handle) { unavailable("window", HRESULT_FROM_WIN32(GetLastError())); return; }
  Ref<IDirect3DDevice9> device;
  D3DPRESENT_PARAMETERS present{};
  present.BackBufferWidth = 320; present.BackBufferHeight = 240;
  present.BackBufferFormat = D3DFMT_A8R8G8B8; present.BackBufferCount = 1;
  present.SwapEffect = D3DSWAPEFFECT_DISCARD; present.hDeviceWindow = window.handle; present.Windowed = TRUE;
  present.EnableAutoDepthStencil = TRUE; present.AutoDepthStencilFormat = D3DFMT_D16;
  const auto created = d3d.value->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_NULLREF, window.handle,
    D3DCREATE_SOFTWARE_VERTEXPROCESSING, &present, &device.value);
  if (FAILED(created)) { unavailable("create_nullref", created); return; }
  auto check = [](HRESULT result) { require(SUCCEEDED(result), "system D3D9 state/draw failed after device creation"); };
  rt_test::Declaration layout;
  Ref<IDirect3DVertexDeclaration9> declaration;
  check(device.value->CreateVertexDeclaration(layout.elements.data(), &declaration.value));
  Ref<IDirect3DVertexBuffer9> buffer;
  check(device.value->CreateVertexBuffer(120, D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED, &buffer.value, nullptr));
  const float triangle[30] = {-1, -1, 3, 0, 0, -1, 0, 0, 0, 0,
                              0, 1, 3, 0, 0, -1, 0, 0, 0, 0,
                              1, -1, 3, 0, 0, -1, 0, 0, 0, 0};
  void* locked = nullptr;
  check(buffer.value->Lock(0, UINT(sizeof(triangle)), &locked, 0));
  require(locked != nullptr, "NULLREF buffer lock returned null");
  std::memcpy(locked, triangle, sizeof(triangle)); check(buffer.value->Unlock());
  check(device.value->SetStreamSource(0, buffer.value, 0, 40));
  check(device.value->SetStreamSourceFreq(0, 1));
  check(device.value->SetVertexDeclaration(declaration.value));
  const D3DVIEWPORT9 viewport{0, 0, 320, 240, 0, 1};
  check(device.value->SetViewport(&viewport));
  check(device.value->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE));
  check(device.value->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE));
  check(device.value->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE));
  check(device.value->SetRenderState(D3DRS_ZWRITEENABLE, TRUE));
  const auto original = rt_test::matrix(123);
  for (auto type : {D3DTS_WORLD, D3DTS_VIEW, D3DTS_PROJECTION}) { check(device.value->SetTransform(type, &original)); }
  DWORD oldFvf = 0; check(device.value->GetFVF(&oldFvf));
  auto snapshot = rt_test::snapshot(); snapshot.camera.width = 320; snapshot.camera.height = 240;
  Report report;
  check(device.value->BeginScene());
  const auto result = execute(device.value, &snapshot, D3DPT_TRIANGLELIST, false, true, [&] {
    return device.value->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 1);
  }, report);
  check(device.value->EndScene()); check(result);
  require(report.code == Code::Applied && report.converted && SUCCEEDED(report.restore), "system D3D9 conversion/restore failed");
  for (auto type : {D3DTS_WORLD, D3DTS_VIEW, D3DTS_PROJECTION}) {
    D3DMATRIX actual{}; check(device.value->GetTransform(type, &actual));
    require(!std::memcmp(&actual, &original, sizeof(actual)), "system D3D9 matrix restore mismatch");
  }
  DWORD restoredFvf = 0; check(device.value->GetFVF(&restoredFvf));
  require(oldFvf == restoredFvf, "system D3D9 FVF restore mismatch");
  Ref<IDirect3DVertexDeclaration9> actual;
  check(device.value->GetVertexDeclaration(&actual.value));
  const GUID unknown{0, 0, 0, {0xc0, 0, 0, 0, 0, 0, 0, 0x46}};
  Ref<IUnknown> expectedIdentity, actualIdentity;
  check(declaration.value->QueryInterface(unknown, reinterpret_cast<void**>(&expectedIdentity.value)));
  check(actual.value->QueryInterface(unknown, reinterpret_cast<void**>(&actualIdentity.value)));
  std::cout << "NULLREF original_fvf=" << oldFvf << " restored_fvf=" << restoredFvf << "\n";
  require(expectedIdentity.value == actualIdentity.value, "system D3D9 declaration COM identity restore mismatch");
  // Exercise the captured 48-byte / extra-stream declaration on real D3D9 too.
  layout.elements.pop_back();
  layout.elements.push_back({2,0,D3DDECLTYPE_FLOAT4,D3DDECLMETHOD_DEFAULT,D3DDECLUSAGE_POSITION,1});
  layout.elements.push_back({2,16,D3DDECLTYPE_FLOAT3,D3DDECLMETHOD_DEFAULT,D3DDECLUSAGE_NORMAL,1});
  layout.elements.push_back(D3DDECL_END());
  Ref<IDirect3DVertexDeclaration9> paddedDeclaration;
  Ref<IDirect3DVertexBuffer9> paddedBuffer;
  check(device.value->CreateVertexDeclaration(layout.elements.data(),&paddedDeclaration.value));
  check(device.value->CreateVertexBuffer(144,D3DUSAGE_WRITEONLY,0,D3DPOOL_MANAGED,&paddedBuffer.value,nullptr));
  check(paddedBuffer.value->Lock(0,144,&locked,0));require(locked!=nullptr,"padded NULLREF lock");
  std::memset(locked,0,144);
  for(unsigned i=0;i<3;++i){std::memcpy(static_cast<char*>(locked)+i*48,triangle+i*10,40);}
  check(paddedBuffer.value->Unlock());
  check(device.value->SetStreamSource(0,paddedBuffer.value,0,48));
  check(device.value->SetStreamSource(2,paddedBuffer.value,0,48));check(device.value->SetStreamSourceFreq(2,1));
  check(device.value->SetVertexDeclaration(paddedDeclaration.value));
  check(device.value->BeginScene());
  const auto paddedResult=execute(device.value,&snapshot,D3DPT_TRIANGLELIST,false,true,[&] {
    return device.value->DrawPrimitive(D3DPT_TRIANGLELIST,0,1);
  },report);
  check(device.value->EndScene());check(paddedResult);
  require(report.converted && SUCCEEDED(report.restore),"real D3D9 padded conversion/restore");
  Ref<IDirect3DVertexDeclaration9> restoredPadded;
  check(device.value->GetVertexDeclaration(&restoredPadded.value));
  Ref<IUnknown> paddedIdentity,restoredIdentity;
  check(paddedDeclaration.value->QueryInterface(unknown,reinterpret_cast<void**>(&paddedIdentity.value)));
  check(restoredPadded.value->QueryInterface(unknown,reinterpret_cast<void**>(&restoredIdentity.value)));
  require(paddedIdentity.value==restoredIdentity.value,"padded declaration identity lost");
  UINT actualOffset=0,actualStride=0;Ref<IDirect3DVertexBuffer9> actualBuffer;
  check(device.value->GetStreamSource(0,&actualBuffer.value,&actualOffset,&actualStride));
  require(actualStride==48 && actualOffset==0,"padded stride was rewritten");
  std::cout << "RT static-world system D3D9 NULLREF: PASS (state/draw only, GPU RT unrun, ptr_bits=" << 8 * sizeof(void*) << ")\n";
}

int main() {
  try {
    conversionAndRestore(); observedPaddedWorldLayout(); rejectionPreservesDraw(); failuresRestoreAndRemainVisible();
    std::cout << "RT static-world scoped D3D9 methods: PASS (3 groups, GPU unrun, ptr_bits=" << 8 * sizeof(void*) << ")\n";
    systemD3D9State();
    return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}

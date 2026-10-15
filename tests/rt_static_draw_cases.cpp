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
  // Restore FVF binding as an FVF binding, not an arbitrary declaration.
  device.SetFVF(D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_TEX2);
  const auto oldFvf = device.currentFvf;
  execute(&device, &snapshot, D3DPT_TRIANGLESTRIP, false, true, [] { return S_OK; }, report);
  require(device.currentFvf == oldFvf && device.currentDeclaration == &device.ffp, "original FVF binding lost");
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
  for (unsigned at = 1; at <= 13; ++at) {
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
    try {
      execute(&device, &snapshot, D3DPT_TRIANGLELIST, false, true, []() -> HRESULT { throw std::runtime_error("draw"); }, report);
      require(false, "missing test exception");
    } catch (const std::runtime_error&) { }
    require(device.restored() && device.leasesReleased(), "exception leaked state");
  }
}

int main() {
  try {
    conversionAndRestore(); rejectionPreservesDraw(); failuresRestoreAndRemainVisible();
    std::cout << "RT static-world scoped D3D9 methods: PASS (3 groups, GPU unrun, ptr_bits=" << 8 * sizeof(void*) << ")\n";
    return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}

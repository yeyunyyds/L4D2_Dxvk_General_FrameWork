#pragma once
#include "rt_draw_packet.h"
#include "rt_static_draw.h"

namespace ldb_rt::consumer_world {
enum class Decision : uint32_t { Disabled, Control, SourceState, Material, Transform, Camera, Target, Pipeline, Selected };
inline Decision classify(const draw_observe::Snapshot& s) noexcept {
  if (s.status != draw_observe::Status::Valid) { return Decision::SourceState; }
  if (!s.material || !s.materialTable || s.materialInfo.flags != (material_decode::FormatRead | material_decode::ShaderRead) ||
      s.materialInfo.format != 0x480003 || !material_decode::worldShader(s.materialInfo.shader)) { return Decision::Material; }
  for (unsigned i = 0; i < 16; ++i) {
    if (!std::isfinite(s.world[i]) || std::abs(s.world[i] - float(i % 5 == 0)) > 1e-6f) { return Decision::Transform; }
  }
  if (!invertible(s.view) || !invertible(s.projection) || s.minZ != 0 || s.maxZ != 1 ||
      s.viewport[0] || s.viewport[1] || !s.viewport[2] || !s.viewport[3] ||
      std::abs(s.projection[11] + 1) > 1e-5f || std::abs(s.projection[15]) > 1e-5f ||
      s.projection[0] <= 0 || s.projection[5] <= 0 || s.projection[10] >= -1) { return Decision::Camera; }
  const float nearPlane = s.projection[14] / s.projection[10];
  if (!std::isfinite(nearPlane) || nearPlane < 4 || nearPlane > 32) { return Decision::Camera; }
  // Exclude scaled sky/view matrices; this intentionally selects a narrow pilot.
  for (unsigned i = 0; i < 3; ++i) {
    for (unsigned j = 0; j < 3; ++j) {
      double dot = 0; for (unsigned k = 0; k < 3; ++k) { dot += double(s.view[i * 4 + k]) * s.view[j * 4 + k]; }
      if (std::abs(dot - double(i == j)) > 0.01) { return Decision::Camera; }
    }
  }
  return Decision::Selected;
}
template<class Device> Decision target(Device* device, const draw_observe::Snapshot& s) {
  IDirect3DSurface9* surface = nullptr;
  D3DSURFACE_DESC desc{};
  const auto result = device->GetRenderTarget(0, &surface);
  HRESULT described = E_FAIL;
  if (surface) { described = surface->GetDesc(&desc); surface->Release(); }
  if (FAILED(result) || FAILED(described) || desc.Width != s.viewport[2] || desc.Height != s.viewport[3] ||
      desc.Format != D3DFMT_A16B16G16R16F) { return Decision::Target; }
  D3DVIEWPORT9 vp{};
  if (FAILED(device->GetViewport(&vp)) || vp.X != s.viewport[0] || vp.Y != s.viewport[1] ||
      vp.Width != s.viewport[2] || vp.Height != s.viewport[3] || vp.MinZ != s.minZ || vp.MaxZ != s.maxZ) { return Decision::Pipeline; }
  const D3DRENDERSTATETYPE states[] = {D3DRS_ALPHABLENDENABLE,D3DRS_ALPHATESTENABLE,D3DRS_ZENABLE,D3DRS_ZWRITEENABLE};
  DWORD values[4]{};
  for (unsigned i=0;i<4;++i) { if (FAILED(device->GetRenderState(states[i],&values[i]))) { return Decision::Pipeline; } }
  if (values[0] || values[1] || values[2]!=D3DZB_TRUE || values[3]!=1) { return Decision::Pipeline; }
  return Decision::Selected;
}
template<class Device, class Draw> HRESULT execute(Device* device, const draw_observe::Snapshot& s,
    D3DPRIMITIVETYPE type, bool userMemory, bool enabled, bool control, Draw&& physicalDraw,
    Decision& decision, static_draw::Report& report) {
  decision = !enabled ? Decision::Disabled : control ? Decision::Control : classify(s);
  if (decision == Decision::Selected) { decision = target(device, s); }
  if (decision != Decision::Selected) { return physicalDraw(); }
  typename static_draw::Scope<Device>::ConsumerInput input;
  std::memcpy(&input.world, s.world.data(), sizeof(input.world)); input.view = s.view; input.projection = s.projection;
  input.width = s.viewport[2]; input.height = s.viewport[3];
  static_draw::Scope<Device> scope(device, input, type, userMemory);
  if (FAILED(scope.report().restore)) { report = scope.report(); return report.restore; }
  const HRESULT result = physicalDraw(); scope.finish(); report = scope.report();
  return FAILED(report.restore) ? report.restore : result;
}
} // namespace ldb_rt::consumer_world

#pragma once

#include "rt_protocol.h"
#include <d3d9.h>

namespace ldb_rt {

// Explicit producer classification, not a guess from a vertex declaration.
constexpr uint32_t kStaticOpaqueWorld = 1;

namespace static_draw {

enum class Code : uint32_t {
  Disabled, Unlabelled, NotStaticWorld, Topology, Layout, Pipeline, Camera,
  CaptureFailed, ApplyFailed, Applied, RestoreFailed
};
struct Report {
  Code code = Code::Disabled;
  HRESULT capture = S_OK, apply = S_OK, restore = S_OK;
  bool converted = false;
};
// Exact observed declarations; never infer a layout from stride alone.
enum class Layout : uint32_t { World, Unlit, Tangent2UV, Tangent3UV };

template<class T> struct Ref {
  T* value = nullptr;
  Ref() = default;
  Ref(const Ref&) = delete;
  Ref& operator=(const Ref&) = delete;
  ~Ref() { if (value) { value->Release(); } }
};

// DrawInfo's affine transform is a Source row-major 3x4, acting on column
// vectors. D3D9 matrices act on row vectors. Camera matrices are already D3D9
// row-vector matrices; the game adapter must explicitly convert them there.
inline D3DMATRIX worldMatrix(const std::array<float, 12>& source) noexcept {
  D3DMATRIX result{};
  for (unsigned r = 0; r < 4; ++r) {
    for (unsigned c = 0; c < 3; ++c) { result.m[r][c] = source[c * 4 + r]; }
  }
  result.m[3][3] = 1;
  return result;
}

// Diagnostic implementation: state queries are deliberate. This is not a
// production performance profile or a complete LightmappedGeneric material port.
// Device is the actual owner-thread D3D9 device; tests supply the same methods.
template<class Device> class Scope final {
  Device* m_device;
  Ref<IDirect3DVertexShader9> m_shader;
  Ref<IDirect3DPixelShader9> m_pixel;
  Ref<IDirect3DVertexDeclaration9> m_declaration;
  Ref<IDirect3DVertexBuffer9> m_buffer;
  D3DMATRIX m_world{}, m_view{}, m_projection{};
  bool m_dirty = false;
  bool m_fixedPixel = false;
  Report m_report;

  bool capture(HRESULT result) noexcept {
    if (FAILED(result)) {
      m_report.capture = result; m_report.code = Code::CaptureFailed; return false;
    }
    return true;
  }
  bool set(HRESULT result) noexcept {
    if (FAILED(result)) {
      m_report.apply = result; m_report.code = Code::ApplyFailed;
      finish(); return false;
    }
    return true;
  }
  void initialize(const D3DMATRIX& world, const std::array<float, 16>& viewValues,
      const std::array<float, 16>& projectionValues, uint32_t width, uint32_t height,
      D3DPRIMITIVETYPE type, bool userMemory, Layout layout = Layout::World) noexcept {
    if (userMemory || (type != D3DPT_TRIANGLELIST && type != D3DPT_TRIANGLESTRIP)) {
      m_report.code = Code::Topology; return;
    }
    if (!m_device || !width || !height || !invertible(viewValues) || !invertible(projectionValues)) {
      m_report.code = Code::Camera; return;
    }
    for (const auto& row : world.m) { for (float value : row) {
      if (!std::isfinite(value)) { m_report.code = Code::Camera; return; }
    } }
    auto* device = m_device;
    D3DVIEWPORT9 viewport{};
    DWORD alpha = 0, test = 0, depthWrite = 0, depth = 0;
    if (!capture(device->GetViewport(&viewport)) ||
        !capture(device->GetRenderState(D3DRS_ALPHABLENDENABLE, &alpha)) ||
        !capture(device->GetRenderState(D3DRS_ALPHATESTENABLE, &test)) ||
        !capture(device->GetRenderState(D3DRS_ZWRITEENABLE, &depthWrite)) ||
        !capture(device->GetRenderState(D3DRS_ZENABLE, &depth))) { return; }
    if (alpha || test || !depthWrite || depth != D3DZB_TRUE ||
        viewport.Width != width || viewport.Height != height || viewport.MinZ != 0 || viewport.MaxZ != 1) {
      m_report.code = Code::Pipeline; return;
    }
    if (!capture(device->GetVertexDeclaration(&m_declaration.value))) { return; }
    if (!m_declaration.value) { m_report.code = Code::Layout; return; }
    D3DVERTEXELEMENT9 elements[MAXD3DDECLLENGTH + 1]{};
    UINT count = MAXD3DDECLLENGTH + 1;
    if (!capture(m_declaration.value->GetDeclaration(elements, &count))) { return; }
    const D3DVERTEXELEMENT9 expected[] = {
      {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
      {0, 12, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_NORMAL, 0},
      {0, 24, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
      {0, 32, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 1},
      D3DDECL_END()
    };
    const D3DVERTEXELEMENT9 observed[] = {
      expected[0], expected[1], expected[2], expected[3],
      {2, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 1},
      {2, 16, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_NORMAL, 1}, D3DDECL_END()
    };
    const bool originalLayout = count == 5 && !std::memcmp(elements, expected, sizeof(expected));
    const bool observedLayout = count == 7 && !std::memcmp(elements, observed, sizeof(observed));
    const D3DVERTEXELEMENT9 unlit[] = {expected[0], expected[1], expected[2],
      {1,0,D3DDECLTYPE_D3DCOLOR,0,D3DDECLUSAGE_COLOR,1},
      {2,0,D3DDECLTYPE_FLOAT4,0,D3DDECLUSAGE_POSITION,1},
      {2,16,D3DDECLTYPE_FLOAT3,0,D3DDECLUSAGE_NORMAL,1},
      {3,0,D3DDECLTYPE_FLOAT1,0,D3DDECLUSAGE_POSITION,2},D3DDECL_END()};
    const bool unlitLayout = count == 8 && !std::memcmp(elements, unlit, sizeof(unlit));
    const D3DVERTEXELEMENT9 tangent2[] = {expected[0], expected[1], expected[2], expected[3],
      {0,40,D3DDECLTYPE_FLOAT3,0,D3DDECLUSAGE_TANGENT,0},
      {0,52,D3DDECLTYPE_FLOAT3,0,D3DDECLUSAGE_BINORMAL,0},
      observed[4],observed[5],D3DDECL_END()};
    const D3DVERTEXELEMENT9 tangent3[] = {expected[0], expected[1], expected[2], expected[3],
      {0,40,D3DDECLTYPE_FLOAT2,0,D3DDECLUSAGE_TEXCOORD,2},
      {0,48,D3DDECLTYPE_FLOAT3,0,D3DDECLUSAGE_TANGENT,0},
      {0,60,D3DDECLTYPE_FLOAT3,0,D3DDECLUSAGE_BINORMAL,0},
      observed[4],observed[5],D3DDECL_END()};
    const bool tangent2Layout = count == 9 && !std::memcmp(elements,tangent2,sizeof(tangent2));
    const bool tangent3Layout = count == 10 && !std::memcmp(elements,tangent3,sizeof(tangent3));
    const bool accepted = layout == Layout::World ? originalLayout || observedLayout :
      layout == Layout::Unlit ? unlitLayout : layout == Layout::Tangent2UV ? tangent2Layout :
      layout == Layout::Tangent3UV && tangent3Layout;
    if (!accepted) { m_report.code = Code::Layout; return; }
    UINT offset = 0, stride = 0, frequency = 0;
    if (!capture(device->GetStreamSource(0, &m_buffer.value, &offset, &stride)) ||
        !capture(device->GetStreamSourceFreq(0, &frequency))) { return; }
    // FVF reads the verified prefix only. Tangents and UV2 are not converted to
    // PBR inputs here; leave the original stream binding/data and stride intact.
    // No stream binding or vertex data is rewritten. Auxiliary stream 2 is not
    // referenced by this FVF and is restored via the original declaration.
    const bool strideAccepted = layout == Layout::World ?
      ((stride == 40 || stride == 48) && (!observedLayout || stride == 48)) :
      stride == (layout == Layout::Unlit ? 32u : layout == Layout::Tangent2UV ? 64u : 80u);
    if (!m_buffer.value || !strideAccepted || frequency != 1 || offset % 4) {
      m_report.code = Code::Layout; return;
    }
    if (!capture(device->GetVertexShader(&m_shader.value)) ||
        !capture(device->GetTransform(D3DTS_WORLD, &m_world)) ||
        !capture(device->GetTransform(D3DTS_VIEW, &m_view)) ||
        !capture(device->GetTransform(D3DTS_PROJECTION, &m_projection))) { return; }
    const bool fixedPixel = layout != Layout::World;
    if (fixedPixel && !capture(device->GetPixelShader(&m_pixel.value))) { return; }
    m_fixedPixel = fixedPixel;
    D3DMATRIX view{}, projection{};
    std::memcpy(&view, viewValues.data(), sizeof(view));
    std::memcpy(&projection, projectionValues.data(), sizeof(projection));
    // A failing setter may already have mutated state. Roll back all saved
    // fields, continue on restore errors, and never depend on cached globals.
    m_dirty = true;
    if (fixedPixel && !set(device->SetPixelShader(nullptr))) { return; }
    if (!set(device->SetVertexShader(nullptr)) ||
        !set(device->SetFVF(D3DFVF_XYZ | D3DFVF_NORMAL | (layout == Layout::Unlit ? D3DFVF_TEX1 : D3DFVF_TEX2))) ||
        !set(device->SetTransform(D3DTS_WORLD, &world)) ||
        !set(device->SetTransform(D3DTS_VIEW, &view)) ||
        !set(device->SetTransform(D3DTS_PROJECTION, &projection))) { return; }
    m_report.code = Code::Applied; m_report.converted = true;
  }
public:
  // Consumer candidate input has no invented Source frame, view ID or category.
  struct ConsumerInput {
    D3DMATRIX world{};
    std::array<float, 16> view{}, projection{};
    uint32_t width = 0, height = 0;
    Layout layout = Layout::World;
  };
  Scope(Device* device, const ConsumerInput& input, D3DPRIMITIVETYPE type, bool userMemory) noexcept : m_device(device) {
    initialize(input.world, input.view, input.projection, input.width, input.height, type, userMemory, input.layout);
  }
  Scope(Device* device, const DrawSnapshot* snapshot, D3DPRIMITIVETYPE type,
        bool userMemory, bool enabled) noexcept : m_device(device) {
    if (!enabled) { return; }
    if (!snapshot) { m_report.code = Code::Unlabelled; return; }
    if (snapshot->draw.categories != kStaticOpaqueWorld || snapshot->draw.modifiers ||
        snapshot->camera.kind != ViewKind::Main) { m_report.code = Code::NotStaticWorld; return; }
    const auto& camera = snapshot->camera;
    if (!std::isfinite(camera.nearPlane) || !std::isfinite(camera.farPlane) ||
        camera.nearPlane <= 0 || camera.farPlane <= camera.nearPlane) { m_report.code = Code::Camera; return; }
    initialize(worldMatrix(snapshot->draw.transform), camera.view, camera.projection,
               camera.width, camera.height, type, userMemory);
  }
  Scope(const Scope&) = delete;
  Scope& operator=(const Scope&) = delete;
  ~Scope() { finish(); }
  void finish() noexcept {
    if (!m_dirty) { return; }
    m_dirty = false;
    auto restore = [&](HRESULT result) {
      if (FAILED(result) && SUCCEEDED(m_report.restore)) { m_report.restore = result; }
    };
    restore(m_device->SetTransform(D3DTS_WORLD, &m_world));
    restore(m_device->SetTransform(D3DTS_VIEW, &m_view));
    restore(m_device->SetTransform(D3DTS_PROJECTION, &m_projection));
    // FVF is derived from the declaration, not an independent binding. Setting
    // an equivalent FVF can select another COM declaration object on Windows.
    // Restore the original declaration, including when it came from SetFVF.
    restore(m_device->SetVertexDeclaration(m_declaration.value));
    restore(m_device->SetVertexShader(m_shader.value));
    if (m_fixedPixel) { restore(m_device->SetPixelShader(m_pixel.value)); }
    if (FAILED(m_report.restore)) { m_report.code = Code::RestoreFailed; }
  }
  const Report& report() const noexcept { return m_report; }
};

template<class Device, class Draw> HRESULT execute(Device* device, const DrawSnapshot* snapshot,
    D3DPRIMITIVETYPE type, bool userMemory, bool enabled, Draw&& draw, Report& report) {
  Scope<Device> scope(device, snapshot, type, userMemory, enabled);
  // If applying state failed but restoration succeeded, preserve the original
  // raster draw and report the conversion failure. Never hide the object.
  if (FAILED(scope.report().restore)) { report = scope.report(); return report.restore; }
  const HRESULT result = draw(); // The destructor restores even if a test throws.
  scope.finish(); report = scope.report();
  return FAILED(report.restore) ? report.restore : result;
}

} // namespace static_draw
} // namespace ldb_rt

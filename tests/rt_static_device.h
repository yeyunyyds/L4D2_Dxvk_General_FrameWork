#pragma once

#include "rt_static_draw.h"
#include <algorithm>

namespace rt_test {

struct Shader final : IDirect3DVertexShader9 {
  ULONG refs = 1;
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** out) override { *out = nullptr; return E_NOINTERFACE; }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
  ULONG STDMETHODCALLTYPE Release() override { return --refs; }
  HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9**) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetFunction(void*, UINT*) override { return E_NOTIMPL; }
};
struct Declaration final : IDirect3DVertexDeclaration9 {
  ULONG refs = 1;
  DWORD fvf = 0;
  HRESULT result = S_OK;
  std::vector<D3DVERTEXELEMENT9> elements{
    {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
    {0, 12, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_NORMAL, 0},
    {0, 24, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
    {0, 32, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 1}, D3DDECL_END()
  };
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** out) override { *out = nullptr; return E_NOINTERFACE; }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
  ULONG STDMETHODCALLTYPE Release() override { return --refs; }
  HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9**) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetDeclaration(D3DVERTEXELEMENT9* out, UINT* count) override {
    if (FAILED(result)) { return result; }
    if (*count < elements.size()) { return E_INVALIDARG; }
    *count = UINT(elements.size()); std::memcpy(out, elements.data(), elements.size() * sizeof(elements[0])); return S_OK;
  }
};
struct Buffer final : IDirect3DVertexBuffer9 {
  ULONG refs = 1;
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** out) override { *out = nullptr; return E_NOINTERFACE; }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
  ULONG STDMETHODCALLTYPE Release() override { return --refs; }
  HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9**) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, const void*, DWORD, DWORD) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, void*, DWORD*) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID) override { return E_NOTIMPL; }
  DWORD STDMETHODCALLTYPE SetPriority(DWORD) override { return 0; }
  DWORD STDMETHODCALLTYPE GetPriority() override { return 0; }
  void STDMETHODCALLTYPE PreLoad() override { }
  D3DRESOURCETYPE STDMETHODCALLTYPE GetType() override { return D3DRTYPE_VERTEXBUFFER; }
  HRESULT STDMETHODCALLTYPE Lock(UINT, UINT, void**, DWORD) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE Unlock() override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetDesc(D3DVERTEXBUFFER_DESC*) override { return E_NOTIMPL; }
};

inline D3DMATRIX matrix(float translation) {
  D3DMATRIX value{};
  for (unsigned i = 0; i < 4; ++i) { value.m[i][i] = 1; }
  value.m[3][0] = translation; return value;
}

// A D3D9-method fixture, not a rasterizer or GPU. COM child interfaces have
// their real declared ABI. Every method also checks the graphics owner thread.
struct Graphics {
  Shader shader;
  Declaration declaration, ffp;
  Buffer buffer;
  IDirect3DVertexShader9* currentShader = &shader;
  IDirect3DVertexDeclaration9* currentDeclaration = &declaration;
  DWORD currentFvf = 0, frequency = 1, alpha = 0, alphaTest = 0, depthWrite = 1, depth = D3DZB_TRUE;
  UINT stride = 40, offset = 0;
  D3DVIEWPORT9 viewport{0, 0, 1920, 1080, 0, 1};
  std::array<D3DMATRIX, 3> transforms{matrix(100), matrix(200), matrix(300)};
  const DWORD owner = GetCurrentThreadId();
  uint32_t gets = 0, sets = 0, failGet = 0;
  std::vector<uint32_t> failSet;
  bool foreign = false;
  Graphics() { shader.AddRef(); declaration.AddRef(); buffer.AddRef(); }
  ~Graphics() {
    if (currentShader) { currentShader->Release(); }
    if (currentDeclaration) { currentDeclaration->Release(); }
    buffer.Release();
  }
  HRESULT operation(bool write) {
    if (GetCurrentThreadId() != owner) { foreign = true; return E_ACCESSDENIED; }
    if (write) {
      ++sets;
      if (std::find(failSet.begin(), failSet.end(), sets) != failSet.end()) { return E_FAIL; }
    } else if (++gets == failGet) { return E_FAIL; }
    return S_OK;
  }
  HRESULT GetViewport(D3DVIEWPORT9* out) { *out = viewport; return operation(false); }
  HRESULT GetRenderTarget(DWORD, IDirect3DSurface9** out) { *out = nullptr; return E_NOTIMPL; }
  HRESULT GetRenderState(D3DRENDERSTATETYPE type, DWORD* out) {
    switch (type) {
    case D3DRS_ALPHABLENDENABLE: *out = alpha; break;
    case D3DRS_ALPHATESTENABLE: *out = alphaTest; break;
    case D3DRS_ZWRITEENABLE: *out = depthWrite; break;
    case D3DRS_ZENABLE: *out = depth; break;
    default: return E_INVALIDARG;
    }
    return operation(false);
  }
  HRESULT GetVertexDeclaration(IDirect3DVertexDeclaration9** out) {
    *out = currentDeclaration; if (*out) { (*out)->AddRef(); } return operation(false);
  }
  HRESULT GetStreamSource(UINT, IDirect3DVertexBuffer9** out, UINT* start, UINT* step) {
    *out = &buffer; buffer.AddRef(); *start = offset; *step = stride; return operation(false);
  }
  HRESULT GetStreamSourceFreq(UINT, UINT* out) { *out = frequency; return operation(false); }
  HRESULT GetVertexShader(IDirect3DVertexShader9** out) {
    *out = currentShader; if (*out) { (*out)->AddRef(); } return operation(false);
  }
  HRESULT GetFVF(DWORD* out) { *out = currentFvf; return operation(false); }
  static unsigned index(D3DTRANSFORMSTATETYPE type) {
    return type == D3DTS_WORLD ? 0 : type == D3DTS_VIEW ? 1 : 2;
  }
  HRESULT GetTransform(D3DTRANSFORMSTATETYPE type, D3DMATRIX* out) {
    *out = transforms[index(type)]; return operation(false);
  }
  HRESULT SetVertexShader(IDirect3DVertexShader9* value) {
    if (value) { value->AddRef(); }
    if (currentShader) { currentShader->Release(); }
    currentShader = value; return operation(true);
  }
  HRESULT SetVertexDeclaration(IDirect3DVertexDeclaration9* value) {
    if (value) { value->AddRef(); }
    if (currentDeclaration) { currentDeclaration->Release(); }
    currentDeclaration = value;
    currentFvf = value == &declaration ? declaration.fvf : value == &ffp ? ffp.fvf : 0;
    return operation(true);
  }
  HRESULT SetFVF(DWORD value) {
    ffp.fvf = value; SetVertexDeclarationWithoutOperation(&ffp); currentFvf = value; return operation(true);
  }
  void SetVertexDeclarationWithoutOperation(IDirect3DVertexDeclaration9* value) {
    value->AddRef(); currentDeclaration->Release(); currentDeclaration = value;
  }
  HRESULT SetTransform(D3DTRANSFORMSTATETYPE type, const D3DMATRIX* value) {
    transforms[index(type)] = *value; return operation(true);
  }
  bool restored() const {
    return currentShader == &shader && currentDeclaration == &declaration && currentFvf == declaration.fvf &&
      transforms[0].m[3][0] == 100 && transforms[1].m[3][0] == 200 && transforms[2].m[3][0] == 300;
  }
  bool leasesReleased() const { return shader.refs == 2 && declaration.refs == 2 && ffp.refs == 1 && buffer.refs == 2; }
};

inline ldb_rt::DrawSnapshot snapshot() {
  ldb_rt::DrawSnapshot value;
  value.camera.width = 1920; value.camera.height = 1080;
  value.camera.nearPlane = 1; value.camera.farPlane = 10000;
  for (unsigned i = 0; i < 4; ++i) { value.camera.view[i * 5] = value.camera.projection[i * 5] = 1; }
  value.draw.categories = ldb_rt::kStaticOpaqueWorld;
  value.draw.transform[0] = value.draw.transform[5] = value.draw.transform[10] = 1;
  return value;
}

} // namespace rt_test

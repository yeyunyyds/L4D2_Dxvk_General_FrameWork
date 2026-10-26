#pragma once
#include "rt_draw_request.h"
#include <d3d9.h>

namespace ldb_rt::draw_observe { struct Snapshot; }
namespace ldb_rt::static_draw { struct Report; }
namespace ldb_rt::world_runtime {
struct State {
  bool active = false;
  void (*bind)(IDirect3DDevice9*) noexcept = nullptr;
  void (*capture)(IDirect3DDevice9*, uint64_t) noexcept = nullptr;
  bool (*raytracing)(bool) noexcept = nullptr;
  void (*draw)(IDirect3DDevice9*) noexcept = nullptr;
  void (*modelScope)(IDirect3DDevice9*,const draw_observe::Snapshot&) noexcept = nullptr;
  void (*audit)(IDirect3DDevice9*, const draw_observe::Snapshot&) noexcept = nullptr;
  void (*inspect)(IDirect3DDevice9*, const draw_observe::Snapshot&, uint32_t, D3DPRIMITIVETYPE, const std::array<uint32_t,6>&) noexcept = nullptr;
  bool (*model)(IDirect3DDevice9*, const draw_observe::Snapshot&, uint32_t, D3DPRIMITIVETYPE, const std::array<uint32_t,6>&, static_draw::Report&, bool&, HRESULT&, HRESULT&) noexcept = nullptr;
  bool (*scene)(IDirect3DDevice9*, const draw_observe::Snapshot&, uint32_t, D3DPRIMITIVETYPE, const std::array<uint32_t,6>&, static_draw::Report&, bool&, HRESULT&, HRESULT&) noexcept = nullptr;
  bool (*overlay)(IDirect3DDevice9*, const draw_observe::Snapshot&, D3DPRIMITIVETYPE, bool, static_draw::Report&, HRESULT&) noexcept = nullptr;
  uint64_t presents = 0;
  void (*presented)(IDirect3DDevice9*,uint64_t) noexcept = nullptr;
  void (*beforePresent)(IDirect3DDevice9*) noexcept = nullptr;
  bool preview=false;
  void (*startPreview)(IDirect3DDevice9*) noexcept = nullptr;
  void (*previewStep)(IDirect3DDevice9*,bool) noexcept = nullptr;
  void (*release)(IDirect3DDevice9*) noexcept = nullptr;
};
inline State& state() { static State value; return value; }
HMODULE load(const draw_observe::Request& request) noexcept;
bool configureRaster() noexcept;
void invalidateTexture(IDirect3DBaseTexture9* texture) noexcept;
void invalidateSurface(IDirect3DSurface9* surface) noexcept;
void beforeDeviceDestroy(IDirect3DDevice9* device) noexcept;
void shutdown() noexcept;
} // namespace ldb_rt::world_runtime

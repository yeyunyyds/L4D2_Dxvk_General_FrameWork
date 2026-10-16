#pragma once
#include "rt_draw_request.h"
#include <d3d9.h>

namespace ldb_rt::draw_observe { struct Snapshot; }
namespace ldb_rt::world_runtime {
struct State {
  bool active = false;
  void (*bind)(IDirect3DDevice9*) noexcept = nullptr;
  void (*capture)(IDirect3DDevice9*, uint64_t) noexcept = nullptr;
  bool (*raytracing)(bool) noexcept = nullptr;
  void (*draw)(IDirect3DDevice9*) noexcept = nullptr;
  void (*audit)(IDirect3DDevice9*, const draw_observe::Snapshot&) noexcept = nullptr;
};
inline State& state() { static State value; return value; }
HMODULE load(const draw_observe::Request& request) noexcept;
bool configureRaster() noexcept;
void shutdown() noexcept;
} // namespace ldb_rt::world_runtime

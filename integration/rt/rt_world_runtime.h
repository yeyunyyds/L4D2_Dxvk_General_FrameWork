#pragma once
#include "rt_draw_request.h"
#include <d3d9.h>

namespace ldb_rt::world_runtime {
struct State {
  bool active = false;
  void (*bind)(IDirect3DDevice9*) noexcept = nullptr;
  void (*capture)(IDirect3DDevice9*, uint64_t) noexcept = nullptr;
};
inline State& state() { static State value; return value; }
HMODULE load(const draw_observe::Request& request) noexcept;
void shutdown() noexcept;
} // namespace ldb_rt::world_runtime

#pragma once
#include "rt_draw_packet.h"
#include "rt_draw_request.h"
#include "rt_consumer_world.h"
#include "rt_world_runtime.h"
#include "rt_world_diagnostics.h"
#include <d3d9.h>
#include <map>
#include <type_traits>

namespace ldb_rt::draw_observe {
template<class Device> Bytes pipeline(Device* device) {
  Writer w;
  uint32_t failures = 0;
  D3DVIEWPORT9 viewport{};
  if (FAILED(device->GetViewport(&viewport))) { failures |= 1; }
  DWORD state[4]{}; const D3DRENDERSTATETYPE names[] = {D3DRS_ALPHABLENDENABLE, D3DRS_ALPHATESTENABLE, D3DRS_ZENABLE, D3DRS_ZWRITEENABLE};
  for (unsigned i = 0; i < 4; ++i) { if (FAILED(device->GetRenderState(names[i], &state[i]))) { failures |= 2u << i; } }
  IDirect3DVertexBuffer9* buffer = nullptr;
  IDirect3DVertexDeclaration9* declaration = nullptr;
  UINT offset = 0, stride = 0, frequency = 0;
  if (FAILED(device->GetStreamSource(0, &buffer, &offset, &stride))) { failures |= 32; }
  if (buffer) { buffer->Release(); }
  if (FAILED(device->GetStreamSourceFreq(0, &frequency))) { failures |= 64; }
  D3DVERTEXELEMENT9 elements[MAXD3DDECLLENGTH + 1]{}; UINT count = MAXD3DDECLLENGTH + 1;
  if (FAILED(device->GetVertexDeclaration(&declaration)) || !declaration) { failures |= 128; count = 0; }
  if (declaration) {
    if (FAILED(declaration->GetDeclaration(elements, &count)) || count > MAXD3DDECLLENGTH + 1) { failures |= 128; count = 0; }
    declaration->Release();
  }
  IDirect3DSurface9* target = nullptr; D3DSURFACE_DESC desc{};
  if (FAILED(device->GetRenderTarget(0, &target)) || !target) { failures |= 256; }
  if (target) { if (FAILED(target->GetDesc(&desc))) { failures |= 256; } target->Release(); }
  w.u32(failures); w.u32(offset); w.u32(stride); w.u32(frequency);
  for (auto value : state) { w.u32(value); }
  w.u32(viewport.X); w.u32(viewport.Y); w.u32(viewport.Width); w.u32(viewport.Height);
  w.f32(viewport.MinZ); w.f32(viewport.MaxZ);
  w.u32(desc.Width); w.u32(desc.Height); w.u32(uint32_t(desc.Format)); w.u32(count);
  for (unsigned i = 0; i < count; ++i) {
    const auto* bytes = reinterpret_cast<const uint8_t*>(&elements[i]);
    w.data.insert(w.data.end(), bytes, bytes + sizeof(elements[i]));
  }
  return w.data;
}

class Recorder {
  Request request;
  std::ofstream trace;
  fs::path file;
  uint64_t written = 0, draws = 0, valid = 0, presents = 0, resets = 0, failures = 0, unobserved = 0;
  uint32_t pendingUid = 0;
  Snapshot nextSnapshot;
  uint32_t nextUid = 0;
  bool hasNext = false;
  IDirect3DDevice9* nativeDevice = nullptr; // Bridge device lifetime, never AddRef'd here.
  bool finished = false, pending = false;
  void record(uint32_t event, uint32_t uid, const Bytes& payload = {}) {
    if (!trace || finished) { return; }
    LARGE_INTEGER now{}; QueryPerformanceCounter(&now);
    Writer header; header.u32(event); header.u32(uint32_t(32 + payload.size())); header.u64(uid);
    header.u32(GetCurrentThreadId()); header.u32(0); header.u64(uint64_t(now.QuadPart));
    trace.write(reinterpret_cast<const char*>(header.data.data()), std::streamsize(header.data.size()));
    if (!payload.empty()) { trace.write(reinterpret_cast<const char*>(payload.data()), std::streamsize(payload.size())); }
    written += header.data.size() + payload.size();
    if (!trace) { throw std::runtime_error("write Draw observation"); }
  }
public:
  uint64_t session() const noexcept { return request.session; }
  Recorder(uint64_t device, uint64_t session) {
    if (!readRequest(request) || request.session != session) { throw std::runtime_error("unmatched Draw observation request"); }
    file = request.output / ("rt-draw-host-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(device) + ".bin");
    if (fs::exists(file)) { throw std::runtime_error("preserve existing Draw trace"); }
    trace.open(file, std::ios::binary);
    if (!trace) { throw std::runtime_error("open Draw observation"); }
    LARGE_INTEGER frequency{}; QueryPerformanceFrequency(&frequency);
    Writer header; header.u32(request.world ? 2 : 1); header.u64(session); header.u64(device); header.u64(uint64_t(frequency.QuadPart));
    if (request.world) { header.u32(240); header.u32(30); } record(0, 0, header.data);
  }
  ~Recorder() { finish(3); if (request.world) { world_runtime::state().active=false; } }
  template<class Device> void draw(Device* device, uint32_t uid, const Snapshot& snapshot, uint32_t kind,
      D3DPRIMITIVETYPE type, const std::array<uint32_t, 6>& args) {
    if (snapshot.session != request.session) { ++failures; return; }
    nextSnapshot = snapshot; nextUid = uid; hasNext = true;
    if constexpr (std::is_base_of<IDirect3DDevice9, Device>::value) {
      nativeDevice = device;
      if (request.world && world_runtime::state().active && world_runtime::state().bind) { world_runtime::state().bind(device); }
    }
    if (finished) { return; }
    if (written > 128 * 1024 * 1024) { finish(2); return; }
    Writer payload; payload.data = pack(snapshot); payload.u32(kind); payload.u32(uint32_t(type));
    for (auto arg : args) { payload.u32(arg); }
    auto state = pipeline(device); payload.data.insert(payload.data.end(), state.begin(), state.end());
    if (pending) { ++failures; }
    pending = true; pendingUid = uid;
    record(1, uid, payload.data); ++draws; if (snapshot.status == Status::Valid) { ++valid; }
  }
  bool take(uint32_t uid, Snapshot& snapshot, bool& control) {
    if (!request.world || !hasNext || uid != nextUid) { return false; }
    snapshot = nextSnapshot; hasNext = false; control = presents < 30; return true;
  }
  void conversion(uint32_t uid, consumer_world::Decision decision, const static_draw::Report& report, bool called, HRESULT physical) {
    // The conversion scope has restored the original public D3D9 state here.
    if (report.converted && SUCCEEDED(report.restore) && nativeDevice && world_runtime::state().active && world_runtime::state().audit) {
      world_runtime::state().audit(nativeDevice, nextSnapshot);
    }
    Writer w; w.u32(uint32_t(decision)); w.u32(uint32_t(report.code));
    w.u32(uint32_t(report.capture)); w.u32(uint32_t(report.apply)); w.u32(uint32_t(report.restore));
    w.u32(report.converted ? 1 : 0); w.u32(called ? 1 : 0); w.u32(uint32_t(physical));
    record(6, uid, w.data);
  }
  void result(uint32_t uid, HRESULT result) {
    if (finished) { return; } Writer w; w.u32(uint32_t(result));
    if (pending && pendingUid == uid) { record(2, uid, w.data); pending = false; }
    else { record(5, uid, w.data); ++unobserved; }
    if (FAILED(result)) { ++failures; }
  }
  void present(uint32_t uid, HRESULT result) {
    if (finished) { return; } Writer w; w.u32(uint32_t(result)); record(3, uid, w.data);
    if (SUCCEEDED(result)) { ++presents; } else { ++failures; }
    if (request.world && world_runtime::state().active && world_runtime::state().capture && nativeDevice &&
        world_diagnostics::readback(presents)) { world_runtime::state().capture(nativeDevice, presents); }
    trace.flush(); if (presents >= (request.world ? 240u : 120u)) { finish(0); }
  }
  void reset(uint32_t uid, HRESULT result) {
    if (finished) { return; } Writer w; w.u32(uint32_t(result)); record(4, uid, w.data); ++resets;
    // This batch is an observation segment. Do not silently join generations.
    finish(4);
    if (request.world) { world_runtime::state().active = false; }
  }
  void finish(uint32_t reason) noexcept {
    if (finished) { return; }
    if (request.world) {
      if (world_runtime::state().raytracing) { (void)world_runtime::state().raytracing(false); }
      world_runtime::state().active = false;
    }
    try {
      if (pending) { ++failures; }
      Writer footer; footer.u64(draws); footer.u64(valid); footer.u64(presents); footer.u64(resets); footer.u64(failures); footer.u64(unobserved); footer.u32(reason);
      record(8, 0, footer.data); trace.close(); finished = true;
      if (request.session && !fs::exists(doneFile(request))) {
        const auto temporary = fs::path(doneFile(request).wstring() + L".tmp");
        std::ofstream output(temporary, std::ios::binary);
        output << "{\"session\":" << request.session << ",\"reason\":" << reason << ",\"draws\":" << draws
          << ",\"valid\":" << valid << ",\"presents\":" << presents << ",\"game_rt\":\""
          << (request.world ? "attempted_requires_review" : "unrun") << "\",\"source_queue\":\"unrun\"}\n";
        output.close(); fs::rename(temporary, doneFile(request));
      }
    } catch (...) { finished = true; }
  }
};
inline std::map<uint64_t, std::unique_ptr<Recorder>>& recorders() {
  static std::map<uint64_t, std::unique_ptr<Recorder>> values; return values;
}
template<class Device> void observe(uint64_t device, uint32_t uid, Device* graphics, const void* bytes, size_t size,
    uint32_t kind, D3DPRIMITIVETYPE type, const std::array<uint32_t, 6>& args) noexcept {
  try {
    Snapshot snapshot;
    if (!unpack(bytes, size, snapshot)) { return; }
    auto& recorder = recorders()[device];
    if (recorder && recorder->session() != snapshot.session) { recorder.reset(); }
    if (!recorder) { recorder = std::make_unique<Recorder>(device, snapshot.session); }
    recorder->draw(graphics, uid, snapshot, kind, type, args);
  } catch (...) { } // Preserve raster rendering on all diagnostic failures.
}
inline void drawResult(uint64_t device, uint32_t uid, HRESULT result) noexcept {
  try { auto it = recorders().find(device); if (it != recorders().end() && it->second) { it->second->result(uid, result); } } catch (...) { }
}
inline void present(uint64_t device, uint32_t uid, HRESULT result) noexcept {
  try { auto it = recorders().find(device); if (it != recorders().end() && it->second) { it->second->present(uid, result); } } catch (...) { }
}
inline void reset(uint64_t device, uint32_t uid, HRESULT result) noexcept {
  try { auto it = recorders().find(device); if (it != recorders().end() && it->second) { it->second->reset(uid, result); } } catch (...) { }
}
inline void destroy(uint64_t device) noexcept { recorders().erase(device); }
} // namespace ldb_rt::draw_observe

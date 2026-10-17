#pragma once

#include "rt_protocol.h"
#include "rt_static_draw.h"
#include "rt_draw_host.h"

#include <windows.h>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>

namespace ldb_rt_host {

struct Observer {
  ldb_rt::Stream stream;
  std::ofstream trace;
  size_t written = 0;
  uint64_t draws = 0, unlabelled = 0, presents = 0, rejected = 0;
  bool failed = false, needsReset = false, resetAvailable = false, truncated = false;
  bool staticDiagnostic = false;
  explicit Observer(uint64_t device) {
    wchar_t enabled[2]{};
    staticDiagnostic = GetEnvironmentVariableW(L"LDB_RT_STATIC_DIAGNOSTIC", enabled, 2) == 1 && enabled[0] == L'1';
    wchar_t directory[32768];
    const auto n = GetEnvironmentVariableW(L"LDB_RT_LOG_DIR", directory, 32768);
    if (n && n < 32768) {
      const std::filesystem::path dir(directory);
      std::filesystem::create_directories(dir);
      trace.open(dir / ("rt-host-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(device) + ".bin"), std::ios::binary);
      LARGE_INTEGER frequency; QueryPerformanceFrequency(&frequency);
      ldb_rt::Writer w; w.u64(uint64_t(frequency.QuadPart));
      record(0, 0, w.data.data(), uint32_t(w.data.size()));
      if (staticDiagnostic) {
        ldb_rt::Writer config; config.u32(1); config.u32(1);
        record(12, 0, config.data.data(), uint32_t(config.data.size()));
      }
    }
  }
  void record(uint32_t event, uint64_t uid, const void* bytes = nullptr, uint32_t size = 0) {
    if (!trace || truncated) { return; }
    if (size > ldb_rt::kMaxPacketBytes || written + 32 + size > 128 * 1024 * 1024) {
      truncated = true; event = 9; bytes = nullptr; size = 0;
    }
    LARGE_INTEGER tick; QueryPerformanceCounter(&tick);
    ldb_rt::Writer w;
    w.u32(event); w.u32(32 + size); w.u64(uid);
    w.u32(GetCurrentThreadId()); w.u32(0); w.u64(uint64_t(tick.QuadPart));
    trace.write(reinterpret_cast<const char*>(w.data.data()), std::streamsize(w.data.size()));
    if (size) { trace.write(static_cast<const char*>(bytes), size); }
    written += 32 + size;
  }
  ~Observer() {
    try {
    ldb_rt::Writer counters;
    counters.u64(draws); counters.u64(unlabelled); counters.u64(presents); counters.u64(rejected);
    counters.u32(failed ? 1 : 0); counters.u32(truncated ? 1 : 0);
    // A bounded footer remains available after the detailed trace budget is exhausted.
    truncated = false;
    const auto old = written; written = 0;
    record(8, 0, counters.data.data(), uint32_t(counters.data.size()));
    written = old;
    } catch (...) { }
  }
};
inline std::map<uint64_t, std::unique_ptr<Observer>>& observers() {
  static std::map<uint64_t, std::unique_ptr<Observer>> values; return values;
}
inline HRESULT submit(uint64_t device, const void* bytes, uint32_t size, uint32_t uid, bool hello) noexcept {
  try {
    ldb_rt::Packet packet;
    if (ldb_rt::decode(bytes, size, packet) != ldb_rt::Error::Ok || packet.header.device != device ||
        hello != (packet.header.kind == ldb_rt::Kind::Hello)) { return E_INVALIDARG; }
    auto& slots = observers();
    auto it = slots.find(device);
    if (it == slots.end()) {
      if (!hello) { return E_UNEXPECTED; }
      auto observer = std::make_unique<Observer>(device);
      it = slots.emplace(device, std::move(observer)).first;
    }
    auto& s = *it->second;
    if (s.failed) { return E_UNEXPECTED; }
    if (s.needsReset && (!s.resetAvailable || packet.header.kind != ldb_rt::Kind::Reset)) {
      s.record(10, uid, bytes, size); return E_UNEXPECTED;
    }
    if (packet.header.kind == ldb_rt::Kind::Reset && !s.needsReset) { return E_UNEXPECTED; }
    const auto error = s.stream.apply(bytes, size, GetCurrentThreadId(), s.needsReset && s.resetAvailable);
    if (error != ldb_rt::Error::Ok) {
      ++s.rejected; s.failed = true;
      const uint32_t reason = static_cast<uint32_t>(error);
      ldb_rt::Writer w; w.u32(reason); s.record(7, uid, w.data.data(), uint32_t(w.data.size()));
      return E_INVALIDARG;
    }
    s.needsReset = false; s.record(1, uid, bytes, size);
    return S_OK;
  } catch (const std::bad_alloc&) {
    const auto it = observers().find(device);
    if (it != observers().end()) { it->second->failed = true; }
    return E_OUTOFMEMORY;
  } catch (...) {
    const auto it = observers().find(device);
    if (it != observers().end()) { it->second->failed = true; }
    return E_FAIL;
  }
}
inline std::unique_ptr<ldb_rt::DrawSnapshot> takeDraw(uint64_t device, uint32_t uid) noexcept {
  try {
    const auto it = observers().find(device);
    if (it == observers().end()) { return nullptr; }
    auto& s = *it->second;
    if (s.failed || s.needsReset) { ++s.unlabelled; return nullptr; }
    std::unique_ptr<ldb_rt::DrawSnapshot> snapshot;
    const auto error = s.stream.draw(uid, GetCurrentThreadId(), snapshot);
    if (error == ldb_rt::Error::MissingDraw) { ++s.unlabelled; return nullptr; }
    if (error != ldb_rt::Error::Ok) { s.failed = true; return nullptr; }
    ++s.draws; s.record(2, uid);
    return snapshot;
  } catch (...) { return nullptr; }
}
inline void draw(uint64_t device, uint32_t uid) noexcept { (void)takeDraw(device, uid); }

// Scope all D3D9 changes on the Host's real Draw owner, never a Source producer.
// This is opt-in state diagnostic/conversion, not evidence that RT executed.
template<class Device, class Draw> HRESULT drawCommand(uint64_t device, uint32_t uid, Device* graphics,
    D3DPRIMITIVETYPE type, bool userMemory, Draw&& physicalDraw) noexcept {
  try {
    if constexpr (std::is_base_of<IDirect3DDevice9, Device>::value) {
      auto& runtime = ldb_rt::world_runtime::state();
      if (runtime.active && runtime.draw) { runtime.draw(graphics); }
    }
    const auto consumer = ldb_rt::draw_observe::recorders().find(device);
    ldb_rt::draw_observe::Snapshot current;
    bool control = false;
    if (consumer != ldb_rt::draw_observe::recorders().end() && consumer->second->take(uid, current, control)) {
      ldb_rt::consumer_world::Decision decision;
      ldb_rt::static_draw::Report report;
      bool called = false; HRESULT physical = E_ABORT;
      if constexpr (std::is_base_of<IDirect3DDevice9, Device>::value) {
        const auto& runtime = ldb_rt::world_runtime::state();
        if (runtime.active && runtime.overlay && consumer->second->expanded() &&
            runtime.overlay(graphics, current, type, userMemory, report, physical)) {
          consumer->second->conversion(uid, ldb_rt::consumer_world::Decision::HudOverlay, report, false, physical);
          return physical;
        }
      }
      const auto result = ldb_rt::consumer_world::execute(graphics, current, type, userMemory,
        ldb_rt::world_runtime::state().active, control, [&] { called = true; physical = physicalDraw(); return physical; }, decision, report,
        ldb_rt::world_runtime::state().raytracing, consumer->second->expanded());
      consumer->second->conversion(uid, decision, report, called, physical);
      return result;
    }
    auto snapshot = takeDraw(device, uid);
    const auto it = observers().find(device);
    const bool enabled = it != observers().end() && it->second->staticDiagnostic && !it->second->failed;
    ldb_rt::static_draw::Report report;
    HRESULT physical = E_ABORT;
    bool called = false;
    const HRESULT result = ldb_rt::static_draw::execute(graphics, snapshot.get(), type, userMemory, enabled, [&] {
      called = true; physical = physicalDraw(); return physical;
    }, report);
    if (enabled && snapshot && snapshot->draw.categories == ldb_rt::kStaticOpaqueWorld) {
      try {
        ldb_rt::Writer payload;
        payload.u32(1); payload.u32(static_cast<uint32_t>(report.code));
        payload.u32(uint32_t(report.capture)); payload.u32(uint32_t(report.apply)); payload.u32(uint32_t(report.restore));
        payload.u32(uint32_t(physical)); payload.u32(report.converted ? 1 : 0); payload.u32(called ? 1 : 0);
        payload.u64(snapshot->header.frame); payload.u64(snapshot->header.view); payload.u64(snapshot->header.sequence);
        it->second->record(11, uid, payload.data.data(), uint32_t(payload.data.size()));
      } catch (...) { it->second->failed = true; }
    }
    return result;
  } catch (...) { return E_FAIL; }
}
inline void present(uint64_t device, uint32_t uid, HRESULT result) noexcept {
  ldb_rt::draw_observe::present(device, uid, result);
  try {
    const auto it = observers().find(device);
    if (it == observers().end()) { return; }
    auto& s = *it->second;
    uint64_t frame = 0;
    if (s.failed || s.needsReset) { return; }
    if (!s.stream.inFrame() && !s.stream.awaitingPresent()) {
      ldb_rt::Writer w; w.u32(uint32_t(result));
      s.record(6, uid, w.data.data(), uint32_t(w.data.size()));
      if (FAILED(result)) { s.needsReset = true; s.resetAvailable = false; }
      return;
    }
    const auto error = s.stream.present(GetCurrentThreadId(), frame);
    ldb_rt::Writer w; w.u64(frame); w.u32(uint32_t(result)); w.u32(uint32_t(error));
    s.record(3, uid, w.data.data(), uint32_t(w.data.size()));
    if (error == ldb_rt::Error::Ok && SUCCEEDED(result)) { ++s.presents; }
    if (error != ldb_rt::Error::Ok) { s.failed = true; }
    if (FAILED(result)) { s.needsReset = true; s.resetAvailable = false; }
    if (s.trace) { s.trace.flush(); }
  } catch (...) { }
}
inline void reset(uint64_t device, uint32_t uid, HRESULT result) noexcept {
  ldb_rt::draw_observe::reset(device, uid, result);
  try {
    const auto it = observers().find(device);
    if (it == observers().end()) { return; }
    auto& s = *it->second;
    s.needsReset = true; s.resetAvailable = SUCCEEDED(result);
    ldb_rt::Writer w; w.u32(uint32_t(result));
    s.record(4, uid, w.data.data(), uint32_t(w.data.size()));
  } catch (...) { }
}
inline void drawResult(uint64_t device, uint32_t uid, HRESULT result) noexcept {
  ldb_rt::draw_observe::drawResult(device, uid, result);
  try {
    const auto it = observers().find(device);
    if (it == observers().end()) { return; }
    auto& s = *it->second;
    ldb_rt::Writer w; w.u32(uint32_t(result));
    s.record(5, uid, w.data.data(), uint32_t(w.data.size()));
    if (FAILED(result)) { s.needsReset = true; s.resetAvailable = false; }
  } catch (...) { }
}
inline void destroy(uint64_t device) noexcept { ldb_rt::draw_observe::destroy(device); observers().erase(device); }

} // namespace ldb_rt_host

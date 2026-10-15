#pragma once

#include "rt_protocol.h"

#include <windows.h>
#include <filesystem>
#include <fstream>
#include <map>

namespace ldb_rt_host {

struct Observer {
  ldb_rt::Stream stream;
  std::ofstream trace;
  size_t written = 0;
  uint64_t draws = 0, unlabelled = 0, presents = 0, rejected = 0;
  bool failed = false, needsReset = false, resetAvailable = false, truncated = false;
  explicit Observer(uint64_t device) {
    wchar_t directory[32768];
    const auto n = GetEnvironmentVariableW(L"LDB_RT_LOG_DIR", directory, 32768);
    if (n && n < 32768) {
      const std::filesystem::path dir(directory);
      std::filesystem::create_directories(dir);
      trace.open(dir / ("rt-host-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(device) + ".bin"), std::ios::binary);
      LARGE_INTEGER frequency; QueryPerformanceFrequency(&frequency);
      ldb_rt::Writer w; w.u64(uint64_t(frequency.QuadPart));
      record(0, 0, w.data.data(), uint32_t(w.data.size()));
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
    if (s.failed || (s.needsReset && (!s.resetAvailable || packet.header.kind != ldb_rt::Kind::Reset))) { return E_UNEXPECTED; }
    const auto error = s.stream.apply(bytes, size, GetCurrentThreadId());
    if (error != ldb_rt::Error::Ok) {
      ++s.rejected; s.failed = true;
      const uint32_t reason = static_cast<uint32_t>(error);
      ldb_rt::Writer w; w.u32(reason); s.record(7, uid, w.data.data(), uint32_t(w.data.size()));
      return E_INVALIDARG;
    }
    s.needsReset = false; s.record(1, uid, bytes, size);
    return S_OK;
  } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
    catch (...) { return E_FAIL; }
}
inline void draw(uint64_t device, uint32_t uid) noexcept {
  try {
    const auto it = observers().find(device);
    if (it == observers().end()) { return; }
    auto& s = *it->second;
    if (s.failed || s.needsReset) { ++s.unlabelled; return; }
    std::unique_ptr<ldb_rt::DrawSnapshot> snapshot;
    const auto error = s.stream.draw(uid, GetCurrentThreadId(), snapshot);
    if (error == ldb_rt::Error::MissingDraw) { ++s.unlabelled; return; }
    if (error != ldb_rt::Error::Ok) { s.failed = true; return; }
    ++s.draws; s.record(2, uid);
  } catch (...) { }
}
inline void present(uint64_t device, uint32_t uid, HRESULT result) noexcept {
  try {
    const auto it = observers().find(device);
    if (it == observers().end()) { return; }
    auto& s = *it->second;
    uint64_t frame = 0;
    if (s.failed || s.needsReset) { return; }
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
  try {
    const auto it = observers().find(device);
    if (it == observers().end()) { return; }
    auto& s = *it->second;
    ldb_rt::Writer w; w.u32(uint32_t(result));
    s.record(5, uid, w.data.data(), uint32_t(w.data.size()));
    if (FAILED(result)) { s.needsReset = true; s.resetAvailable = false; }
  } catch (...) { }
}
inline void destroy(uint64_t device) noexcept { observers().erase(device); }

} // namespace ldb_rt_host

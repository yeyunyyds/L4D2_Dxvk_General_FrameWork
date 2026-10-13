#pragma once
// Diagnostic observer for the serialized LDB Host device-command dispatcher.
// No Client wire changes, persistent COM references, or frame-generation execution.
#include <windows.h>
#include <d3d9.h>
#include <cstdint>
#if defined(_WIN64)
#include "dxvk_capture_shared.h"
#include <map>
#include <memory>
#include <mutex>
#include <limits>

namespace ldb_capture {
using namespace dxvk_capture;
inline std::wstring env(const wchar_t* name) {
  DWORD n = GetEnvironmentVariableW(name, nullptr, 0);
  if (!n) return {};
  std::wstring value(n, L'\0');
  DWORD count = GetEnvironmentVariableW(name, value.data(), n);
  if (!count || count >= n) return {};
  value.resize(count); return value;
}
struct Config {
  std::filesystem::path root;
  std::filesystem::path trigger;
  uint32_t start = 180, frames = 2, snapshots = 8;
  bool enabled = false;
  Config() noexcept {
    try {
      root = env(L"LDB_CAPTURE_DIR");
      if (root.empty() || !root.is_absolute()) return;
      trigger = env(L"LDB_CAPTURE_TRIGGER");
      if (!trigger.empty() && !trigger.is_absolute()) return;
      auto number = [](const wchar_t* key, uint32_t fallback, uint32_t maximum) {
        auto text = env(key); if (text.empty()) return fallback;
        size_t used = 0; auto n = std::stoul(text, &used);
        if (used != text.size() || n > maximum) throw std::runtime_error("Invalid capture setting");
        return uint32_t(n);
      };
      start = number(L"LDB_CAPTURE_START", start, 1000000);
      frames = number(L"LDB_CAPTURE_FRAMES", frames, 8);
      snapshots = number(L"LDB_CAPTURE_SNAPSHOTS", snapshots, 16);
      enabled = frames > 0 && snapshots > 0;
    } catch (...) { enabled = false; }
  }
};
inline const Config& config() { static const Config value; return value; }
inline bool enabled() noexcept { return config().enabled; }
inline uint64_t id(const void* value) { return uint64_t(reinterpret_cast<uintptr_t>(value)); }
inline std::string viewportJson(const D3DVIEWPORT9& v) {
  std::ostringstream o;
  o << "{\"x\":" << v.X << ",\"y\":" << v.Y << ",\"width\":" << v.Width << ",\"height\":" << v.Height
    << ",\"min_z\":" << v.MinZ << ",\"max_z\":" << v.MaxZ << '}'; return o.str();
}
struct State {
  uint64_t deviceId = 0, frame = 0, sequence = 0, draws = 0, segmentDraws = 0;
  uint32_t generation = 0, snapshots = 0, clearAttempts = 0, otherAttempts = 0, rangeAttempts = 0, presentAttempts = 0, events = 0;
  uint64_t lastSnapshotDraws = UINT64_MAX, lastColor = 0, lastDepth = 0;
  std::string lastPrefix;
  uint64_t totalBytes = 0;
  bool stopped = false;
  bool armed = false;
  std::filesystem::path directory;
  std::ofstream log;
  explicit State(IDirect3DDevice9* device) : deviceId(id(device)) {
    armed = config().trigger.empty();
    auto name = std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(deviceId);
    directory = config().root / name;
    if (!std::filesystem::create_directories(directory)) throw std::runtime_error("Capture directory already exists");
    log.open(directory / "events.jsonl", std::ios::binary);
    if (!log) throw std::runtime_error("Cannot open capture log");
    emit("session", 0, "\"start_frame\":" + std::to_string(config().start) + ",\"frames_requested\":" + std::to_string(config().frames) +
      ",\"max_snapshots_per_frame\":" + std::to_string(config().snapshots) +
      ",\"diagnostic_gpu_waits\":true,\"semantic_labels\":\"unknown\",\"gpu_fg_execution\":\"unrun\"");
  }
  bool active() const { return armed && !stopped && frame >= config().start && frame < uint64_t(config().start) + config().frames; }
  void emit(const char* event, uint32_t uid, const std::string& fields = {}) {
    if (sequence >= 100000 || log.tellp() > std::streampos(64ull * 1024 * 1024))
      throw std::runtime_error("Diagnostic event/log byte budget exceeded");
    log << "{\"event\":" << quote(event) << ",\"device_id\":" << deviceId << ",\"sequence\":" << sequence++
      << ",\"frame\":" << frame << ",\"generation\":" << generation << ",\"uid\":" << uid;
    if (!fields.empty()) log << ',' << fields;
    log << "}\n"; log.flush();
    if (!log) throw std::runtime_error("Capture log write failed");
  }
};
inline std::map<IDirect3DDevice9*, std::unique_ptr<State>>& states() {
  static std::map<IDirect3DDevice9*, std::unique_ptr<State>> value; return value;
}
inline std::mutex& mutex() { static std::mutex value; return value; }
// Every callback catches diagnostic failures and preserves the backend HRESULT.
template<class F> inline void observe(IDirect3DDevice9* device, F&& f) noexcept {
  if (!enabled() || !device) return;
  try {
    std::lock_guard<std::mutex> lock(mutex());
    auto& value = states()[device];
    if (!value) value = std::make_unique<State>(device);
    if (value->stopped) return;
    try { f(*value); }
    catch (const std::exception& e) {
      value->stopped = true;
      try { value->emit("diagnostic_error", 0, "\"message\":" + quote(e.what())); } catch (...) {}
    }
  } catch (...) {}
}
inline void snapshot(State& s, IDirect3DDevice9* device, const char* reason, uint32_t uid, IDirect3DSwapChain9* chain = nullptr) {
  if (!s.active() || !s.segmentDraws) return;
  // Separate budgets keep early shadow/RT switches from exhausting depth-clear
  // and final Present observations. Rejected candidates also consume budget.
  uint32_t rangeLimit = config().snapshots >= 4 ? 2u : 0u;
  uint32_t clearLimit = (config().snapshots - rangeLimit - 1) / 2;
  uint32_t otherLimit = config().snapshots - rangeLimit - clearLimit - 1;
  if (std::string(reason) == "Present") {
    if (s.presentAttempts++) return;
  } else if (std::string(reason) == "DepthRangeChange") {
    if (s.rangeAttempts >= rangeLimit) return;
    s.rangeAttempts++;
  } else if (std::string(reason) == "Clear") {
    if (s.clearAttempts >= clearLimit) return;
    s.clearAttempts++;
  } else {
    if (s.otherAttempts >= otherLimit) return;
    s.otherAttempts++;
  }
  Com<IDirect3DSurface9> colorSurface, depthSurface;
  HRESULT colorHr = chain ? chain->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, colorSurface.put()) : device->GetRenderTarget(0, colorSurface.put());
  HRESULT depthHr = device->GetDepthStencilSurface(depthSurface.put());
  auto skip = [&](const char* message) { s.emit("snapshot_skipped", uid, "\"reason\":" + quote(reason) + ",\"message\":" + quote(message)); };
  if (FAILED(colorHr) || FAILED(depthHr) || !colorSurface.get() || !depthSurface.get()) { skip("No color/depth pair bound"); return; }
  Com<ID3D9VkInteropTexture> colorInterop, depthInterop;
  ImageInfo color, depth;
  try {
    color = imageInfo(colorSurface.get(), colorInterop); depth = imageInfo(depthSurface.get(), depthInterop);
  } catch (const std::exception& e) { skip(e.what()); return; }
  std::string details = "\"reason\":" + quote(reason) + ",\"draws\":" + std::to_string(s.draws) +
    ",\"color_id\":" + std::to_string(id(colorSurface.get())) + ",\"depth_id\":" + std::to_string(id(depthSurface.get())) +
    ",\"color\":" + imageJson(color) + ",\"depth\":" + imageJson(depth);
  s.emit("snapshot_candidate", uid, details);
  uint64_t pixels = uint64_t(color.create.extent.width) * color.create.extent.height;
  const bool hdrColor = color.create.format == VK_FORMAT_R16G16B16A16_SFLOAT;
  uint64_t bytes = pixels * (hdrColor ? 17u : 9u) + 128;
  if (color.create.extent.width != depth.create.extent.width || color.create.extent.height != depth.create.extent.height ||
      color.create.extent.width > 4096 || color.create.extent.height > 4096 || !pixels || s.totalBytes + bytes > 256ull * 1024 * 1024) {
    skip("Mismatched extent or capture byte/extent budget exceeded"); return;
  }
  if ((color.create.format != VK_FORMAT_B8G8R8A8_UNORM && color.create.format != VK_FORMAT_R8G8B8A8_UNORM && color.create.format != VK_FORMAT_R16G16B16A16_SFLOAT) ||
      (depth.create.format != VK_FORMAT_D32_SFLOAT && depth.create.format != VK_FORMAT_D24_UNORM_S8_UINT && depth.create.format != VK_FORMAT_D32_SFLOAT_S8_UINT)) {
    skip("Unsupported diagnostic format"); return;
  }
  Com<ID3D9VkInteropDevice> interop;
  hr(device->QueryInterface(__uuidof(ID3D9VkInteropDevice), reinterpret_cast<void**>(interop.put())), "Device Vulkan interop QI");
  Capture capture(interop.get());
  capture.initialize(color, depth);
  VkImageSubresourceRange cr{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}, dr{capture.aspects[1],0,1,0,1};
  // Restoration is queued on every exit after entering transfer layouts.
  struct Restore {
    ID3D9VkInteropDevice* device;
    ID3D9VkInteropTexture *color, *depth;
    VkImageSubresourceRange cr, dr;
    VkImageLayout cl, dl;
    bool armed = true;
    void apply() noexcept {
      if (!armed) return;
      device->TransitionTextureLayout(color, &cr, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, cl);
      device->TransitionTextureLayout(depth, &dr, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dl);
      device->FlushRenderingCommands(); armed = false;
    }
    ~Restore() { apply(); }
  } restore{interop.get(),colorInterop.get(),depthInterop.get(),cr,dr,color.layout,depth.layout};
  interop->TransitionTextureLayout(colorInterop.get(), &cr, color.layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  interop->TransitionTextureLayout(depthInterop.get(), &dr, depth.layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  interop->FlushRenderingCommands();
  if (!interop->WaitForResource(colorSurface.get(), 0) || !interop->WaitForResource(depthSurface.get(), 0))
    throw std::runtime_error("DXVK capture resource wait failed");
  capture.copy(color, depth);
  restore.apply();
  if (!interop->WaitForResource(colorSurface.get(), 0) || !interop->WaitForResource(depthSurface.get(), 0))
    throw std::runtime_error("DXVK restoration wait failed");
  auto name = std::to_string(s.frame) + "-" + std::to_string(s.generation) + "-" + std::to_string(s.snapshots);
  capture.save(s.directory / name);
  s.lastSnapshotDraws = s.draws; s.lastColor = id(colorSurface.get()); s.lastDepth = id(depthSurface.get()); s.lastPrefix = name;
  s.totalBytes += bytes; s.snapshots++;
  details += ",\"color_preview\":" + quote(hdrColor ? "reinhard_linear_no_gamma" : "unorm_rgb") +
    ",\"raw_hdr_color\":" + (hdrColor ? "true" : "false");
  s.emit("snapshot", uid, details + ",\"prefix\":" + quote(name) + ",\"physical_device\":" + quote(capture.physicalProperties.deviceName) +
    ",\"source_layout_restored\":true,\"gpu_copy_completed\":true,\"pixel_verification\":\"unknown_game_content\"");
}
inline void boundary(IDirect3DDevice9* device, const char* name, uint32_t uid, uint64_t argument = 0) noexcept {
  observe(device, [&](State& s) {
    if (!s.active()) return;
    s.emit("boundary_before", uid, "\"api\":" + quote(name) + ",\"argument\":" + std::to_string(argument));
    snapshot(s, device, name, uid);
    s.segmentDraws = 0;
  });
}
inline void viewportBefore(IDirect3DDevice9* device, const D3DVIEWPORT9* next, uint32_t uid) noexcept {
  observe(device, [&](State& s) {
    if (!s.active() || !next) return;
    D3DVIEWPORT9 previous{};
    if (FAILED(device->GetViewport(&previous))) return;
    s.emit("viewport_before", uid, "\"previous\":" + viewportJson(previous) + ",\"next\":" + viewportJson(*next));
    if (previous.MinZ == next->MinZ && previous.MaxZ == next->MaxZ) return;
    Com<IDirect3DSurface9> color, depth;
    device->GetRenderTarget(0, color.put()); device->GetDepthStencilSurface(depth.put());
    // Reuse an immediately preceding snapshot only for the same bindings/draw count.
    bool reuse = !s.lastPrefix.empty() && s.lastSnapshotDraws == s.draws &&
      s.lastColor == id(color.get()) && s.lastDepth == id(depth.get());
    if (!reuse) {
      s.segmentDraws = s.draws;
      snapshot(s, device, "DepthRangeChange", uid);
      reuse = !s.lastPrefix.empty() && s.lastSnapshotDraws == s.draws &&
        s.lastColor == id(color.get()) && s.lastDepth == id(depth.get());
    }
    s.emit("depth_range_before", uid, "\"previous\":" + viewportJson(previous) + ",\"next\":" + viewportJson(*next) +
      ",\"snapshot_prefix\":" + (reuse ? quote(s.lastPrefix) : "null") + ",\"semantic_label\":\"candidate_only\"");
    s.segmentDraws = 0;
  });
}
inline void result(IDirect3DDevice9* device, const char* name, uint32_t uid, HRESULT hresult) noexcept {
  observe(device,[&](State& s) {
    if (!s.active()) return;
    if (SUCCEEDED(hresult) && std::string(name) == "Clear") { s.lastPrefix.clear(); s.lastSnapshotDraws=UINT64_MAX; }
    s.emit("api_result",uid,"\"api\":"+quote(name)+",\"hresult\":"+std::to_string(uint32_t(hresult)));
  });
}
inline void draw(IDirect3DDevice9* device, const char* name, uint32_t uid, HRESULT hresult, uint32_t primitives) noexcept {
  observe(device, [&](State& s) {
    if (!s.active()) return;
    if (SUCCEEDED(hresult)) { s.draws++; s.segmentDraws++; }
    if (s.events++ >= 12000) return;
    Com<IDirect3DSurface9> rt, ds;
    Com<IDirect3DVertexShader9> vs; Com<IDirect3DPixelShader9> ps;
    HRESULT rh = device->GetRenderTarget(0,rt.put()), dh = device->GetDepthStencilSurface(ds.put());
    HRESULT vh = device->GetVertexShader(vs.put()), ph = device->GetPixelShader(ps.put());
    D3DVIEWPORT9 v{}; HRESULT viewport = device->GetViewport(&v);
    DWORD z = 0, zw = 0, blend = 0;
    HRESULT zh = device->GetRenderState(D3DRS_ZENABLE,&z), zwh = device->GetRenderState(D3DRS_ZWRITEENABLE,&zw), bh = device->GetRenderState(D3DRS_ALPHABLENDENABLE,&blend);
    s.emit("draw",uid,"\"api\":"+quote(name)+",\"hresult\":"+std::to_string(uint32_t(hresult))+",\"primitives\":"+std::to_string(primitives)+
      ",\"draw_index\":"+std::to_string(s.draws)+",\"color_id\":"+std::to_string(id(rt.get()))+",\"depth_id\":"+std::to_string(id(ds.get()))+
      ",\"vertex_shader_id\":"+std::to_string(id(vs.get()))+",\"pixel_shader_id\":"+std::to_string(id(ps.get()))+
      ",\"viewport\":"+viewportJson(v)+",\"z_enable\":"+std::to_string(z)+",\"z_write\":"+std::to_string(zw)+",\"alpha_blend\":"+std::to_string(blend)+
      ",\"state_queries_succeeded\":"+((SUCCEEDED(rh)&&SUCCEEDED(dh)&&SUCCEEDED(vh)&&SUCCEEDED(ph)&&SUCCEEDED(viewport)&&SUCCEEDED(zh)&&SUCCEEDED(zwh)&&SUCCEEDED(bh))?"true":"false"));
  });
}
inline void beforePresent(IDirect3DDevice9* device, uint32_t uid, IDirect3DSwapChain9* chain = nullptr) noexcept {
  observe(device,[&](State& s) {
    if (!s.active()) return;
    // Even after a last boundary with no new draws, preserve the final binding.
    s.segmentDraws = s.draws;
    snapshot(s,device,"Present",uid,chain);
    s.emit("present_before",uid,"\"draws\":"+std::to_string(s.draws)+",\"draw_events_truncated\":"+(s.events>12000?"true":"false")+
      ",\"swapchain_id\":"+std::to_string(id(chain)));
  });
}
inline void afterPresent(IDirect3DDevice9* device, uint32_t uid, HRESULT hresult) noexcept {
  observe(device,[&](State& s) {
    if (!s.armed) {
      if (std::filesystem::is_regular_file(config().trigger)) {
        s.armed = true; s.emit("triggered", uid);
      }
      return;
    }
    if (s.active()) s.emit("present_result",uid,"\"hresult\":"+std::to_string(uint32_t(hresult))+",\"snapshots\":"+std::to_string(s.snapshots));
    // An attempted Present defines an observation frame, even if it failed.
    s.frame++; s.draws=0; s.segmentDraws=0; s.snapshots=0; s.clearAttempts=0; s.otherAttempts=0; s.rangeAttempts=0; s.presentAttempts=0; s.lastSnapshotDraws=UINT64_MAX; s.lastPrefix.clear(); s.events=0;
    if (s.frame >= uint64_t(config().start)+config().frames) {
      s.emit("completed",uid,"\"game_capture\":\"observed_dispatcher\",\"semantic_labels\":\"unknown\",\"gpu_fg_execution\":\"unrun\""); s.stopped=true;
    }
  });
}
inline void reset(IDirect3DDevice9* device, uint32_t uid, HRESULT hresult) noexcept {
  observe(device,[&](State& s) {
    if(SUCCEEDED(hresult)) { s.generation++; s.segmentDraws=0; s.lastPrefix.clear(); s.lastSnapshotDraws=UINT64_MAX; }
    s.emit("reset_result",uid,"\"hresult\":"+std::to_string(uint32_t(hresult)));
  });
}
inline void destroy(IDirect3DDevice9* device) noexcept {
  if (!enabled()) return;
  try { std::lock_guard<std::mutex> lock(mutex()); auto found=states().find(device);
    if(found!=states().end()) { if(!found->second->stopped) found->second->emit("device_destroyed",0); states().erase(found); }
  } catch (...) {}
}
inline void swapBefore(IDirect3DSwapChain9* chain, uint32_t uid) noexcept {
  if(!enabled() || !chain) return;
  Com<IDirect3DDevice9> device; if(SUCCEEDED(chain->GetDevice(device.put()))) beforePresent(device.get(),uid,chain);
}
inline void swapAfter(IDirect3DSwapChain9* chain, uint32_t uid, HRESULT hresult) noexcept {
  if(!enabled() || !chain) return;
  Com<IDirect3DDevice9> device; if(SUCCEEDED(chain->GetDevice(device.put()))) afterPresent(device.get(),uid,hresult);
}
}
#else
namespace ldb_capture {
inline bool enabled() noexcept { return false; }
inline void boundary(IDirect3DDevice9*,const char*,uint32_t,uint64_t=0) noexcept {}
inline void viewportBefore(IDirect3DDevice9*,const D3DVIEWPORT9*,uint32_t) noexcept {}
inline void result(IDirect3DDevice9*,const char*,uint32_t,HRESULT) noexcept {}
inline void draw(IDirect3DDevice9*,const char*,uint32_t,HRESULT,uint32_t) noexcept {}
inline void beforePresent(IDirect3DDevice9*,uint32_t,IDirect3DSwapChain9* = nullptr) noexcept {}
inline void afterPresent(IDirect3DDevice9*,uint32_t,HRESULT) noexcept {}
inline void reset(IDirect3DDevice9*,uint32_t,HRESULT) noexcept {}
inline void destroy(IDirect3DDevice9*) noexcept {}
inline void swapBefore(IDirect3DSwapChain9*,uint32_t) noexcept {}
inline void swapAfter(IDirect3DSwapChain9*,uint32_t,HRESULT) noexcept {}
}
#endif

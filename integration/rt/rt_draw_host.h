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
template<class Device> Bytes pipeline(Device* device, bool coverageV7 = false) {
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
  DWORD cull = 0,lighting=0,fog=0,category=0;
  if (coverageV7 && FAILED(device->GetRenderState(D3DRS_CULLMODE,&cull))) { failures |= 512; }
  if (coverageV7 && FAILED(device->GetRenderState(D3DRS_LIGHTING,&lighting))) { failures |= 1024; }
  if (coverageV7 && FAILED(device->GetRenderState(D3DRS_FOGENABLE,&fog))) { failures |= 2048; }
  if (coverageV7 && FAILED(device->GetRenderState(static_cast<D3DRENDERSTATETYPE>(42),&category))) { failures |= 4096; }
  DWORD blend[8]{};
  if(coverageV7) {
    const D3DRENDERSTATETYPE extra[]={D3DRS_SRCBLEND,D3DRS_DESTBLEND,D3DRS_BLENDOP,D3DRS_SEPARATEALPHABLENDENABLE,D3DRS_STENCILENABLE,D3DRS_CLIPPLANEENABLE,D3DRS_COLORWRITEENABLE,D3DRS_ZFUNC};
    for(unsigned i=0;i<8;++i) { if(FAILED(device->GetRenderState(extra[i],&blend[i]))) { failures|=8192u<<i; } }
  }
  w.u32(failures); w.u32(offset); w.u32(stride); w.u32(frequency);
  for (auto value : state) { w.u32(value); }
  w.u32(viewport.X); w.u32(viewport.Y); w.u32(viewport.Width); w.u32(viewport.Height);
  w.f32(viewport.MinZ); w.f32(viewport.MaxZ);
  w.u32(desc.Width); w.u32(desc.Height); w.u32(uint32_t(desc.Format)); w.u32(count);
  if (coverageV7) { w.u32(cull);w.u32(lighting);w.u32(fog);w.u32(category);for(auto value:blend) { w.u32(value); } }
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
  std::map<Bytes,uint32_t> templates;
  uint32_t pendingUid = 0;
  Snapshot nextSnapshot;
  uint32_t nextUid = 0, nextKind = 0;
  std::array<uint32_t,6> nextArgs{};
  bool hasNext = false;
  IDirect3DDevice9* nativeDevice = nullptr; // Bridge device lifetime, never AddRef'd here.
  bool finished = false, pending = false, haveConvertedWorld = false;
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
    Writer header; header.u32(request.beta ? 21 : request.world ? 20 : 1); header.u64(session); header.u64(device); header.u64(uint64_t(frequency.QuadPart));
    if (request.world) { header.u32(uint32_t(request.beta ? 30 : world_diagnostics::kCoveragePresents)); header.u32(30); } record(0, 0, header.data);
  }
  ~Recorder() { finish(3); if (request.world) { world_runtime::state().active=false; } }
  bool expanded() const noexcept { return request.world && ((request.beta&&world_runtime::state().preview)||presents >= 120) && (!finished||world_runtime::state().preview); }
  bool models() const noexcept { return request.world && ((request.beta&&world_runtime::state().preview)||presents >= 180) && (!finished||world_runtime::state().preview); }
  bool sceneReady() const noexcept { return request.world && haveConvertedWorld && (!finished||world_runtime::state().preview); }
  template<class Device> void draw(Device* device, uint32_t uid, const Snapshot& snapshot, uint32_t kind,
      D3DPRIMITIVETYPE type, const std::array<uint32_t, 6>& args) {
    if (snapshot.session != request.session) { ++failures; return; }
    runtime_profile::Scope metadataTiming(runtime_profile::Stage::Metadata);
    nextSnapshot = snapshot; nextUid = uid; nextKind=kind;nextArgs=args;hasNext = true;
    if constexpr (std::is_base_of<IDirect3DDevice9, Device>::value) {
      nativeDevice = device;
      if (request.world && world_runtime::state().active && world_runtime::state().bind) { world_runtime::state().bind(device); }
    }
    if constexpr (std::is_base_of<IDirect3DDevice9, Device>::value) {
      if (request.world && world_runtime::state().active && world_runtime::state().inspect) { world_runtime::state().inspect(device,snapshot,kind,type,args); }
    }
    if (finished) { return; }
    runtime_profile::Scope traceTiming(runtime_profile::Stage::Trace);
    if (written > 256 * 1024 * 1024) { finish(2); return; }
    Writer payload; payload.data = pack(snapshot);const auto snapshotBytes=payload.data.size();payload.u32(kind); payload.u32(uint32_t(type));
    for (auto arg : args) { payload.u32(arg); }
    auto state = pipeline(device, request.world); payload.data.insert(payload.data.end(), state.begin(), state.end());
    if (pending) { ++failures; }
    pending = true; pendingUid = uid;
    if(request.world&&templates.size()<4094) {
      const auto define=[&](uint32_t type,const uint8_t* bytes,size_t size) {
        Bytes key;key.reserve(size+1);key.push_back(uint8_t(type));key.insert(key.end(),bytes,bytes+size);
        auto found=templates.find(key);
        if(found==templates.end()) {
          const auto id=uint32_t(templates.size()+1);Writer definition;definition.u32(id);definition.u32(type);
          definition.data.insert(definition.data.end(),bytes,bytes+size);record(9,0,definition.data);found=templates.emplace(std::move(key),id).first;
        }return found->second;
      };
      const auto snapshotId=define(1,payload.data.data()+40,snapshotBytes-40),pipelineId=define(2,state.data(),state.size());
      Writer compact;compact.data.insert(compact.data.end(),payload.data.begin(),payload.data.begin()+40);
      compact.data.insert(compact.data.end(),payload.data.begin()+snapshotBytes,payload.data.begin()+snapshotBytes+32);
      compact.u32(snapshotId);compact.u32(pipelineId);record(10,uid,compact.data);
    }else { record(1,uid,payload.data); }
    ++draws; if (snapshot.status == Status::Valid) { ++valid; }
  }
  uint32_t kind() const { return nextKind; }
  const std::array<uint32_t,6>& args() const { return nextArgs; }
  bool take(uint32_t uid, Snapshot& snapshot, bool& control) {
    if (!request.world || !hasNext || uid != nextUid) { return false; }
    snapshot = nextSnapshot; hasNext = false; control = presents < 30; return true;
  }
  void conversion(uint32_t uid, consumer_world::Decision decision, const static_draw::Report& report, bool called, HRESULT physical) {
    if (decision==consumer_world::Decision::Selected && report.converted && SUCCEEDED(report.restore)) { haveConvertedWorld=true; }
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
    if(finished) {
      if(request.world&&world_runtime::state().preview&&nativeDevice) {
        if(SUCCEEDED(result)) { ++presents;world_runtime::state().presents=presents; }
        if(world_runtime::state().previewStep) { world_runtime::state().previewStep(nativeDevice,SUCCEEDED(result)); }
      }return;
    } Writer w; w.u32(uint32_t(result)); record(3, uid, w.data);
    if (SUCCEEDED(result)) { ++presents; } else { ++failures; }
    if (request.world) { world_runtime::state().presents=presents; }
    if (request.world && SUCCEEDED(result) && world_runtime::state().active && nativeDevice && world_runtime::state().presented) {
      world_runtime::state().presented(nativeDevice,presents);
    }
    if (request.world && !request.beta && world_runtime::state().active && world_runtime::state().capture && nativeDevice &&
        world_diagnostics::readback(presents)) { world_runtime::state().capture(nativeDevice, presents); }
    const auto target=request.world?(request.beta?30u:unsigned(world_diagnostics::kCoveragePresents)):120u;
    templates.clear();trace.flush(); if (presents >= target) { finish(0); }
  }
  void reset(uint32_t uid, HRESULT result) {
    if(finished&&request.world&&world_runtime::state().previewStep&&world_runtime::state().preview) { world_runtime::state().previewStep(nativeDevice,false); }
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
      if(reason!=0&&nativeDevice&&world_runtime::state().release) { world_runtime::state().release(nativeDevice); }
    }
    try {
      if (pending) { ++failures; }
      Writer footer; footer.u64(draws); footer.u64(valid); footer.u64(presents); footer.u64(resets); footer.u64(failures); footer.u64(unobserved); footer.u32(reason);
      record(8, 0, footer.data); trace.close(); finished = true;
      // The immutable 600-Present trace ends before the independent bounded
      // preview starts. Publish its running marker before done to avoid a
      // client polling race which would stop snapshots at600.
      if(reason==0&&request.world&&nativeDevice&&world_runtime::state().startPreview) { world_runtime::state().startPreview(nativeDevice); }
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

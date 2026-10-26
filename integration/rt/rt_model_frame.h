#pragma once

#include "rt_model_resource.h"

#include <array>
#include <cmath>
#include <map>
#include <memory>
#include <set>

namespace ldb_rt::model_frame {

constexpr uint32_t kArenaCount = 3;
constexpr uint32_t kMaxInstances = 4096;
constexpr size_t kMaxFrameBytes = 16 * 1024 * 1024;

struct Instance {
  uint64_t id=0,generation=0,model=0,modelGeneration=0,view=0;
  uint32_t flags=0;
  std::array<float,12> world{};
  std::vector<std::array<float,12>> bones;
  std::vector<uint32_t> visibleSurfaces;
};

inline size_t payloadBytes(const Instance& value) noexcept {
  return 56+sizeof(value.world)+value.bones.size()*sizeof(value.bones[0])+value.visibleSurfaces.size()*sizeof(uint32_t);
}
inline bool finite(const std::array<float,12>& matrix) noexcept {
  for(const auto value:matrix) { if(!std::isfinite(value)||std::abs(value)>1e8f) { return false; } }return true;
}
inline model_resource::Code validate(const Instance& value,const model_resource::Definition& model) noexcept {
  if(!value.id||!value.generation||!value.model||value.model!=model.id||value.modelGeneration!=model.generation||!value.view||
     value.flags||!finite(value.world)||value.bones.empty()||value.bones.size()>model.maxBones) { return model_resource::Code::Invalid; }
  for(const auto& bone:value.bones) { if(!finite(bone)) { return model_resource::Code::Invalid; } }
  const auto words=(model.surfaces.size()+31)/32;
  if(value.visibleSurfaces.size()!=words) { return model_resource::Code::Invalid; }
  if(model.surfaces.size()%32) {
    const auto validBits=(uint32_t(1)<<(model.surfaces.size()%32))-1;
    if(value.visibleSurfaces.back()&~validBits) { return model_resource::Code::Invalid; }
  }
  bool visible=false;for(const auto word:value.visibleSurfaces) { visible=visible||word; }return visible?model_resource::Code::Ok:model_resource::Code::Invalid;
}

inline uint64_t digest(const Instance& value) noexcept {
  uint64_t hash=1469598103934665603ull;
  const auto append=[&](const void* data,size_t size) {
    const auto* bytes=static_cast<const uint8_t*>(data);for(size_t n=0;n<size;++n) { hash^=bytes[n];hash*=1099511628211ull; }
  };
  append(&value.id,sizeof(value.id));append(&value.generation,sizeof(value.generation));append(&value.model,sizeof(value.model));
  append(&value.modelGeneration,sizeof(value.modelGeneration));append(&value.view,sizeof(value.view));append(value.world.data(),sizeof(value.world));
  append(value.bones.data(),value.bones.size()*sizeof(value.bones[0]));append(value.visibleSurfaces.data(),value.visibleSurfaces.size()*sizeof(uint32_t));
  return hash;
}

inline std::vector<uint8_t> encode(const Instance& value) {
  model_resource::Writer w;w.u32(model_resource::kWireMagic);w.u32(model_resource::kWireVersion);
  w.u32(uint32_t(model_resource::EventKind::Instance));w.u32(0);w.u64(value.id);w.u64(value.generation);w.u64(value.model);
  w.u64(value.modelGeneration);w.u64(value.view);w.u32(value.flags);w.u32(uint32_t(value.bones.size()));w.u32(uint32_t(value.visibleSurfaces.size()));w.u32(0);
  const auto floats=[&](const auto& values) { for(const auto& row:values) { uint32_t bits=0;std::memcpy(&bits,&row,4);w.u32(bits); } };
  floats(value.world);for(const auto& bone:value.bones) { floats(bone); }for(const auto word:value.visibleSurfaces) { w.u32(word); }
  const auto size=uint32_t(w.data.size());for(unsigned n=0;n<4;++n) { w.data[12+n]=uint8_t(size>>(8*n)); }return w.data;
}

inline model_resource::Code decodeInstance(const void* data,size_t size,Instance& output) noexcept {
  if(!data||size<128||size>kMaxFrameBytes) { return model_resource::Code::Wire; }
  model_resource::Reader r(data,size);bool ok=true;
  if(r.u32(ok)!=model_resource::kWireMagic||r.u32(ok)!=model_resource::kWireVersion||
     r.u32(ok)!=uint32_t(model_resource::EventKind::Instance)||r.u32(ok)!=size) { return model_resource::Code::Wire; }
  Instance value;value.id=r.u64(ok);value.generation=r.u64(ok);value.model=r.u64(ok);value.modelGeneration=r.u64(ok);value.view=r.u64(ok);
  value.flags=r.u32(ok);const auto bones=r.u32(ok),words=r.u32(ok);if(r.u32(ok)||!ok||!bones||bones>model_resource::kMaxBones||!words||words>8) { return model_resource::Code::Wire; }
  const auto floats=[&](auto& values) { for(auto& row:values) { const auto bits=r.u32(ok);std::memcpy(&row,&bits,4); } };
  floats(value.world);value.bones.resize(bones);for(auto& bone:value.bones) { floats(bone); }
  value.visibleSurfaces.resize(words);for(auto& word:value.visibleSurfaces) { word=r.u32(ok); }
  if(!ok||!r.end()) { return model_resource::Code::Wire; }output=std::move(value);return model_resource::Code::Ok;
}

struct Frame {
  uint64_t id=0,generation=0,digest=0;
  size_t payloadBytes=0,paletteBytes=0;
  std::vector<Instance> instances;
};
struct Commit {
  uint64_t frame=0,generation=0,instances=0,payloadBytes=0,digest=0;
};
inline std::vector<uint8_t> encode(const Commit& value) {
  model_resource::Writer w;w.u32(model_resource::kWireMagic);w.u32(model_resource::kWireVersion);
  w.u32(uint32_t(model_resource::EventKind::Commit));w.u32(56);w.u64(value.frame);w.u64(value.generation);
  w.u64(value.instances);w.u64(value.payloadBytes);w.u64(value.digest);return w.data;
}
inline model_resource::Code decodeCommit(const void* data,size_t size,Commit& output) noexcept {
  if(!data||size!=56) { return model_resource::Code::Wire; }model_resource::Reader r(data,size);bool ok=true;
  if(r.u32(ok)!=model_resource::kWireMagic||r.u32(ok)!=model_resource::kWireVersion||
     r.u32(ok)!=uint32_t(model_resource::EventKind::Commit)||r.u32(ok)!=size) { return model_resource::Code::Wire; }
  Commit value;value.frame=r.u64(ok);value.generation=r.u64(ok);value.instances=r.u64(ok);value.payloadBytes=r.u64(ok);value.digest=r.u64(ok);
  if(!ok||!r.end()||!value.frame||!value.generation||value.instances>kMaxInstances||value.payloadBytes>kMaxFrameBytes) { return model_resource::Code::Invalid; }
  output=value;return model_resource::Code::Ok;
}
struct Statistics {
  uint64_t frames=0,instances=0,paletteBytes=0,rejected=0,omitted=0;
  uint64_t cpuPosedVertices=0,cpuRewrittenIndices=0,compatibilityFallbacks=0;
};

class TripleArena {
  enum class State { Free,Writing,Committed,InFlight };
  struct Slot { State state=State::Free;Frame frame; };
  std::array<Slot,kArenaCount> m_slots{};
  Slot* m_writing=nullptr;
  uint64_t m_lastFrame=0;
  Statistics m_stats;
  model_resource::Code reject(model_resource::Code code) noexcept { ++m_stats.rejected;return code; }
public:
  model_resource::Code begin(uint64_t frame,uint64_t generation) noexcept {
    if(m_writing||!frame||!generation||frame<=m_lastFrame) { return reject(model_resource::Code::FrameOrder); }
    auto& slot=m_slots[frame%kArenaCount];if(slot.state!=State::Free) { return reject(model_resource::Code::FrameBusy); }
    slot.frame={};slot.frame.id=frame;slot.frame.generation=generation;slot.frame.digest=1469598103934665603ull;
    slot.state=State::Writing;m_writing=&slot;return model_resource::Code::Ok;
  }
  model_resource::Code add(Instance value,const model_resource::Registry& registry) {
    if(!m_writing||m_writing->state!=State::Writing) { return reject(model_resource::Code::FrameOrder); }
    const auto* resource=registry.find(value.model,value.modelGeneration);if(!resource) { ++m_stats.omitted;return reject(model_resource::Code::NotReady); }
    const auto checked=validate(value,resource->definition);if(checked!=model_resource::Code::Ok) { ++m_stats.omitted;return reject(checked); }
    if(m_writing->frame.instances.size()>=kMaxInstances) { ++m_stats.omitted;return reject(model_resource::Code::Limit); }
    for(const auto& current:m_writing->frame.instances) {
      if(current.id==value.id&&current.generation==value.generation&&current.view==value.view) { ++m_stats.omitted;return reject(model_resource::Code::Duplicate); }
    }
    const auto bytes=model_frame::payloadBytes(value);if(bytes>kMaxFrameBytes-m_writing->frame.payloadBytes) { ++m_stats.omitted;return reject(model_resource::Code::Limit); }
    const auto instanceDigest=digest(value);for(unsigned n=0;n<8;++n) { m_writing->frame.digest^=uint8_t(instanceDigest>>(8*n));m_writing->frame.digest*=1099511628211ull; }
    m_writing->frame.payloadBytes+=bytes;
    m_writing->frame.paletteBytes+=value.bones.size()*sizeof(value.bones[0]);m_writing->frame.instances.push_back(std::move(value));return model_resource::Code::Ok;
  }
  model_resource::Code commit(size_t expectedInstances,size_t expectedBytes,uint64_t expectedDigest=0) noexcept {
    if(!m_writing||m_writing->state!=State::Writing) { return reject(model_resource::Code::FrameOrder); }
    auto& frame=m_writing->frame;if(frame.instances.size()!=expectedInstances||frame.payloadBytes!=expectedBytes||
       (expectedDigest&&frame.digest!=expectedDigest)) { return reject(model_resource::Code::CommitMismatch); }
    m_writing->state=State::Committed;m_lastFrame=frame.id;++m_stats.frames;m_stats.instances+=frame.instances.size();m_stats.paletteBytes+=frame.paletteBytes;m_writing=nullptr;return model_resource::Code::Ok;
  }
  const Frame* acquire(uint64_t frame) noexcept {
    auto& slot=m_slots[frame%kArenaCount];if(slot.state!=State::Committed||slot.frame.id!=frame) { return nullptr; }
    slot.state=State::InFlight;return &slot.frame;
  }
  model_resource::Code complete(uint64_t frame) noexcept {
    auto& slot=m_slots[frame%kArenaCount];if(slot.state!=State::InFlight||slot.frame.id!=frame) { return reject(model_resource::Code::FrameOrder); }
    slot.state=State::Free;slot.frame={};return model_resource::Code::Ok;
  }
  const Statistics& statistics() const noexcept { return m_stats; }
};

} // namespace ldb_rt::model_frame

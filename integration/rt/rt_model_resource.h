#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

namespace ldb_rt::model_resource {

constexpr uint32_t kWireMagic = 0x524d444c; // LDMR
constexpr uint32_t kWireVersion = 1;
constexpr uint32_t kMaxSurfaces = 256;
constexpr uint32_t kMaxBones = 256;
constexpr uint32_t kMaxVerticesPerSurface = 1u << 24;
constexpr uint32_t kMaxIndicesPerSurface = 1u << 26;
constexpr size_t kMaxDefinitionBytes = 256 * 1024;

enum class EventKind : uint32_t { Define = 1, Instance, Invalidate, Commit };
enum class Code : uint32_t {
  Ok, Wire, Invalid, Limit, Stale, Duplicate, MissingResource, NotReady,
  FrameBusy, FrameOrder, CommitMismatch
};
inline const char* name(Code code) noexcept {
  static const char* values[] = {"ok","wire","invalid","limit","stale","duplicate","missing_resource","not_ready",
    "frame_busy","frame_order","commit_mismatch"};
  const auto index = static_cast<unsigned>(code);
  return index < sizeof(values) / sizeof(values[0]) ? values[index] : "unknown";
}

struct BufferRef {
  uint64_t id = 0, generation = 0;
  bool operator==(const BufferRef& other) const noexcept { return id == other.id && generation == other.generation; }
  bool operator<(const BufferRef& other) const noexcept { return std::tie(id,generation) < std::tie(other.id,other.generation); }
};

enum SurfaceFlags : uint32_t {
  AlphaTest = 1u << 0,
  AlphaBlend = 1u << 1,
  DoubleSided = 1u << 2,
  ViewModel = 1u << 3
};
constexpr uint32_t kSurfaceFlags = AlphaTest | AlphaBlend | DoubleSided | ViewModel;

struct Surface {
  uint32_t vertexCount = 0, indexCount = 0, materialSlot = 0;
  uint32_t bonesPerVertex = 0, flags = 0;
  BufferRef vertices, indices, skinning, material;
};

struct Definition {
  uint64_t id = 0, generation = 0, topology = 0;
  uint32_t lod = 0, body = 0, skin = 0, maxBones = 0;
  std::vector<Surface> surfaces;
};

class Writer {
public:
  std::vector<uint8_t> data;
  void u32(uint32_t value) { for(unsigned n=0;n<4;++n) { data.push_back(uint8_t(value>>(8*n))); } }
  void u64(uint64_t value) { u32(uint32_t(value));u32(uint32_t(value>>32)); }
};
class Reader {
  const uint8_t* m_data;
  size_t m_size;
public:
  size_t offset = 0;
  Reader(const void* data,size_t size):m_data(static_cast<const uint8_t*>(data)),m_size(size) {}
  uint32_t u32(bool& ok) noexcept {
    if(!ok||offset>m_size||m_size-offset<4) { ok=false;return 0; }
    uint32_t value=0;for(unsigned n=0;n<4;++n) { value|=uint32_t(m_data[offset++])<<(8*n); }return value;
  }
  uint64_t u64(bool& ok) noexcept { const auto lo=u32(ok),hi=u32(ok);return uint64_t(lo)|(uint64_t(hi)<<32); }
  bool end() const noexcept { return offset==m_size; }
};

inline void buffer(Writer& w,const BufferRef& value) { w.u64(value.id);w.u64(value.generation); }
inline BufferRef buffer(Reader& r,bool& ok) noexcept { return {r.u64(ok),r.u64(ok)}; }

inline bool valid(const BufferRef& value) noexcept { return value.id && value.generation; }
inline Code validate(const Definition& value) noexcept {
  if(!value.id||!value.generation||!value.topology||!value.maxBones||value.maxBones>kMaxBones||
      value.surfaces.empty()||value.surfaces.size()>kMaxSurfaces) { return Code::Invalid; }
  std::set<uint32_t> materialSlots;
  for(const auto& surface:value.surfaces) {
    if(!surface.vertexCount||surface.vertexCount>kMaxVerticesPerSurface||!surface.indexCount||
        surface.indexCount>kMaxIndicesPerSurface||surface.indexCount%3||!surface.bonesPerVertex||
        surface.bonesPerVertex>4||(surface.flags&~kSurfaceFlags)||!valid(surface.vertices)||!valid(surface.indices)||
        !valid(surface.skinning)||!valid(surface.material)||!materialSlots.insert(surface.materialSlot).second) { return Code::Invalid; }
  }
  return Code::Ok;
}

inline std::vector<uint8_t> encode(const Definition& value) {
  Writer w;w.u32(kWireMagic);w.u32(kWireVersion);w.u32(uint32_t(EventKind::Define));w.u32(0);
  w.u64(value.id);w.u64(value.generation);w.u64(value.topology);
  w.u32(value.lod);w.u32(value.body);w.u32(value.skin);w.u32(value.maxBones);w.u32(uint32_t(value.surfaces.size()));w.u32(0);
  for(const auto& surface:value.surfaces) {
    w.u32(surface.vertexCount);w.u32(surface.indexCount);w.u32(surface.materialSlot);w.u32(surface.bonesPerVertex);w.u32(surface.flags);w.u32(0);
    buffer(w,surface.vertices);buffer(w,surface.indices);buffer(w,surface.skinning);buffer(w,surface.material);
  }
  const auto size=uint32_t(w.data.size());for(unsigned n=0;n<4;++n) { w.data[12+n]=uint8_t(size>>(8*n)); }return w.data;
}

inline Code decodeDefinition(const void* data,size_t size,Definition& output) noexcept {
  if(!data||size<64||size>kMaxDefinitionBytes) { return Code::Wire; }
  Reader r(data,size);bool ok=true;
  if(r.u32(ok)!=kWireMagic||r.u32(ok)!=kWireVersion||r.u32(ok)!=uint32_t(EventKind::Define)||r.u32(ok)!=size) { return Code::Wire; }
  Definition value;value.id=r.u64(ok);value.generation=r.u64(ok);value.topology=r.u64(ok);
  value.lod=r.u32(ok);value.body=r.u32(ok);value.skin=r.u32(ok);value.maxBones=r.u32(ok);const auto count=r.u32(ok);
  if(r.u32(ok)||!ok||!count||count>kMaxSurfaces) { return Code::Wire; }
  value.surfaces.resize(count);
  for(auto& surface:value.surfaces) {
    surface.vertexCount=r.u32(ok);surface.indexCount=r.u32(ok);surface.materialSlot=r.u32(ok);
    surface.bonesPerVertex=r.u32(ok);surface.flags=r.u32(ok);if(r.u32(ok)) { return Code::Wire; }
    surface.vertices=buffer(r,ok);surface.indices=buffer(r,ok);surface.skinning=buffer(r,ok);surface.material=buffer(r,ok);
  }
  if(!ok||!r.end()) { return Code::Wire; }const auto code=validate(value);if(code!=Code::Ok) { return code; }
  output=std::move(value);return Code::Ok;
}

struct Invalidation { uint64_t id=0,generation=0; };
inline std::vector<uint8_t> encode(const Invalidation& value) {
  Writer w;w.u32(kWireMagic);w.u32(kWireVersion);w.u32(uint32_t(EventKind::Invalidate));w.u32(32);
  w.u64(value.id);w.u64(value.generation);return w.data;
}
inline Code decodeInvalidation(const void* data,size_t size,Invalidation& output) noexcept {
  if(!data||size!=32) { return Code::Wire; }Reader r(data,size);bool ok=true;
  if(r.u32(ok)!=kWireMagic||r.u32(ok)!=kWireVersion||r.u32(ok)!=uint32_t(EventKind::Invalidate)||r.u32(ok)!=size) { return Code::Wire; }
  Invalidation value{r.u64(ok),r.u64(ok)};if(!ok||!r.end()||!value.id||!value.generation) { return Code::Invalid; }
  output=value;return Code::Ok;
}

struct Statistics {
  uint64_t definitions=0,ready=0,invalidations=0,rejected=0;
  uint64_t surfaces=0,vertices=0,indices=0;
};

class Registry {
public:
  enum class State : uint32_t { Pending, Ready };
  struct Record { Definition definition;State state=State::Pending; };
private:
  std::map<uint64_t,Record> m_records;
  std::map<uint64_t,uint64_t> m_lastGeneration;
  Statistics m_stats;
  Code reject(Code code) noexcept { ++m_stats.rejected;return code; }
public:
  Code define(Definition value) {
    const auto check=validate(value);if(check!=Code::Ok) { return reject(check); }
    if(m_records.count(value.id)) { return reject(Code::Duplicate); }
    const auto last=m_lastGeneration.find(value.id);
    if((last==m_lastGeneration.end()&&value.generation!=1)||
       (last!=m_lastGeneration.end()&&(last->second==UINT64_MAX||value.generation!=last->second+1))) { return reject(Code::Stale); }
    for(const auto& surface:value.surfaces) {
      ++m_stats.surfaces;m_stats.vertices+=surface.vertexCount;m_stats.indices+=surface.indexCount;
    }
    m_lastGeneration[value.id]=value.generation;m_records.emplace(value.id,Record{std::move(value),State::Pending});++m_stats.definitions;
    return Code::Ok;
  }
  Code markReady(uint64_t id,uint64_t generation) noexcept {
    const auto found=m_records.find(id);if(found==m_records.end()||found->second.definition.generation!=generation) { return reject(Code::Stale); }
    if(found->second.state==State::Ready) { return reject(Code::Duplicate); }
    found->second.state=State::Ready;++m_stats.ready;return Code::Ok;
  }
  Code invalidate(uint64_t id,uint64_t generation) noexcept {
    const auto found=m_records.find(id);if(found==m_records.end()||found->second.definition.generation!=generation) { return reject(Code::Stale); }
    m_records.erase(found);++m_stats.invalidations;return Code::Ok;
  }
  const Record* find(uint64_t id,uint64_t generation,bool requireReady=true) const noexcept {
    const auto found=m_records.find(id);if(found==m_records.end()||found->second.definition.generation!=generation||
       (requireReady&&found->second.state!=State::Ready)) { return nullptr; }return &found->second;
  }
  size_t size() const noexcept { return m_records.size(); }
  const Statistics& statistics() const noexcept { return m_stats; }
  void reset() noexcept { m_records.clear();m_lastGeneration.clear(); }
};

} // namespace ldb_rt::model_resource

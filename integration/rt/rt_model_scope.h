#pragma once

#include "rt_model_resource.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <utility>

namespace ldb_rt::model_scope {

using Matrix3x4 = std::array<float,12>;
constexpr size_t kMaxPath = 260;

struct Input {
  const char* path=nullptr;
  uint32_t checksum=0;
  uintptr_t renderable=0;
  int32_t entity=-1,skin=0,body=0,lod=0;
  uint16_t handle=0;
  const Matrix3x4* bones=nullptr;
  uint32_t boneCount=0;
};
struct Identity {
  uint64_t resource=0,instance=0;
  uint32_t checksum=0,boneCount=0;
  int32_t entity=-1,skin=0,body=0,lod=0;
  uint16_t handle=0;
  std::array<char,kMaxPath> path{};
};
struct SurfaceView {
  const Identity* identity=nullptr;
  const Matrix3x4* bones=nullptr;
  uint64_t scope=0;
  uint32_t surface=0,boneCount=0;
  bool first=false;
};
struct Captured {
  Identity identity;
  std::array<Matrix3x4,model_resource::kMaxBones> bones{};
  uint64_t scope=0;
  uint32_t boneCount=0;
};

inline uint64_t hashAppend(uint64_t hash,const void* data,size_t size) noexcept {
  const auto* bytes=static_cast<const uint8_t*>(data);for(size_t n=0;n<size;++n) { hash^=bytes[n];hash*=1099511628211ull; }return hash;
}
template<class T> inline uint64_t hashAppend(uint64_t hash,const T& value) noexcept { return hashAppend(hash,&value,sizeof(value)); }
inline bool boundedPath(const char* path,size_t& length) noexcept {
  if(!path) { return false; }for(length=0;length<kMaxPath&&path[length];++length) { }
  return length&&length<kMaxPath;
}
inline bool finite(const Matrix3x4& value) noexcept {
  for(const auto x:value) { if(!std::isfinite(x)||std::abs(x)>1e8f) { return false; } }return true;
}
inline bool identify(const Input& input,Identity& output) noexcept {
  size_t pathLength=0;if(!boundedPath(input.path,pathLength)||!input.checksum||!input.renderable||input.entity<0||
    input.skin<0||input.body<0||input.lod<0||!input.bones||!input.boneCount||input.boneCount>model_resource::kMaxBones) { return false; }
  for(uint32_t n=0;n<input.boneCount;++n) { if(!finite(input.bones[n])) { return false; } }
  Identity value;value.checksum=input.checksum;value.boneCount=input.boneCount;value.entity=input.entity;value.skin=input.skin;
  value.body=input.body;value.lod=input.lod;value.handle=input.handle;std::memcpy(value.path.data(),input.path,pathLength);
  uint64_t resource=1469598103934665603ull;resource=hashAppend(resource,input.path,pathLength);resource=hashAppend(resource,input.checksum);
  resource=hashAppend(resource,input.skin);resource=hashAppend(resource,input.body);resource=hashAppend(resource,input.lod);
  uint64_t instance=1469598103934665603ull;instance=hashAppend(instance,input.renderable);instance=hashAppend(instance,input.entity);
  instance=hashAppend(instance,input.handle);instance=hashAppend(instance,resource);
  value.resource=resource?resource:1;value.instance=instance?instance:1;output=value;return true;
}

constexpr size_t kMaxScopeDepth=8;
struct ScopeState {
  Identity identity;
  std::array<Matrix3x4,model_resource::kMaxBones> bones;
  uint64_t scope;
  uint32_t boneCount,surfaces;
};
inline thread_local std::array<ScopeState,kMaxScopeDepth> g_scopeStack{};
inline thread_local size_t g_scopeDepth=0;
inline std::atomic<uint64_t> g_nextScope{0};
inline std::atomic<uint64_t> g_surfacesTaken{0};

inline uint64_t nextScope() noexcept {
  auto value=g_nextScope.fetch_add(1,std::memory_order_relaxed)+1;
  if(!value) { value=g_nextScope.fetch_add(1,std::memory_order_relaxed)+1; }
  return value;
}
inline bool capture(const Input& input,Captured& output) noexcept {
  Captured value;if(!identify(input,value.identity)) { return false; }
  std::copy(input.bones,input.bones+input.boneCount,value.bones.begin());value.boneCount=input.boneCount;value.scope=nextScope();
  output=std::move(value);return true;
}
inline bool begin(const Captured& input) noexcept {
  if(!input.scope||!input.boneCount||input.boneCount>model_resource::kMaxBones||g_scopeDepth>=kMaxScopeDepth) { return false; }
  auto& state=g_scopeStack[g_scopeDepth];state={};state.identity=input.identity;state.bones=input.bones;
  state.boneCount=input.boneCount;state.scope=input.scope;++g_scopeDepth;return true;
}
inline bool end(uint64_t scope) noexcept {
  if(!scope||!g_scopeDepth||g_scopeStack[g_scopeDepth-1].scope!=scope) { return false; }
  --g_scopeDepth;return true;
}
inline uint64_t surfacesTaken() noexcept { return g_surfacesTaken.load(std::memory_order_relaxed); }
inline void resetSurfacesTaken() noexcept { g_surfacesTaken.store(0,std::memory_order_relaxed); }

class Scope {
  size_t m_depth=0;
  bool m_valid=false;
public:
  explicit Scope(const Input& input) noexcept {
    Captured captured;if(!capture(input,captured)||!begin(captured)) { return; }m_depth=g_scopeDepth;m_valid=true;
  }
  explicit Scope(const Captured& input) noexcept {
    if(!begin(input)) { return; }m_depth=g_scopeDepth;m_valid=true;
  }
  Scope(const Scope&)=delete;Scope& operator=(const Scope&)=delete;
  ~Scope() { if(m_valid&&g_scopeDepth==m_depth) { end(g_scopeStack[m_depth-1].scope); } }
  bool valid() const noexcept { return m_valid; }
  uint32_t surfaces() const noexcept { return m_valid&&m_depth&&g_scopeDepth>=m_depth?g_scopeStack[m_depth-1].surfaces:0; }
  static SurfaceView takeSurface() noexcept {
    SurfaceView out;if(!g_scopeDepth) { return out; }auto& current=g_scopeStack[g_scopeDepth-1];
    out.identity=&current.identity;out.bones=current.bones.data();out.scope=current.scope;
    out.surface=current.surfaces++;out.first=out.surface==0;out.boneCount=current.boneCount;g_surfacesTaken.fetch_add(1,std::memory_order_relaxed);return out;
  }
  static const Identity* currentIdentity() noexcept { return g_scopeDepth?&g_scopeStack[g_scopeDepth-1].identity:nullptr; }
};

} // namespace ldb_rt::model_scope

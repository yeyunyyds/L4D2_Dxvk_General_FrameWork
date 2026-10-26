#pragma once

#include "rt_model_scope.h"

#include <atomic>

#if defined(_WIN32) && (defined(_M_IX86) || defined(__i386__))
#include <windows.h>
#endif

namespace ldb_rt::model_hook {

enum class Status : uint32_t { Unavailable, Installed, Failed };

#if defined(_WIN32) && (defined(_M_IX86) || defined(__i386__))

struct Vector { float x,y,z; };
struct StudioHeaderPrefix {
  int32_t id,version,checksum;
  char name[64];
  int32_t length;
  Vector eyePosition,illumPosition,hullMin,hullMax,viewMin,viewMax;
  int32_t flags,numbones;
};
struct ModelPrefix { uint32_t functionHandle;char path[260]; };
struct DrawModelState {
  uint32_t studioHeader,studioHardware,renderable,modelToWorld,decals;
  int32_t drawFlags,lod;
};
struct ModelRenderInfo {
  Vector origin,angles;
  uint32_t renderable,model,modelToWorld,lightingOffset,lightingOrigin;
  int32_t flags,entity,skin,body,hitboxSet;
  uint16_t instance,padding;
};
static_assert(sizeof(StudioHeaderPrefix)==160,"L4D2 studio header prefix ABI");
static_assert(sizeof(ModelPrefix)==264,"L4D2 model prefix ABI");
static_assert(sizeof(DrawModelState)==28,"L4D2 DrawModelState ABI");
static_assert(sizeof(ModelRenderInfo)==68,"L4D2 ModelRenderInfo ABI");

using DrawModelExecute = void(__fastcall*)(void*,void*,const DrawModelState&,const ModelRenderInfo&,model_scope::Matrix3x4*);
inline DrawModelExecute g_original=nullptr;
inline std::atomic<Status> g_status{Status::Unavailable};
inline std::atomic_flag g_installing=ATOMIC_FLAG_INIT;

template<class T> inline bool readProcess(const void* address,T& output) noexcept {
  SIZE_T bytes=0;return address&&ReadProcessMemory(GetCurrentProcess(),address,&output,sizeof(output),&bytes)&&bytes==sizeof(output);
}
inline bool executable(const void* address) noexcept {
  MEMORY_BASIC_INFORMATION page{};if(!address||!VirtualQuery(address,&page,sizeof(page))||page.State!=MEM_COMMIT||page.Type!=MEM_IMAGE) { return false; }
  const auto protect=page.Protect&0xff;return protect==PAGE_EXECUTE||protect==PAGE_EXECUTE_READ||protect==PAGE_EXECUTE_READWRITE||protect==PAGE_EXECUTE_WRITECOPY;
}
inline void __fastcall hooked(void* self,void* edx,const DrawModelState& stateRef,const ModelRenderInfo& infoRef,model_scope::Matrix3x4* customBones) noexcept {
  DrawModelState state{};ModelRenderInfo info{};StudioHeaderPrefix studio{};ModelPrefix model{};
  std::array<model_scope::Matrix3x4,model_resource::kMaxBones> bones{};
  bool captured=readProcess(&stateRef,state)&&readProcess(&infoRef,info)&&readProcess(reinterpret_cast<const void*>(uintptr_t(state.studioHeader)),studio)&&
    readProcess(reinterpret_cast<const void*>(uintptr_t(info.model)),model)&&studio.checksum!=0&&studio.numbones>0&&studio.numbones<=int32_t(bones.size())&&
    info.renderable&&info.entity>=0&&customBones;
  if(captured) {
    SIZE_T bytes=0;const auto wanted=size_t(studio.numbones)*sizeof(bones[0]);
    captured=ReadProcessMemory(GetCurrentProcess(),customBones,bones.data(),wanted,&bytes)&&bytes==wanted;
  }
  if(captured) {
    model_scope::Input input;input.path=model.path;input.checksum=uint32_t(studio.checksum);input.renderable=info.renderable;input.entity=info.entity;
    input.skin=info.skin;input.body=info.body;input.lod=state.lod;input.handle=info.instance;input.bones=bones.data();input.boneCount=uint32_t(studio.numbones);
    model_scope::Scope scope(input);g_original(self,edx,stateRef,infoRef,customBones);return;
  }
  g_original(self,edx,stateRef,infoRef,customBones);
}

inline Status install() noexcept {
  const auto current=g_status.load(std::memory_order_acquire);if(current!=Status::Unavailable) { return current; }
  if(g_installing.test_and_set(std::memory_order_acquire)) { return Status::Unavailable; }
  struct Unlock { ~Unlock() { g_installing.clear(std::memory_order_release); } } unlock;
  if(g_status.load(std::memory_order_relaxed)!=Status::Unavailable) { return g_status.load(std::memory_order_relaxed); }
  const auto engine=GetModuleHandleW(L"engine.dll");
  using CreateInterface=void*(__cdecl*)(const char*,int*);
  CreateInterface create=nullptr;const auto symbol=engine?GetProcAddress(engine,"CreateInterface"):nullptr;
  static_assert(sizeof(create)==sizeof(symbol),"Windows function pointer ABI");std::memcpy(&create,&symbol,sizeof(create));
  void* object=create?create("VEngineModel016",nullptr):nullptr;void** table=nullptr;
  if(!object||!readProcess(object,table)||!table) { g_status.store(Status::Failed,std::memory_order_release);return Status::Failed; }
  void* target=nullptr;if(!readProcess(table+19,target)||!executable(target)) { g_status.store(Status::Failed,std::memory_order_release);return Status::Failed; }
  DWORD old=0;if(!VirtualProtect(table+19,sizeof(void*),PAGE_READWRITE,&old)) { g_status.store(Status::Failed,std::memory_order_release);return Status::Failed; }
  g_original=reinterpret_cast<DrawModelExecute>(target);
  const auto previous=InterlockedCompareExchangePointer(reinterpret_cast<void* volatile*>(table+19),reinterpret_cast<void*>(&hooked),target);
  DWORD ignored=0;(void)VirtualProtect(table+19,sizeof(void*),old,&ignored);FlushInstructionCache(GetCurrentProcess(),table+19,sizeof(void*));
  const auto result=previous==target?Status::Installed:Status::Failed;if(result==Status::Failed) { g_original=nullptr; }
  g_status.store(result,std::memory_order_release);return result;
}

#else

inline Status install() noexcept { return Status::Unavailable; }

#endif

} // namespace ldb_rt::model_hook

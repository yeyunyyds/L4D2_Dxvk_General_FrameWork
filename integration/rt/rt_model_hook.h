#pragma once

#include "rt_model_scope.h"

#include <atomic>

#if defined(LDB_RT_ENABLE_MODEL_HOOK) && defined(_WIN32) && (defined(_M_IX86) || defined(__i386__))
#include <windows.h>
#include <detours.h>
#endif

namespace ldb_rt::model_hook {

enum class Status : uint32_t { Unavailable, Installed, Failed };

#if defined(LDB_RT_ENABLE_MODEL_HOOK) && defined(_WIN32) && (defined(_M_IX86) || defined(__i386__))

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
inline std::atomic<uint64_t> g_calls{0},g_captured{0},g_rejected{0};
inline std::atomic<uintptr_t> g_target{0};

template<class T> inline bool readProcess(const void* address,T& output) noexcept {
  SIZE_T bytes=0;return address&&ReadProcessMemory(GetCurrentProcess(),address,&output,sizeof(output),&bytes)&&bytes==sizeof(output);
}
inline bool executable(const void* address) noexcept {
  MEMORY_BASIC_INFORMATION page{};if(!address||!VirtualQuery(address,&page,sizeof(page))||page.State!=MEM_COMMIT||page.Type!=MEM_IMAGE) { return false; }
  const auto protect=page.Protect&0xff;return protect==PAGE_EXECUTE||protect==PAGE_EXECUTE_READ||protect==PAGE_EXECUTE_READWRITE||protect==PAGE_EXECUTE_WRITECOPY;
}
inline void __fastcall hooked(void* self,void* edx,const DrawModelState& stateRef,const ModelRenderInfo& infoRef,model_scope::Matrix3x4* customBones) noexcept {
  g_calls.fetch_add(1,std::memory_order_relaxed);
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
    model_scope::Scope scope(input);
    if(scope.valid()) { g_captured.fetch_add(1,std::memory_order_relaxed);g_original(self,edx,stateRef,infoRef,customBones);return; }
  }
  g_rejected.fetch_add(1,std::memory_order_relaxed);
  g_original(self,edx,stateRef,infoRef,customBones);
}

inline uint64_t calls() noexcept { return g_calls.load(std::memory_order_relaxed); }
inline uint64_t captured() noexcept { return g_captured.load(std::memory_order_relaxed); }
inline uint64_t rejected() noexcept { return g_rejected.load(std::memory_order_relaxed); }
inline uintptr_t target() noexcept { return g_target.load(std::memory_order_relaxed); }

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
  // The engine invokes CModelRender::DrawModelExecute directly as well as
  // through IVModelRender. Replacing only vtable slot 19 misses those direct
  // calls. Detour the function entry, matching the proven l4d2-rtx strategy.
  g_original=reinterpret_cast<DrawModelExecute>(target);
  LONG error=DetourTransactionBegin();
  if(error==NO_ERROR) { error=DetourUpdateThread(GetCurrentThread()); }
  if(error==NO_ERROR) { error=DetourAttach(reinterpret_cast<PVOID*>(&g_original),reinterpret_cast<PVOID>(&hooked)); }
  if(error!=NO_ERROR) { DetourTransactionAbort();g_original=nullptr;g_status.store(Status::Failed,std::memory_order_release);return Status::Failed; }
  error=DetourTransactionCommit();
  const auto result=error==NO_ERROR?Status::Installed:Status::Failed;
  if(result==Status::Installed) { g_target.store(reinterpret_cast<uintptr_t>(target),std::memory_order_release); }
  else { g_original=nullptr; }
  g_status.store(result,std::memory_order_release);return result;
}

inline bool uninstall() noexcept {
  if(g_status.load(std::memory_order_acquire)!=Status::Installed||!g_original) { return true; }
  if(g_installing.test_and_set(std::memory_order_acquire)) { return false; }
  struct Unlock { ~Unlock() { g_installing.clear(std::memory_order_release); } } unlock;
  LONG error=DetourTransactionBegin();
  if(error==NO_ERROR) { error=DetourUpdateThread(GetCurrentThread()); }
  if(error==NO_ERROR) { error=DetourDetach(reinterpret_cast<PVOID*>(&g_original),reinterpret_cast<PVOID>(&hooked)); }
  if(error!=NO_ERROR) { DetourTransactionAbort();return false; }
  error=DetourTransactionCommit();
  if(error==NO_ERROR) { g_original=nullptr;g_target.store(0,std::memory_order_release);g_status.store(Status::Unavailable,std::memory_order_release);return true; }
  return false;
}

#else

inline Status install() noexcept { return Status::Unavailable; }
inline bool uninstall() noexcept { return true; }
inline uint64_t calls() noexcept { return 0; }
inline uint64_t captured() noexcept { return 0; }
inline uint64_t rejected() noexcept { return 0; }
inline uintptr_t target() noexcept { return 0; }

#endif

} // namespace ldb_rt::model_hook

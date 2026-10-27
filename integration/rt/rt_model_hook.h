#pragma once

#include "rt_model_scope.h"

#include <atomic>

#if defined(LDB_RT_ENABLE_MODEL_HOOK) && defined(_WIN32) && (defined(_M_IX86) || defined(__i386__))
#include "rt_file_hash.h"
#include <windows.h>
#include <detours.h>
#endif

namespace ldb_rt::model_hook {

enum class Status : uint32_t { Unavailable, Installed, Failed };
enum class QueueStatus : uint32_t { Unavailable, ObservedQueued, Immediate, VersionMismatch, Failed };

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
inline std::atomic<QueueStatus> g_queueStatus{QueueStatus::Unavailable};
inline std::atomic_flag g_installing=ATOMIC_FLAG_INIT;
inline std::atomic_flag g_binding=ATOMIC_FLAG_INIT;
inline std::atomic<bool> g_captureActive{false};
inline std::atomic<uint64_t> g_calls{0},g_captured{0},g_rejected{0};
inline std::atomic<uint64_t> g_observedQueued{0},g_immediate{0},g_probeFailures{0};
inline std::atomic<uintptr_t> g_target{0},g_materialBase{0},g_materialSystem{0};
constexpr uint32_t kFirstCandidateSlot=140,kCandidateSlots=11,kCandidateCodeWords=2;
inline std::array<std::atomic<uintptr_t>,kCandidateSlots> g_candidateRva{};
inline std::array<std::array<std::atomic<uint64_t>,kCandidateCodeWords>,kCandidateSlots> g_candidateCode{};
inline std::atomic<uintptr_t> g_contextVtableRva{0};

template<class T> inline bool readProcess(const void* address,T& output) noexcept {
  SIZE_T bytes=0;return address&&ReadProcessMemory(GetCurrentProcess(),address,&output,sizeof(output),&bytes)&&bytes==sizeof(output);
}
inline bool executable(const void* address) noexcept {
  MEMORY_BASIC_INFORMATION page{};if(!address||!VirtualQuery(address,&page,sizeof(page))||page.State!=MEM_COMMIT||page.Type!=MEM_IMAGE) { return false; }
  const auto protect=page.Protect&0xff;return protect==PAGE_EXECUTE||protect==PAGE_EXECUTE_READ||protect==PAGE_EXECUTE_READWRITE||protect==PAGE_EXECUTE_WRITECOPY;
}

inline bool initializeMaterialBinding() noexcept {
  if(g_materialSystem.load(std::memory_order_acquire)) { return true; }
  const auto terminal=g_queueStatus.load(std::memory_order_acquire);
  if(terminal==QueueStatus::VersionMismatch||terminal==QueueStatus::Failed) { return false; }
  if(g_binding.test_and_set(std::memory_order_acquire)) { return false; }
  struct Unlock { ~Unlock() { g_binding.clear(std::memory_order_release); } } unlock;
  if(g_materialSystem.load(std::memory_order_relaxed)) { return true; }
  const auto module=GetModuleHandleW(L"materialsystem.dll");
  wchar_t path[32768]{};
  if(!module||!GetModuleFileNameW(module,path,32768)) { return false; }
  if(ldb_rt_file::fileSha256(path)!="09fd324cd977d5d4e2a216ee831c07ff8b882efd024137c27de4155fbdfcf0ba") {
    g_queueStatus.store(QueueStatus::VersionMismatch,std::memory_order_release);return false;
  }
  using CreateInterface=void*(__cdecl*)(const char*,int*);
  CreateInterface create=nullptr;const auto symbol=GetProcAddress(module,"CreateInterface");
  static_assert(sizeof(create)==sizeof(symbol),"Windows function pointer ABI");std::memcpy(&create,&symbol,sizeof(create));
  void* object=create?create("VMaterialSystem080",nullptr):nullptr;void** table=nullptr;void* method=nullptr;
  const auto base=reinterpret_cast<uintptr_t>(module);
  if(!object||!readProcess(object,table)||reinterpret_cast<uintptr_t>(table)!=base+641060||
     !readProcess(table+95,method)||reinterpret_cast<uintptr_t>(method)!=base+100368||!executable(method)) {
    g_queueStatus.store(QueueStatus::VersionMismatch,std::memory_order_release);return false;
  }
  g_materialBase.store(base,std::memory_order_relaxed);g_materialSystem.store(reinterpret_cast<uintptr_t>(object),std::memory_order_release);return true;
}

struct Context {
  void* object=nullptr;
  bool queued=false;
  bool immediate=false;
};
inline void release(Context& value) noexcept {
  if(!value.object) { return; }
  void** table=nullptr;void* method=nullptr;
  if(readProcess(value.object,table)&&readProcess(table+1,method)&&executable(method)) {
    using Release=int(__thiscall*)(void*);reinterpret_cast<Release>(method)(value.object);
  }
  value={};
}
inline uint64_t codeWord(const void* address,size_t offset) noexcept {
  uint64_t value=0;SIZE_T bytes=0;
  return ReadProcessMemory(GetCurrentProcess(),static_cast<const uint8_t*>(address)+offset,&value,sizeof(value),&bytes)&&bytes==sizeof(value)?value:0;
}
inline Context context() noexcept {
  Context out;if(!initializeMaterialBinding()) { return out; }
  const auto base=g_materialBase.load(std::memory_order_relaxed);auto* material=reinterpret_cast<void*>(g_materialSystem.load(std::memory_order_acquire));
  void** materialTable=nullptr;void* method=nullptr;
  if(!readProcess(material,materialTable)||!readProcess(materialTable+95,method)||reinterpret_cast<uintptr_t>(method)!=base+100368) {
    g_queueStatus.store(QueueStatus::Failed,std::memory_order_release);return out;
  }
  using GetRenderContext=void*(__thiscall*)(void*);out.object=reinterpret_cast<GetRenderContext>(method)(material);
  void** table=nullptr;
  if(!out.object) { g_queueStatus.store(QueueStatus::Failed,std::memory_order_release);return out; }
  if(!readProcess(out.object,table)) { g_queueStatus.store(QueueStatus::Failed,std::memory_order_release);release(out);return {}; }
  const auto vtable=reinterpret_cast<uintptr_t>(table);
  if(vtable==base+638876) {
    g_contextVtableRva.store(vtable-base,std::memory_order_relaxed);
    for(uint32_t index=0;index<kCandidateSlots;++index) {
      void* candidate=nullptr;
      if(!readProcess(table+kFirstCandidateSlot+index,candidate)||!executable(candidate)) { continue; }
      const auto address=reinterpret_cast<uintptr_t>(candidate);
      g_candidateRva[index].store(address>=base?address-base:address,std::memory_order_relaxed);
      for(uint32_t word=0;word<kCandidateCodeWords;++word)g_candidateCode[index][word].store(codeWord(candidate,word*sizeof(uint64_t)),std::memory_order_relaxed);
    }
    // v49 called slot 142 as GetCallQueue. The returned object's slot zero
    // entered engine.dll+0xe0940 and crashed reading 0xc. Until the game ABI is
    // proven from these bytes, this path is deliberately observation-only.
    out.queued=true;g_queueStatus.store(QueueStatus::ObservedQueued,std::memory_order_release);return out;
  }
  if(vtable==base+650092) {
    if(!readProcess(table+142,method)||reinterpret_cast<uintptr_t>(method)!=base+141712) {
      g_queueStatus.store(QueueStatus::VersionMismatch,std::memory_order_release);release(out);return {};
    }
    out.immediate=true;g_queueStatus.store(QueueStatus::Immediate,std::memory_order_release);return out;
  }
  g_queueStatus.store(QueueStatus::VersionMismatch,std::memory_order_release);release(out);return {};
}

inline bool capture(const DrawModelState& stateRef,const ModelRenderInfo& infoRef,model_scope::Matrix3x4* customBones,model_scope::Captured& output) noexcept {
  DrawModelState state{};ModelRenderInfo info{};StudioHeaderPrefix studio{};ModelPrefix model{};
  std::array<model_scope::Matrix3x4,model_resource::kMaxBones> bones{};
  bool valid=readProcess(&stateRef,state)&&readProcess(&infoRef,info)&&readProcess(reinterpret_cast<const void*>(uintptr_t(state.studioHeader)),studio)&&
    readProcess(reinterpret_cast<const void*>(uintptr_t(info.model)),model)&&studio.checksum!=0&&studio.numbones>0&&studio.numbones<=int32_t(bones.size())&&
    info.renderable&&info.entity>=0&&customBones;
  if(valid) {
    SIZE_T bytes=0;const auto wanted=size_t(studio.numbones)*sizeof(bones[0]);
    valid=ReadProcessMemory(GetCurrentProcess(),customBones,bones.data(),wanted,&bytes)&&bytes==wanted;
  }
  if(!valid) { return false; }
  model_scope::Input input;input.path=model.path;input.checksum=uint32_t(studio.checksum);input.renderable=info.renderable;input.entity=info.entity;
  input.skin=info.skin;input.body=info.body;input.lod=state.lod;input.handle=info.instance;input.bones=bones.data();input.boneCount=uint32_t(studio.numbones);
  return model_scope::capture(input,output);
}

inline void __fastcall hooked(void* self,void* edx,const DrawModelState& stateRef,const ModelRenderInfo& infoRef,model_scope::Matrix3x4* customBones) noexcept {
  if(!g_captureActive.load(std::memory_order_acquire)) { g_original(self,edx,stateRef,infoRef,customBones);return; }
  g_calls.fetch_add(1,std::memory_order_relaxed);model_scope::Captured captured;
  if(!capture(stateRef,infoRef,customBones,captured)) { g_rejected.fetch_add(1,std::memory_order_relaxed);g_original(self,edx,stateRef,infoRef,customBones);return; }
  if(g_queueStatus.load(std::memory_order_acquire)==QueueStatus::ObservedQueued) {
    g_observedQueued.fetch_add(1,std::memory_order_relaxed);g_captured.fetch_add(1,std::memory_order_relaxed);
    g_original(self,edx,stateRef,infoRef,customBones);return;
  }
  auto current=context();
  if(current.queued) {
    g_observedQueued.fetch_add(1,std::memory_order_relaxed);g_captured.fetch_add(1,std::memory_order_relaxed);
    release(current);g_original(self,edx,stateRef,infoRef,customBones);return;
  } else if(current.immediate) {
    model_scope::Scope scope(captured);
    if(scope.valid()) { g_immediate.fetch_add(1,std::memory_order_relaxed);g_captured.fetch_add(1,std::memory_order_relaxed);g_original(self,edx,stateRef,infoRef,customBones);release(current);return; }
  } else { g_probeFailures.fetch_add(1,std::memory_order_relaxed); }
  release(current);g_original(self,edx,stateRef,infoRef,customBones);
}

inline void setCaptureActive(bool active) noexcept {
  if(active) {
    g_calls.store(0);g_captured.store(0);g_rejected.store(0);g_observedQueued.store(0);g_immediate.store(0);g_probeFailures.store(0);
    g_queueStatus.store(QueueStatus::Unavailable,std::memory_order_release);
    g_contextVtableRva.store(0);for(auto& rva:g_candidateRva)rva.store(0);for(auto& row:g_candidateCode)for(auto& word:row)word.store(0);
    model_scope::resetSurfacesTaken();
  }
  g_captureActive.store(active,std::memory_order_release);
}
inline uint64_t calls() noexcept { return g_calls.load(std::memory_order_relaxed); }
inline uint64_t captured() noexcept { return g_captured.load(std::memory_order_relaxed); }
inline uint64_t rejected() noexcept { return g_rejected.load(std::memory_order_relaxed); }
inline uint64_t queued() noexcept { return g_observedQueued.load(std::memory_order_relaxed); }
inline uint64_t immediate() noexcept { return g_immediate.load(std::memory_order_relaxed); }
inline uint64_t queueBegun() noexcept { return 0; }
inline uint64_t queueEnded() noexcept { return 0; }
inline uint64_t queueCancelled() noexcept { return 0; }
inline uint64_t queueFailures() noexcept { return g_probeFailures.load(std::memory_order_relaxed); }
inline uint64_t surfaces() noexcept { return model_scope::surfacesTaken(); }
inline QueueStatus queueStatus() noexcept { return g_queueStatus.load(std::memory_order_relaxed); }
inline uintptr_t target() noexcept { return g_target.load(std::memory_order_relaxed); }
inline uintptr_t contextVtableRva() noexcept { return g_contextVtableRva.load(std::memory_order_relaxed); }
inline uintptr_t candidateRva(uint32_t index) noexcept { return index<kCandidateSlots?g_candidateRva[index].load(std::memory_order_relaxed):0; }
inline uint64_t candidateCode(uint32_t index,uint32_t word) noexcept { return index<kCandidateSlots&&word<kCandidateCodeWords?g_candidateCode[index][word].load(std::memory_order_relaxed):0; }

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
  g_original=reinterpret_cast<DrawModelExecute>(target);
  LONG error=DetourTransactionBegin();
  if(error==NO_ERROR) { error=DetourUpdateThread(GetCurrentThread()); }
  if(error==NO_ERROR) { error=DetourAttach(&(PVOID&)g_original,hooked); }
  if(error!=NO_ERROR) { DetourTransactionAbort();g_original=nullptr;g_status.store(Status::Failed,std::memory_order_release);return Status::Failed; }
  error=DetourTransactionCommit();const auto result=error==NO_ERROR?Status::Installed:Status::Failed;
  if(result==Status::Installed) { g_target.store(reinterpret_cast<uintptr_t>(target),std::memory_order_release); }
  else { g_original=nullptr; }
  g_status.store(result,std::memory_order_release);return result;
}

inline bool uninstall() noexcept {
  setCaptureActive(false);
  if(g_status.load(std::memory_order_acquire)!=Status::Installed||!g_original) { return true; }
  if(g_installing.test_and_set(std::memory_order_acquire)) { return false; }
  struct Unlock { ~Unlock() { g_installing.clear(std::memory_order_release); } } unlock;
  LONG error=DetourTransactionBegin();
  if(error==NO_ERROR) { error=DetourUpdateThread(GetCurrentThread()); }
  if(error==NO_ERROR) { error=DetourDetach(&(PVOID&)g_original,hooked); }
  if(error!=NO_ERROR) { DetourTransactionAbort();return false; }
  error=DetourTransactionCommit();
  if(error==NO_ERROR) { g_original=nullptr;g_target.store(0,std::memory_order_release);g_status.store(Status::Unavailable,std::memory_order_release);return true; }
  return false;
}

#else

inline Status install() noexcept { return Status::Unavailable; }
inline bool uninstall() noexcept { return true; }
inline void setCaptureActive(bool) noexcept { }
inline uint64_t calls() noexcept { return 0; }
inline uint64_t captured() noexcept { return 0; }
inline uint64_t rejected() noexcept { return 0; }
inline uint64_t queued() noexcept { return 0; }
inline uint64_t immediate() noexcept { return 0; }
inline uint64_t queueBegun() noexcept { return 0; }
inline uint64_t queueEnded() noexcept { return 0; }
inline uint64_t queueCancelled() noexcept { return 0; }
inline uint64_t queueFailures() noexcept { return 0; }
inline uint64_t surfaces() noexcept { return 0; }
inline QueueStatus queueStatus() noexcept { return QueueStatus::Unavailable; }
inline uintptr_t target() noexcept { return 0; }
constexpr uint32_t kFirstCandidateSlot=140,kCandidateSlots=11,kCandidateCodeWords=2;
inline uintptr_t contextVtableRva() noexcept { return 0; }
inline uintptr_t candidateRva(uint32_t) noexcept { return 0; }
inline uint64_t candidateCode(uint32_t,uint32_t) noexcept { return 0; }

#endif

} // namespace ldb_rt::model_hook

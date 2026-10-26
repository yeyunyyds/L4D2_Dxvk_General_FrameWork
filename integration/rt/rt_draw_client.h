#pragma once
#include "rt_draw_packet.h"
#include "rt_draw_request.h"
#include "rt_file_hash.h"
#include "rt_model_hook.h"
#include <bcrypt.h>
#include <d3d9.h>
#include <mutex>
#include <set>

namespace ldb_rt::draw_observe {
struct LocalMemory {
  bool read(uint32_t address, void* output, size_t bytes) const noexcept {
    SIZE_T got = 0;
    return address && uint64_t(address) + bytes <= UINT32_MAX &&
      ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(uintptr_t(address)), output, bytes, &got) && got == bytes;
  }
  bool code(uint32_t address, void* output, size_t bytes) const noexcept {
    MEMORY_BASIC_INFORMATION page{};
    if (!VirtualQuery(reinterpret_cast<const void*>(uintptr_t(address)), &page, sizeof(page)) ||
        page.Type != MEM_IMAGE || page.State != MEM_COMMIT || (page.Protect & (PAGE_GUARD | PAGE_NOACCESS)) ||
        !(page.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) ||
        uint64_t(address) + bytes > reinterpret_cast<uintptr_t>(page.BaseAddress) + page.RegionSize) { return false; }
    return read(address, output, bytes);
  }
};
using ldb_rt_file::fileSha256;

// Data-only decoder for the exact shaderapivk.dll uploaded in the v6 run.
// It never calls GetBufferedState, GetBoundMaterial or any unverified method.
class Binding {
  LocalMemory memory;
  uint32_t base = 0;
  Status identity = Status::ModuleMissing;
public:
  Status verify() {
    const auto module = GetModuleHandleW(L"shaderapivk.dll");
    if (!module) { return Status::ModuleMissing; }
    if (base) { return identity; }
    base = uint32_t(reinterpret_cast<uintptr_t>(module));
    wchar_t path[32768]{};
    if (!GetModuleFileNameW(module, path, 32768) ||
        fileSha256(path) != "f3611dcb95625bb41dc38edd799607f85f18142bc23eab4e23162bcd50d72adb") {
      return identity = Status::VersionMismatch;
    }
    return identity = Status::Valid;
  }
  Status capture(Snapshot& s) {
    const auto checked = verify(); if (checked != Status::Valid) { return checked; }
    uint32_t api = 0, table = 0, buffered = 0, bound = 0;
    if (!memory.read(base + 0xc9c50, &api, 4) || !memory.read(api, &table, 4) ||
        table != base + 0x7666c || !memory.read(table + 250 * 4, &buffered, 4) ||
        !memory.read(table + 255 * 4, &bound, 4) || buffered != base + 0x194a0 || bound != base + 0x13fd0) {
      return Status::VersionMismatch;
    }
    const unsigned char bufferedPrefix[] = {0x55,0x8b,0xec,0x51,0x53,0x56,0x57,0x8b,0xc1,0x89,0x45,0xfc,0x8b,0x88,0x70,0x02,0,0,
      0x8b,0x5d,0x08,0x8b,0x71,0x1c,0xc1,0xe6,0x06,0x03,0x71,0x08,0xb9,0x10};
    unsigned char actual[sizeof(bufferedPrefix)]{};
    const unsigned char boundBody[] = {0x8b,0x81,0xd4,0x2b,0,0,0xc3};
    unsigned char actualBound[sizeof(boundBody)]{};
    if (!memory.read(buffered, actual, sizeof(actual)) || std::memcmp(actual, bufferedPrefix, sizeof(actual)) ||
        !memory.read(bound, actualBound, sizeof(actualBound)) || std::memcmp(actualBound, boundBody, sizeof(actualBound))) { return Status::VersionMismatch; }
    uint32_t before = 0, after = 0;
    if (!memory.read(api + 0x2bd4, &before, 4)) { return Status::ReadFailed; }
    auto status = matrices(memory, api, s);
    if (status == Status::Valid && before) { s.materialInfo = material_decode::decode(memory, before, [&] {
      uint32_t tableValue = 0; memory.read(before, &tableValue, 4); return tableValue;
    }()); }
    if (!memory.read(api + 0x2bd4, &after, 4)) { return Status::ReadFailed; }
    if (before != after) { return Status::ChangedDuringRead; }
    s.material = before;
    if (before && !memory.read(before, &s.materialTable, 4)) { return Status::ReadFailed; }
    return status;
  }
};

class Collector {
  std::mutex mutex;
  Binding binding;
  Request request;
  uint64_t ordinal = 0, pollAt = 0, startedAt = 0;
  bool down = false, active = false, finished = false;
  std::set<uint32_t> materials;
  std::ofstream materialLog;
  std::ofstream controlLog;
  unsigned controlEvents = 0;
  uint64_t heartbeatAt = 0, drawsSeen = 0;
  model_hook::Status modelHookStatus=model_hook::Status::Unavailable;
  bool modelHookAttempted=false;
  bool rawDown = false;
  void control(const char* name, uint64_t value = 0, uint64_t other = 0) {
    if (!request.session || controlEvents >= 512) { return; }
    if (!controlLog.is_open()) {
      controlLog.open(request.output / ("rt-draw-control-" + std::to_string(GetCurrentProcessId()) + "-" +
        std::to_string(reinterpret_cast<uintptr_t>(this)) + ".jsonl"), std::ios::binary);
    }
    if (controlLog) {
      controlLog << "{\"event\":\"" << name << "\",\"session\":" << request.session
        << ",\"pid\":" << GetCurrentProcessId() << ",\"tick_ms\":" << GetTickCount64()
        << ",\"value\":" << value << ",\"other\":" << other << "}\n" << std::flush;
      ++controlEvents;
    }
  }
  static std::string bytesHex(const void* data, size_t size) {
    static const char chars[] = "0123456789abcdef";
    std::string value; const auto* bytes = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; ++i) { value += chars[bytes[i] >> 4]; value += chars[bytes[i] & 15]; }
    return value;
  }
  void material(const Snapshot& s) {
    if (!s.material || materials.count(s.material) || materials.size() >= 1024) { return; }
    materials.insert(s.material);
    if (!materialLog.is_open()) {
      materialLog.open(request.output / ("rt-draw-materials-" + std::to_string(GetCurrentProcessId()) + "-" +
        std::to_string(reinterpret_cast<uintptr_t>(this)) + ".jsonl"), std::ios::binary);
    }
    if (!materialLog) { return; }
    LocalMemory memory; unsigned char object[512]{}; uint32_t table[52]{};
    const bool objectRead = memory.read(s.material, object, sizeof(object));
    const bool tableRead = memory.read(s.materialTable, table, sizeof(table));
    materialLog << "{\"ordinal\":" << s.ordinal << ",\"thread\":" << s.thread << ",\"material\":" << s.material
      << ",\"vtable\":" << s.materialTable << ",\"object\":\"" << (objectRead ? bytesHex(object, sizeof(object)) : "")
      << "\",\"methods\":[";
    bool comma = false;
    for (unsigned slot : {0u, 20u, 49u}) {
      unsigned char code[128]{};
      const bool read = tableRead && memory.read(table[slot], code, sizeof(code));
      if (comma) { materialLog << ','; } comma = true;
      materialLog << "{\"slot\":" << slot << ",\"address\":" << (tableRead ? table[slot] : 0)
        << ",\"code\":\"" << (read ? bytesHex(code, sizeof(code)) : "") << "\"}";
    }
    unsigned char shaderObject[128]{}, shaderCode[128]{};
    const auto& info = s.materialInfo;
    const bool shaderRead = info.shaderObject && memory.read(info.shaderObject, shaderObject, sizeof(shaderObject));
    const bool codeRead = info.shaderMethod && memory.code(info.shaderMethod, shaderCode, sizeof(shaderCode));
    materialLog << "],\"vertex_format\":" << info.format << ",\"decode_flags\":" << info.flags
      << ",\"shader_name\":\"" << info.shader.data() << "\",\"name_pointer_slot\":" << info.nameSlot
      << ",\"name_address\":" << info.nameAddress << ",\"shader_object\":\"" << (shaderRead ? bytesHex(shaderObject, sizeof(shaderObject)) : "")
      << "\",\"shader_method\":" << info.shaderMethod << ",\"shader_code\":\"" << (codeRead ? bytesHex(shaderCode, sizeof(shaderCode)) : "")
      << "\",\"method_calls_executed\":false}\n" << std::flush;
  }
public:
  template<class Device> Bytes snapshot(Device* device) noexcept {
    try {
      // Only this device's diagnostic bookkeeping is protected. No engine
      // producer queue is locked and no lock survives the Draw RPC construction.
      std::unique_lock<std::mutex> guard(mutex);
      const auto now = GetTickCount64();
      ++drawsSeen;
      if (now >= pollAt) {
        pollAt = now + 16;
        if (!active) {
          Request next;
          if (readRequest(next) && next.session != request.session) {
            request = std::move(next); ordinal = 0; materials.clear(); materialLog.close(); finished = false;
            controlLog.close(); controlEvents = 0; heartbeatAt = 0;modelHookAttempted=modelHookStatus==model_hook::Status::Installed;control("request_seen");
          }
          if(request.session&&request.performance&&!modelHookAttempted) {
            modelHookAttempted=true;
            modelHookStatus=model_hook::install();control("model_hook_status",uint64_t(modelHookStatus),uint64_t(model_hook::target()));
          }
          DWORD pid = 0; GetWindowThreadProcessId(GetForegroundWindow(), &pid);
          const bool raw = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
          const bool pressed = pid == GetCurrentProcessId() && raw;
          if (raw && !rawDown) { control("f9_key_down", pid, pressed ? 1 : 0); } rawDown = raw;
          if (now >= heartbeatAt) {
            control("waiting_for_f9", pid, drawsSeen);
            heartbeatAt = now + 5000;
          }
          if (pressed && !down && request.session && !finished) {
            active = true; startedAt = now; control("capture_started", pid, drawsSeen);
            control("model_hook_capture_start",model_hook::calls(),model_hook::captured());
          }
          down = pressed;
        }
        if (active && (now - startedAt > (request.world ? 420000u : 60000u) || (fs::exists(doneFile(request)) && (!request.world || !fs::exists(previewRunningFile(request)) || fs::exists(previewDoneFile(request)))))) {
          control("capture_stopped", ordinal);control("model_hook_capture_end",model_hook::calls(),model_hook::captured());
          control("model_hook_rejected",model_hook::rejected(),0);active = false; finished = true;
        }
      }
      if (!active) { return {}; }
      Snapshot s; s.session = request.session; s.ordinal = ++ordinal; s.thread = GetCurrentThreadId();
      LARGE_INTEGER ticks{}; QueryPerformanceCounter(&ticks); s.ticks = uint64_t(ticks.QuadPart);
      s.status = binding.capture(s);
      if (s.status == Status::Valid) { material(s); }
      // Avoid taking the device's public-API lock while holding this diagnostic
      // lock. The Host independently compares the viewport of the actual Draw.
      guard.unlock();
      D3DVIEWPORT9 vp{};
      if (SUCCEEDED(device->GetViewport(&vp))) {
        s.viewport = {vp.X, vp.Y, vp.Width, vp.Height}; s.minZ = vp.MinZ; s.maxZ = vp.MaxZ;
      } else { s.status = Status::ReadFailed; }
      const auto model=model_scope::Scope::takeSurface();
      if(model.identity) {
        s.modelScope=model.scope;s.modelResource=model.identity->resource;s.modelInstance=model.identity->instance;s.modelSurface=model.surface;
        s.modelBoneCount=model.boneCount;s.modelEntity=model.identity->entity;s.modelFlags=kModelPresent|(model.first?kModelFirstSurface:0);
        if(model.first) { s.modelBones.assign(model.bones,model.bones+model.boneCount); }
      }
      return pack(s);
    } catch (...) { return {}; } // Diagnostic failures never suppress a game Draw.
  }
};
} // namespace ldb_rt::draw_observe

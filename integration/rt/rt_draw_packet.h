#pragma once
#include "rt_protocol.h"
#include "rt_material_decode.h"
#include "rt_model_scope.h"

// Consumer observations deliberately have no Source frame/view classification.
// They travel INSIDE the physical Draw RPC, never in a preceding side channel.
namespace ldb_rt::draw_observe {
constexpr uint32_t kDrawMagic = 0x314f444c, kDrawVersion = 3;
constexpr size_t kSnapshotV1Bytes = 264, kSnapshotV2Bytes = 344, kSnapshotBytes = 384;
constexpr uint32_t kModelPresent=1u<<0,kModelFirstSurface=1u<<1,kModelFlags=kModelPresent|kModelFirstSurface;
enum class Status : uint32_t { Valid = 1, ModuleMissing, VersionMismatch, ReadFailed,
  StackInvalid, ChangedDuringRead, InvalidMatrices, NotSampled };
struct Snapshot {
  uint64_t session = 0, ordinal = 0, ticks = 0;
  uint32_t thread = 0;
  Status status = Status::NotSampled;
  std::array<float, 16> world{}, view{}, projection{};
  uint32_t material = 0, materialTable = 0;
  std::array<uint32_t, 4> viewport{};
  float minZ = 0, maxZ = 1;
  material_decode::Info materialInfo;
  uint64_t modelScope=0,modelResource=0,modelInstance=0;
  uint32_t modelSurface=0,modelBoneCount=0;
  int32_t modelEntity=-1;
  uint32_t modelFlags=0;
  std::vector<model_scope::Matrix3x4> modelBones;
};
inline Bytes pack(const Snapshot& s) {
  Writer w; w.u32(kDrawMagic); w.u32(kDrawVersion);
  w.u64(s.session); w.u64(s.ordinal); w.u64(s.ticks); w.u32(s.thread); w.u32(uint32_t(s.status));
  for (const auto* matrix : {&s.world, &s.view, &s.projection}) { for (float x : *matrix) { w.f32(x); } }
  w.u32(s.material); w.u32(s.materialTable);
  for (auto x : s.viewport) { w.u32(x); } w.f32(s.minZ); w.f32(s.maxZ);
  w.u64(s.materialInfo.format); w.u32(s.materialInfo.flags); w.u32(0);
  for (char c : s.materialInfo.shader) { w.data.push_back(uint8_t(c)); }
  w.u64(s.modelScope);w.u64(s.modelResource);w.u64(s.modelInstance);w.u32(s.modelSurface);w.u32(s.modelBoneCount);
  w.u32(uint32_t(s.modelEntity));w.u32(s.modelFlags);
  for(const auto& bone:s.modelBones) { for(const auto value:bone) { w.f32(value); } }
  return w.data;
}
inline bool unpack(const void* data, size_t size, Snapshot& s) noexcept {
  if (!data || (size != kSnapshotV1Bytes && size != kSnapshotV2Bytes && (size < kSnapshotBytes || size > kSnapshotBytes+model_resource::kMaxBones*sizeof(model_scope::Matrix3x4)))) { return false; }
  try {
    Reader r{static_cast<const uint8_t*>(data), size};
    if (r.u32() != kDrawMagic) { return false; }
    const auto version = r.u32();
    if (!((version == 1 && size == kSnapshotV1Bytes) || (version == 2 && size == kSnapshotV2Bytes) || (version == 3 && size >= kSnapshotBytes))) { return false; }
    Snapshot v; v.session = r.u64(); v.ordinal = r.u64(); v.ticks = r.u64(); v.thread = r.u32();
    const auto status = r.u32();
    if (!v.session || !v.ordinal || !v.thread || status < 1 || status > 8) { return false; }
    v.status = static_cast<Status>(status);
    for (auto* matrix : {&v.world, &v.view, &v.projection}) { for (float& x : *matrix) { x = r.f32(); } }
    v.material = r.u32(); v.materialTable = r.u32();
    for (auto& x : v.viewport) { x = r.u32(); } v.minZ = r.f32(); v.maxZ = r.f32();
    if (version >= 2) {
      v.materialInfo.format = r.u64(); v.materialInfo.flags = r.u32();
      if (r.u32() || v.materialInfo.flags & ~3u) { return false; }
      if (r.offset > r.size || r.size - r.offset < v.materialInfo.shader.size()) { return false; }
      std::memcpy(v.materialInfo.shader.data(), r.data + r.offset, v.materialInfo.shader.size()); r.offset += v.materialInfo.shader.size();
      if (v.materialInfo.shader.back() || ((v.materialInfo.flags & material_decode::ShaderRead) &&
          (!(v.materialInfo.flags & material_decode::FormatRead) || !v.materialInfo.shader[0]))) { return false; }
    }
    if(version==3) {
      v.modelScope=r.u64();v.modelResource=r.u64();v.modelInstance=r.u64();v.modelSurface=r.u32();v.modelBoneCount=r.u32();
      v.modelEntity=int32_t(r.u32());v.modelFlags=r.u32();
      if(v.modelFlags&~kModelFlags) { return false; }
      const bool present=v.modelFlags&kModelPresent,first=v.modelFlags&kModelFirstSurface;
      if(present!=bool(v.modelScope&&v.modelResource&&v.modelInstance&&v.modelBoneCount&&v.modelBoneCount<=model_resource::kMaxBones&&v.modelEntity>=0)||
         first!=(present&&v.modelSurface==0)) { return false; }
      const auto transmitted=first?v.modelBoneCount:0u;
      if(size!=kSnapshotBytes+size_t(transmitted)*sizeof(model_scope::Matrix3x4)) { return false; }
      v.modelBones.resize(transmitted);for(auto& bone:v.modelBones) { for(auto& value:bone) { value=r.f32();if(!std::isfinite(value)||std::abs(value)>1e8f) { return false; } } }
    }
    if (!r.end()) { return false; }
    if (v.status == Status::Valid && (!invertible(v.view) || !invertible(v.projection) ||
        !v.viewport[2] || !v.viewport[3] || v.viewport[2] > 32768 || v.viewport[3] > 32768 ||
        !std::isfinite(v.minZ) || !std::isfinite(v.maxZ) || v.minZ < 0 || v.maxZ > 1 || v.minZ > v.maxZ)) { return false; }
    if (v.status == Status::Valid) { for (float x : v.world) { if (!std::isfinite(x)) { return false; } } }
    s = v; return true;
  } catch (...) { return false; }
}

// The pinned game's GetBufferedState implementation copies these matrix
// stacks. Read its data on the Draw caller; do not execute private vtable slots.
// Reader::read must perform a bounded read and return false on invalid memory.
template<class Memory> Status matrices(Memory& memory, uint32_t api, Snapshot& s) {
  const uint32_t fields[] = {0x270, 0x248, 0x24c};
  std::array<float, 16>* outputs[] = {&s.world, &s.view, &s.projection};
  uint32_t stacks[3]{}, tops[3]{}, bases[3]{};
  auto read = [&](uint32_t pointer, uint32_t offset, void* out, size_t bytes) {
    return pointer && uint64_t(pointer) + offset + bytes <= UINT32_MAX && memory.read(pointer + offset, out, bytes);
  };
  for (unsigned i = 0; i < 3; ++i) {
    if (!read(api, fields[i], &stacks[i], 4) || !read(stacks[i], 0x1c, &tops[i], 4) ||
        !read(stacks[i], 8, &bases[i], 4)) { return Status::ReadFailed; }
    if (tops[i] > 1024 || !bases[i]) { return Status::StackInvalid; }
    if (!read(bases[i], tops[i] * 64, outputs[i]->data(), 64)) { return Status::ReadFailed; }
  }
  for (unsigned i = 0; i < 3; ++i) {
    uint32_t stack = 0, top = 0, base = 0;
    if (!read(api, fields[i], &stack, 4) || !read(stacks[i], 0x1c, &top, 4) ||
        !read(stacks[i], 8, &base, 4)) { return Status::ReadFailed; }
    if (stack != stacks[i] || top != tops[i] || base != bases[i]) { return Status::ChangedDuringRead; }
  }
  for (float x : s.world) { if (!std::isfinite(x)) { return Status::InvalidMatrices; } }
  return invertible(s.view) && invertible(s.projection) ? Status::Valid : Status::InvalidMatrices;
}
} // namespace ldb_rt::draw_observe

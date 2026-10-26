#pragma once

#include "rt_draw_packet.h"

#include <cmath>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace ldb_rt::model_assembly {

enum class Code : uint32_t { Ok, Ignored, Orphan, Identity, Palette, Order, Limit, Busy, Empty };

template<class Surface> struct Completed {
  uint64_t scope=0,resource=0,instance=0;
  int32_t entity=-1;
  std::vector<model_scope::Matrix3x4> palette;
  std::vector<std::pair<uint32_t,Surface>> surfaces;
};

template<class Surface> class Collector {
  Completed<Surface> m_active;
  std::optional<Completed<Surface>> m_completed;
  bool m_have=false;

  Code finishActive() {
    if(!m_have) { return Code::Ignored; }
    if(m_completed) { return Code::Busy; }
    if(m_active.surfaces.empty()) { m_active={};m_have=false;return Code::Empty; }
    m_completed.emplace(std::move(m_active));m_active={};m_have=false;return Code::Ok;
  }
  static bool finite(const std::vector<model_scope::Matrix3x4>& palette) {
    if(palette.empty()||palette.size()>model_resource::kMaxBones) { return false; }
    for(const auto& bone:palette) { for(const auto value:bone) { if(!std::isfinite(value)||std::abs(value)>1e8f) { return false; } } }
    return true;
  }
public:
  Code observe(const draw_observe::Snapshot& snapshot) {
    if(!(snapshot.modelFlags&draw_observe::kModelPresent)) { return Code::Ignored; }
    const bool first=(snapshot.modelFlags&draw_observe::kModelFirstSurface)!=0;
    if(first) {
      const auto finished=finishActive();if(finished==Code::Busy) { return finished; }
      if(!snapshot.modelScope||!snapshot.modelResource||!snapshot.modelInstance||snapshot.modelSurface||snapshot.modelEntity<0||
         snapshot.modelBoneCount!=snapshot.modelBones.size()||!finite(snapshot.modelBones)) { return Code::Palette; }
      m_active.scope=snapshot.modelScope;m_active.resource=snapshot.modelResource;m_active.instance=snapshot.modelInstance;
      m_active.entity=snapshot.modelEntity;m_active.palette=snapshot.modelBones;m_active.surfaces.reserve(16);m_have=true;return Code::Ok;
    }
    if(!m_have) { return Code::Orphan; }
    if(snapshot.modelScope!=m_active.scope||snapshot.modelResource!=m_active.resource||snapshot.modelInstance!=m_active.instance||
       snapshot.modelEntity!=m_active.entity||snapshot.modelBoneCount!=m_active.palette.size()||!snapshot.modelBones.empty()) { return Code::Identity; }
    return Code::Ok;
  }
  Code append(const draw_observe::Snapshot& snapshot,Surface surface) {
    if(!m_have||snapshot.modelScope!=m_active.scope||snapshot.modelResource!=m_active.resource||snapshot.modelInstance!=m_active.instance) { return Code::Orphan; }
    if(m_active.surfaces.size()>=model_resource::kMaxSurfaces) { return Code::Limit; }
    if(!m_active.surfaces.empty()&&snapshot.modelSurface<=m_active.surfaces.back().first) { return Code::Order; }
    m_active.surfaces.emplace_back(snapshot.modelSurface,std::move(surface));return Code::Ok;
  }
  Code finishFrame() { return finishActive(); }
  std::optional<Completed<Surface>> take() {
    if(!m_completed) { return {}; }auto value=std::move(m_completed);m_completed.reset();return value;
  }
  bool active() const noexcept { return m_have; }
  size_t surfaceCount() const noexcept { return m_have?m_active.surfaces.size():0; }
  void reset() { m_active={};m_completed.reset();m_have=false; }
};

} // namespace ldb_rt::model_assembly

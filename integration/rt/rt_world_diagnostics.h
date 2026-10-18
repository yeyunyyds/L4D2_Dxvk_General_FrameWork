#pragma once
#include <array>
#include <cstdint>
#include <cstddef>

namespace ldb_rt::world_diagnostics {
enum class Phase : uint32_t { Raster, BaselineSphere, StrongSphere, Distant };
struct Option { const char* name; const char* value; };
inline const char* name(Phase phase) noexcept {
  switch (phase) {
    case Phase::BaselineSphere: return "baseline_sphere";
    case Phase::StrongSphere: return "strong_sphere";
    case Phase::Distant: return "distant";
    default: return "raster";
  }
}
constexpr uint64_t kCoveragePresents=600;
inline bool readback(uint64_t frame) noexcept { return frame==30 || (frame>=120 && frame<=kCoveragePresents && frame%60==0); }
enum class ModelStage : uint32_t { Off, Ordinary, Neko, Combined, Recovery, NekoPreserve, NekoRecheck, Matte, Directional, Count };
constexpr size_t kModelStages=size_t(ModelStage::Count);
constexpr unsigned kModelSampleLimit=64;
inline ModelStage modelStage(uint64_t last) noexcept {
  return last<180?ModelStage::Off:last<240?ModelStage::Neko:last<300?ModelStage::NekoPreserve:
    last<360?ModelStage::NekoRecheck:last<420?ModelStage::Combined:last<480?ModelStage::Matte:
    last<540?ModelStage::Directional:ModelStage::Recovery;
}
inline const char* name(ModelStage stage) noexcept {
  switch(stage) {
    case ModelStage::Ordinary:return "ordinary";case ModelStage::Neko:return "neko";
    case ModelStage::Combined:return "combined";case ModelStage::Recovery:return "recovery";
    case ModelStage::NekoPreserve:return "neko_preserve";case ModelStage::NekoRecheck:return "neko_recheck";
    case ModelStage::Matte:return "combined_matte";case ModelStage::Directional:return "combined_directional";
    default:return "off";
  }
}
inline bool includeModel(ModelStage stage,bool neko) noexcept {
  return stage==ModelStage::Combined || stage==ModelStage::Matte || stage==ModelStage::Directional ||
    (neko?(stage==ModelStage::Neko || stage==ModelStage::NekoPreserve || stage==ModelStage::NekoRecheck):stage==ModelStage::Ordinary);
}
inline bool preserve(ModelStage stage) noexcept { return stage==ModelStage::NekoPreserve; }
inline bool matte(ModelStage stage) noexcept { return stage==ModelStage::Matte; }
inline bool directional(ModelStage stage) noexcept { return stage==ModelStage::Directional; }
inline uint64_t modelPayloadLimit(ModelStage stage) noexcept {
  return uint64_t(stage==ModelStage::Combined?16:
    stage==ModelStage::Neko || stage==ModelStage::NekoPreserve || stage==ModelStage::NekoRecheck?12:0)*1024*1024;
}
enum class CoveragePhase : uint32_t { Raster, BaselineWorld, ExpandedWorld, ExpandedModels };
inline const char* name(CoveragePhase phase) noexcept {
  return phase == CoveragePhase::Raster ? "raster" : phase == CoveragePhase::BaselineWorld ? "baseline_world" :
    phase == CoveragePhase::ExpandedWorld ? "expanded_sky" : "expanded_models";
}
class CoverageComparison {
  uint64_t m_last = 0;
public:
  ModelStage models() const noexcept { return modelStage(m_last); }
  CoveragePhase phase() const noexcept { return m_last < 30 ? CoveragePhase::Raster : m_last < 120 ? CoveragePhase::BaselineWorld :
    m_last < 180 ? CoveragePhase::ExpandedWorld : CoveragePhase::ExpandedModels; }
  bool afterCapture(uint64_t frame) noexcept {
    const uint64_t expected = m_last==0?30:m_last==30?120:m_last+60;
    if (m_last==kCoveragePresents || frame!=expected) { return false; }
    m_last = frame; return true;
  }
  template<class Setter> bool afterCapture(uint64_t frame,Setter&& set) {
    auto next=*this;
    if (!next.afterCapture(frame)) { return false; }
    if (frame<kCoveragePresents) {
      const auto stage=next.models();
      const Option options[]={
        {"rtx.enablePreservePath",preserve(stage)?"True":"False"},
        {"rtx.fallbackLightType",directional(stage)?"0":"1"},
        {"rtx.fallbackLightRadiance",directional(stage)?"3,3,3":"3000,3000,3000"}
      };
      // A queued API request is not proof of effective GPU state. On partial
      // failure, retain the old label; the caller stops RT and collection.
      for (const auto& option:options) { if (!set(option.name,option.value)) { return false; } }
    }
    *this=next;return true;
  }

};
// Apply AFTER the old phase's readback. Remix defers changes to a frame
// boundary, leaving 59+ subsequent frames before the next diagnostic readback.
class Comparison {
  Phase m_phase = Phase::Raster;
  uint64_t m_last = 0;
  bool m_failed = false;
public:
  Phase phase() const noexcept { return m_phase; }
  bool failed() const noexcept { return m_failed; }
  template<class Setter> bool afterCapture(uint64_t frame, Setter&& set) {
    const uint64_t expected = m_last == 0 ? 30 : m_last == 30 ? 120 : m_last == 120 ? 180 : 240;
    if (m_failed || frame != expected || m_last == 240) { m_failed = true; return false; }
    if (frame == 240) { m_last = frame; return true; }
    const auto next = frame == 30 ? Phase::BaselineSphere : frame == 120 ? Phase::StrongSphere : Phase::Distant;
    const std::array<Option, 7> options{{
      {"rtx.fallbackLightMode", "2"},
      {"rtx.fallbackLightType", next == Phase::Distant ? "0" : "1"},
      {"rtx.fallbackLightRadiance", next == Phase::Distant ? "3,3,3" : next == Phase::StrongSphere ? "3000,3000,3000" : "30,30,30"},
      {"rtx.fallbackLightRadius", "5"}, {"rtx.fallbackLightPositionOffset", "0,0,0"},
      {"rtx.fallbackLightDirection", "0,0,-1"}, {"rtx.enableFallbackLightShaping", "False"}
    }};
    for (const auto& option : options) {
      if (!set(option.name, option.value)) { m_failed = true; return false; }
    }
    m_phase = next; m_last = frame; return true;
  }
};
} // namespace ldb_rt::world_diagnostics

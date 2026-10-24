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
// Persistent GPU skinning is safe at 16 submissions in the validated low-density
// scene. A later gameplay beta lost the B580 device while total model traffic was
// roughly 629 submissions per frame even though this limit was respected. Use the
// preceding complete frame as a density gate. High-density scenes keep a small
// GPU baseline instead of disabling the path. Zero means that no representative
// frame exists yet, so the stable CPU pose path remains active for the warm-up.
constexpr unsigned kGpuSkinSubmissionsPerFrame=16;
constexpr unsigned kGpuSkinModelDensityLimit=256;
constexpr unsigned kGpuSkinHighDensitySubmissionsPerFrame=4;
// v38 retains the v37 Arc lifecycle bounds, while native vertex capture keeps
// original programmable draws visible whenever no explicit replacement was
// submitted. Move from native-only fallback to the bounded 8- and 16-mesh
// explicit loads, then turn RT off, retire every API mesh/fence and prove that
// ordinary Present survives.
constexpr unsigned kGpuSkinPerformanceStaticFrames=30;
constexpr unsigned kGpuSkinPerformanceEightFrames=60;
constexpr unsigned kGpuSkinPerformanceSixteenFrames=120;
constexpr unsigned kGpuSkinPerformanceRtFrames=kGpuSkinPerformanceStaticFrames+
  kGpuSkinPerformanceEightFrames+kGpuSkinPerformanceSixteenFrames;
constexpr unsigned kGpuSkinPerformanceRasterFrames=60;
constexpr unsigned kGpuSkinPerformanceFrames=kGpuSkinPerformanceRtFrames+kGpuSkinPerformanceRasterFrames;
constexpr unsigned kGpuSkinPerformanceLimit=16;
inline int performanceGpuSkinPhase(uint64_t completedFrames) noexcept {
  return completedFrames<kGpuSkinPerformanceStaticFrames?0:
    completedFrames<kGpuSkinPerformanceStaticFrames+kGpuSkinPerformanceEightFrames?1:
    completedFrames<kGpuSkinPerformanceRtFrames?2:3;
}
inline unsigned performanceGpuSkinLimit(uint64_t completedFrames) noexcept {
  return completedFrames<kGpuSkinPerformanceStaticFrames?0:
    completedFrames<kGpuSkinPerformanceStaticFrames+kGpuSkinPerformanceEightFrames?8:
    completedFrames<kGpuSkinPerformanceRtFrames?16:0;
}
inline bool performanceGpuSkinReport(uint64_t completedFrames) noexcept {
  return completedFrames==kGpuSkinPerformanceStaticFrames ||
    completedFrames==kGpuSkinPerformanceStaticFrames+kGpuSkinPerformanceEightFrames ||
    completedFrames==kGpuSkinPerformanceRtFrames;
}
inline unsigned persistentGpuSkinLimit(unsigned previousModelDraws) noexcept {
  return previousModelDraws==0?0:previousModelDraws<=kGpuSkinModelDensityLimit?
    kGpuSkinSubmissionsPerFrame:kGpuSkinHighDensitySubmissionsPerFrame;
}
inline bool allowPersistentGpuSkin(unsigned previousModelDraws,unsigned currentGpuSkinDraws) noexcept {
  return currentGpuSkinDraws<persistentGpuSkinLimit(previousModelDraws);
}
enum class ModelStage : uint32_t { Off, Ordinary, Neko, Combined, Recovery, NekoPreserve, NekoRecheck, Matte, Directional, Preview, Count };
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
    case ModelStage::Combined:return "combined";case ModelStage::Preview:return "api_continuous_preview";
    case ModelStage::Recovery:return "recovery";
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

// Keep the old comparison available for archived traces and regression checks.
// v16 uses the same bounded 600-Present collection with live API material stages.
inline const char* liveName(ModelStage stage) noexcept {
  switch(stage) {
    case ModelStage::Neko:return "api_neko_neutral";
    case ModelStage::NekoPreserve:return "api_combined_neutral";
    case ModelStage::NekoRecheck:return "api_combined_rough";
    case ModelStage::Combined:return "api_combined_metal";
    case ModelStage::Matte:return "api_combined_alpha";
    case ModelStage::Directional:return "api_combined_emissive";
    case ModelStage::Preview:return "api_continuous_preview";
    case ModelStage::Recovery:return "recovery";
    default:return "off";
  }
}
inline const char* texturedName(ModelStage stage) noexcept {
  switch(stage) {
    case ModelStage::Neko:return "api_neko_textured";
    case ModelStage::NekoPreserve:return "api_combined_textured";
    case ModelStage::NekoRecheck:return "api_combined_textured_low_light";
    case ModelStage::Combined:return "api_combined_textured_metal";
    case ModelStage::Matte:return "api_combined_textured_alpha";
    case ModelStage::Directional:return "api_combined_textured_emissive";
    case ModelStage::Preview:return "api_continuous_preview";
    case ModelStage::Recovery:return "recovery";
    default:return "off";
  }
}
inline bool liveInclude(ModelStage stage,bool neko) noexcept {
  return stage==ModelStage::Neko?neko:stage==ModelStage::Preview ||stage==ModelStage::NekoPreserve || stage==ModelStage::NekoRecheck ||
    stage==ModelStage::Combined || stage==ModelStage::Matte || stage==ModelStage::Directional;
}
class LiveCoverageComparison {
  CoverageComparison gate;
public:
  ModelStage models() const noexcept { return gate.models(); }
  CoveragePhase phase() const noexcept { return gate.phase(); }
  template<class Setter> bool afterCapture(uint64_t frame,Setter&& set) {
    auto next=gate;if(!next.afterCapture(frame)) { return false; }
    if(frame<kCoveragePresents) {
      const Option options[]={{"rtx.enablePreservePath","False"},{"rtx.useBuffersDirectly","False"},
        {"rtx.bloom.enable","False"},{"rtx.postfx.enable","False"}};
      for(const auto& option:options) { if(!set(option.name,option.value)) { return false; } }
    }
    gate=next;return true;
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

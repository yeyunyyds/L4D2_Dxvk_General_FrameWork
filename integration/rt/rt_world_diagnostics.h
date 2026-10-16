#pragma once
#include <array>
#include <cstdint>

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
inline bool readback(uint64_t frame) noexcept { return frame == 30 || frame == 120 || frame == 180 || frame == 240; }
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

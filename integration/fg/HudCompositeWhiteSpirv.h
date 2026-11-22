// Debug-only variant of HudCompositeSpirv: write solid white to protected pixels.
// The original shader's neighbourhood coverage test and image bindings are unchanged.
// This is a deterministic SPIR-V transformation rather than a second shader dependency.
// Fail compilation if the pinned baseline shader bytecode changes: regenerate/review it.
#pragma once
#include "HudCompositeSpirv.h"
#include <array>
#include <cstdint>

namespace asteria_fg_runtime
{
static_assert(HudCompositeSpirvSize == 508u * sizeof(uint32_t), "HUD shader changed");
static_assert(HudCompositeSpirv[0] == 0x07230203u, "Not SPIR-V");
static_assert(HudCompositeSpirv[3] == 0x0000007au, "Unexpected SPIR-V ID bound");
static_assert(HudCompositeSpirv[192] == 0x00040017u &&
              HudCompositeSpirv[193] == 0x0000004cu &&
              HudCompositeSpirv[194] == 0x00000013u &&
              HudCompositeSpirv[195] == 4u, "Unexpected vec4 type");
static_assert(HudCompositeSpirv[203] == 0x0004002bu &&
              HudCompositeSpirv[204] == 0x00000013u &&
              HudCompositeSpirv[205] == 0x00000067u &&
              HudCompositeSpirv[206] == 0x3f800000u, "Unexpected 1.0 constant");
static_assert(HudCompositeSpirv[230] == 0x00050036u, "Unexpected function section");
static_assert(HudCompositeSpirv[494] == 0x00040063u &&
              HudCompositeSpirv[497] == 0x0000006bu, "Unexpected image-store input");

inline constexpr std::array<uint32_t, 515> HudCompositeWhiteSpirv = [] {
    std::array<uint32_t, 515> words{};
    // Add one vec4(1,1,1,1) constant in the types/constants section.
    // Original OpImageWrite texel operand then points to that constant.
    for (std::size_t i = 0; i < 230; ++i)
        words[i] = HudCompositeSpirv[i];
    words[3] = 0x0000007bu; // ID 0x7a is now allocated.
    words[230] = 0x0007002cu; // OpConstantComposite, 7 words
    words[231] = 0x0000004cu; // float vec4 type
    words[232] = 0x0000007au; // new constant result ID
    words[233] = words[234] = words[235] = words[236] = 0x00000067u; // 1.0
    for (std::size_t i = 230; i < 508; ++i)
        words[i + 7] = HudCompositeSpirv[i];
    words[494 + 7 + 3] = 0x0000007au; // OpImageWrite Texel
    return words;
}();
static_assert(HudCompositeWhiteSpirv[494 + 7] == 0x00040063u &&
              HudCompositeWhiteSpirv[494 + 7 + 3] == 0x0000007au,
              "Debug shader must store pure white");
} // namespace asteria_fg_runtime

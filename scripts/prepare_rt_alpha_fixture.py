"""Compile-test the fixed opaque alpha branch from checksum-pinned shader source.

Only the non-blended prefix is used. The shader's inout spelling is translated
to C++ references; the switch and early returns are kept as written upstream.
This is a CPU contract check, not a Slang compiler or GPU execution check.
"""
import argparse
import hashlib
import json
from pathlib import Path

PIN = 'b73eb23053aa50d3787b0c6c8b4b8ae49962880e'
SOURCES = {
    'src/dxvk/shaders/rtx/concept/surface/surface_shared.h':
        '878312d41d2333f89c78fa0d526f6400fff2f9bafb0cce3c25db5b94bff36fa3',
    'src/dxvk/shaders/rtx/concept/surface_material/opaque_surface_material_blending.slangh':
        'a47f74b754a4af1cef1b19ea80fdeb8bbc7dc676516158d687003083e8e5bdc9',
}


def prepare(source, destination):
    texts = []
    for name, expected in SOURCES.items():
        raw = (source / name).read_bytes()
        if hashlib.sha256(raw).hexdigest() != expected:
            raise ValueError('Pinned shader source differs: ' + name)
        texts.append(raw.decode('utf-8'))
    begin = texts[0].index('enum class AlphaTestType : uint8_t {')
    end = texts[0].index('\n};', begin) + len('\n};')
    enumeration = texts[0][begin:end]
    begin = texts[1].index('void calcOpaqueSurfaceMaterialOpacity(')
    end = texts[1].index('  // Handle Alpha Blending emulation', begin)
    prefix = texts[1][begin:end].replace('inout float opacity', 'float& opacity').replace(
        'inout float emissiveBlendOverrideInfluence', 'float& emissiveBlendOverrideInfluence')
    # Parameters only used by the omitted blending branch remain in the real
    # signature. Explicitly mark them used without suppressing compiler checks.
    opening = prefix.index('{') + 1
    prefix = (prefix[:opening] + '\n  (void) originalAlbedo; (void) emissiveBlendOverrideInfluence; '
              '(void) enableOpacityEmissiveBlendModes;\n' + prefix[opening:])
    destination.parent.mkdir(parents=True, exist_ok=True)
    # Preserve the upstream license notice alongside the extracted code.
    license_end = texts[1].index('*/') + 2
    header = texts[1][:license_end] + '''
#pragma once
#include <cstdint>
#include <stdexcept>
namespace pinned_alpha_fixture {
using uint = unsigned int;
struct vec3 { float x, y, z; };
''' + enumeration + '''
struct Surface {
  bool isFullyOpaque = false;
  bool isBlendingDisabled = true;
  float alphaTestReferenceValue = 0;
  AlphaTestType alphaTestType = AlphaTestType::kAlways;
};
''' + prefix + '''
  throw std::runtime_error("Blended branch is outside the fixed opaque fixture");
}
inline float evaluate(int alphaTestType, float originalAlpha = 1.f) {
  Surface surface;
  surface.alphaTestType = static_cast<AlphaTestType>(alphaTestType);
  float opacity = originalAlpha, emissiveInfluence = 1.f;
  calcOpaqueSurfaceMaterialOpacity({1,1,1}, originalAlpha, surface, opacity, emissiveInfluence);
  return opacity;
}
} // namespace pinned_alpha_fixture
'''
    destination.write_text(header, encoding='utf-8')
    receipt = {'schema': 1, 'runtime_commit': PIN, 'sources': SOURCES,
               'scope': 'CPU execution of pinned nonblended alpha-test prefix; GPU unrun',
               'translations': ['Slang inout float to C++ reference', 'mark unused blending-only parameters used'],
               'header_sha256': hashlib.sha256(destination.read_bytes()).hexdigest()}
    destination.with_suffix('.json').write_text(json.dumps(receipt, indent=2) + '\n')
    print('Pinned shader alpha prefix prepared; GPU execution unrun')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    prepare(args.source, args.output)

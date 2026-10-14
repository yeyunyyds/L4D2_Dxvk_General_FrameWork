"""Build official FSR FI/OF components with explicit host adaptations, FP32 SDR.

This component route has no SDK swapchain proxy. It does not enable game FG.
"""
import argparse
import concurrent.futures
import hashlib
import json
import re
from pathlib import Path
import struct
import subprocess

from prepare_research import ROOT, require_source
from verify_fsr_shaders import shader_jobs

PASSES = {
    "frameinterpolation": [
        "reconstruct_and_dilate", "setup", "reconstruct_previous_depth",
        "game_motion_vector_field", "optical_flow_vector_field", "disocclusion_mask",
        "", "compute_inpainting_pyramid", "inpainting",
        "compute_game_vector_field_inpainting_pyramid", "debug_view"],
    "opticalflow": ["prepare_luma", "compute_luminance_pyramid", "generate_scd_histogram",
                    "compute_scd_divergence", "compute_optical_flow_advanced_pass_v5",
                    "filter_optical_flow_pass_v5", "scale_optical_flow_advanced_pass_v5"],
}


def reflect(data):
    """Reflect the descriptor subset used by the pinned shaders; reject unknowns."""
    if len(data) < 20 or len(data) % 4:
        raise ValueError("Malformed SPIR-V")
    words = struct.unpack(f"<{len(data)//4}I", data)
    if words[0] != 0x07230203:
        raise ValueError("SPIR-V magic")
    names, decorations, types, variables = {}, {}, {}, []
    i = 5
    while i < len(words):
        count, op = words[i] >> 16, words[i] & 65535
        if not count or i + count > len(words):
            raise ValueError("SPIR-V instruction length")
        a = words[i+1:i+count]
        if op == 5:
            names[a[0]] = struct.pack(f"<{len(a)-1}I", *a[1:]).split(b"\0")[0].decode()
        elif op == 71:
            decorations.setdefault(a[0], {})[a[1]] = a[2:]
        elif op in (25, 26, 28, 29, 30, 32):
            types[a[0]] = (op, a[1:])
        elif op == 59:
            variables.append(a)
        i += count
    groups = [[] for _ in range(7)]  # CB, SRV image, UAV image, SRV/UAV buffer, sampler, AS
    for pointer, ident, storage, *_ in variables:
        d = decorations.get(ident, {})
        if 33 not in d:
            continue
        if d.get(34) != (0,):
            raise ValueError("Only descriptor set zero is supported")
        op, args = types[types[pointer][1][1]]
        name = names.get(ident, "")
        if storage == 2 and op == 30:
            name = names[types[pointer][1][1]].removesuffix("_t")
            group = 0
        elif storage == 0 and op == 25:
            group = {1: 1, 2: 2}.get(args[5])
        elif storage == 0 and op == 26:
            group = 5
        elif storage in (2, 12) and op == 30:
            group = 4 if name.startswith("rw_") else 3
        else:
            raise ValueError(f"Unsupported descriptor type {op}/{storage}")
        if group is None or not name:
            raise ValueError("Unrecognized shader resource")
        groups[group].append((d[33][0], name))
    for group in groups:
        group.sort()
    return groups


def build(args):
    source = args.source.resolve()
    lock = json.loads((ROOT / "research/sources.lock.json").read_text())
    require_source(source, lock["sources"]["fidelityfx"]["commit"])
    require_source(ROOT/'.research/sources/vulkan_headers', lock['sources']['vulkan_headers']['commit'])
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    # Linux wchar_t is 4 bytes; SDK private pipeline names exceed the public
    # Windows-sized opaque buffer. Increase only host storage in a generated
    # include overlay. Keep the SDK's size assertion and algorithm unchanged.
    header_path = Path('FidelityFX/host/ffx_frameinterpolation.h')
    original = (source/'sdk/include'/header_path).read_text()
    enlarged, count = re.subn(r'(#define FFX_FRAMEINTERPOLATION_CONTEXT_SIZE)\s+\(?FFX_SDK_DEFAULT_CONTEXT_SIZE\)?',
                             r'\1 (FFX_SDK_DEFAULT_CONTEXT_SIZE * 2)', original)
    if count != 1:
        raise RuntimeError('Unexpected SDK opaque context declaration')
    overlay = out/'include'/header_path
    overlay.parent.mkdir(parents=True, exist_ok=True)
    overlay.write_text(enlarged)
    # The pinned backend refuses all host-visible device-local heaps. Software
    # Vulkan (and some unified/BAR layouts) has no invisible heap. Preserve the
    # preference, then fall back to a compatible visible device-local heap.
    original_backend = (source/'sdk/src/backends/vk/ffx_vk.cpp').read_text()
    backend_text = original_backend
    old = '(memProperties.memoryTypes[i].propertyFlags & requestedProperties))'
    if backend_text.count(old) != 1:
        raise RuntimeError('Unexpected SDK memory flags predicate')
    backend_text = backend_text.replace(old,
        '((memProperties.memoryTypes[i].propertyFlags & requestedProperties) == requestedProperties))')
    old = '    uint32_t bestCandidate = UINT32_MAX;'
    if backend_text.count(old) != 1:
        raise RuntimeError('Unexpected SDK memory selector')
    backend_text = backend_text.replace(old, old+'\n    uint32_t visibleFallback = UINT32_MAX;')
    old = '''            if (requestedProperties == VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT && (memProperties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
                continue;'''
    if backend_text.count(old) != 1:
        raise RuntimeError('Unexpected SDK visible heap exclusion')
    backend_text = backend_text.replace(old, '''            if (requestedProperties == VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT && (memProperties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
                visibleFallback = i;
                continue;
            }''')
    old = '\n    return bestCandidate;\n}'
    if backend_text.count(old) != 1:
        raise RuntimeError('Unexpected SDK memory selector return')
    backend_text = backend_text.replace(old, '''
    if (bestCandidate == UINT32_MAX && visibleFallback != UINT32_MAX) {
        outProperties = memProperties.memoryTypes[visibleFallback].propertyFlags;
        return visibleFallback;
    }
    return bestCandidate;
}''', 1)
    generated_backend = out/'ffx_vk_adapted.cpp'
    generated_backend.write_text(backend_text)
    gpu = source / "sdk/include/FidelityFX/gpu"
    glslang = ROOT / ".research/tools/root/usr/bin/glslangValidator"
    validator = glslang.with_name("spirv-val")
    records = []
    # Full-resolution, unjittered, conventional depth; SDR optical flow.
    for shader, options, variant in shader_jobs(source):
        if any(options.values()):
            continue
        effect = shader.parent.name
        stem = shader.stem.removeprefix(f"ffx_{effect}_")
        short = stem.removesuffix("_pass")
        if short == "pass":
            short = ""
        index = PASSES[effect].index(short)
        target = out / f"{effect}-{index}.spv"
        cmd = [str(glslang), "-V", "--target-env", "vulkan1.2", "-S", "comp", "-Os",
               "-DFFX_GLSL=1", "-DFFX_GPU=1", "-DFFX_HALF=0",
               f"-I{gpu}", f"-I{gpu/effect}"]
        if effect == "frameinterpolation":
            cmd += ["-DFFX_FRAMEINTERPOLATION_OPTION_UPSAMPLE_SAMPLERS_USE_DATA_HALF=0",
                    "-DFFX_FRAMEINTERPOLATION_OPTION_ACCUMULATE_SAMPLERS_USE_DATA_HALF=0",
                    "-DFFX_FRAMEINTERPOLATION_OPTION_REPROJECT_SAMPLERS_USE_DATA_HALF=1",
                    "-DFFX_FRAMEINTERPOLATION_OPTION_POSTPROCESSLOCKSTATUS_SAMPLERS_USE_DATA_HALF=0",
                    "-DFFX_FRAMEINTERPOLATION_OPTION_UPSAMPLE_USE_LANCZOS_TYPE=2"]
        cmd += [f"-D{k}={v}" for k, v in options.items()]
        cmd += [str(shader), "-o", str(target)]
        subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL)
        subprocess.run([str(validator), "--target-env", "vulkan1.2", str(target)], check=True)
        records.append((effect, index, target, reflect(target.read_bytes()), cmd))
    if len(records) != 18:
        raise RuntimeError("Expected all 18 official passes")
    text = ['#include "ffx_shader_blobs.h"', '#include <cstring>']
    for effect, index, target, groups, _ in records:
        key = f"{effect}_{index}"
        words = struct.unpack(f"<{target.stat().st_size//4}I", target.read_bytes())
        text.append(f"alignas(4) static const uint32_t {key}_data[] = {{" +
                    ','.join(hex(w) for w in words) + '};')
        fields = []
        for j, group in enumerate(groups):
            if not group:
                fields += ['nullptr']*4
                continue
            for suffix, ctype, values in [
                ('names', 'const char*', ','.join(json.dumps(n) for _, n in group)),
                ('bindings', 'uint32_t', ','.join(str(b) for b, _ in group)),
                ('counts', 'uint32_t', ','.join('1' for _ in group)),
                ('spaces', 'uint32_t', ','.join('0' for _ in group))]:
                name = f"{key}_{j}_{suffix}"
                qualifier = '' if suffix == 'names' else 'const '
                text.append(f"static {qualifier}{ctype} {name}[] = {{{values}}};")
                fields.append(name)
        text.append(f"static const FfxShaderBlob {key} = {{"
                    f"reinterpret_cast<const uint8_t*>({key}_data), sizeof({key}_data)," +
                    ','.join(str(len(g)) for g in groups) + ',' + ','.join(fields) + '};')
    text += ['extern "C" FfxErrorCode ffxGetPermutationBlobByIndex(FfxEffect effect, FfxPass pass,',
             ' FfxBindStage stage, uint32_t options, FfxShaderBlob* out) {',
             ' if (!out || stage != FFX_BIND_COMPUTE_SHADER_STAGE || options != 0) return FFX_ERROR_INVALID_ARGUMENT;',
             ' const FfxShaderBlob* blob = nullptr;']
    for effect, index, *_ in records:
        text.append(f" if (effect == FFX_EFFECT_{effect.upper()} && pass == {index}) blob = &{effect}_{index};")
    text += [' if (!blob) return FFX_ERROR_INVALID_ARGUMENT;',
             ' std::memcpy(out, blob, sizeof(*out)); return FFX_OK; }',
             'extern "C" FfxErrorCode ffxIsWave64(FfxEffect, uint32_t options, bool& wave) {',
             ' wave = false; return options == 0 ? FFX_OK : FFX_ERROR_INVALID_ARGUMENT; }']
    generated = out / 'shader_blobs.cpp'
    generated.write_text('\n'.join(text)+'\n')
    sources = [generated_backend,
               source/'sdk/src/components/frameinterpolation/ffx_frameinterpolation.cpp',
               source/'sdk/src/components/opticalflow/ffx_opticalflow.cpp',
               source/'sdk/src/shared/ffx_object_management.cpp',
               source/'sdk/src/shared/ffx_breadcrumbs_list.cpp',
               ROOT/'tools/fsr_sdk_support.cpp', generated,
               ROOT/'tools/fsr_vulkan_smoke.cpp']
    common = ['c++', '-std=c++17', '-DFFX_GCC', '-D_DEBUG', '-O1', '-g',
              '-include', str(ROOT/'tools/fsr_sdk_portability.h'),
              *[f'-I{p}' for p in [out/'include', source/'sdk/include', source/'sdk/src/shared',
                                  source/'sdk/src/backends/shared', ROOT/'.research/sources/vulkan_headers/include',
                                  ROOT/'tools', ROOT/'integration/ldb']]]
    def compile_one(src):
        obj = out/(src.stem+'.o')
        cmd = common+[str(src), '-c', '-o', str(obj)]
        result = subprocess.run(cmd, capture_output=True, text=True)
        (out/(src.stem+'.log')).write_text(result.stdout+result.stderr)
        if result.returncode:
            raise RuntimeError(f"Compilation failed: {src}; see {out/(src.stem+'.log')}")
        return obj
    with concurrent.futures.ThreadPoolExecutor(max_workers=2) as executor:
        objects = list(executor.map(compile_one, sources))
    exe = out/'fsr_vulkan_smoke'
    subprocess.run(['c++', *map(str, objects), '-Wl,-l:libvulkan.so.1', '-o', str(exe)], check=True)
    report = {'sdk_commit': lock['sources']['fidelityfx']['commit'],
              'host_adaptations': ['Linux opaque context capacity for 4-byte wchar_t',
                                   'Require all memory flags; prefer invisible local heap, fall back to visible local heap',
                                   'KHR memory requirements entry point resolves to Vulkan 1.1 core alias'],
              'generated_backend_sha256': hashlib.sha256(generated_backend.read_bytes()).hexdigest(),
              'profile': 'FP32 SDR full-resolution MV conventional-depth unjittered; no SDK swapchain proxy',
              'shaders': [{'effect':e, 'pass':i, 'sha256':hashlib.sha256(p.read_bytes()).hexdigest(),
                           'reflection':g, 'command':c} for e,i,p,g,c in records],
              'exe_sha256':hashlib.sha256(exe.read_bytes()).hexdigest()}
    report['project_source_hashes'] = {str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest()
                                     for p in [Path(__file__), ROOT/'tools/fsr_sdk_portability.h',
                                               ROOT/'tools/fsr_sdk_support.cpp', ROOT/'tools/fsr_component_backend.h',
                                               ROOT/'tools/fsr_vulkan_smoke.cpp', ROOT/'integration/ldb/fsr_camera.h']}
    (out/'build.json').write_text(json.dumps(report, indent=2)+'\n')
    print(exe)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=ROOT/'.research/sources/fidelityfx')
    parser.add_argument('--output', type=Path, default=ROOT/'.research/fsr-vulkan')
    build(parser.parse_args())

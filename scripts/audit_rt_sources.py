"""Record pinned RT source evidence without assuming the game's queue ABI."""

import hashlib
import json
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def audit():
    lock = json.loads((ROOT / 'integration/rt/baseline.json').read_text())
    evidence = []

    def read(cache, commit, path):
        data = subprocess.check_output(['git', '-C', str(ROOT / '.research/sources' / cache), 'show', commit + ':' + path])
        evidence.append({'commit': commit, 'path': path, 'sha256': hashlib.sha256(data).hexdigest()})
        return data.decode('utf-8-sig')

    compat = lock['compat_commit']; runtime = lock['runtime_commit']; upstream = lock['upstream_commit']
    main = read('rtx-compat', compat, 'src/components/modules/main_module.cpp')
    patterns = read('rtx-compat', compat, 'src/game/l4d2.cpp')
    structs = read('rtx-compat', compat, 'src/game/structs.hpp')
    readme = read('rtx-compat', compat, 'README.md')
    premake = read('rtx-compat', compat, 'premake5.lua')
    mod_api = read('rtx-compat', compat, 'deps/bridge_api/remix/remix_c.h')
    runtime_api = read('rtx-runtime', runtime, 'public/include/remix/remix_c.h')
    bridge_api = read('bridge-upstream', upstream, 'public/include/remix/remix_c.h')
    decoder = read('rtx-runtime', runtime, 'src/d3d9/d3d9_rtx_utils.cpp')
    read('rtx-runtime', runtime, 'src/dxvk/rtx_render/rtx_materials.h')
    api_impl = read('rtx-runtime', runtime, 'src/dxvk/rtx_render/rtx_remix_api.cpp')
    assert 'game/l4d2_rebase1' in readme
    assert 'cppdialect "C++20"' in premake and 'architecture "x86"' in premake

    def version(text):
        return '.'.join(re.search(r'#define REMIXAPI_VERSION_' + name + r'\s+(\d+)', text)[1]
                        for name in ['MAJOR', 'MINOR', 'PATCH'])

    def interface(text):
        body = re.search(r'typedef struct remixapi_Interface\s*\{(.*?)\}\s*remixapi_Interface', text, re.S)[1]
        return re.findall(r'PFN_(\w+)\s+(\w+)\s*;', body)

    registers = sorted({int(x) for x in re.findall(r'renderStates\[(\d+)\]', decoder)})
    critical = [42, 149, 150, 169, 177, 196, 197, 210, 211, 220]
    assert set(critical) <= set(registers)
    symbols = [
        'hk_addr__cviewrenderer_renderview', 'kh_addr__cmeshdx8_renderpass_pre_draw',
        'kh_addr__cmeshdx8_renderpass_post_draw', 'hk_addr__render_spritecard_new',
        'hk_addr__rope_mgr_draw_render_cache', 'hk_addr__on_map_load', 'hk_addr__on_host_disconnect'
    ]
    hooks = []
    for symbol in symbols:
        match = re.search(r'PATTERN_OFFSET_SIMPLE\((\w+),\s*' + symbol + r',\s*"([^"]+)",\s*(\d+),\s*(0x[0-9A-Fa-f]+)\)', patterns)
        if not match:
            raise RuntimeError('Missing reviewed hook reference: ' + symbol)
        hooks.append({'symbol': symbol, 'module': match[1], 'pattern': match[2], 'byte_offset': int(match[3]),
                      'reference_static_rva': match[4], 'game_validation': 'unrun',
                      'thread_role': 'unknown_requires_trace', 'installed': False})
    cvars = dict(re.findall(r'cvar_uncheat_and_set_int\("([^"]+)",\s*([01])\)', main))
    thread_settings = {name: int(cvars[name]) for name in
                       ['mat_queue_mode', 'r_threaded_particles', 'r_threadeddetailprops', 'r_queued_ropes', 'mat_softwareskin']}
    material_context = re.search(r'struct IMatRenderContext_vtbl\s*\{(.*?)\};', structs, re.S)[1]
    return {
        'schema': 1, 'scope': 'pinned_source_audit_not_hardware_or_game_execution',
        'bridge_commit': lock['bridge_commit'], 'previous_fg_bridge_unchanged': True,
        'runtime_source_candidate': {'repository': lock['runtime_repository'], 'commit': runtime,
                                     'branch_reference': lock['runtime_branch_reference'], 'selection': 'unvalidated_binary_tuple'},
        'custom_render_states_confirmed_in_decoder': critical,
        'api': {'mod': version(mod_api), 'runtime': version(runtime_api), 'bridge': version(bridge_api),
                'interface_function_order_equal': interface(mod_api) == interface(runtime_api) == interface(bridge_api),
                'function_count': len(interface(runtime_api)),
                'runtime_development_minor_version_check_observed': 'each minor change is breaking' in api_impl,
                'opaque_material_difference': 'Bridge 0.6.5 appends four DLSS-control fields; runtime uses 0.6.4 prefix. Bridge has an explicit 0.6.4 caller conversion; actual runtime initialization and material calls remain unrun.',
                'runtime_abi_execution': 'unrun'},
        'compat_build': {'arch': 'x86', 'standard': 'C++20', 'system': 'Windows/VS2022 project generation'},
        'upstream_forced_thread_and_skin_settings': thread_settings,
        'source_queue': {'GetCallQueue_present_in_reference_context_vtable': 'GetCallQueue' in material_context,
                         'l4d2_sdk_reference': 'research/rt-source-queue-sdk.json',
                         'known_context_methods': ['GetMatrix1', 'GetMatrix2'],
                         'GetCallQueue_slot_selected': None, 'queued_mesh_abi_verified': False,
                         'decision': 'Do not invent a slot or use static-RVA fallback. Establish a version-checked queue callback boundary before producing engine-labelled frames.'},
        'hook_references': hooks,
        'transport': {'metadata_channel': 'DeviceBridge', 'hello_opcode': '0x1e10', 'metadata_opcode': '0x1e11',
                      'packet_header_bytes': 104, 'version': 1,
                      'source_queue_order_proven_by_transport_only': False},
        'unrun': ['Actual Source queue callback', 'Game hook pattern uniqueness', 'Matched runtime binary initialization', 'B580 RT', 'Image quality', 'High-pressure 60 FPS'],
        'sources': evidence
    }


if __name__ == '__main__':
    report = audit()
    output = ROOT / 'research/rt-source-audit.json'
    output.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'output': str(output), 'hook_references': len(report['hook_references']),
                      'API': report['api'], 'source_queue': report['source_queue']}))

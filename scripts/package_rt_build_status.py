"""Publish native foundation evidence; this is not a playable RT package."""

import hashlib
import json
import os
import re
from pathlib import Path
import struct
import zipfile

ROOT = Path(__file__).resolve().parents[1]

if __name__ == '__main__':
    source = ROOT / '.research/rt-observer-source'
    output = ROOT / '.research/dist/rt-foundation-build-status.zip'
    output.parent.mkdir(parents=True, exist_ok=True)
    status = {'schema': 1, 'commit': os.environ['GITHUB_SHA'],
              'native_outcome': os.environ.get('RT_BUILD_OUTCOME', 'unknown'),
              'scope': 'Bridge + COM/protocol/Host + scoped static D3D9 diagnostic, not game hooks or GPU RT',
              'game_queue_execution': 'unrun', 'b580_rt': 'unrun', 'binaries': {},
              'sdk_queue_development_execution': 'unrun',
              'game_binding_probe_native_execution': 'unrun',
              'static_draw_development_execution': 'unrun',
              'consumer_draw_development_execution': 'unrun',
              'consumer_world_development_execution': 'unrun',
              'world_startup_gate_development_execution': 'unrun',
              'material_global_name_decoder_development_execution': 'unrun',
              'world_transport_backpressure_development_execution': 'unrun',
              'world_lighting_comparison_development_execution': 'unrun',
              'world_coverage_overlay_development_execution': 'unrun',
              'world_tangent_layout_development_execution': 'unrun',
              'world_scene_scope_development_execution': 'unrun',
              'world_sky_reconstruction_development_execution': 'unrun',
              'world_bulk_static_development_execution': 'unrun',
              'world_original_input_development_execution': 'unrun',
              'world_owned_geometry_development_execution': 'unrun',
              'system_d3d9_nullref_state': {'x86': 'unrun', 'x64': 'unrun'},
              'artifact_validation': 'unrun'}
    with zipfile.ZipFile(output, 'w', zipfile.ZIP_DEFLATED) as archive:
        for file in sorted((ROOT / 'ci-rt-logs').glob('*.log')):
            archive.write(file, 'logs/' + file.name)
        receipt = source / 'rt-source.json'
        identity = None
        if receipt.exists():
            archive.write(receipt, 'rt-source.json')
            identity = ('ldb-rt-observer+' + json.loads(receipt.read_text())['fingerprint'][:16]).encode()
        sdk_receipt = ROOT / '.research/rt-sdk-source/rt-sdk-source.json'
        if sdk_receipt.exists():
            archive.write(sdk_receipt, 'rt-sdk-source.json')
        queue_source = ROOT / 'integration/rt/rt_source_queue.h'
        if queue_source.exists():
            status['source_queue_sha256'] = hashlib.sha256(queue_source.read_bytes()).hexdigest()
        for arch, relative in [('x64', 'server/L4D2Bridge64.exe'), ('x86', 'client/d3d9.dll')]:
            file = source / f'bridge/_rtRelease_{arch}/src' / relative
            if file.exists():
                data = file.read_bytes()
                if status['native_outcome'] == 'success':
                    if len(data) < 64 or data[:2] != b'MZ' or not identity or identity not in data:
                        raise RuntimeError('Missing PE/compiled identity: ' + str(file))
                    offset = struct.unpack_from('<I', data, 60)[0]
                    expected_machine = 0x8664 if arch == 'x64' else 0x14c
                    if data[offset:offset+4] != b'PE\0\0' or struct.unpack_from('<H', data, offset+4)[0] != expected_machine:
                        raise RuntimeError('Wrong native architecture: ' + str(file))
                    commands = file.parents[2] / 'compile_commands.json'
                    actual = [x['command'] for x in json.loads(commands.read_text()) if x['file'].endswith('.cpp')]
                    if not actual or not all('/O2' in x and 'NDEBUG' in x and 'DEBUGOPT' not in x for x in actual):
                        raise RuntimeError('Release compiler commands not observed: ' + arch)
                    archive.write(commands, 'development/' + arch + '/compile_commands.json')
                destination = 'development/' + arch + '/' + file.name
                status['binaries'][destination] = hashlib.sha256(data).hexdigest()
                archive.write(file, destination)
            for file in sorted((source / f'checks-{arch}').rglob('*')):
                if file.is_file() and file.suffix in ('.exe', '.bin', '.asm', '.json'):
                    archive.write(file, 'checks/' + arch + '/' + str(file.relative_to(source / f'checks-{arch}')))
        if status['native_outcome'] == 'success':
            if len(status['binaries']) != 2:
                raise RuntimeError('Missing native client/Host binary')
            status['artifact_validation'] = 'pass'
        queue_log = ROOT / 'ci-rt-logs/native.log'
        if queue_log.exists():
            for outcome, bits in re.findall(r'RT static-world system D3D9 NULLREF: (PASS|UNAVAILABLE) \([^\r\n]*ptr_bits=(32|64)\)',
                                           queue_log.read_text(errors='replace')):
                status['system_d3d9_nullref_state']['x86' if bits == '32' else 'x64'] = outcome.lower()
        if (queue_log.exists() and queue_log.read_text(errors='replace').count('RT static-world scoped D3D9 methods: PASS') == 2
                and all((source / ('checks-' + arch) / 'static-draw.exe').exists() for arch in ('x86', 'x64'))):
            status['static_draw_development_execution'] = 'pass'
        if (queue_log.exists() and queue_log.read_text(errors='replace').count('RT Draw native request/CNG/Host observation: PASS') == 2
                and all((source / ('checks-' + arch) / 'draw-observation-review.json').exists() for arch in ('x86', 'x64'))):
            if all(json.loads((source / ('checks-' + arch) / 'draw-observation-review.json').read_text())['pass'] for arch in ('x86', 'x64')):
                status['consumer_draw_development_execution'] = 'pass'
        if queue_log.exists() and queue_log.read_text(errors='replace').count('RT consumer world control/filter/layout/restore: PASS') == 2:
            reviews=[source/('checks-'+arch)/'draw-observation-review.json' for arch in ('x86','x64')]
            if all(p.is_file() and any(t.get('world_normalization')=='pass' for t in
                   json.loads(p.read_text()).get('traces',{}).values()) for p in reviews):
                status['consumer_world_development_execution']='pass'
        if queue_log.exists() and queue_log.read_text(errors='replace').count('RT world startup gate/activation failure/raster restoration: PASS') == 2:
            status['world_startup_gate_development_execution']='pass'
        if queue_log.exists() and queue_log.read_text(errors='replace').count('RT bounded A1 shader-name pointer reread: PASS') == 2:
            status['material_global_name_decoder_development_execution']='pass'
        if (queue_log.exists() and queue_log.read_text(errors='replace').count(
                'RT full IPC normal-timeout/world-delayed-consumer/FIFO/peer-close: PASS') == 2
                and all((source/('checks-'+arch)/'transport.exe').is_file() for arch in ('x86','x64'))):
            status['world_transport_backpressure_development_execution']='pass'
        if (queue_log.exists() and queue_log.read_text(errors='replace').count(
                'RT world lighting comparison/order/partial-failure: PASS') == 2
                and all((source/('checks-'+arch)/'world-lighting.exe').is_file() for arch in ('x86','x64'))):
            status['world_lighting_comparison_development_execution']='pass'
        if (queue_log.exists() and queue_log.read_text(errors='replace').count(
                'RT rigid-unlit/overlay rollback/system NULLREF: PASS') == 2
                and all((source/('checks-'+arch)/'world-overlay.exe').is_file() for arch in ('x86','x64'))):
            status['world_coverage_overlay_development_execution']='pass'
        if (queue_log.exists() and queue_log.read_text(errors='replace').count(
                'RT tangent64/80 exact inputs/phase/rollback/system NULLREF: PASS') == 2
                and all((source/('checks-'+arch)/'world-overlay.exe').is_file() for arch in ('x86','x64'))):
            status['world_tangent_layout_development_execution']='pass'
        if (queue_log.exists() and queue_log.read_text(errors='replace').count(
                'RT scene sky/rigid scope/rollback/system NULLREF: PASS') == 2
                and all((source/('checks-'+arch)/'world-overlay.exe').is_file() for arch in ('x86','x64'))):
            status['world_scene_scope_development_execution']='pass'
        if (queue_log.exists() and queue_log.read_text(errors='replace').count(
                'RT original sky shader bounded replay/UV/unprojection/rejection: PASS')==2
                and queue_log.read_text(errors='replace').count(
                'RT sky original VS reconstruction/geometry bounds/binding rollback/system NULLREF: PASS')==2
                and all((source/('checks-'+arch)/'sky-shader.exe').is_file() for arch in ('x86','x64'))):
            status['world_sky_reconstruction_development_execution']='pass'
        if (queue_log.exists() and queue_log.read_text(errors='replace').count(
                'RT bulk static families/affine/cutout/base-layer/rollback/system NULLREF: PASS')==2
                and all((source/('checks-'+arch)/'world-overlay.exe').is_file() for arch in ('x86','x64'))):
            status['world_bulk_static_development_execution']='pass'
        if (queue_log.exists() and all(queue_log.read_text(errors='replace').count(marker)==2 for marker in (
                'RT audited model matrix/UV/bone0/range/rejection: PASS',
                'RT full sky interpolants/world-coordinate precision: PASS',
                'RT original model transform/rebuilt stream rollback/system NULLREF: PASS'))):
            status['world_original_input_development_execution']='pass'
        if (queue_log.exists() and all(queue_log.read_text(errors='replace').count(marker)==2 for marker in (
                'RT compact owned 16/32-bit indices/signed-base/offset correspondence: PASS',
                'RT owned VB/IB upload readback/binding/fault rollback/system NULLREF: PASS',
                'RT model stencil/clip/color/depth special-pass rejection/system NULLREF: PASS',
                'RT neutral model color/original texture alpha/owned IB/fault rollback/system NULLREF: PASS',
                'RT full geometry hash beyond shader cap/empty index: PASS',
                'RT model preserve OFF/ON/OFF/material/light/recovery/partial-failure: PASS'))):
            status['world_owned_geometry_development_execution']='pass'
        if (sdk_receipt.exists() and (source / 'checks-x86/source-queue.exe').exists() and queue_log.exists()
                and 'RT L4D2 SDK queue/COM/Host: PASS' in queue_log.read_text(errors='replace')):
            status['sdk_queue_development_execution'] = 'pass'
        probe = source / 'checks-x86/game-probe.json'
        if probe.exists():
            result = json.loads(probe.read_text())
            if result.get('result') == 'pass' and result.get('scope') == 'external_x64_reader_x86_fixture_not_game_binding':
                status['game_binding_probe_native_execution'] = 'pass'
        archive.writestr('status.json', json.dumps(status, indent=2) + '\n')
        archive.writestr('README.txt', 'Development build evidence only. No Source game hook or Remix RT runtime is included.\nDo not install this ZIP as combined test package A.\n')
    print(output)

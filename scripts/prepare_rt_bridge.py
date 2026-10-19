"""Prepare a separate pinned RT observer Bridge; never includes FG assets."""

import argparse
import hashlib
import io
import json
from pathlib import Path
import shutil
import subprocess
import tarfile

ROOT = Path(__file__).resolve().parents[1]


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def git(path, *arguments):
    return subprocess.check_output(['git', '-C', str(path), *arguments])


def source_object(path, repository, commit):
    """Read pinned git objects without checking out or overwriting local changes."""
    if not path.exists():
        path.mkdir(parents=True)
        subprocess.run(['git', 'init', '-q', str(path)], check=True)
        subprocess.run(['git', '-C', str(path), 'remote', 'add', 'origin', repository], check=True)
    top = Path(git(path, 'rev-parse', '--show-toplevel').decode().strip()).resolve()
    if top != path.resolve():
        raise RuntimeError(f'Not an independent source cache: {path}')
    found = subprocess.run(['git', '-C', str(path), 'cat-file', '-e', commit + '^{commit}'],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if found.returncode:
        subprocess.run(['git', '-C', str(path), 'fetch', '--depth=1', 'origin', commit], check=True)
    return path


def prepare(output, bridge, upstream, detours):
    if output.exists():
        raise RuntimeError('Preserve existing output; select a new --output directory')
    baseline = json.loads((ROOT / 'integration/rt/baseline.json').read_text())
    runtime = ROOT / '.research/sources/rtx-runtime'
    for path, prefix in [(bridge, 'bridge'), (upstream, 'upstream'), (detours, 'detours'), (runtime, 'runtime')]:
        source_object(path, baseline[prefix + '_repository'], baseline[prefix + '_commit'])
    patch = git(bridge, 'show', baseline['bridge_commit'] + ':patches/l4d2-bridge.patch')
    if hashlib.sha256(patch).hexdigest() != baseline['bridge_patch_sha256']:
        raise RuntimeError('RT Bridge baseline patch hash differs')
    archive = git(upstream, 'archive', baseline['upstream_commit'], 'bridge', 'public', 'LICENSE',
                  'src/util/util_per_game_config.h', 'src/util/util_crash_report.h', 'src/util/xxHash')
    output.mkdir(parents=True)
    with tarfile.open(fileobj=io.BytesIO(archive)) as tar:
        tar.extractall(output, filter='data')
    subprocess.run(['git', 'init', '-q', str(output)], check=True)
    base_patch = output / 'ldb-baseline.patch'
    base_patch.write_bytes(patch)
    hooks = ROOT / 'integration/rt/bridge-hooks.patch'
    consumer_hooks = ROOT / 'integration/rt/consumer-hooks.patch'
    world_hooks = ROOT / 'integration/rt/world-hooks.patch'
    transport_hooks = ROOT / 'integration/rt/transport-hooks.patch'
    for current in [base_patch, hooks, consumer_hooks, world_hooks, transport_hooks]:
        subprocess.run(['git', '-C', str(output), 'apply', '--check', str(current)], check=True)
        subprocess.run(['git', '-C', str(output), 'apply', str(current)], check=True)
    base_patch.unlink()
    inputs = {'bridge-hooks.patch': digest(hooks), 'consumer-hooks.patch': digest(consumer_hooks),
              'world-hooks.patch': digest(world_hooks), 'transport-hooks.patch': digest(transport_hooks)}
    for role, names in [('client', ['rt_protocol.h', 'rt_bridge_api.h', 'rt_client.h', 'rt_draw_packet.h', 'rt_draw_request.h', 'rt_draw_client.h', 'rt_material_decode.h', 'rt_file_hash.h']),
                        ('server', ['rt_protocol.h', 'rt_host.h', 'rt_static_draw.h', 'rt_draw_packet.h', 'rt_draw_request.h', 'rt_draw_host.h',
                                    'rt_consumer_world.h', 'rt_material_decode.h', 'rt_world_runtime.h', 'rt_world_runtime.cpp', 'rt_world_diagnostics.h', 'rt_overlay.h', 'rt_file_hash.h', 'rt_scene_draw.h', 'rt_sky_shader.h', 'rt_scene_input.h', 'rt_bulk_static.h', 'rt_model_shader.h', 'rt_model_input.h', 'rt_live_mesh.h']),
                        ('util', ['rt_draw_request.h', 'rt_transport_policy.h'])]:
        for name in names:
            source = ROOT / 'integration/rt' / name
            shutil.copy2(source, output / 'bridge/src' / role / name)
            inputs[name] = digest(source)
    api = git(runtime, 'show', baseline['runtime_commit'] + ':public/include/remix/remix_c.h')
    if hashlib.sha256(api).hexdigest() != '58867d1dbc7a0980e3a5241750f573c9c8b38cb8f9aee1aba4fcb93bd78ec20e':
        raise RuntimeError('Pinned world API header differs')
    (output / 'bridge/src/server/rt_world_remix_c.h').write_bytes(api)
    inputs['rt_world_remix_c.h'] = hashlib.sha256(api).hexdigest()
    detours_archive = git(detours, 'archive', baseline['detours_commit'])
    destination = output / 'submodules/Detours'
    destination.mkdir(parents=True)
    with tarfile.open(fileobj=io.BytesIO(detours_archive)) as tar:
        tar.extractall(destination, filter='data')
    # Pin the private Remix extension and the missing-normal fallback as well
    # as the public ABI. These are contracts used by rt_scene_draw.h.
    scene_backend_contract = {}
    checks = {
        'src/d3d9/d3d9_rtx_utils.cpp': [b'remixTextureCategoryFlagsFromD3D', b'renderStates[42]', b'0xfefefefe', b'materialData.alphaTestReferenceValue', b'D3DRS_ALPHAFUNC', b'D3DRS_ALPHAREF'],
        'src/dxvk/rtx_render/rtx_types.h': [b'enum class InstanceCategories : uint32_t', b'WorldUI,', b'WorldMatte,', b'Sky,', b'Ignore,'],
        'src/dxvk/shaders/rtx/concept/surface/surface_interaction.slangh': [b'surface.normalBufferIndex == BINDING_INDEX_INVALID', b'surfaceInteraction.geometryNormal = flippedTriangleNormal;'],
    }
    checks['src/dxvk/rtx_render/rtx_remix_api.cpp'] = [b'CreateMesh', b'DestroyMesh', b'EmitCs', b'memcpy', b'vertices_values', b'indices_values']
    for name, required in checks.items():
        data = git(runtime, 'show', baseline['runtime_commit'] + ':' + name)
        if not all(pattern in data for pattern in required):
            raise RuntimeError('Pinned scene backend contract differs: ' + name)
        scene_backend_contract[name] = hashlib.sha256(data).hexdigest()
    receipt = {**baseline, 'inputs_sha256': inputs,
               'scene_backend_contract': scene_backend_contract,
               'upstream_archive_sha256': hashlib.sha256(archive).hexdigest(),
               'native_build': 'unrun', 'game_queue_execution': 'unrun',
               'contains_fg_assets': False, 'runtime_dll_included': False}
    receipt['fingerprint'] = hashlib.sha256(json.dumps(receipt, sort_keys=True).encode()).hexdigest()
    (output / 'rt-source.json').write_text(json.dumps(receipt, indent=2) + '\n')
    subprocess.run(['git', '-C', str(output), 'add', '.'], check=True)
    subprocess.run(['git', '-C', str(output), '-c', 'user.name=Codex', '-c', 'user.email=codex@local.invalid',
                    'commit', '-qm', 'Pinned LDB RT observer source'], check=True)
    print(output)


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output', type=Path, default=ROOT / '.research/rt-observer-source')
    p.add_argument('--bridge', type=Path, default=ROOT / '.research/sources/bridge-131')
    p.add_argument('--upstream', type=Path, default=ROOT / '.research/sources/bridge-upstream')
    p.add_argument('--detours', type=Path, default=ROOT / '.research/sources/detours')
    a = p.parse_args()
    try:
        prepare(*(getattr(a, key).resolve() for key in ['output', 'bridge', 'upstream', 'detours']))
    except (RuntimeError, OSError, subprocess.CalledProcessError) as e:
        p.exit(1, f'{e}\n')

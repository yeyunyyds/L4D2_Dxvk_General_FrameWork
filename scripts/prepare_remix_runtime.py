"""Prepare a new checkout of the exact L4D2 custom runtime and its submodules."""

import argparse
import hashlib
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
RUNTIME_SKINNING_SOURCE = Path('src/dxvk/rtx_render/rtx_remix_api.cpp')
RUNTIME_GEOMETRY_SOURCE = Path('src/dxvk/rtx_render/rtx_geometry_utils.h')
RUNTIME_ASSET_SOURCE = Path('src/dxvk/rtx_render/rtx_asset_replacer.cpp')
RUNTIME_ASSET_HEADER = Path('src/dxvk/rtx_render/rtx_asset_replacer.h')
RUNTIME_SKINNING_PATCH = ROOT / 'integration/rt/remix-runtime-skinning.patch'


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def validate_runtime_skinning_source(data):
    required = [
        b'expectedSkinningValues',
        b'skinning.blendWeights_count != expectedSkinningValues',
        b'skinning.blendIndices_count != expectedSkinningValues',
        b'sizeof(float) * src.skinning_value.bonesPerVertex',
    ]
    if any(marker not in data for marker in required):
        raise RuntimeError('Runtime skinning patch contract is incomplete')
    if b'blendWeightBuffer = dxvk::RasterBuffer { blendWeightsSlice, 0, sizeof(float),' in data:
        raise RuntimeError('Runtime still uses the invalid one-float skinning vertex stride')


def validate_runtime_skinning_flush(geometry):
    if b'm_skinningContext->flushCommandList();\n        m_skinningCommands = 0;' not in geometry:
        raise RuntimeError('GPU skinning command count is not reset after submission')


def validate_runtime_persistent_mesh(api, asset, header):
    required_api = [b'remixapi_LdbUpdateMesh', b'remixapi_CreateOrUpdateMesh',
                    b'assets->updateExternalMesh', b'ldbPersistentHash',
                    b'HashComponents::Indices', b'HashComponents::GeometryDescriptor',
                    b'const auto vertexGeneration = hack_getNextGeomHash()']
    if any(marker not in api for marker in required_api):
        raise RuntimeError('Persistent external mesh update export is incomplete')
    if (b'void AssetReplacer::updateExternalMesh' not in asset or
            b'm_extMeshes.emplace(handle, std::make_unique<std::vector<RasterGeometry>>' not in asset or
            b'*found->second = std::move(submeshes);' not in asset):
        raise RuntimeError('Persistent external mesh registry update is incomplete')
    if b'void updateExternalMesh' not in header:
        raise RuntimeError('Persistent external mesh declaration is missing')


def prepare(output):
    if output.exists():
        raise RuntimeError('Preserve existing runtime output; choose a new --output')
    baseline = json.loads((ROOT / 'integration/rt/baseline.json').read_text())
    output.mkdir(parents=True)
    def git(*args):
        return subprocess.check_output(['git', '-C', str(output), *args])
    git('init', '-q')
    git('remote', 'add', 'origin', baseline['runtime_repository'])
    git('fetch', '--depth=1', 'origin', baseline['runtime_commit'])
    git('checkout', '-q', '--detach', baseline['runtime_commit'])
    if git('rev-parse', 'HEAD').decode().strip() != baseline['runtime_commit']:
        raise RuntimeError('Runtime checkout differs from pin')
    git('apply', '--check', str(RUNTIME_SKINNING_PATCH))
    git('apply', str(RUNTIME_SKINNING_PATCH))
    runtime_skinning_source = output / RUNTIME_SKINNING_SOURCE
    validate_runtime_skinning_source(runtime_skinning_source.read_bytes())
    runtime_geometry_source = output / RUNTIME_GEOMETRY_SOURCE
    validate_runtime_skinning_flush(runtime_geometry_source.read_bytes())
    runtime_asset_source = output / RUNTIME_ASSET_SOURCE
    runtime_asset_header = output / RUNTIME_ASSET_HEADER
    validate_runtime_persistent_mesh(runtime_skinning_source.read_bytes(), runtime_asset_source.read_bytes(),
                                     runtime_asset_header.read_bytes())
    api = output / 'public/include/remix/remix_c.h'
    receipt = {'schema': 3, 'repository': baseline['runtime_repository'],
               'commit': baseline['runtime_commit'], 'branch_reference': baseline['runtime_branch_reference'],
               'api_sha256': digest(api),
               'smoke_source_sha256': digest(ROOT / 'integration/rt/rt_runtime_smoke.cpp'),
               'runtime_skinning_patch_sha256': digest(RUNTIME_SKINNING_PATCH),
               'runtime_skinning_source_sha256': digest(runtime_skinning_source),
               'runtime_geometry_source_sha256': digest(runtime_geometry_source),
               'runtime_asset_source_sha256': digest(runtime_asset_source),
               'runtime_asset_header_sha256': digest(runtime_asset_header),
               'runtime_skinning_weight_stride': 'sizeof(float)*bonesPerVertex',
               'runtime_skinning_command_counter': 'reset_after_flush',
               'runtime_persistent_mesh_update': 'remixapi_LdbUpdateMesh',
               'runtime_persistent_topology_identity': 'first_frame_upsert_stable_indices_descriptor_dynamic_vertex_generation',
               'submodules': 'pending', 'runtime_execution': 'unrun', 'b580_rt': 'unrun'}
    def save():
        (output / 'rt-runtime-source.json').write_text(json.dumps(receipt, indent=2) + '\n')
    save()
    git('submodule', 'update', '--init', '--recursive', '--depth=1')
    receipt['submodules'] = 'ready'
    receipt['submodule_status'] = git('submodule', 'status', '--recursive').decode().splitlines()
    save()
    print(output)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT / '.research/rt-remix-runtime')
    args = parser.parse_args()
    try:
        prepare(args.output.resolve())
    except (RuntimeError, OSError, subprocess.CalledProcessError) as error:
        parser.exit(1, f'{error}\n')

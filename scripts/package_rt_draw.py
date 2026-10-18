"""Package the verified native Draw observer and pinned self-contained Python."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import urllib.request
import zipfile
from package_rt_preflight import python_files, PYTHON_URL
from rt_draw_suite import TARGETS, SUPPORT_FILES, verify_package, runtime_for_world

ROOT=Path(__file__).resolve().parents[1]


def sha(data):return hashlib.sha256(data).hexdigest()


def observer_files():
    names=['scripts/'+name for name in ['rt_draw_suite.py','analyze_rt_draw.py','analyze_rt_scene.py','analyze_rt_world.py','rt_game_probe.py',
                                       'rt_preflight.py','rt_visual.py','rt_suite.py']]
    names+=list(SUPPORT_FILES)
    entries={name:(ROOT/name).read_bytes() for name in names}
    for target,source in [('run-test.cmd','run-draw-test.cmd'),('restore.cmd','restore-draw-test.cmd'),
                          ('README.txt','draw-observer-readme.txt')]:
        entries[target]=(ROOT/'integration/rt'/source).read_bytes()
    return entries


def package(source,output,python_package):
    commit=os.environ['GITHUB_SHA']
    with zipfile.ZipFile(ROOT/'.research/dist/rt-foundation-build-status.zip') as z:
        status=json.loads(z.read('status.json'))
    if (status['commit']!=commit or status['native_outcome']!='success' or status['consumer_draw_development_execution']!='pass'
            or status.get('consumer_world_development_execution')!='pass' or status.get('world_startup_gate_development_execution')!='pass'
            or status.get('material_global_name_decoder_development_execution')!='pass'
            or status.get('world_transport_backpressure_development_execution')!='pass'
            or status.get('world_lighting_comparison_development_execution')!='pass'
            or status.get('world_coverage_overlay_development_execution')!='pass'
            or status.get('world_tangent_layout_development_execution')!='pass'
            or status.get('world_scene_scope_development_execution')!='pass'
            or status.get('world_sky_reconstruction_development_execution')!='pass'
            or status.get('world_bulk_static_development_execution')!='pass'
            or status.get('world_original_input_development_execution')!='pass'
            or status.get('world_owned_geometry_development_execution')!='pass'):
        raise ValueError('Current native paired observer and execution evidence required')
    if output.exists():raise FileExistsError('Preserve previous package directory')
    output.mkdir(parents=True)
    if not python_package.is_file():
        python_package.parent.mkdir(parents=True,exist_ok=True)
        with urllib.request.urlopen(PYTHON_URL,timeout=60) as response:
            data=response.read(40*1024*1024)
        python_package.write_bytes(data)
    entries=dict(python_files(python_package))
    receipt=json.loads((source/'rt-source.json').read_text())
    build_id=('ldb-rt-observer+'+receipt['fingerprint'][:16]).encode()
    binaries={}
    for arch,relative,target,machine in [('x86','client/d3d9.dll',TARGETS[0],0x14c),('x64','server/L4D2Bridge64.exe',TARGETS[1],0x8664)]:
        data=(source/f'bridge/_rtRelease_{arch}/src'/relative).read_bytes()
        if data[:2]!=b'MZ' or build_id not in data:raise ValueError('Missing paired native identity')
        pe=struct.unpack_from('<I',data,60)[0]
        if data[pe:pe+4]!=b'PE\0\0' or struct.unpack_from('<H',data,pe+4)[0]!=machine:raise ValueError('Wrong observer architecture')
        expected=status['binaries']['development/'+arch+'/'+Path(relative).name]
        if sha(data)!=expected:raise ValueError('Native evidence differs from product')
        binaries[target]={'sha256':sha(data),'size':len(data),'machine':hex(machine)}
        entries['payload/'+target]=data
    entries.update(observer_files())
    entries['integration/rt/world-runtime.json']=(ROOT/'integration/rt/world-runtime.json').read_bytes()
    entries['README.txt']=(ROOT/'integration/rt/world-pilot-readme.txt').read_bytes()
    # Preserve the full dependency notices from the exact, already-published
    # native runtime product. The runtime binaries themselves are not rebuilt.
    prerequisite=ROOT/'.research/world-prerequisite.zip'
    if not prerequisite.is_file():
        prerequisite.write_bytes(subprocess.check_output(['git','show','75e75334074a15f2a912cc91396be2280d3733ec:downloads/rt-preflight-dev.zip'],cwd=ROOT))
    if sha(prerequisite.read_bytes())!='24867acab96ddae995c68e72b12175d475af2aa08c07900844643a24249e7a34':
        raise ValueError('Prerequisite notices source differs from verified v6 product')
    with zipfile.ZipFile(prerequisite) as prior:
        for name in prior.namelist():
            if name.startswith('licenses/') or name.startswith('payloads/'):
                entries[name]=prior.read(name)
    entries['licenses/Remix-LICENSE']=(source/'LICENSE').read_bytes()
    entries['licenses/Detours-LICENSE']=(source/'submodules/Detours/LICENSE').read_bytes()
    entries['licenses/LDB-LICENSE']=subprocess.check_output(['git','-C',str(ROOT/'.research/sources/bridge-131'),
                                                          'show',receipt['bridge_commit']+':LICENSE'])
    manifest={'schema':1,'scope':'selected_static_world_runtime_pilot','profile':'world_pilot','pilot_revision':15,
              'readback_presents':[30,120,180,240,300,360,420,480,540,600],
              'coverage_comparison':['raster','baseline_world','expanded_sky','expanded_models'],
              'baseline_sphere_radiance':3000,'directional_radiance':3,'expanded_coverage_after_present':120,
              'baseline_world_revision':7,'hud_after_present':30,
              'expanded_world_vertex_formats':['0x80001','0x80503','0xa0403'],
              'sky_after_present':120,'sky_rebuild_after_present':120,
              'sky_rebuild':'position_dp4_matrix_proof_full_ps_uv_six_face_gate_else_v8',
              'scene_input_samples_per_phase':198,'scene_input_max_vertices':4096,'scene_input_max_indices':16384,'rigid_after_present':180,'rigid_normals':'geometric_triangle_fallback',
              'bulk_static_after_present':180,'bulk_static_families':['LightmappedGeneric','LightmappedGeneric_DX9','UnlitGeneric','VertexLitGeneric','WorldVertexTransition','WorldVertexTransition_DX9','WorldTwoTextureBlend','NekoToon'],
              'bulk_static_material':'first_diffuse_layer_original_alpha_test; transition_stage7; no complete blend/PBR',
              'model_rebuild':'audited_full_vs_matrix_proof_rigid_or_three_weight_palette_with_complete_morph_position',
              'model_submission':'compact_owned_readable_default_vb_ib_readback_and_binding_verified; remix_staging_copy',
              'model_input_pairs':'readback_present_draw_original_geometry_vs_ps_constants_declaration_morph_and_owned_payload',
              'model_stage_readbacks':{240:'neko',300:'neko_preserve',360:'neko_recheck',420:'combined',480:'combined_matte',540:'combined_directional',600:'recovery'},
              'preserve_path_default':False,'preserve_path_control_presents':[241,300],
              'model_neutral_color_present_range':[421,480],'model_directional_light_present_range':[481,540],
              'exposure_changed':False,'model_samples_per_readback_limit':64,
              'model_payload_limits_kib':{'neko':12288,'neko_preserve':12288,'neko_recheck':12288,'combined':16384,'combined_matte':0,'combined_directional':0},
              'model_special_passes':'reject_stencil_user_clip_partial_rgb_and_non_less_depth',
              'sky_rebuilt_stride':140,'model_geometry_max_vertices':32768,'model_geometry_max_indices':131072,
              'neko_material':'audited_full_vs_and_ps_sha256_first_diffuse_layer_original_alpha_test_no_full_toon',
              'model_morph':'complete_referenced_stream2_position1_float4_before_skinning_readonly',
              'scene_input_ps_float4_constants':224,'scene_input_complete_morph_positions':True,
              'bulk_static_stride_max':128,'scene_input_family_limits':[6,48,96,48],'scene_input_policy':'skinned_materials_v2','scene_input_payload_limits_mib':[2,2,2,12],'scene_input_family_payload_limits_kib':[[128,640,512,768]]*3+[[256,4096,1792,6144]],'scene_input_geometry_window':True,'audit_only_families':[],'custom_audit_max_vertices':32768,'custom_audit_max_indices':131072,
              'sky_category_contract':{'render_state':42,'sky_bit':4,'unused':0xfefefefe},
              'draw_trace_version':12,'original_cull_mode_in_trace':True,'original_scene_states_in_trace':True,'world_material_attribution_limit':256,
              'material_samples_per_phase_limit':64,
              'world_ipc_producer_wait_ms':60000,'normal_ipc_producer_wait_ms':2000,
              'startup_raytracing':False,'rt_activation':'after_control_and_successful_world_scope_then_deferred_by_runtime',
              'build_commit':commit,'bridge_baseline':'1.3.2',
              'build_id':build_id.decode(),'binaries':binaries,'runtime_dll_replaced':False,
              'private_game_methods_called':False,'game_rt':'unrun','source_queue_execution':'unrun',
              'source_parallel_preparation':'unrun','performance':'unrun',
              'files':{name:{'sha256':sha(data),'size':len(data)} for name,data in entries.items()}}
    entries['draw-observer-manifest.json']=(json.dumps(manifest,indent=2)+'\n').encode()
    primary=output/'rt-world-pilot-dev.zip'
    with zipfile.ZipFile(primary,'w',zipfile.ZIP_DEFLATED) as z:
        for name,data in entries.items():z.writestr(name,data)
    with zipfile.ZipFile(primary) as z:
        if z.testzip() is not None:raise ValueError('Product CRC failed')
        for name,entry in manifest['files'].items():
            if sha(z.read(name))!=entry['sha256']:raise ValueError('Product member mismatch')
    if os.name!='nt':raise OSError('Native packaged Python execution requires Windows')
    with tempfile.TemporaryDirectory(prefix='rt-observer-package-') as directory:
        with zipfile.ZipFile(primary) as z:z.extractall(directory)
        root=Path(directory)
        verify_package(root)
        process=subprocess.run([str(root/'python/python.exe'),'-I',str(root/'scripts/rt_draw_suite.py'),'--help'],
                               capture_output=True,text=True,timeout=30)
        if process.returncode:raise ValueError('Packaged entry point failed: '+process.stderr)
        process=subprocess.run([str(root/'python/python.exe'),'-I',str(root/'scripts/rt_draw_suite.py'),'--verify-only'],
                               capture_output=True,text=True,timeout=30)
        if process.returncode:raise ValueError('Packaged collector configuration failed: '+process.stderr)
        catalog=json.loads(entries['integration/rt/world-runtime.json'])
        cache=ROOT/'.research/world-payload-cache';cache.mkdir(parents=True,exist_ok=True)
        from rt_preflight import check_file
        for part in catalog['payloads']:
            path=cache/part['name']
            if not check_file(path,part):
                path.write_bytes(subprocess.check_output(['git','show',part['ref']+':downloads/'+part['name']],cwd=ROOT))
            if not check_file(path,part):raise ValueError('Immutable runtime dependency differs from pin')
        runtime=runtime_for_world(root,print,payload_cache=cache)
        loader=source/'checks-x64/world-loader.exe'
        evidence=output/'world-loader-evidence'
        process=subprocess.run([str(loader),str(runtime),str(evidence.resolve())],capture_output=True,text=True,timeout=60)
        print(process.stdout,flush=True)
        if process.returncode:raise ValueError('Native world loader initialization failed: '+process.stderr)
        with zipfile.ZipFile(output/'rt-world-loader-validation.zip','w',zipfile.ZIP_DEFLATED) as z:
            for path in evidence.rglob('*'):
                if path.is_file():z.write(path,path.relative_to(evidence))
    validation={'schema':1,'commit':commit,'package_validation':'pass','launcher':primary.name,'payloads':[],
                'primary_sha256':sha(primary.read_bytes()),'primary_size':primary.stat().st_size,
                'packaged_python_isolated_entry':'pass','packaged_collector_configuration':'pass',
                'native_consumer_fixture_execution':'pass','pinned_runtime_dependency_load':'pass',
                'world_startup_gate_development_execution':'pass',
                'material_global_name_decoder_development_execution':'pass',
                'world_transport_backpressure_development_execution':'pass',
                'world_lighting_comparison_development_execution':'pass',
                'world_coverage_overlay_development_execution':'pass',
                'world_tangent_layout_development_execution':'pass',
                'world_scene_scope_development_execution':'pass',
                'world_sky_reconstruction_development_execution':'pass',
                'world_bulk_static_development_execution':'pass',
                'world_original_input_development_execution':'pass',
                'world_owned_geometry_development_execution':'pass',
                'pinned_native_loader_api_initialization':'pass',
                'game_rt':'unrun','actual_game_observation':'unrun'}
    (output/'publish-index.json').write_text(json.dumps(validation,indent=2)+'\n')
    print(json.dumps(validation,indent=2))


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--source',type=Path,default=ROOT/'.research/rt-observer-source')
    p.add_argument('--output',type=Path,default=ROOT/'.research/dist/rt-game-observe')
    p.add_argument('--python-package',type=Path,default=ROOT/'.research/python.3.12.10.nupkg')
    a=p.parse_args();package(a.source.resolve(),a.output.resolve(),a.python_package.resolve())

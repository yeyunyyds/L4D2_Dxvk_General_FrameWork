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
    names=['scripts/'+name for name in ['rt_draw_suite.py','rt_gpu_telemetry.py','analyze_rt_runtime.py','analyze_rt_draw.py','analyze_rt_scene.py','analyze_rt_world.py','analyze_rt_live.py','analyze_rt_textures.py','analyze_rt_infected.py','rt_game_probe.py',
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
            or status.get('world_owned_geometry_development_execution')!='pass'
            or status.get('world_live_mesh_development_execution')!='pass'
            or status.get('world_live_mesh_budget_development_execution')!='pass'
            or status.get('world_model_texture_development_execution')!='pass'
            or status.get('world_live_material_rebind_development_execution')!='pass'):
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
    manifest={'schema':1,'scope':'selected_world_and_live_model_runtime_pilot','profile':'world_pilot','pilot_revision':27,
              'readback_presents':[30,120,180,240,300,360,420,480,540,600],
              'coverage_comparison':['raster','baseline_world','expanded_sky','expanded_models'],
              'baseline_sphere_radiance':30,'directional_radiance':3,'expanded_coverage_after_present':120,
              'baseline_world_revision':7,'hud_after_present':30,
              'expanded_world_vertex_formats':['0x80001','0x80503','0xa0403'],
              'sky_after_present':120,'sky_rebuild_after_present':120,
              'sky_rebuild':'position_dp4_matrix_proof_full_ps_uv_six_face_gate_else_v8',
              'scene_input_samples_per_phase':222,'scene_input_max_vertices':4096,'scene_input_max_indices':16384,'rigid_after_present':180,'rigid_normals':'area_weighted_geometric_fallback_preserving_source_vertex_seams',
              'bulk_static_after_present':180,'bulk_static_families':['LightmappedGeneric','LightmappedGeneric_DX9','UnlitGeneric','VertexLitGeneric','WorldVertexTransition','WorldVertexTransition_DX9','WorldTwoTextureBlend','NekoToon','Infected'],
              'bulk_static_material':'first_diffuse_layer_original_alpha_test; transition_stage7; no complete blend/PBR',
              'model_rebuild':'audited_full_vs_matrix_proof; CPU_skinned_pose_geometry_for_all_supported_animated_models; persistent_GPU_skinning_isolated_after_B580_device_loss',
              'model_submission':'per_pose_CPU_skinned_mesh; original_raster_Draw_once_with_Ignore_bit3_and_exact_restore',
              'gpu_skinning':{'enabled_by_default':False,'status':'disabled_after_v25_v26_B580_fragmentation_and_device_loss','isolated_profiles':['Skin3','Neko3'],'bridge_scalar_count_transport_fix_retained':True,'runtime_stride_patch_retained':True,'expected_submission_count':0},
              'model_input_pairs':'canonical_CPU_vertices_and_normalized_indices_at_239_240_299_300; original_input_audit_retained_separately',
              'model_stage_readbacks':{240:'api_neko_textured',300:'api_combined_textured',360:'api_combined_textured_low_light',420:'api_combined_textured_metal',480:'api_combined_textured_alpha',540:'api_combined_textured_emissive',600:'recovery'},
              'preserve_path_default':False,'preserve_path_control_presents':[],
              'model_material':'indexed_audited_source_normals_else_area_weighted; sampler0_all_source_mips_DDS_DX10_original_sRGB_and_alpha; explicit_synthetic_fallback; audited_Infected_float3_smooth_normals_else_geometric',
              'live_mesh_limits':{'bytes':536870912,'frame_new_bytes':201326592,'draws_per_frame':1024,'entries':2048,'pending_gpu_fences':8},
              'live_mesh_retirement':'before_first_model_and_after_Present_event_query_completion_and_two_frames_idle;idle_materials_after_last_referencing_mesh;recovery_shutdown_completion_gated',
              'live_mesh_budget_policy':'omit_only_affected_model_with_explicit_reason_and_totals; no_rt_shutdown_for_application_budget; actual_API_failure_remains_fatal',
              'previous_pose_presents':[239,299],
              'model_payload_budget_scope':'16_MiB_shared_per_neutral_stage_including_previous_pose',
              'model_light_controls':{'after_300':'3,3,3','after_360':'30,30,30','after_540':'30,30,30'},
              'model_texture_limits':{'bytes':4294967296,'resident_bytes':134217728,'evidence_bytes':67108864,'runtime_files':4096,'held_refs':256,'reads_per_frame':16,'max_dimension':4096,'max_payload_bytes':16777216,'max_mip_payload_bytes':25165824},
              'model_material_update':'GPU_completed_old_use_then_DestroyMaterial_CreateMaterial_same_handle; geometry_retains_stable_material_identity',
              'trace_encoding':'lossless_per_present_snapshot_tail_and_pipeline_dictionaries;4096_definitions;all_Draw_conversion_result_and_footer_counts_preserved;256MiB_cap',
              'model_geometry':'indexed_64byte_API_vertices;original_triangle_order_winding_UV_preserved;generic_normals_area_weighted;audited_FLOAT3_normals_preferred',
              'model_handle_limits':{'materials':512,'draws':1024,'meshes':2048,'geometry_bytes':536870912},
              'texture_backing_reuse':'resource_private_cookie_prevents_pointer_reuse;invalidate_on_Surface_UnlockRect_UpdateSurface_UpdateTexture;4096_DDS_associations_without_source_COM_refs;source_only_ref_eviction_independent_of_API_GPU_use;16_new_exports_per_frame',
              'runtime_texture_reservation':{'total':4294967296,'ordinary_limit':4026531840,'infected_priority_reserved':268435456,'files':4096,'max_new_reads_per_frame':16,'max_new_read_bytes_per_frame':100665664},
              'texture_retry_policy':'resource_cookie_srgb_priority_negative_cache_after_spool_rejection;does_not_consume_read_slot;mutation_invalidates_cookie',
              'material_evidence_links':'bindings_include_source_material_and_shader;near_targets_include_API_material_handle_and_textured_flag;unknown_inputs_include_exact_render_states',
              'model_alpha_policy':'opaque_and_standard_SRCALPHA_INVSRCALPHA_ADD_no_separate_alpha;source_DDS_alpha_and_test_preserved;other_special_passes_rejected',
              'texture_evidence_reservation':{'total':67108864,'ordinary_limit':50331648,'infected_priority_reserved':16777216},
              'device_cleanup':'before_IDirect3DDevice9Ex_Destroy_safeDestroy;after_command_loop_before_other_teardown;abort_cleanup;final_partial_frame_EVENT;idempotent',
              'actual_model_materials':'neutral_source_alpha_throughout_except_Infected_bright_control;legacy_stage_names_retained_only_as_trace_phase_IDs',
              'bounded_preview':{'after_diagnostic_present':600,'duration_ms':60000,'recovery_timeout_ms':5000,'mode':'neutral;30s_no_AA_then30s_native_TAA;same_one_F9;no_Draw_trace_extension;requires_review'},
              'result_archive_policy':'runtime-textures_spool_excluded;64MiB_DDS_evidence_explicit_omissions;INCOMPLETE_recovery_ZIP_before_exit_wait;atomic_replace_same_ZIP_after_final_analysis',
              'pending_material_audit':{'shader':'Infected','samples_per_phase':24,'render_admission':'exact_VS_PS_declaration_rigid48_only'},
              'exit_review':'final_Bridge_log_exceptions_and_backend_cleanup_reported_separately_from_collection',
              'diagnostic_coverage_revision':23,'preview_edge_comparison':{'first_seconds':30,'baseline_upscaler':0,'second_upscaler':3,'resolution_scale':1.0,'native_bgra8_max_bytes_per_sample':16777216,'crop_if_larger':[1024,768],'sequential_not_matched_AB':True},
              'runtime_profiling':'nested_QPC_elapsed_scopes_not_CPU_usage_or_GPU_duration;optional_bounded_one_second_PDH_per_process_engine_samples;do_not_sum_engines',
              'shader_census':'up_to128_source_shader_names_cumulative_per_segment;not_entity_or_image_coverage',
              'hash_optimization':'immutable_shared_SHA256_algorithm_provider;independent_hash_object_every_call;no_mutable_geometry_or_texture_cache',
              'exposure_changed':False,'model_samples_per_readback_limit':64,
              'model_payload_limits_kib':{'api_neko_textured':16384,'api_combined_textured':16384,'api_combined_textured_low_light':0,'api_combined_textured_metal':0,'api_combined_textured_alpha':0,'api_combined_textured_emissive':0},
              'model_special_passes':'reject_stencil_user_clip_partial_rgb_and_non_less_depth',
              'sky_rebuilt_stride':140,'model_geometry_max_vertices':32768,'model_geometry_max_indices':131072,
              'infected_diffuse':{'samplers':[0,4,5],'bake_max_dimension':256,'bakes_per_frame':1,'recipes':128,'output':'linear_BGRA8_opaque_generated_full_mips','excluded':'vertex_lighting_screen_AO_fog; base_level_bake_no_original_derivatives'},
              'infected_control':{'presents':[361,420],'material':'bright_green_opaque_emissive_double_sided','other_stages':'reconstructed_diffuse_opaque_neutral'},
              'near_target_selection':'Infected_or_VertexLitGeneric_three_weight_at_least512_vertices;ready;center_in_expanded_frustum;not_entity_identity',
              'infected_near_target':{'distance_source_units':1024,'cadence_presents':60,'rows':256,'rows_per_frame':4,'payload_bytes':16777216,'preview':True},
              'unknown_model_probe':{'cadence_presents':60,'samples':32,'payload_bytes':16777216,'preview':True},
              'new_vertexlitgeneric_profiles':{'rigid':3,'three_weight':2,'capture':'141352','admission':'full_VS_hash_layout_morph_and_camera_proof'},
              'infected_material':'exact_VS_PS_declaration48_rigid_c58_world_c8_clip_direct_baseUV_FLOAT3_normals; no_full_original_PS',
              'neko_material':'audited_full_vs_and_ps_sha256_sampler0_base_texture_original_alpha; no_full_toon_or_original_lighting',
              'model_morph':'complete_referenced_stream2_position1_float4_before_skinning_readonly',
              'scene_input_ps_float4_constants':224,'scene_input_complete_morph_positions':True,
              'bulk_static_stride_max':128,'scene_input_family_limits':[6,48,96,48,24],'scene_input_policy':'skinned_materials_v2','scene_input_payload_limits_mib':[3,3,3,16],'scene_input_family_payload_limits_kib':[[128,640,512,768,512]]*3+[[256,4096,1792,6144,4096]],'scene_input_geometry_window':True,'audit_only_families':[],'custom_audit_max_vertices':32768,'custom_audit_max_indices':131072,
              'sky_category_contract':{'render_state':42,'sky_bit':4,'unused':0xfefefefe},
              'draw_trace_version':20,'original_cull_mode_in_trace':True,'original_scene_states_in_trace':True,'world_material_attribution_limit':256,
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
                'world_live_mesh_development_execution':'pass',
                'world_live_mesh_budget_development_execution':'pass',
                'world_model_texture_development_execution':'pass',
                'world_live_material_rebind_development_execution':'pass',
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

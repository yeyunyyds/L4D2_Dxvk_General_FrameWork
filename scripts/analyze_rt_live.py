"""Validate v16/v17 live API evidence; GPU geometry/image acceptance remains separate."""
from collections import Counter
import hashlib
import math
from pathlib import Path
import struct

FRAMES=(30,120,180,240,300,360,420,480,540,600)
STAGES=('off','off','off','api_neko_neutral','api_combined_neutral','api_combined_rough','api_combined_metal','api_combined_alpha','api_combined_emissive','recovery')
MATERIALS=('neutral',)*5+('rough','metal','alpha','emissive','neutral')
STAGES_V18=('off','off','off','api_neko_textured','api_combined_textured','api_combined_textured_low_light','api_combined_textured_metal','api_combined_textured_alpha','api_combined_textured_emissive','recovery')
MATERIALS_V18=('neutral',)*6+('metal','alpha','emissive','neutral')
CONFIG=('rtx.enablePreservePath','rtx.useBuffersDirectly','rtx.bloom.enable','rtx.postfx.enable')


def analyze(directory,events):
    errors=[];directory=Path(directory);samples=[];payload_bytes=0
    v18=any(e.get('event')=='world_coverage_revision' and e.get('value')=='18' and e.get('code')==0 for e in events)
    v17=v18 or any(e.get('event')=='world_coverage_revision' and e.get('value')=='17' and e.get('code')==0 for e in events)
    stages=STAGES_V18 if v18 else STAGES;materials=MATERIALS_V18 if v18 else MATERIALS
    reasons=('draw_limit','frame_geometry_bytes','live_geometry_bytes','mesh_entries','gpu_backlog')
    def check(value,why):
        if not value:raise ValueError(why)
    for e in events:
        if e.get('event','') in ('api_model_submit_failed','api_model_ignore_failed','api_model_ignore_binding_failed',
                'api_model_ignore_restore_failed','api_model_exception','api_mesh_fence_failed','api_mesh_fence_budget',
                'api_mesh_retire_failed','api_mesh_present_exception'):
            errors.append(e['event'])
    for name in ('post_device_configuration_requested','api_mesh_policy'):
        if not any(e.get('event')==name and e.get('code')==0 for e in events):errors.append('missing_success:'+name)
    requests=[(e.get('after_present'),e.get('key'),e.get('value'),e.get('code')) for e in events if e.get('event')=='model_path_config_requested']
    if requests!=[(f,k,'False',0) for f in FRAMES[:-1] for k in CONFIG]:errors.append('live_configuration_sequence')
    frames=[e for e in events if e.get('event')=='api_mesh_frame'];previous={}
    if [e.get('frame') for e in frames]!=list(range(1,601)):errors.append('missing_live_present_accounting')
    for e in frames:
        try:
            for k in ('submitted','created','reused','destroyed','entries','bytes','peak_bytes','completed_through','pending_fences'):
                check(type(e.get(k)) is int and e[k]>=0,'invalid_live_counter')
            for k in ('submitted','created','reused','destroyed','peak_bytes','completed_through'):
                check(e[k]>=previous.get(k,0),'regressing_live_counter')
            check(e['created']-e['destroyed']==e['entries'] and e['entries']<=512,'live_resource_accounting')
            check(e['bytes']<=e['peak_bytes']<=256*1024*1024 and e['pending_fences']<=8,'live_resource_budget')
            check(e['completed_through']<=e['frame'],'invalid_live_completion')
            if e['destroyed']>previous.get('destroyed',0):check(e['completed_through']>0,'retirement_without_completion')
            if e['frame']<=180:check(e['submitted']==0,'premature_live_submission')
            if v17:
                for k in ('frame_draws','frame_new_bytes','frame_omitted','omitted_total'):
                    check(type(e.get(k)) is int and e[k]>=0,'invalid_live_budget_counter')
                check(e['frame_draws']==e['submitted']-previous.get('submitted',0) and e['frame_draws']<=512,'invalid_live_frame_draws')
                check(e['frame_new_bytes']<=96*1024*1024,'invalid_live_frame_bytes')
                check(e['omitted_total']-previous.get('omitted_total',0)==e['frame_omitted'],'live_omission_delta')
                omission=e.get('omission_reasons',{})
                check(set(omission)==set(reasons) and all(type(n) is int and n>=0 for n in omission.values()) and sum(omission.values())==e['omitted_total'],'live_omission_reason_accounting')
                check(all(omission[k]>=previous.get('omission_reasons',{}).get(k,0) for k in reasons),'regressing_live_omission_reason')
            previous=e
        except (KeyError,TypeError,ValueError) as exc:errors.append(str(exc))
    readbacks=[e for e in events if e.get('event')=='api_model_path_readback']
    if [(e.get('frame'),e.get('stage'),e.get('material')) for e in readbacks]!=list(zip(FRAMES,stages,materials)):errors.append('live_readback_sequence')
    rows=[e for e in events if e.get('event') in (('api_model_sample','api_model_previous_sample') if v17 else ('api_model_sample',))];ordinals=set();files=set();counts=Counter();stage_bytes=Counter()
    for e in rows:
        try:
            frame=e['frame'];previous_sample=e['event']=='api_model_previous_sample';check(frame in ((239,299) if previous_sample else FRAMES[3:-1]),'invalid_live_sample_frame');index=FRAMES.index(frame+1 if previous_sample else frame)
            check(e['stage']==stages[index] and e['material']==materials[index],'live_sample_material_stage')
            counts[frame]+=1;check(counts[frame]<=64,'live_sample_budget')
            check(type(e['ordinal']) is int and e['ordinal']>0 and e['ordinal'] not in ordinals,'live_sample_identity');ordinals.add(e['ordinal'])
            check(e['ignored_category_at_original_draw']&8 and e['cull'] in (1,2,3),'original_model_not_isolated')
            check(e['gpu_upload']=='pinned_CreateMesh_synchronous_copy_queued_owned_buffers','unknown_live_upload_contract')
            check(e['shader'] in ('NekoToon','VertexLitGeneric') and (frame not in (239,240) or e['shader']=='NekoToon'),'unapproved_live_shader')
            check(e['kind'] in (1,2) and e['topology'] in (4,5) and 0<e['vertices']<=32768 and 0<e['triangles']<=131072,'live_geometry_range')
            for key in ('world','view','projection'):
                check(len(e[key])==16 and all(type(x) in (float,int) and math.isfinite(x) for x in e[key]),'live_invalid_matrix')
            check(all(abs(e['world'][i]-v)<=1e-4 for i,v in ((3,0),(7,0),(11,0),(15,1))),'live_non_affine_world')
            for key in ('vertices_sha256','indices_sha256'):
                check(isinstance(e[key],str) and len(e[key])==64 and all(c in '0123456789abcdef' for c in e[key]),'live_missing_digest')
            payload={}
            for item in e['files']:
                name=item['file'];check(Path(name).name==name and name.startswith('api-'+str(e['ordinal'])+'-') and name not in files,'live_unsafe_payload');files.add(name)
                suffix=name.split(str(e['ordinal'])+'-',1)[1];check(suffix in ('vertices.bin','indices.bin'),'live_unexpected_payload')
                path=directory/name;check(path.is_file() and 0<item['size']<=1048576 and path.stat().st_size==item['size'],'live_missing_payload')
                raw=path.read_bytes();digest=hashlib.sha256(raw).hexdigest();check(digest==item['sha256']==e[suffix.split('.')[0]+'_sha256'],'live_payload_digest')
                payload[suffix]=raw;payload_bytes+=len(raw);stage_bytes[frame+1 if previous_sample else frame]+=len(raw)
            check(stage_bytes[frame+1 if previous_sample else frame]<=16*1024*1024,'live_payload_budget')
            if e['payload_saved']:
                check(frame in ((239,240,299,300) if v17 else (240,300)),'live_unrequested_payload');vertices=payload['vertices.bin'];check(len(vertices)==e['vertices']*32,'live_vertex_correspondence')
                for at in range(0,len(vertices),32):check(all(math.isfinite(x) and abs(x)<=1e7 for x in struct.unpack_from('<5f',vertices,at)),'live_nonfinite_vertex')
                if e['kind']==2:
                    check(e['index_bytes'] in (2,4),'live_index_width');raw=payload['indices.bin'];n=e['triangles']*3 if e['topology']==4 else e['triangles']+2
                    check(len(raw)==n*e['index_bytes'],'live_index_count');values=struct.unpack('<'+('H' if e['index_bytes']==2 else 'I')*n,raw);check(max(values)<e['vertices'],'live_index_outside_vertices')
                else:check(not payload.get('indices.bin') and e['vertices']==(e['triangles']*3 if e['topology']==4 else e['triangles']+2),'live_nonindexed_range')
            else:check(not payload,'live_omitted_payload_present')
            samples.append(e)
        except (KeyError,ValueError,TypeError,OSError,struct.error) as exc:errors.append(str(exc))
    for e in readbacks:
        try:
            check(e['submission_samples']==counts[e['frame']] and 0<=e['submission_sample_omitted'],'live_sample_count_mismatch')
            if e['frame'] in FRAMES[3:-1]:check(e['submission_samples']>0,'live_model_stage_unrun')
            if e['frame']==600:check(e['live_entries']==0,'live_recovery_resources_remaining')
            if v17:
                check(e['previous_samples']==counts[e['frame']-1] if e['frame'] in (240,300) else e['previous_samples']==0,'live_previous_count_mismatch')
                if e['frame'] in (240,300):check(e['previous_samples']>0,'missing_live_previous_pose')
                account=next((f for f in frames if f['frame']==e['frame']),{})
                check(e['omitted_total']==account.get('omitted_total'),'live_readback_omission_mismatch')
        except (KeyError,ValueError,TypeError) as exc:errors.append(str(exc))
    if not stage_bytes[240] or not stage_bytes[300]:errors.append('live_missing_neutral_geometry_pairs')
    textures=None
    if v18:
        from analyze_rt_textures import analyze as analyze_textures
        textures=analyze_textures(directory,events);errors.extend(textures['errors'])
    omissions=frames[-1].get('omitted_total',0) if v17 and frames else 0
    return {'schema':1,**({'textures':textures} if v18 else {}),'budget_omissions':omissions,'model_budget_coverage':'partial_due_to_budget' if omissions else 'no_recorded_budget_omissions','previous_pose_samples':sum(e.get('event')=='api_model_previous_sample' for e in samples),'pass':not errors,'status':'valid_evidence_requires_image_review' if not errors else 'incomplete',
            'errors':sorted(set(errors)),'readbacks':readbacks,'payload_bytes':payload_bytes,'samples':samples,
            'final_counters':frames[-1] if frames else {},'gpu_image_acceptance':'unrun',
            'limits':'API success, CPU hashes and event completion do not establish correct BLAS, materials, original textures or game performance'}

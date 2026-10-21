"""Verify bounded original scene inputs and project raw positions for diagnosis."""
from collections import Counter
import hashlib
import json
import math
from pathlib import Path
import struct

PHASES=('raster','baseline_world','expanded_sky','expanded_models')


def instructions(raw):
    if len(raw)%4:return {'error':'unaligned_shader'}
    words=struct.unpack('<'+'I'*(len(raw)//4),raw);at=1;ops=Counter()
    while at<len(words):
        code=words[at];at+=1;op=code&65535
        if op==65535:return {'version':hex(words[0]),'opcodes':dict(ops),'ended':at==len(words)}
        count=(code>>16)&32767 if op==65534 else (code>>24)&15
        if count>len(words)-at:return {'error':'truncated_shader'}
        if op!=65534:ops[str(op)]+=1
        at+=count
    return {'error':'missing_shader_end'}


def projection(sample,vertices):
    stride=sample['stride'];position=[];clip=[]
    matrices=[sample[k] for k in ('world','view','projection')]
    if any(len(m)!=16 or not all(isinstance(x,(int,float)) and math.isfinite(x) for x in m) for m in matrices):raise ValueError('invalid_scene_matrix')
    if stride<12:return {'status':'position_layout_unavailable'}
    for at in range(0,len(vertices),stride):
        v=list(struct.unpack_from('<3f',vertices,at))+[1.]
        if not all(math.isfinite(x) and abs(x)<1e8 for x in v):return {'status':'nonfinite_or_unbounded_raw_position'}
        position.append(v[:3])
        for m in matrices:v=[sum(v[i]*m[i*4+j] for i in range(4)) for j in range(4)]
        if v[3]>1e-8:clip.append([v[i]/v[3] for i in range(3)])
    bounds=lambda rows:{'min':[min(x[i] for x in rows) for i in range(3)],'max':[max(x[i] for x in rows) for i in range(3)]} if rows else None
    return {'status':'raw_position_projection_only','raw_position_bounds':bounds(position),'front_vertex_ndc_bounds':bounds(clip),
            'front_vertices':len(clip),'vertices_inside_clip':sum(-1<=x<=1 and -1<=y<=1 and 0<=z<=1 for x,y,z in clip),
            'limits':'original VS, bones, morphs, clipping and occlusion not applied; vertex projection is not triangle visibility or RT instance proof'}


def analyze(directory):
    directory=Path(directory);path=directory/'world-runtime.jsonl'
    if not path.is_file():return {'schema':1,'status':'not_requested','pass':True,'samples':[]}
    if path.stat().st_size>4*1024*1024:raise ValueError('Runtime event budget exceeded')
    events=[json.loads(s) for s in path.read_text().splitlines() if s.strip()]
    v20=any(e.get('event')=='world_coverage_revision' and e.get('value') in ('20','21','22','23') for e in events)
    v19=v20 or any(e.get('event')=='world_coverage_revision' and e.get('value')=='19' and e.get('code')==0 for e in events)
    v18=v19 or any(e.get('event')=='world_coverage_revision' and e.get('value')=='18' and e.get('code')==0 for e in events)
    v17=v18 or any(e.get('event')=='world_coverage_revision' and e.get('code')==0 and e.get('value')=='17' for e in events)
    v16=v17 or any(e.get('event')=='world_coverage_revision' and e.get('code')==0 and e.get('value')=='16' for e in events)
    v15=v16 or any(e.get('event')=='world_coverage_revision' and e.get('value')=='15' for e in events)
    v14=v15 or any(e.get('event')=='world_coverage_revision' and e.get('value')=='14' for e in events)
    v13=v14 or any(e.get('event')=='world_coverage_revision' and e.get('value')=='13' for e in events)
    v12=v13 or any(e.get('event')=='world_coverage_revision' and e.get('value')=='12' for e in events)
    v11=v12 or any(e.get('event')=='world_coverage_revision' and e.get('value')=='11' for e in events)
    v10=v11 or any(e.get('event')=='world_coverage_revision' and e.get('value')=='10' for e in events)
    requested=v10 or any(e.get('event')=='world_coverage_revision' and e.get('value')=='9' for e in events)
    skinned=v13 and any(e.get('event')=='scene_input_policy' and e.get('code')==0 and e.get('value')=='skinned_materials_v2' for e in events)
    wide=skinned or v12 and any(e.get('event')=='scene_input_policy' and e.get('code')==0 and e.get('value')=='wide_materials_v1' for e in events)
    sample_limit=222 if v19 else 198 if wide else 34 if v12 else 24 if v10 else 12;stride_limit=128 if v10 else 64;payload_limit=(80 if v10 else 24)*1024*1024
    samples=[e for e in events if e.get('event')=='scene_input_sample'];summaries=[e for e in events if e.get('event')=='sky_rebuild_counts']
    errors=[e['event'] for e in events if e.get('event') in ('scene_input_exception','sky_rebuild_exception','model_rebuild_exception','scene_input_identity_failed','model_payload_write_failed')];reviews=[];ordinals=set();phases=Counter();family_counts=Counter();files=set();used=0
    for sample in samples:
        try:
            phase=sample['phase'];ordinal=sample['ordinal'];phases[phase]+=1
            if phase not in PHASES or phases[phase]>sample_limit or ordinal in ordinals or not isinstance(ordinal,int) or ordinal<=0:raise ValueError('invalid_scene_sample_identity_or_budget')
            if v10:
                group=4 if v19 and sample['shader']=='Infected' else 0 if sample['shader']=='NekoSky' else 1 if sample['shader']=='VertexLitGeneric' else 3 if v12 and sample['shader']=='NekoToon' else 2
                family_counts[(phase,group)]+=1
                if family_counts[(phase,group)]>((6,48,96,48,24) if v19 else (6,48,96,48) if wide else (6,12,10,6) if v12 else (6,8,10))[group]:raise ValueError('scene_family_quota_exceeded')
            ordinals.add(ordinal);payload={}
            for item in sample['files']:
                name=item['file'];file=directory/name
                if Path(name).name!=name or not name.startswith('scene-'+str(ordinal)+'-') or name in files:raise ValueError('unsafe_or_duplicate_scene_file')
                files.add(name)
                if not file.is_file() or not 0<item['size']<=1024*1024 or file.stat().st_size!=item['size']:raise ValueError('missing_or_oversized_scene_file')
                raw=file.read_bytes();used+=len(raw)
                if used>payload_limit or hashlib.sha256(raw).hexdigest()!=item['sha256']:raise ValueError('scene_file_hash_or_total_budget')
                payload[name.split(str(ordinal)+'-',1)[1]]=raw
            auxiliary=sample.get('auxiliary',[])
            if len(auxiliary)>3 or len({a['stream'] for a in auxiliary})!=len(auxiliary):raise ValueError('invalid_auxiliary_streams')
            for a in auxiliary:
                if a['stream'] not in (1,2,3) or not 0<=a['prefix_bytes']<=256 or (a['prefix_bytes'] and len(payload.get('stream-'+str(a['stream'])+'.bin',b''))!=a['prefix_bytes']):raise ValueError('auxiliary_prefix_mismatch')
            for field,name,size in [('integer_constants_hresult','vs-integers.bin',256),('bool_constants_hresult','vs-bools.bin',64),('pixel_constants_hresult','ps-constants.bin',3584 if v13 else 512)]:
                if field in sample and sample[field]==0 and len(payload.get(name,b''))!=size:raise ValueError('missing_optional_shader_constants')
            if v13:
                morph=sample.get('morph_position')
                if not isinstance(morph,dict):raise ValueError('missing_morph_position_metadata')
                if morph:
                    n=morph.get('vertices',0)
                    if not isinstance(n,int) or not 0<=n<=32768:raise ValueError('invalid_morph_vertex_count')
                    if n:
                        if morph.get('hresult')!=0 or morph.get('stream')!=2 or morph.get('frequency')!=1 or not 16<=morph.get('stride',0)<=128 or morph['stride']%4 or morph.get('offset',-1)<0 or morph['offset']%4 or morph.get('first_absolute_vertex')!=sample['first_absolute_vertex'] or n!=sample['vertex_count'] or len(payload.get('morph-positions.bin',b''))!=n*16:raise ValueError('invalid_complete_morph_position')
                        if morph['offset']+(morph['first_absolute_vertex']+n-1)*morph['stride']+16>morph.get('source_buffer_bytes',0):raise ValueError('morph_position_outside_source_buffer')
                    elif payload.get('morph-positions.bin') or morph.get('hresult')==0:raise ValueError('orphan_morph_position_payload')
            review={k:sample[k] for k in ('phase','ordinal','material','shader','format','geometry_status','replay_status')}
            review['shader_bytecode']={k:instructions(v) for k,v in payload.items() if k in ('vs.bin','ps.bin')}
            if sample['geometry_hresult']==0:
                count=sample['vertex_count'];first=sample['first_absolute_vertex'];stride=sample['stride'];kind=sample['kind'];args=sample['args']
                max_vertices=32768 if v13 and sample['shader'] in (('VertexLitGeneric','NekoToon','Infected') if v19 else ('VertexLitGeneric','NekoToon')) else 16384 if v12 and sample['shader'] in (('VertexLitGeneric','NekoToon','Infected') if v19 else ('VertexLitGeneric','NekoToon')) else 4096
                window=wide and sample.get('geometry_window') is True
                if not 0<count<=max_vertices or not 0<=first or (not window and first+count>max_vertices) or not 1<=stride<=stride_limit or len(payload.get('vertices.bin',b''))!=count*stride:raise ValueError('scene_vertex_range_or_payload')
                decl=payload.get('declaration.bin',b'')
                if len(decl)%8 or len(decl)>520 or decl[-8:]!=bytes.fromhex('ff00000011000000'):raise ValueError('invalid_scene_declaration')
                if kind==2:
                    width=sample['index_bytes'];n=args[4]*3 if sample['topology']==4 else args[4]+2
                    if width not in (2,4) or n>(131072 if v13 and sample['shader'] in (('VertexLitGeneric','NekoToon','Infected') if v19 else ('VertexLitGeneric','NekoToon')) else 131072 if v12 and sample['shader']=='NekoToon' else 65536 if v12 and sample['shader']=='VertexLitGeneric' else 16384) or len(payload.get('indices.bin',b''))!=n*width:raise ValueError('invalid_scene_index_payload')
                    indices=struct.unpack('<'+('H' if width==2 else 'I')*n,payload['indices.bin'])
                    signed=args[0] if args[0]<2**31 else args[0]-2**32
                    if (first!=signed+min(indices) or count!=max(indices)-min(indices)+1 if window else first!=signed+args[1] or count!=args[2]) or any(not args[1]<=i<args[1]+args[2] for i in indices):raise ValueError('scene_index_outside_vertices')
                elif kind!=1 or first!=args[0] or count!=(args[1]*3 if sample['topology']==4 else args[1]+2):raise ValueError('scene_draw_range_mismatch')
                if window and (not 0<sample.get('source_vertex_buffer_bytes',0)<=2**32-1 or not 0<=sample.get('stream_offset',-1) or sample['stream_offset']+(first+count)*stride>sample['source_vertex_buffer_bytes']):raise ValueError('scene_window_outside_source_buffer')
                pos=any(struct.unpack_from('<HH4B',decl,i)==(0,0,2,0,0,0) for i in range(0,len(decl),8))
                review['raw_geometry']=projection(sample,payload['vertices.bin']) if pos else {'status':'position_layout_unavailable'}
            else:review['raw_geometry']={'status':'unavailable','hresult':sample['geometry_hresult']}
            if sample['shader_hresult']==0 and any(k not in payload or not instructions(payload[k]).get('ended') for k in ('vs.bin','ps.bin')):raise ValueError('missing_or_malformed_shader_bytecode')
            if sample['constants_hresult']==0 and len(payload.get('constants.bin',b''))!=4096:raise ValueError('missing_shader_constants')
            reviews.append(review)
        except (ValueError,KeyError,TypeError,struct.error,OSError) as e:errors.append(str(e))
    summary_frames=(30,120,180,600) if v15 else (30,120,180,420) if v14 else (30,120,180,240)
    if requested:
        if not samples:errors.append('missing_scene_input_samples')
        if [(e.get('frame'),e.get('phase')) for e in summaries]!=list(zip(summary_frames,PHASES)):errors.append('missing_scene_rebuild_summaries')
        for e in summaries:
            if not all(isinstance(e.get(k),int) and e[k]>=0 for k in ('attempts','ready','reconstructed','fallback','input_samples')) or e['input_samples']>sample_limit:errors.append('invalid_rebuild_counts');continue
            if e['input_samples']!=phases[e['phase']]:errors.append('scene_sample_count_mismatch')
            if e['frame']<(180 if v14 else 240) and e['attempts']:errors.append('premature_sky_reconstruction')
            if e['attempts']!=e['reconstructed']+e['fallback'] or not e['reconstructed']<=e['ready']<=e['attempts'] or sum(e.get('reasons',{}).values())!=e['attempts']:errors.append('inconsistent_rebuild_counts')
    capture_counts=[e for e in events if e.get('event')=='scene_input_counts']
    if wide:
        if [(e.get('frame'),e.get('phase')) for e in capture_counts]!=list(zip(summary_frames,PHASES)):errors.append('missing_wide_capture_summaries')
        for e in capture_counts:
            phase=e.get('phase');position=PHASES.index(phase) if phase in PHASES else 0
            rows=[s for s in samples if s.get('phase')==phase];saved=sum(i['size'] for s in rows for i in s.get('files',[]))
            counts=e.get('family_captured',[]);quota=e.get('family_quota_omitted',[]);payload=e.get('family_payload_omitted',[])
            if any(len(v)!=(5 if v19 else 4) or any(not isinstance(x,int) or x<0 for x in v) for v in (counts,quota,payload)) or counts!=[family_counts[(phase,i)] for i in range(5 if v19 else 4)] or e.get('captured')!=len(rows) or e.get('payload_bytes')!=saved or saved>((16 if v19 else 12 if skinned else 8) if position==3 else 3 if v19 else 2)*1024*1024:errors.append('inconsistent_wide_capture_counts')
            family_bytes=e.get('family_payload_bytes',[])
            group=lambda s:4 if v19 and s['shader']=='Infected' else 0 if s['shader']=='NekoSky' else 1 if s['shader']=='VertexLitGeneric' else 3 if s['shader']=='NekoToon' else 2
            saved_by_family=[sum(f['size'] for s in rows if group(s)==i for f in s.get('files',[])) for i in range(5 if v19 else 4)]
            family_limits=((256,4096,1792,6144,4096) if v19 else (256,4096,1792,6144) if skinned else (256,3072,1792,3072)) if position==3 else (128,640,512,768,512) if v19 else (128,640,512,768)
            if family_bytes!=saved_by_family or any(n>limit*1024 for n,limit in zip(saved_by_family,family_limits)):errors.append('inconsistent_wide_family_payload')
            if e.get('identity_failures',0):errors.append('scene_input_identity_failed')
    models=[e for e in events if e.get('event')=='model_rebuild_counts']
    model_samples=[e for e in events if e.get('event')=='model_rebuild_sample']
    if v11:
        if [(e.get('frame'),e.get('phase')) for e in models]!=(list(zip((30,120,180,240,300,360,420,480,540,600),PHASES[:3]+('expanded_models',)*7)) if v15 else list(zip((30,120,180,240,300,360,420),PHASES[:3]+('expanded_models',)*4)) if v14 else list(zip(summary_frames,PHASES))):errors.append('missing_model_rebuild_summaries')
        for e in models:
            keys=('attempts','ready','reconstructed','fallback')
            if not all(isinstance(e.get(k),int) and e[k]>=0 for k in keys):errors.append('invalid_model_rebuild_counts');continue
            if e['frame']<240 and e['attempts']:errors.append('premature_model_reconstruction')
            if e['attempts']!=e['reconstructed']+e['fallback'] or not e['reconstructed']<=e['ready']<=e['attempts'] or sum(e.get('reasons',{}).values())!=e['attempts']:errors.append('inconsistent_model_rebuild_counts')
        if len(model_samples)>(384 if v15 else 72 if v14 else 64 if v13 else 32) or len({e.get('ordinal') for e in model_samples})!=len(model_samples):errors.append('model_sample_identity_or_budget')
        for e in model_samples:
            if e.get('phase')!='expanded_models' or e.get('reason') not in (('original_vs_model_matrix','original_vs_rigid_bone_zero','original_vs_three_weight','original_vs_neko_three_weight') if v13 else ('original_vs_model_matrix','original_vs_rigid_bone_zero')) or e.get('rebuilt_stride')!=32 or not 0<e.get('rebuilt_vertex_bytes',0)<=(32768 if v13 else 16384 if v12 else 4096)*32:errors.append('invalid_model_rebuild_sample')
            matrix=e.get('applied_world',[])
            if len(matrix)!=16 or not all(isinstance(v,(float,int)) and math.isfinite(v) for v in matrix):errors.append('invalid_model_rebuild_world')
        if not v16 and models and models[-1].get('reconstructed',0)>0 and not model_samples:errors.append('missing_model_rebuild_samples')
    model_payload_bytes=verify_model_payloads(directory,model_samples,errors,revision=15 if v15 else 14) if v14 and not v16 else 0
    from analyze_rt_live import analyze as analyze_live
    live=analyze_live(directory,events) if v16 else None
    if live:errors.extend(live["errors"])
    final=summaries[-1] if summaries else {}
    rebuilt=final.get('reconstructed',0);attempts=final.get('attempts',0)
    return {'schema':1,'status':'valid_evidence_requires_review' if requested and not errors else 'incomplete' if requested else 'not_requested',
            **({'live_mesh':live} if v16 else {}),'pass':not errors,'errors':sorted(set(errors)),'capture_policy':'skinned_materials_v2' if skinned else 'wide_materials_v1' if wide else 'legacy','capture_summaries':capture_counts,'sample_counts':dict(phases),'payload_bytes':used,'samples':reviews,'sky_rebuild_summaries':summaries,
            **({'model_payload_bytes':model_payload_bytes} if v14 else {}),'model_rebuild_summaries':models,'model_rebuild_samples':model_samples,
            'model_rebuild_result':'applied_requires_image_review' if models and models[-1].get('reconstructed',0)>0 else 'no_model_reconstruction' if v11 else 'not_requested',
            'sky_rebuild_result':'applied_requires_image_review' if attempts and rebuilt==attempts else 'fallback_preserved_v8' if attempts and not rebuilt else 'partial_or_unrun',
            'limits':'verified input files and bounded raw position projection; shader replay API success is not image acceptance, per-object visibility or Source threading evidence'}


def verify_model_payloads(directory,samples,errors,revision=14):
    used=0;files=set();stage_bytes=Counter()
    stage_counts=Counter()
    frames={'neko':240,'neko_preserve':300,'neko_recheck':360,'combined':420,'combined_matte':480,'combined_directional':540} if revision==15 else {'ordinary':240,'neko':300,'combined':360}
    for e in samples:
        try:
            stage=e['model_stage'];stage_counts[stage]+=1
            if stage_counts[stage]>(64 if revision==15 else 24):raise ValueError('model_stage_sample_budget')
            if stage not in frames or any(e.get(k) is not True for k in ('upload_verified','binding_verified','owned_draw')):raise ValueError('unverified_model_submission')
            if e.get('frame')!=frames[stage]:raise ValueError('model_submission_readback_frame_mismatch')
            payload={}
            for item in e['files']:
                name=item['file'];path=directory/name
                if Path(name).name!=name or not name.startswith('model-'+str(e['ordinal'])+'-') or name in files:raise ValueError('unsafe_model_payload_identity')
                files.add(name)
                if not path.is_file() or not 0<item['size']<=1024*1024 or path.stat().st_size!=item['size']:raise ValueError('invalid_model_payload_size')
                raw=path.read_bytes();used+=len(raw);stage_bytes[stage]+=len(raw)
                if hashlib.sha256(raw).hexdigest()!=item['sha256']:raise ValueError('model_payload_hash_mismatch')
                payload[name.split(str(e['ordinal'])+'-',1)[1]]=raw
            limit=(16 if stage=='combined' else 12 if stage in ('neko','neko_preserve','neko_recheck') else 0) if revision==15 else (2 if stage=='neko' else 1)
            if stage_bytes[stage]>limit*1024*1024:raise ValueError('model_payload_budget_exceeded')
            if revision==15:
                if e.get('preserve_requested')!=(stage=='neko_preserve') or e.get('neutral_material')!=(stage=='combined_matte'):raise ValueError('model_submission_policy_mismatch')
                if any(not isinstance(e.get(k),str) or len(e[k])!=64 or any(c not in '0123456789abcdef' for c in e[k]) for k in ('vertices_sha256','indices_sha256')):raise ValueError('missing_model_submission_hash')
                if e.get('material_query_failure_bits')!=0 or e.get('material_stage0')!=[2,3 if stage=='combined_matte' else 2,2,2]:raise ValueError('model_material_binding_mismatch')
                if stage=='combined_matte' and e.get('material_texture_factor')!=0xff808080:raise ValueError('model_neutral_factor_mismatch')
                for suffix,key in [('vertices.bin','vertices_sha256'),('indices.bin','indices_sha256')]:
                    if suffix in payload and hashlib.sha256(payload[suffix]).hexdigest()!=e[key]:raise ValueError('model_submission_payload_digest_mismatch')
            if e.get('payload_omitted') is True:
                if payload:raise ValueError('omitted_model_payload_present')
                continue
            vertices=payload['vertices.bin'];args=e['draw_args'];width=e['index_bytes']
            if len(vertices)!=e['rebuilt_vertex_bytes'] or len(vertices)%32 or len(args)!=6 or args[0]!=0:raise ValueError('invalid_compact_model_vertices')
            for at in range(0,len(vertices),32):
                if not all(math.isfinite(v) for v in struct.unpack_from('<5f',vertices,at)):raise ValueError('nonfinite_model_submission')
            if width:
                indices=payload['indices.bin']
                if width not in (2,4) or len(indices)%width or args[1]!=0 or args[2]!=len(vertices)//32 or args[3]!=0:raise ValueError('invalid_compact_model_indices')
                values=struct.unpack('<'+('H' if width==2 else 'I')*(len(indices)//width),indices)
                if not values or max(values)>=args[2]:raise ValueError('compact_model_index_outside_vertices')
            original=e['original_args'];count=e['source_count'];first=e['source_first'];topology=e['topology'];kind=e['kind']
            if len(original)!=6 or count!=len(vertices)//32 or e['source_stride']!=32 or len(payload['source-vertices.bin'])!=count*32 or topology not in (4,5):raise ValueError('unpaired_model_source_geometry')
            if len(payload['constants.bin'])!=4096 or any(not instructions(payload[k]).get('ended') for k in ('vs.bin','ps.bin')):raise ValueError('missing_paired_model_program')
            declaration=payload['declaration.bin']
            if len(declaration)%8 or declaration[-8:]!=bytes.fromhex('ff00000011000000'):raise ValueError('missing_paired_model_declaration')
            if struct.unpack_from('<f',payload['constants.bin'],13*16)[0]!=0 and len(payload.get('morph-positions.bin',b''))!=count*16:raise ValueError('missing_paired_model_morph')
            if kind==2:
                source=payload['source-indices.bin'];signed=original[0] if original[0]<2**31 else original[0]-2**32
                if not width or len(source)!=len(indices) or first!=signed+original[1] or count!=original[2]:raise ValueError('paired_model_source_index_range')
                source_values=struct.unpack('<'+('H' if width==2 else 'I')*(len(source)//width),source)
                expected=tuple(signed+i-first for i in source_values)
                if values!=expected or args[4]!=original[4] or len(values)!=(args[4]*3 if topology==4 else args[4]+2):raise ValueError('model_index_correspondence_changed')
            elif kind!=1 or first!=original[0] or args[1]!=original[1] or count!=(args[1]*3 if topology==4 else args[1]+2):raise ValueError('paired_model_nonindexed_range')
        except (KeyError,ValueError,TypeError,OSError,struct.error) as exc:errors.append(str(exc))
    return used

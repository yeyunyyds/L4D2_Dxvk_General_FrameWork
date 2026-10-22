"""Review v24+ native edge evidence and nested Host elapsed timing windows."""
from collections import defaultdict
import hashlib
import json
import math
from pathlib import Path


def analyze(directory):
    root=Path(directory);events=[];errors=[]
    for name in ('world-runtime.jsonl','preview-runtime.jsonl'):
        p=root/name
        if p.is_file() and p.stat().st_size<=32*1024*1024:
            events.extend(json.loads(line) for line in p.read_text(encoding='utf-8').splitlines() if line.strip())
    revisions=[int(e['value']) for e in events if e.get('event')=='pilot_delivery_revision' and str(e.get('value','')).isdigit()]
    performance=any(e.get('event')=='performance_ramp_begin' and e.get('code')==0 for e in events)
    requested=any(value>=24 for value in revisions);gpu_pilot=any(25<=value<27 for value in revisions)
    cpu_rollback=any(value==27 for value in revisions);bounded_canary=any(value==28 for value in revisions)
    safety_limit=any(value==29 for value in revisions);density_limit=any(value>=30 for value in revisions)
    if not requested:return {'schema':1,'status':'not_requested','pass':True}
    windows=[e for e in events if e.get('event')=='runtime_timing_window'];totals=defaultdict(lambda:defaultdict(lambda:{'calls':0,'elapsed_ms':0.,'max_ms':0.}))
    for e in windows:
        if not math.isfinite(e.get('wall_ms',-1)) or e['wall_ms']<0:errors.append('invalid_timing_wall');continue
        for name,counter in e.get('scopes',{}).items():
            calls=counter.get('calls',-1);elapsed=counter.get('elapsed_ms',-1);maximum=counter.get('max_ms',-1)
            if not isinstance(calls,int) or calls<0 or not all(isinstance(v,(int,float)) and math.isfinite(v) and v>=0 for v in (elapsed,maximum)) or maximum>elapsed+0.001:
                errors.append('invalid_timing_counter');continue
            target=totals[e.get('segment','unknown')][name];target['calls']+=calls;target['elapsed_ms']+=elapsed;target['max_ms']=max(target['max_ms'],maximum)
    if not windows:errors.append('missing_runtime_timing')
    taa=[e for e in events if e.get('event')=='native_taa_requested']
    if not performance and (len(taa)!=1 or any(taa[0].get(k)!=v for k,v in {'scale_code':0,'taa_code':0,'resolution_scale':1,'upscaler_type':3}.items())):errors.append('missing_or_failed_native_taa_request')
    samples=[]
    for e in events:
        if e.get('event')!='native_edge_readback':continue
        name=e.get('file','');path=root/name;width=e.get('width',0);height=e.get('height',0)
        okay=(e.get('mode') in ('no-aa','taa') and name=='native-'+e['mode']+'.bgra8' and isinstance(width,int) and isinstance(height,int) and
              0<width<=4096 and 0<height<=2160 and width*height*4<=16*1024*1024 and e.get('resampled') is False and
              e.get('copy_code')==0 and e.get('hresult')==0 and e.get('written') is True and
              path.is_file() and path.stat().st_size==width*height*4 and
              all(isinstance(e.get(k),int) and e[k]>=0 for k in ('x','y','source_width','source_height')) and
              e['x']+width<=e['source_width']<=4096 and e['y']+height<=e['source_height']<=2160)
        samples.append(dict(e,payload_valid=okay,sha256=hashlib.sha256(path.read_bytes()).hexdigest() if okay else None))
    edge_complete=len(samples)==2 and {x.get('mode') for x in samples}=={'no-aa','taa'} and all(x['payload_valid'] for x in samples)
    gpu_status=root/'gpu-engine-status.json'
    gpu=json.loads(gpu_status.read_text(encoding='utf-8')) if gpu_status.is_file() else {'status':'unavailable','error':'missing_sampler_status'}
    skin={'status':'not_requested'}
    if gpu_pilot or cpu_rollback or bounded_canary or safety_limit or density_limit:
        frames=[e for e in events if e.get('event')=='api_mesh_frame'];ends=[e for e in events if e.get('event')=='preview_end']
        keys=('skinned_submitted','skinned_created','skinned_reused')
        valid=all(all(isinstance(e.get(k),int) and e[k]>=0 for k in keys) for e in frames+ends)
        if not frames or len(ends)!=1 or not valid:errors.append('missing_or_invalid_gpu_skinning_counters')
        elif gpu_pilot:
            if ends[0]['skinned_submitted']==0:errors.append('gpu_skinning_not_exercised')
            elif ends[0]['skinned_created']+ends[0]['skinned_reused']!=ends[0]['skinned_submitted'] or ends[0]['skinned_created']>ends[0]['skinned_submitted']:
                errors.append('invalid_gpu_skinning_accounting')
        elif cpu_rollback and any(e[k] for e in frames+ends for k in keys):errors.append('gpu_skinning_safety_rollback_not_effective')
        elif bounded_canary or safety_limit:
            bounded=all(isinstance(e.get('skinned_frame_submitted'),int) and 0<=e['skinned_frame_submitted']<=16 and e.get('skinned_frame_limit')==16 for e in frames)
            if not bounded:errors.append('gpu_skinning_canary_frame_limit')
            if not ends[0]['skinned_submitted'] or not any(e.get('skinned_frame_submitted',0)>0 for e in frames):errors.append('gpu_skinning_canary_not_exercised')
            if ends[0]['skinned_created']+ends[0]['skinned_reused']!=ends[0]['skinned_submitted']:errors.append('invalid_gpu_skinning_accounting')
        elif performance:
            expected=[4,64,256,1024]
            preview_keys=('preview_skinned_submitted','preview_skinned_created','preview_skinned_reused')
            ramp_valid=len(frames)==4 and [e.get('gpu_skin_performance_phase') for e in frames]==list(range(4)) and [e.get('skinned_frame_limit') for e in frames]==expected and all(
                isinstance(e.get('skinned_frame_submitted'),int) and 0<=e['skinned_frame_submitted']<=e['skinned_frame_limit'] and e.get('skinned_frame_max')==1024 and
                all(isinstance(e.get(k),int) and e[k]>=0 for k in preview_keys) for e in frames)
            previous={k:0 for k in preview_keys}
            for e in frames:
                submitted=e.get('preview_skinned_submitted',0)-previous['preview_skinned_submitted']
                created=e.get('preview_skinned_created',0)-previous['preview_skinned_created']
                reused=e.get('preview_skinned_reused',0)-previous['preview_skinned_reused']
                if submitted<=0 or created<0 or reused<0 or created+reused!=submitted:ramp_valid=False
                previous={k:e.get(k,0) for k in preview_keys}
            if not ramp_valid:errors.append('invalid_gpu_skinning_performance_ramp')
            if ends[0]['skinned_created']+ends[0]['skinned_reused']!=ends[0]['skinned_submitted']:errors.append('invalid_gpu_skinning_accounting')
        elif density_limit:
            density_valid=all(isinstance(e.get('gpu_skin_previous_frame_draws'),int) and e['gpu_skin_previous_frame_draws']>=0 and
                              e.get('gpu_skin_density_limit')==256 and e.get('gpu_skin_high_density_limit')==4 and
                              isinstance(e.get('gpu_skin_density_throttle_frames'),int) and e['gpu_skin_density_throttle_frames']>=0 and
                              isinstance(e.get('skinned_frame_submitted'),int) and e.get('skinned_frame_max')==16 and
                              e.get('skinned_frame_limit')==(0 if e['gpu_skin_previous_frame_draws']==0 else 16 if e['gpu_skin_previous_frame_draws']<=256 else 4) and
                              0<=e['skinned_frame_submitted']<=e['skinned_frame_limit'] for e in frames)
            if not density_valid:errors.append('invalid_gpu_skinning_density_policy')
            throttle=ends[0].get('gpu_skin_density_throttle_frames',-1)
            if not isinstance(throttle,int) or throttle<0:errors.append('invalid_gpu_skinning_density_throttle_count')
            if not ends[0]['skinned_submitted'] or not any(e.get('skinned_frame_submitted',0)>0 for e in frames):errors.append('gpu_skinning_density_policy_not_exercised')
            if ends[0]['skinned_created']+ends[0]['skinned_reused']!=ends[0]['skinned_submitted']:errors.append('invalid_gpu_skinning_accounting')
        bounded_ok=valid and frames and len(ends)==1 and ends[0].get('skinned_submitted',0)>0 and all(0<=e.get('skinned_frame_submitted',-1)<=16 and e.get('skinned_frame_limit')==16 for e in frames)
        density_ok=density_limit and not performance and valid and frames and len(ends)==1 and ends[0].get('skinned_submitted',0)>0 and not any(error in errors for error in ('invalid_gpu_skinning_density_policy','invalid_gpu_skinning_density_throttle_count','gpu_skinning_density_policy_not_exercised','invalid_gpu_skinning_accounting'))
        performance_ok=performance and valid and frames and len(ends)==1 and not any(error in errors for error in ('invalid_gpu_skinning_performance_ramp','invalid_gpu_skinning_accounting'))
        status=('performance_ramp_requires_review' if performance_ok else 'density_adaptive_requires_image_review' if density_ok else 'safety_limit_requires_image_review' if safety_limit and bounded_ok else 'bounded_canary_requires_image_review' if bounded_canary and bounded_ok
                else 'disabled_cpu_fallback' if cpu_rollback and valid and frames and len(ends)==1 and not any(e[k] for e in frames+ends for k in keys)
                else 'exercised_requires_image_review' if gpu_pilot and valid and ends and ends[0].get('skinned_submitted',0)>0 else 'unexercised_or_incomplete')
        skin={'status':status,
              'preview':{k:ends[0].get(k) for k in keys} if ends else {},
              'mesh_frames':len(frames),'limits':'v31 performance mode ramps GPU skin limits through 4, 64, 256 and 1024 for 60 frames each without the duplicate full CPU skin oracle. Normal preview retains the v30 density policy. Image review remains required.' if performance else 'v27 requires zero persistent skinning. v28-v29 enforce at most 16 successful GPU-skinned submissions per frame. v30 keeps 16 at up to 256 preceding-frame model submissions and a 4-submission GPU baseline above that density; the warm-up frame and excess models use CPU poses. Image review remains required.'}
    return {'schema':1,'pass':not errors,'status':'valid_evidence_requires_review' if not errors else 'incomplete','errors':sorted(set(errors)),
            'timing_windows':len(windows),'window_timeline':[{k:e.get(k) for k in ('frame','first_after_frame','segment','wall_ms','qpc_begin','qpc_end','qpc_frequency','utc_filetime_100ns')} for e in windows],'elapsed_scopes_by_segment':dict(totals),'native_taa_requests':taa,
            'native_edge_payloads':samples,'native_edge_evidence':'not_requested_performance_ramp' if performance else 'complete_requires_image_review' if edge_complete else 'unavailable_or_incomplete',
            'source_shader_census':[e for e in events if e.get('event')=='source_shader_census'],'gpu_skinning':skin,
            'gpu_engine_collection':gpu,'limits':'Nested QPC scopes include blocking and overlap; do not sum. PDH engines overlap; do not sum as whole-GPU utilization. Sequential dynamic scenes are not matched A/B. Queued TAA request is not proof of effective image quality; motion-history/ghosting requires review.'}

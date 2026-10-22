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
    requested=any(value>=24 for value in revisions);gpu_pilot=any(25<=value<27 for value in revisions);cpu_rollback=any(value>=27 for value in revisions)
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
    if len(taa)!=1 or any(taa[0].get(k)!=v for k,v in {'scale_code':0,'taa_code':0,'resolution_scale':1,'upscaler_type':3}.items()):errors.append('missing_or_failed_native_taa_request')
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
    if gpu_pilot or cpu_rollback:
        frames=[e for e in events if e.get('event')=='api_mesh_frame'];ends=[e for e in events if e.get('event')=='preview_end']
        keys=('skinned_submitted','skinned_created','skinned_reused')
        valid=all(all(isinstance(e.get(k),int) and e[k]>=0 for k in keys) for e in frames+ends)
        if not frames or len(ends)!=1 or not valid:errors.append('missing_or_invalid_gpu_skinning_counters')
        elif gpu_pilot:
            if ends[0]['skinned_submitted']==0:errors.append('gpu_skinning_not_exercised')
            elif ends[0]['skinned_created']+ends[0]['skinned_reused']!=ends[0]['skinned_submitted'] or ends[0]['skinned_created']>ends[0]['skinned_submitted']:
                errors.append('invalid_gpu_skinning_accounting')
        elif any(e[k] for e in frames+ends for k in keys):errors.append('gpu_skinning_safety_rollback_not_effective')
        status=('disabled_cpu_fallback' if cpu_rollback and valid and frames and len(ends)==1 and not any(e[k] for e in frames+ends for k in keys)
                else 'exercised_requires_image_review' if gpu_pilot and valid and ends and ends[0].get('skinned_submitted',0)>0 else 'unexercised_or_incomplete')
        skin={'status':status,
              'preview':{k:ends[0].get(k) for k in keys} if ends else {},
              'mesh_frames':len(frames),'limits':'For v27+, zero counters prove the unstable persistent GPU-skin path stayed isolated; image review still validates CPU-skinned animated pixels.'}
    return {'schema':1,'pass':not errors,'status':'valid_evidence_requires_review' if not errors else 'incomplete','errors':sorted(set(errors)),
            'timing_windows':len(windows),'window_timeline':[{k:e.get(k) for k in ('frame','first_after_frame','segment','wall_ms','qpc_begin','qpc_end','qpc_frequency','utc_filetime_100ns')} for e in windows],'elapsed_scopes_by_segment':dict(totals),'native_taa_requests':taa,
            'native_edge_payloads':samples,'native_edge_evidence':'complete_requires_image_review' if edge_complete else 'unavailable_or_incomplete',
            'source_shader_census':[e for e in events if e.get('event')=='source_shader_census'],'gpu_skinning':skin,
            'gpu_engine_collection':gpu,'limits':'Nested QPC scopes include blocking and overlap; do not sum. PDH engines overlap; do not sum as whole-GPU utilization. Sequential dynamic scenes are not matched A/B. Queued TAA request is not proof of effective image quality; motion-history/ghosting requires review.'}

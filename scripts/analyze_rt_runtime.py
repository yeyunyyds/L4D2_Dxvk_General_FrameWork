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
    stability=any(e.get('event')=='performance_stability_begin' for e in events)
    lifecycle=any(e.get('event')=='performance_lifecycle_begin' for e in events)
    performance=stability or lifecycle or any(e.get('event')=='performance_ramp_begin' and e.get('code')==0 for e in events)
    requested=any(value>=24 for value in revisions);gpu_pilot=any(25<=value<27 for value in revisions)
    cpu_rollback=any(value==27 for value in revisions);bounded_canary=any(value==28 for value in revisions);custom_profile=any(value>=36 for value in revisions)
    native_fallback=any(value==38 for value in revisions);complete_fallback=any(value>=39 for value in revisions)
    scaling=any(value==40 for value in revisions);optimized=any(value==41 for value in revisions);rebalanced=any(value==42 for value in revisions);structural=any(value>=43 for value in revisions);persistent=any(value>=44 for value in revisions)
    safety_limit=any(value==29 for value in revisions);density_limit=any(value>=30 for value in revisions);pose_slots=any(value>=33 for value in revisions)
    if not requested:return {'schema':1,'status':'not_requested','pass':True}
    windows=[e for e in events if e.get('event')=='runtime_timing_window'];totals=defaultdict(lambda:defaultdict(lambda:{'calls':0,'elapsed_ms':0.,'max_ms':0.}))
    for e in windows:
        if not math.isfinite(e.get('wall_ms',-1)) or e['wall_ms']<0:errors.append('invalid_timing_wall');continue
        if custom_profile:
            thread_cpu=e.get('thread_cpu_ms',-1)
            if e.get('thread_times_available') is not True or not isinstance(thread_cpu,(int,float)) or not math.isfinite(thread_cpu) or not 0<=thread_cpu<=e['wall_ms']+1:errors.append('invalid_thread_cpu_timing')
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
        # The same F9 session emits diagnostic mesh counters before the
        # dedicated performance preview. Only explicitly numbered ramp frames
        # belong to the four-phase performance contract.
        if performance:frames=[e for e in frames if e.get('gpu_skin_performance_phase',-1)>=0 and
            (e.get('stage','api_continuous_preview')=='api_continuous_preview' if lifecycle else e.get('preview_submitted',0)>0)]
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
        elif performance and structural:
            expected=[80,0,0];flushes=[e for e in events if e.get('event')=='frame_batch_flush']
            frame_batch_ok=(len(frames)==3 and [e.get('gpu_skin_performance_phase') for e in frames]==[0,1,2] and
                [e.get('skinned_frame_limit') for e in frames]==expected and frames[0].get('skinned_frame_max')==80 and
                isinstance(frames[0].get('skinned_frame_submitted'),int) and 0<frames[0]['skinned_frame_submitted']<=80 and
                frames[0].get('skinned_frame_unique_meshes')==frames[0]['skinned_frame_submitted'] and frames[0].get('skinned_frame_repeat_draws')==0 and
                all(e.get('skinned_frame_max')==80 and e.get('skinned_frame_submitted')==0 for e in frames[1:]) and len(flushes)==60 and
                all(all(isinstance(e.get(k),int) and e[k]>=0 for k in ('source_draws','source_vertices','source_indices','batches','submitted_source_draws','worker_submitted','worker_completed','worker_failed','worker_threads','worker_max_active')) and
                    e['source_draws']>0 and e['source_vertices']>0 and e['source_indices']>0 and e['batches']>0 and
                    e['submitted_source_draws']==e['source_draws'] and e['worker_failed']==0 and e['worker_threads']==4 for e in flushes))
            source=sum(e.get('source_draws',0) for e in flushes);batches=sum(e.get('batches',0) for e in flushes);end=ends[0] if len(ends)==1 else {}
            frame_batch_ok=frame_batch_ok and batches<source and max((e.get('worker_max_active',0) for e in flushes),default=0)>=2 and all([
                end.get('complete') is True,end.get('rt_frames')==90,end.get('raster_survival_frames',0)>=60,end.get('resources_recovered') is True,
                end.get('frame_batch_flushes')==60,end.get('frame_batch_source_draws')==source,end.get('frame_batch_submitted_source_draws')==source,
                end.get('frame_batch_failures')==0,end.get('frame_batch_worker_submitted')==source,end.get('frame_batch_worker_completed')==source,
                end.get('frame_batch_worker_failed')==0,end.get('frame_batch_worker_threads')==4,end.get('frame_batch_worker_max_active',0)>=2])
            profile=[e for e in events if e.get('event')=='performance_lifecycle_begin']
            recovered=[e for e in events if e.get('event')=='performance_resources_recovered' and e.get('code')==0]
            survived=[e for e in events if e.get('event')=='performance_raster_survival_complete' and e.get('code')==0]
            frame_batch_ok=frame_batch_ok and len(profile)==1 and profile[0].get('code')==0 and len(recovered)==1 and len(survived)==1 and all(end.get(k)==0 for k in ('entries','pending_fences','held'))
            if any(value>=44 for value in revisions):
                warm,persistent=flushes[:30],flushes[30:]
                frame_batch_ok=frame_batch_ok and all(e.get('persistent_phase') is False for e in warm) and bool(persistent) and all(
                    e.get('persistent_phase') is True and isinstance(e.get('persistent_created'),int) and isinstance(e.get('persistent_updated'),int)
                    for e in persistent) and persistent[0].get('persistent_created',0)>0 and all(
                    e.get('persistent_created')==0 and e.get('persistent_updated',0)>0 for e in persistent[1:]) and end.get('persistent_mesh_updates')==sum(
                    e.get('persistent_updated',0) for e in persistent)
            if not frame_batch_ok:errors.append('invalid_structural_frame_batch')
            if ends[0]['skinned_created']+ends[0]['skinned_reused']!=ends[0]['skinned_submitted']:errors.append('invalid_gpu_skinning_accounting')
        elif performance:
            expected=([64,80,96] if rebalanced else [64,64,64] if optimized else [16,64,128] if scaling else [0,8,16]) if lifecycle else [32]*4 if stability else [16,32,48,64]
            preview_keys=('preview_skinned_submitted','preview_skinned_created','preview_skinned_reused')
            ramp_valid=len(frames)==len(expected) and [e.get('gpu_skin_performance_phase') for e in frames]==list(range(len(expected))) and [e.get('skinned_frame_limit') for e in frames]==expected and all(
                isinstance(e.get('skinned_frame_submitted'),int) and 0<=e['skinned_frame_submitted']<=e['skinned_frame_limit'] and e.get('skinned_frame_max')==((96 if rebalanced else 64 if optimized else 128 if scaling else 16) if lifecycle else 32 if stability else 64) and
                isinstance(e.get('skinned_frame_unique_meshes'),int) and isinstance(e.get('skinned_frame_repeat_draws'),int) and
                e['skinned_frame_unique_meshes']+e['skinned_frame_repeat_draws']==e['skinned_frame_submitted'] and
                (not pose_slots or (e['skinned_frame_unique_meshes']==e['skinned_frame_submitted'] and e['skinned_frame_repeat_draws']==0)) and
                isinstance(e.get('preview_submitted'),int) and
                (e['preview_submitted']==e.get('preview_skinned_submitted') if not complete_fallback else
                 all(isinstance(e.get(k),int) and e[k]>=0 for k in ('preview_cpu_pose_submitted','preview_cpu_pose_created','preview_cpu_pose_reused')) and
                 e['preview_cpu_pose_submitted']>0 and e['preview_cpu_pose_created']+e['preview_cpu_pose_reused']==e['preview_cpu_pose_submitted'] and
                 e['preview_submitted']==e.get('preview_skinned_submitted',0)+e['preview_cpu_pose_submitted']) and
                all(isinstance(e.get(k),int) and e[k]>=0 for k in preview_keys) for e in frames)
            if stability:
                profile=[e for e in events if e.get('event')=='performance_stability_begin']
                ramp_valid=ramp_valid and len(profile)==1 and profile[0].get('code')==0
            if lifecycle:
                profile=[e for e in events if e.get('event')=='performance_lifecycle_begin']
                ends_lifecycle=[e for e in events if e.get('event')=='preview_end']
                recovered=[e for e in events if e.get('event')=='performance_resources_recovered' and e.get('code')==0]
                survived=[e for e in events if e.get('event')=='performance_raster_survival_complete' and e.get('code')==0]
                target_rt_frames=90 if complete_fallback else 210
                ramp_valid=ramp_valid and len(profile)==1 and profile[0].get('code')==0 and len(recovered)==1 and len(survived)==1 and len(ends_lifecycle)==1 and ends_lifecycle[0].get('complete') is True and ends_lifecycle[0].get('rt_frames')==target_rt_frames and ends_lifecycle[0].get('raster_survival_frames',0)>=60 and ends_lifecycle[0].get('resources_recovered') is True and all(ends_lifecycle[0].get(k)==0 for k in ('entries','pending_fences','held'))
                if native_fallback:
                    ramp_valid=ramp_valid and isinstance(ends_lifecycle[0].get('original_fallback_draws'),int) and ends_lifecycle[0]['original_fallback_draws']>0
                if complete_fallback:
                    ramp_valid=ramp_valid and all(isinstance(ends_lifecycle[0].get(k),int) and ends_lifecycle[0][k]>=0 for k in ('cpu_pose_submitted','cpu_pose_created','cpu_pose_reused')) and ends_lifecycle[0]['cpu_pose_submitted']>0 and ends_lifecycle[0]['cpu_pose_created']+ends_lifecycle[0]['cpu_pose_reused']==ends_lifecycle[0]['cpu_pose_submitted'] and ends_lifecycle[0].get('submitted')==ends_lifecycle[0].get('skinned_submitted',0)+ends_lifecycle[0]['cpu_pose_submitted'] and ends_lifecycle[0].get('budget_omitted')==0
            warmup=[e for e in events if e.get('event')=='api_mesh_frame' and e.get('gpu_skin_performance_phase')==-1 and e.get('preview_submitted',0)>0]
            previous={k:(warmup[-1].get(k,0) if stability and warmup else 0) for k in preview_keys}
            for index,e in enumerate(frames):
                submitted=e.get('preview_skinned_submitted',0)-previous['preview_skinned_submitted']
                created=e.get('preview_skinned_created',0)-previous['preview_skinned_created']
                reused=e.get('preview_skinned_reused',0)-previous['preview_skinned_reused']
                zero_baseline=lifecycle and index==0 and not scaling and not optimized and not rebalanced
                if (zero_baseline and (submitted or created or reused)) or (not zero_baseline and submitted<=0) or created<0 or reused<0 or created+reused!=submitted:ramp_valid=False
                previous={k:e.get(k,0) for k in preview_keys}
            if not ramp_valid:errors.append('invalid_gpu_skinning_performance_ramp')
            if ends[0]['skinned_created']+ends[0]['skinned_reused']!=ends[0]['skinned_submitted']:errors.append('invalid_gpu_skinning_accounting')
            if not complete_fallback and ends[0].get('submitted')!=ends[0].get('skinned_submitted'):errors.append('performance_cpu_pose_submission')
            if complete_fallback and (ends[0].get('cpu_pose_submitted',0)<=0 or ends[0].get('cpu_pose_created',-1)+ends[0].get('cpu_pose_reused',-1)!=ends[0].get('cpu_pose_submitted') or ends[0].get('submitted')!=ends[0].get('skinned_submitted',0)+ends[0].get('cpu_pose_submitted',0) or ends[0].get('budget_omitted')!=0):errors.append('performance_complete_cpu_pose_fallback')
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
        performance_ok=performance and valid and frames and len(ends)==1 and not any(error in errors for error in ('invalid_structural_frame_batch','invalid_gpu_skinning_performance_ramp','invalid_gpu_skinning_accounting','performance_cpu_pose_submission','performance_complete_cpu_pose_fallback'))
        status=('performance_persistent_mesh_requires_visual_review' if performance_ok and persistent else 'performance_structural_batch_requires_visual_review' if performance_ok and structural else 'performance_complete_lifecycle_requires_visual_review' if performance_ok and lifecycle and complete_fallback else 'performance_hybrid_lifecycle_requires_visual_review' if performance_ok and lifecycle and native_fallback else 'performance_lifecycle_requires_review' if performance_ok and lifecycle else 'performance_stability_requires_review' if performance_ok and stability else 'performance_ramp_requires_review' if performance_ok else 'density_adaptive_requires_image_review' if density_ok else 'safety_limit_requires_image_review' if safety_limit and bounded_ok else 'bounded_canary_requires_image_review' if bounded_canary and bounded_ok
                else 'disabled_cpu_fallback' if cpu_rollback and valid and frames and len(ends)==1 and not any(e[k] for e in frames+ends for k in keys)
                else 'exercised_requires_image_review' if gpu_pilot and valid and ends and ends[0].get('skinned_submitted',0)>0 else 'unexercised_or_incomplete')
        if not performance:
            skin_limits='v27 requires zero persistent skinning. v28-v29 enforce at most 16 successful GPU-skinned submissions per frame. v30 keeps 16 at up to 256 preceding-frame model submissions and a 4-submission GPU baseline above that density; the warm-up frame and excess models use CPU poses. Image review remains required.'
        elif lifecycle:
            skin_limits='v46 compares GPU-80, ordinary frame batching, and persistent dynamic meshes whose topology hashes stay stable from the first Runtime upsert; vertex changes must reuse a BlasEntry and select BVH update/refit before all resources retire and 60 raster Presents survive.' if any(value>=46 for value in revisions) else 'v45 compares GPU-80, ordinary frame batching, and persistent same-handle dynamic mesh updates in one run; expected unsupported model passes fall back per Draw, steady persistent frames must issue no new dynamic CreateMesh calls, then all resources retire before 60 raster Presents.' if any(value>=44 for value in revisions) else 'v43 keeps a 30-frame GPU-80 explicit oracle, then audits 60 frames of four-worker CPU reconstruction and material/transform batching with independent Source-draw and API-batch accounting, followed by full retirement and 60 raster Presents. Visual completeness requires review.' if structural else 'v42 retains the optimized complete CPU-pose fallback and measures persistent GPU skinning at 64, 80 and 96 submissions per frame for 30 frames each, then requires resource retirement and 60 raster Presents. Visual completeness requires review.' if rebalanced else 'v41 fixes persistent GPU skinning at the validated limit of 64 for three 30-frame windows and optimizes the complete CPU-pose fallback, then requires resource retirement and 60 raster Presents. Visual completeness requires review.' if optimized else 'v40 retains complete explicit CPU-pose fallback and scales persistent GPU skinning through 16, 64 and 128 submissions per frame for 30 frames each, then requires resource retirement and 60 surviving raster Presents. Visual completeness requires review.' if scaling else 'v39 disables global shader vertex capture and restores explicit CPU-pose meshes for every accepted part outside the persistent GPU-skin budget. It compares complete CPU, GPU-8 and GPU-16 scenes for 30 frames each, then requires resource retirement and 60 raster Presents. Visual completeness requires review.' if complete_fallback else 'v38 hybrid lifecycle keeps native vertex-captured programmable draws when an explicit replacement is unsupported or outside the 0/8/16 budget; only successfully replaced draws receive the Remix Ignore category. It still requires full RT resource retirement and 60 surviving raster Presents. Visual completeness requires review.' if native_fallback else 'v37 lifecycle mode uses explicit ReSTIR GI, 30 static RT frames, 60 RT frames at 8, 120 RT frames at 16, full RT resource retirement and 60 surviving raster Presents. CPU-pose meshes remain omitted; this is lifecycle evidence, not image acceptance.'
        elif stability:
            skin_limits=('v36 retains the v35 fixed-32 Arc stability load, locks the Custom graphics preset and records Host device-thread CPU time.' if custom_profile else 'v35 performance mode warms from 4 to 32 GPU-skinned pose meshes over 8 frames, then measures four 30-frame windows at a fixed limit of 32 with TAAU 0.75, direct light only, secondary bounces off and RTXDI off.')+' CPU-pose meshes remain omitted; this is stability and throughput evidence, not image acceptance.'
        else:
            skin_limits='v34 performance mode retains v33 pose-slot isolation and measures reusable exact-hash supply cost at limits 16, 32, 48 and 64 for 30 frames each. Ineligible and excess CPU-pose meshes are omitted from the explicit RT scene; every submitted model mesh must therefore be GPU-skinned. This is a stability and supply-cost test, not image acceptance.'
        skin={'status':status,
              'preview':{k:ends[0].get(k) for k in keys} if ends else {},
              'mesh_frames':len(frames),'frame_batch':{'flushes':len(flushes),'source_draws':source,'api_batches':batches,'compression_ratio':source/batches if batches else None} if structural else None,'limits':skin_limits}
    return {'schema':1,'pass':not errors,'status':'valid_evidence_requires_review' if not errors else 'incomplete','errors':sorted(set(errors)),
            'timing_windows':len(windows),'window_timeline':[{k:e.get(k) for k in ('frame','first_after_frame','segment','wall_ms','thread_times_available','thread_kernel_ms','thread_user_ms','thread_cpu_ms','qpc_begin','qpc_end','qpc_frequency','utc_filetime_100ns')} for e in windows],'elapsed_scopes_by_segment':dict(totals),'native_taa_requests':taa,
            'native_edge_payloads':samples,'native_edge_evidence':'not_requested_performance_ramp' if performance else 'complete_requires_image_review' if edge_complete else 'unavailable_or_incomplete',
            'source_shader_census':[e for e in events if e.get('event')=='source_shader_census'],'gpu_skinning':skin,
            'gpu_engine_collection':gpu,'limits':'Nested QPC scopes include blocking and overlap; do not sum. PDH engines overlap; do not sum as whole-GPU utilization. Sequential dynamic scenes are not matched A/B. Queued TAA request is not proof of effective image quality; motion-history/ghosting requires review.'}

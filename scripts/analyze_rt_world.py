"""Inspect actual runtime outputs without treating successful API calls as RT proof."""
from array import array
import hashlib
import json
import math
import statistics
from pathlib import Path
import struct
import sys
import zlib


def png(path,width,height,rgb):
    def chunk(kind,data):
        return struct.pack('>I',len(data))+kind+data+struct.pack('>I',zlib.crc32(kind+data)&0xffffffff)
    rows=b''.join(b'\0'+rgb[y*width*3:(y+1)*width*3] for y in range(height))
    path.write_bytes(b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>2I5B',width,height,8,2,0,0,0))
                     +chunk(b'IDAT',zlib.compress(rows))+chunk(b'IEND',b''))


def decode_normal(word):
    """Pinned GBuffer Snorm2x16 uses unsigned offsets and 65534, not SNORM casts."""
    low,high=word&65535,word>>16
    if low==65535 or high==65535:return None
    x,y=low/65534*2-1,high/65534*2-1
    z=1-abs(x)-abs(y);t=max(-z,0)
    x+=-t if x>=0 else t;y+=-t if y>=0 else t
    length=math.sqrt(x*x+y*y+z*z)
    return x/length,y/length,z/length


def read_floats(path):
    if not path.is_file() or path.stat().st_size!=640*400*16:return None
    values=array('f');values.frombytes(path.read_bytes())
    if sys.byteorder!='little':values.byteswap()
    return values


def luminance(values):
    result=[.2126*values[i]+.7152*values[i+1]+.0722*values[i+2] for i in range(0,len(values),4)
            if all(math.isfinite(v) for v in values[i:i+3])]
    if not result:return {'finite_pixels':0}
    return {'finite_pixels':len(result),'mean':statistics.fmean(result),'median':statistics.median(result),
            'below_0_05_fraction':sum(v<.05 for v in result)/len(result),
            'above_0_95_fraction':sum(v>.95 for v in result)/len(result)}


def analyze(directory,previews=True):
    directory=Path(directory);errors=[];samples=[];seen=set()
    path=directory/'world-runtime.jsonl'
    if not path.is_file():
        return {'schema':1,'runtime_outputs':'incomplete','errors':['missing_world_runtime_events'],'game_rt':'unrun'}
    if path.stat().st_size>4*1024*1024:raise ValueError('Runtime event budget exceeded')
    events=[json.loads(s) for s in path.read_text(encoding='utf-8').splitlines() if s.strip()]
    required=('loaded_pinned_runtime','initialize_api_0_6_4','register_game_device')
    for name in required:
        if not any(e.get('event')==name and e.get('code')==0 for e in events):errors.append('missing_success:'+name)
    # The v2 startup gate makes a successful library load distinct from an RT
    # request. Retain compatibility with v1 recordings which lack this policy.
    gated=any(e.get('event')=='startup_raster_policy' for e in events)
    if gated:
        if not any(e.get('event')=='rtx.enableRaytracing' and e.get('value')=='False' and e.get('code')==0 for e in events):
            errors.append('startup_rt_not_disabled')
        on=[i for i,e in enumerate(events) if e.get('event')=='raytracing_requested_on' and e.get('code')==0]
        off=[i for i,e in enumerate(events) if e.get('event')=='raytracing_requested_off' and e.get('code')==0]
        if len(on)!=1 or not off or off[-1]<=on[0]:errors.append('missing_gated_rt_on_off_sequence')
    comparison=any(e.get('event')=='lighting_comparison_policy' for e in events)
    coverage=any(e.get('event')=='coverage_comparison_policy' for e in events)
    v11=any(e.get('event')=='world_coverage_revision' and e.get('code')==0 and e.get('value')=='11' for e in events)
    v10=v11 or any(e.get('event')=='world_coverage_revision' and e.get('code')==0 and e.get('value')=='10' for e in events)
    if v10 and not any(e.get('event')=='rtx.enableAlphaTest' and e.get('code')==0 and e.get('value')=='True' for e in events):
        errors.append('missing_bulk_static_alpha_test_enable')
    v9=v10 or any(e.get('event')=='world_coverage_revision' and e.get('code')==0 and e.get('value')=='9' for e in events)
    v8=v9 or any(e.get('event')=='world_coverage_revision' and e.get('code')==0 and e.get('value')=='8' for e in events)
    v7=v8 or any(e.get('event')=='world_coverage_revision' and e.get('code')==0 and e.get('value')=='7' for e in events)
    frames=(30,120,180,240) if comparison or coverage else (30,120,240)
    phase_names={30:'raster',120:'baseline_sphere',180:'strong_sphere',240:'distant'}
    if coverage:
        phase_names={30:'raster',120:'baseline_world',180:'expanded_world',240:'expanded_world'}
        if v8:phase_names={30:'raster',120:'baseline_world',180:'expanded_sky',240:'expanded_models'}
        requested=[(e.get('after_present'),e.get('phase'),e.get('code')) for e in events if e.get('event')=='coverage_phase_requested']
        wanted=[(30,'baseline_world',0),(120,'expanded_sky',0),(180,'expanded_models',0),(240,'expanded_models',0)] if v8 else [(30,'baseline_world',0),(120,'expanded_world',0),(180,'expanded_world',0),(240,'expanded_world',0)]
        if requested!=wanted:
            errors.append('incomplete_or_failed_coverage_phase_requests')
    if comparison:
        requested=[(e.get('after_present'),e.get('phase'),e.get('code')) for e in events if e.get('event')=='lighting_phase_requested']
        if requested!=[(30,'baseline_sphere',0),(120,'strong_sphere',0),(180,'distant',0),(240,'distant',0)]:
            errors.append('incomplete_or_failed_lighting_phase_requests')
    for e in events:
        if e.get('event')!='world_readback':continue
        frame,kind=e.get('frame'),e.get('kind');key=(frame,kind)
        if frame not in frames or kind not in (0,1,2) or key in seen:
            errors.append('invalid_or_duplicate_readback_identity');continue
        seen.add(key);name=f'world-{frame}-{kind}.rgba32f';file=directory/name
        sample={'frame':frame,'kind':kind,'file':name,'copy_code':e.get('copy_code'),'hresult':e.get('hresult')}
        samples.append(sample)
        if comparison or coverage:
            sample['phase']=e.get('phase')
            if sample['phase']!=phase_names[frame]:errors.append('readback_phase_mismatch:'+name)
        if (e.get('file')!=name or (e.get('width'),e.get('height'))!=(640,400)
                or e.get('initialized_to_zero') is not True or e.get('written') is not True
                or e.get('copy_code')!=0 or e.get('hresult',0)&0x80000000
                or not file.is_file() or file.stat().st_size!=640*400*16):
            errors.append('missing_or_failed_readback:'+name);continue
        raw=file.read_bytes();values=array('f');values.frombytes(raw)
        if sys.byteorder!='little':values.byteswap()
        words=array('I');words.frombytes(raw)
        if sys.byteorder!='little':words.byteswap()
        finite=sum(math.isfinite(v) for v in values);pixels=len(values)//4
        first=values[0::4];positive=sum(math.isfinite(v) and 0<v<1e8 for v in first)
        variation=len({tuple(round(v,4) if math.isfinite(v) and abs(v)<1e8 else 'invalid' for v in values[i:i+3])
                       for i in range(0,len(values),16)})
        sample.update(sha256=hashlib.sha256(raw).hexdigest(),pixels=pixels,finite_fraction=finite/len(values),
                      nonzero_values=sum(v!=0 for v in values),positive_bounded_first_channel_fraction=positive/pixels,
                      nonzero_finite_rgb_values=sum(math.isfinite(v) and v!=0 for i,v in enumerate(values) if i%4!=3),
                      nonzero_rgb_words=sum(v!=0 for i,v in enumerate(words) if i%4!=3),first_rgba_u32=list(words[:4]),
                      sampled_rgb_variants=variation,zero_initialized_buffer_unchanged=all(v==0 for v in values))
        depth_values=values if kind==1 else read_floats(directory/f'world-{frame}-1.rgba32f') if kind==2 else None
        depths=depth_values[0::4] if depth_values is not None else []
        mask=[math.isfinite(v) and 0<v<1 for v in depths]
        if kind==0:sample['luminance']=luminance(values)
        if kind==1:
            hits=[v for v,hit in zip(first,mask) if hit]
            sample['projected_depth']={'valid_fraction':len(hits)/pixels,'min_hit':min(hits,default=None),'max_hit':max(hits,default=None)}
        if kind==2:
            normal_words=words[0::4]
            decoded=[decode_normal(w) if hit else None for w,hit in zip(normal_words,mask)]
            invalid=sum(hit and n is None for hit,n in zip(mask,decoded))
            sample['decoded_normals']={'valid_depth_mask_pixels':sum(mask),'invalid_encoding_on_depth_hits':invalid,
                'valid_fraction':sum(n is not None for n in decoded)/pixels,
                'unique_words_on_depth_hits':len({w for w,hit in zip(normal_words,mask) if hit})}
            if invalid:errors.append('invalid_packed_normal_on_depth_hit:'+name)
        if previews:
            rgb=bytearray()
            for pixel,i in enumerate(range(0,len(values),4)):
                channels=values[i:i+3]
                if kind==1:
                    # Projected depth z/w: white is the closest observed hit;
                    # misses/clear zero/invalid are black. This is NOT distance.
                    nearest=sample['projected_depth']['min_hit']
                    value=(1-values[i])/max(1-(nearest or 0),1e-8) if mask[pixel] else 0
                    channels=(value,)*3
                elif kind==2:
                    normal=decoded[pixel] if decoded else None
                    channels=tuple(v*.5+.5 for v in normal) if normal is not None else (0,0,0)
                rgb.extend(int(max(0,min(1,v))*255+.5) if math.isfinite(v) else 0 for v in channels)
            preview=directory/f'world-{frame}-{kind}.png';png(preview,640,400,bytes(rgb));sample['preview']=preview.name
    expected={(f,k) for f in frames for k in (0,1,2)}
    if seen!=expected:errors.append('incomplete_control_and_world_output_set')
    world=[s for s in samples if s['frame']!=30 and s['kind'] in (1,2)]
    nonzero=bool(world) and len(world)==(len(frames)-1)*2 and all(
        s.get('projected_depth',{}).get('valid_fraction',0)>0 if s['kind']==1 else s.get('decoded_normals',{}).get('valid_fraction',0)>0
        for s in world)
    material_samples=[e for e in events if e.get('event')=='world_material_sample']
    material_counts={phase:sum(e.get('phase')==phase for e in material_samples) for phase in phase_names.values() if phase!='raster'}
    if (comparison or coverage) and any(n>64 for n in material_counts.values()):errors.append('material_sample_budget_exceeded')
    cameras={e.get('frame'):e for e in events if e.get('event')=='world_camera_sample'}
    movement=[]
    if comparison or coverage:
        base=cameras.get(120,{})
        for frame in (180,240):
            other=cameras.get(frame,{})
            valid=base.get('available') is True and other.get('available') is True
            differences=[]
            for key in ('view','projection'):
                a,b=base.get(key,[]),other.get(key,[])
                if len(a)!=16 or len(b)!=16 or not all(isinstance(v,(int,float)) and math.isfinite(v) for v in a+b):
                    valid=False
                else:differences.extend(abs(x-y) for x,y in zip(a,b))
            movement.append({'frame':frame,'camera_sample_available':valid,
                             'max_matrix_delta':max(differences) if valid else None,
                             'matched_within_1e_4':valid and max(differences)<1e-4})
    hud=[e for e in events if e.get('event')=='hud_injection']
    if coverage and (not hud or any(e.get('replaced_post') is not True or e.get('scope_code')!=9 or not e.get('ordinal',0)
                                   or any(e.get(k,0)&0x80000000 for k in ('capture_hresult','apply_hresult','draw_hresult','restore_hresult')) for e in hud)):
        errors.append('missing_or_failed_hud_injection')
    hud_phases={phase:sum(e.get('phase')==phase for e in hud) for phase in (('baseline_world','expanded_sky','expanded_models') if v8 else ('baseline_world','expanded_world'))}
    if v7 and (not coverage or not all(hud_phases.values()) or any(e.get('phase') not in hud_phases for e in hud)):
        errors.append('missing_baseline_or_expanded_hud_injection')
    return {'schema':2,'scope':'reduced_runtime_output_readback_not_full_game_rt_or_performance_acceptance',
            'runtime_outputs':'pass' if not errors else 'incomplete','errors':errors,'samples':samples,
            'startup_raster_gate':'requested_events_present_not_effective_state_proof' if gated else 'legacy_ungated',
            'rt_activation_requests':sum(e.get('event')=='raytracing_requested_on' and e.get('code')==0 for e in events),
            'nonzero_world_depth_and_normals':nonzero,
            'coverage_comparison':{'present':coverage,'revision':11 if v11 else 10 if v10 else 9 if v9 else 8 if v8 else 7 if v7 else 6 if coverage else None,'phases':phase_names if coverage else {},'hud_injection_attempts':len(hud),'hud_post_replacements':sum(e.get('replaced_post') is True for e in hud),'hud_phase_counts':hud_phases,'camera_comparisons':movement,'same_camera_at_readbacks':bool(movement) and all(e['matched_within_1e_4'] for e in movement),'limits':'native UI trigger and post replacement require actual image review; weapons/skinning unimplemented'},
            'lighting_comparison':{'present':comparison,'status':'requests_complete_requires_image_review' if comparison and not errors else 'incomplete' if comparison else 'not_requested',
                'phases':phase_names if comparison else {},'camera_comparisons':movement,
                'same_camera_at_readbacks':bool(movement) and all(e['matched_within_1e_4'] for e in movement),
                'limits':'camera samples are last converted world Draw; matching endpoints do not prove a static scene, exposure convergence, or effective light settings'},
            'material_audit':{'samples':len(material_samples),'counts':material_counts,'per_phase_limit':64,
                'status':'samples_available' if all(material_counts.values()) else 'incomplete' if comparison or coverage else 'not_requested',
                'texture_stage_fields':['COLOROP','COLORARG1','COLORARG2','ALPHAOP','ALPHAARG1','ALPHAARG2','TEXCOORDINDEX','TEXTURETRANSFORMFLAGS','RESULTARG'],
                'render_state_fields':['TEXTUREFACTOR','LIGHTING','COLORVERTEX','DIFFUSEMATERIALSOURCE','AMBIENTMATERIALSOURCE','AMBIENT','FOGENABLE','SRGBWRITEENABLE'],
                'query_failures':sum(bool(e.get('pixel_shader_hresult',0)&0x80000000) or bool(e.get('render_state_query_failure_bits',0)) or any(
                    t.get('query_failure_bits',0) or t.get('texture_hresult',0)&0x80000000 or t.get('srgb_hresult',0)&0x80000000
                    for t in e.get('stages',[])) for e in material_samples),
                'category_query_failures':sum(bool(e.get('category_hresult',0)&0x80000000) for e in material_samples),
                'cull_query_failures':sum(bool(e.get('cull_hresult',0)&0x80000000) for e in material_samples),
                'limits':'first restored original D3D9 state per material identity/phase; bounded samples, not every Draw or final PBR materials'},
            'geometry_visual_review':'required' if nonzero else 'incomplete',
            'source_parallel_preparation':'unrun','performance':'unrun','game_rt':'requires_visual_and_thread_review',
            'preview_mapping':{'color':'clamped RGB [0,1]','depth':'projected z/w; (1-depth)/(1-min_hit); clear/miss/invalid black; not linear distance',
                               'normals':'offset Snorm2x16 /65534 then signed octahedral decode; RGB normal*.5+.5; same-frame 0<depth<1 mask'},
            'normal_source_format_from_pinned_runtime':'VK_FORMAT_R32_UINT; first RGBA word is packed normal, including float NaN bit patterns; verified against pinned primary GBuffer and actual v4 readback',
            'limits':'decoded normal/depth spatial correspondence still requires review; no API return code or nonzero buffer alone proves correct ray-traced game output'}


if __name__=='__main__':
    import argparse
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('directory',type=Path);p.add_argument('--output',type=Path)
    a=p.parse_args();r=analyze(a.directory)
    if a.output:a.output.write_text(json.dumps(r,indent=2)+'\n')
    print(json.dumps({k:v for k,v in r.items() if k!='samples'},indent=2))
    raise SystemExit(0 if r['runtime_outputs']=='pass' else 1)

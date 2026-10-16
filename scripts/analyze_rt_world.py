"""Inspect actual runtime outputs without treating successful API calls as RT proof."""
from array import array
import hashlib
import json
import math
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
    for e in events:
        if e.get('event')!='world_readback':continue
        frame,kind=e.get('frame'),e.get('kind');key=(frame,kind)
        if frame not in (30,120,240) or kind not in (0,1,2) or key in seen:
            errors.append('invalid_or_duplicate_readback_identity');continue
        seen.add(key);name=f'world-{frame}-{kind}.rgba32f';file=directory/name
        sample={'frame':frame,'kind':kind,'file':name,'copy_code':e.get('copy_code'),'hresult':e.get('hresult')}
        samples.append(sample)
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
        if previews:
            finite_depth=[v for v in first if math.isfinite(v) and 0<v<1e8]
            depth_max=max(finite_depth,default=1)
            rgb=bytearray()
            for i in range(0,len(values),4):
                channels=values[i:i+3]
                if kind==1:
                    value=math.log1p(max(0,values[i]))/math.log1p(depth_max) if math.isfinite(values[i]) else 0
                    channels=(value,)*3
                elif kind==2:channels=tuple(v*.5+.5 for v in channels)
                rgb.extend(int(max(0,min(1,v))*255+.5) if math.isfinite(v) else 0 for v in channels)
            preview=directory/f'world-{frame}-{kind}.png';png(preview,640,400,bytes(rgb));sample['preview']=preview.name
    expected={(f,k) for f in (30,120,240) for k in (0,1,2)}
    if seen!=expected:errors.append('incomplete_control_and_world_output_set')
    world=[s for s in samples if s['frame'] in (120,240) and s['kind'] in (1,2)]
    nonzero=bool(world) and len(world)==4 and all(
        s.get('positive_bounded_first_channel_fraction',0)>0 if s['kind']==1 else s.get('nonzero_rgb_words',0)>0
        for s in world)
    return {'schema':1,'scope':'reduced_runtime_output_readback_not_full_game_rt_or_performance_acceptance',
            'runtime_outputs':'pass' if not errors else 'incomplete','errors':errors,'samples':samples,
            'startup_raster_gate':'requested_events_present_not_effective_state_proof' if gated else 'legacy_ungated',
            'rt_activation_requests':sum(e.get('event')=='raytracing_requested_on' and e.get('code')==0 for e in events),
            'nonzero_world_depth_and_normals':nonzero,
            'geometry_visual_review':'required' if nonzero else 'incomplete',
            'source_parallel_preparation':'unrun','performance':'unrun','game_rt':'requires_visual_and_thread_review',
            'preview_mapping':{'color':'clamped RGB [0,1]','depth':'positive first channel logarithmic normalization',
                               'normals':'legacy float-channel visualization, not decoded packed normals; raw uint32 bits retained'},
            'normal_source_format_from_pinned_runtime':'VK_FORMAT_R32_UINT; first RGBA word may contain packed normal; transfer interpretation needs actual RT data',
            'limits':'normal encoding and scene correspondence require review; no API return code or nonzero buffer proves ray dispatch by itself'}


if __name__=='__main__':
    import argparse
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('directory',type=Path);p.add_argument('--output',type=Path)
    a=p.parse_args();r=analyze(a.directory)
    if a.output:a.output.write_text(json.dumps(r,indent=2)+'\n')
    print(json.dumps({k:v for k,v in r.items() if k!='samples'},indent=2))
    raise SystemExit(0 if r['runtime_outputs']=='pass' else 1)

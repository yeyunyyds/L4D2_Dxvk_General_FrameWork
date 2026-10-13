"""Validate GPU diagnostic files and compare sampled Vulkan outputs to a CPU reference."""
import argparse
from array import array
import hashlib
import json
from pathlib import Path
import math
import struct
import sys
from analyze_ldb_capture import analyze
from capture_math import inverse, mul


def analyze_gpu(directory):
    directory=Path(directory);base=analyze(directory)
    es=[json.loads(l) for l in (directory/'events.jsonl').read_text().splitlines() if l.strip()]
    snapshots={e['prefix']:e for e in base['snapshots']}
    matrices={e['prefix']:e for e in es if e['event'] in ('gpu_history','gpu_reprojection')}
    cache={};actual_matrices={}
    def depth(prefix):
        if prefix not in cache:
            a=array('f');a.frombytes((directory/(prefix+'-depth.f32')).read_bytes())
            if sys.byteorder!='little':a.byteswap()
            cache[prefix]=a
        return cache[prefix]
    pairs=[]
    for e in es:
        if e['event']!='gpu_reprojection':continue
        previous,current=e['previous_prefix'],e['prefix'];a,b=snapshots[previous],snapshots[current]
        if a['generation']!=b['generation'] or a['frame']+1!=b['frame'] or e['previous_frame']!=a['frame']:raise ValueError('GPU pair crosses Reset or missing frame')
        ae,be=matrices[previous],matrices[current]
        for event,snapshot in [(ae,a),(be,b)]:
            d=es[event['matrix_draw_sequence']];state=d['state'];reg=event['matrix_register']
            if d['event']!='draw' or d['frame']!=snapshot['frame'] or d['generation']!=snapshot['generation'] or d['hresult']!=0 or d['sequence']>=snapshot['sequence'] or d['color_id']!=snapshot['color_id'] or d['depth_id']!=snapshot['depth_id']:raise ValueError('GPU matrix source binding/frame mismatch')
            if state['vs_shader']!=event['matrix_shader']:raise ValueError('GPU shader source mismatch')
            actual=struct.unpack_from('<16f',(directory/state['vs_f32']).read_bytes(),reg*16)
            if any(abs(x-y)>max(1e-6,abs(x)*1e-8) for x,y in zip(actual,event['matrix_rows'])):raise ValueError('GPU matrix differs from actual constants')
            actual_matrices[event['prefix']]=actual
        ex=b['export'];w,h=ex['width'],ex['height'];n=w*h
        if a['export']!=ex or a['color']!=b['color'] or a['depth']!=b['depth']:raise ValueError('GPU image shape/format mismatch')
        raw=(directory/(current+'-gpu.f32')).read_bytes()
        if len(raw)!=n*32:raise ValueError('Incomplete GPU float output')
        for suffix in ('gpu-warp.ppm','gpu-valid.ppm'):
            ppm=(directory/(current+'-'+suffix)).read_bytes().split(b'\n',3)
            if len(ppm)!=4 or ppm[:3]!=[b'P6',f'{w} {h}'.encode(),b'255'] or len(ppm[3])!=n*3:raise ValueError('Incomplete GPU preview')
        old=depth(previous);now=depth(current);hdr=(directory/(previous+'-color.rgba16f')).read_bytes()
        inv=inverse(actual_matrices[current]);pm=actual_matrices[previous];fullw=b['color']['width'];fullh=b['color']['height']
        counts=[0]*6;max_motion_error=max_depth_error=max_color_error=max_exact_color_error=0.;checked=boundary_points=alternate_samples=0
        for i,v in enumerate(struct.iter_unpack('<8f',raw)):
            if not all(math.isfinite(x) for x in v):raise ValueError('Nonfinite GPU output')
            status=int(v[7]);
            if not 0<=status<=5 or status!=v[7]:raise ValueError('Invalid GPU status')
            counts[status]+=1
            if status not in (1,2):continue
            x=i%w;y=i//w;ox=min(x*ex['stride']+ex['offset'],fullw-1);oy=min(y*ex['stride']+ex['offset'],fullh-1)
            clip=[2*ox/fullw-1,1-2*oy/fullh,now[i],1];p=mul(pm,mul(inv,clip))
            if p[3]<=0:raise ValueError('GPU accepted invalid W')
            px=(p[0]/p[3]+1)*fullw/2;py=(1-p[1]/p[3])*fullh/2;pz=p[2]/p[3]
            sx=round((px-ex['offset'])/ex['stride']);sy=round((py-ex['offset'])/ex['stride'])
            max_motion_error=max(max_motion_error,abs(v[4]-(ox-px)),abs(v[5]-(oy-py)))
            # Float32 dispatch and float64 reference can straddle a nearest-neighbor tie.
            # Only allow adjacent samples within 0.002 original pixels of that tie;
            # raw HDR values and the corresponding depth still require strict checks.
            def options(position,limit):
                sample=(position-ex['offset'])/ex['stride'];lower=math.floor(sample)
                near=abs(sample-lower-.5)*ex['stride']<=.002
                values=[lower,lower+1] if near else [round(sample)]
                return [x for x in values if 0<=x<limit],near
            xs,bx=options(px,w);ys,by=options(py,h);boundary_points+=bx or by
            if not xs or not ys:raise ValueError('GPU accepted out-of-bounds sample')
            exact_j=sy*w+sx if 0<=sx<w and 0<=sy<h else None
            if exact_j is not None:
                exact=struct.unpack_from('<4e',hdr,exact_j*8)
                max_exact_color_error=max(max_exact_color_error,max(abs(x-y) for x,y in zip(v[:4],exact)))
            candidates=[y*w+x for y in ys for x in xs]
            if exact_j in candidates:candidates.remove(exact_j);candidates.insert(0,exact_j)
            accepted=None;accepted_color_error=accepted_depth_error=math.inf
            for j in candidates:
                expected=struct.unpack_from('<4e',hdr,j*8)
                color_error=max(abs(x-y) for x,y in zip(v[:4],expected))
                depth_error=abs(v[6]-abs(old[j]-pz))
                if .2<old[j]<1 and color_error<=1e-5 and depth_error<=2e-5 and depth_error<accepted_depth_error:
                    accepted=j;accepted_color_error=color_error;accepted_depth_error=depth_error
            if accepted is None:raise ValueError('GPU/CPU mismatch: raw HDR/depth does not match the bounded nearest sample')
            max_color_error=max(max_color_error,accepted_color_error);max_depth_error=max(max_depth_error,accepted_depth_error)
            alternate_samples+=accepted!=exact_j
            if not .2<now[i]<1:raise ValueError('GPU accepted sky/near depth')
            if status!=(1 if v[6]<=e['depth_tolerance'] else 2):raise ValueError('GPU classification differs from depth error')
            checked+=1
        if counts!=e['status_counts'] or counts[1]+counts[2]!=e['valid_pixels'] or counts[1]!=e['depth_consistent_pixels']:raise ValueError('GPU status summary differs from buffer')
        if not checked:raise ValueError('No valid GPU diagnostic pixels')
        if max_motion_error>.02 or max_depth_error>2e-5 or max_color_error>1e-5:raise ValueError(f'GPU/CPU mismatch: motion={max_motion_error},depth={max_depth_error},color={max_color_error}')
        pairs.append({'previous':previous,'current':current,'pixels_checked':checked,'status_counts':counts,'max_motion_error_pixels':max_motion_error,'max_depth_error':max_depth_error,'max_raw_hdr_error':max_color_error,'max_exact_cpu_sample_hdr_error':max_exact_color_error,'nearest_boundary_pixels':boundary_points,'accepted_adjacent_samples':alternate_samples,'nearest_boundary_tolerance_original_pixels':.002,'raw_sha256':hashlib.sha256(raw).hexdigest()})
    return {'status':'passed_gpu_compute_diagnostic' if pairs else 'no_gpu_pairs','gpu_pairs':pairs,'world_motion_verified':False,'gpu_fg_execution':'unrun','game_image_modified':False}

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('directory',type=Path);a=p.parse_args()
    try:print(json.dumps(analyze_gpu(a.directory),indent=2))
    except (ValueError,OSError,KeyError,struct.error) as e:p.exit(1,f'GPU check failed: {e}\n')

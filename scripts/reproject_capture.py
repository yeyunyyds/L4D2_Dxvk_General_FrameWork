"""Sparse CPU reprojection of an explicitly selected matrix hypothesis; not game motion validation."""
import argparse
import csv
import hashlib
import json
from pathlib import Path
import struct
from capture_math import inverse, transpose, project_previous, shader_usage
from analyze_ldb_capture import analyze


def reproject(directory, previous, current, mapping, output, step=8):
    directory=Path(directory);output=Path(output)
    if output.exists(): raise ValueError('Output exists; preserve it and choose another directory')
    if step<1: raise ValueError('Invalid grid step')
    analysis=analyze(directory);snaps={s['prefix']:s for s in analysis['snapshots']}
    a,b=snaps[previous],snaps[current]
    if a['frame']>=b['frame'] or a['generation']!=b['generation']: raise ValueError('Pair is not chronological or crosses Reset')
    if not mapping.get('evidence'): raise ValueError('Record why this matrix hypothesis was selected')
    events={e['sequence']:e for e in [json.loads(l) for l in (directory/'events.jsonl').read_text().splitlines() if l.strip()]}
    def selected(s):
        d=events[s['last_draw_sequence']];state=d['state'];shader=(directory/state['vs_shader']).read_bytes()
        if hashlib.sha256(shader).hexdigest()!=mapping['shader_sha256']: raise ValueError('Selected shader does not match this snapshot producer')
        start=mapping['register_start']
        if start not in shader_usage(shader)['matrix_starts']: raise ValueError('Register block is not a supported shader matrix candidate')
        raw=(directory/state['vs_f32']).read_bytes()
        if start<0 or (start+4)*16>len(raw): raise ValueError('Matrix register out of range')
        m=list(struct.unpack_from('<16f',raw,start*16))
        if mapping['storage']=='transposed_register_rows':m=transpose(m)
        elif mapping['storage']!='register_rows':raise ValueError('Unknown matrix storage')
        inverse(m);return m,d['viewport']
    am,av=selected(a);bm,bv=selected(b)
    ai=a.get('export',{'width':a['color']['width'],'height':a['color']['height'],'stride':1,'offset':0})
    bi=b.get('export',{'width':b['color']['width'],'height':b['color']['height'],'stride':1,'offset':0})
    ad=(directory/(previous+'-depth.f32')).read_bytes();bd=(directory/(current+'-depth.f32')).read_bytes()
    center=mapping.get('pixel_center_offset',0.0)
    if center not in (0.0,0.5): raise ValueError('Choose integer D3D9 centers or explicit corrected half-pixel centers')
    inv=inverse(bm);rows=[];pixels=[];valid=consistent=0
    xs=list(range(0,bi['width'],step));ys=list(range(0,bi['height'],step))
    for y in ys:
        for x in xs:
            z=struct.unpack_from('<f',bd,(y*bi['width']+x)*4)[0]
            ox=min(x*bi['stride']+bi['offset'],b['color']['width']-1)+center
            oy=min(y*bi['stride']+bi['offset'],b['color']['height']-1)+center
            row={'x':ox,'y':oy,'valid':False};pixel=(0,0,0)
            try:
                px,py,pz=project_previous(ox,oy,z,inv,am,bv,av)
                sx=round((px-center-ai['offset'])/ai['stride']);sy=round((py-center-ai['offset'])/ai['stride'])
                if not (0<=sx<ai['width'] and 0<=sy<ai['height'] and av['min_z']<=pz<=av['max_z']):raise ValueError('Reprojection out of bounds')
                old=struct.unpack_from('<f',ad,(sy*ai['width']+sx)*4)[0];error=abs(old-pz)
                ok=error<=0.001;valid+=1;consistent+=ok
                row.update(valid=True,dx=ox-px,dy=oy-py,depth_error=error)
                pixel=(min(255,int(error*25500)),255 if ok else 0,0)
            except ValueError:pass
            rows.append(row);pixels.extend(pixel)
    output.mkdir(parents=True)
    with (output/'motion-hypothesis.csv').open('w',newline='') as f:
        writer=csv.DictWriter(f,fieldnames=['x','y','valid','dx','dy','depth_error']);writer.writeheader();writer.writerows(rows)
    (output/'depth-consistency.ppm').write_bytes(f'P6\n{len(xs)} {len(ys)}\n255\n'.encode()+bytes(pixels))
    result={'status':'diagnostic_matrix_hypothesis','previous':previous,'current':current,'mapping':mapping,
      'grid_points':len(rows),'valid_points':valid,'depth_consistent_points':consistent,'depth_tolerance':0.001,
      'motion_units':'current minus previous position in full render-surface pixels','camera_semantics_verified':False,'world_motion_verified':False,
      'limitations':['Sparse nearest-depth test; visibility, moving objects, jitter, matrix space and clean-world classification still need validation']}
    (output/'result.json').write_text(json.dumps(result,indent=2)+'\n');return result


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('directory',type=Path)
    p.add_argument('--previous',required=True);p.add_argument('--current',required=True);p.add_argument('--mapping',type=Path,required=True);p.add_argument('--output',type=Path,required=True);p.add_argument('--step',type=int,default=8);a=p.parse_args()
    try: print(json.dumps(reproject(a.directory,a.previous,a.current,json.loads(a.mapping.read_text()),a.output,a.step),indent=2))
    except (ValueError,OSError,KeyError,TypeError,struct.error) as e:p.exit(1,f'Reprojection failed: {e}\n')

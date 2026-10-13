"""Inventory actual shader/state blobs and rank matrix candidates without labeling cameras."""
import argparse
import collections
import hashlib
import json
from pathlib import Path
import struct
from analyze_ldb_capture import analyze
from capture_math import inverse, shader_usage


def inventory(directory):
    directory=Path(directory);base=analyze(directory)
    events=[json.loads(l) for l in (directory/'events.jsonl').read_text().splitlines() if l.strip()]
    shaders={};constants={};groups={};frames=collections.defaultdict(list)
    for e in events:
        if e['event']!='draw' or 'state' not in e: continue
        s=e['state'];frames[e['frame']].append(e)
        for key in ('vs_shader','ps_shader'):
            name=s.get(key)
            if name and name not in shaders:
                raw=(directory/name).read_bytes();shaders[name]={'sha256':hashlib.sha256(raw).hexdigest(),**shader_usage(raw)}
        name=s.get('vs_shader');blob=s.get('vs_f32')
        if not name or not blob or e.get('hresult')!=0: continue
        if blob not in constants:
            raw=(directory/blob).read_bytes();constants[blob]=struct.unpack('<'+'f'*(len(raw)//4),raw)
        values=constants[blob]
        for start in shaders[name]['matrix_starts']:
            m=values[start*4:start*4+16]
            if len(m)!=16: continue
            try: inverse(m)
            except ValueError: continue
            k=(shaders[name]['sha256'],start)
            g=groups.setdefault(k,{'shader_sha256':k[0],'register_start':start,'draw_occurrences':0,'frames':set(),'matrices':set()})
            g['draw_occurrences']+=1;g['frames'].add(e['frame']);g['matrices'].add(m)
    candidates=[]
    for g in groups.values():
        candidates.append({k:v for k,v in g.items() if k not in ('frames','matrices')}|{'frames':sorted(g['frames']),'distinct_matrices':len(g['matrices']),
          'sample_register_rows':list(next(iter(g['matrices']))),'camera_semantics_verified':False})
    candidates.sort(key=lambda x:(-len(x['frames']),-x['draw_occurrences'],x['distinct_matrices']))
    return {'status':'shader_state_inventory','capture':{k:v for k,v in base.items() if k not in ('snapshots','skips')},
      'shaders':shaders,'matrix_candidates':candidates[:64],
      'frame_draw_counts':{str(k):len(v) for k,v in frames.items()},
      'cpu_qpc_frequency':events[0].get('qpc_frequency'),'timestamps_are':'Host CPU observation times including diagnostic stalls; not game simulation/GPU times',
      'snapshots':[{k:e.get(k) for k in ('prefix','frame','generation','reason','last_draw_sequence','cpu_qpc','export','depth_content')} for e in base['snapshots']],
      'world_motion_verified':False,'warning':'Candidates are finite invertible register blocks read by shaders, not proven view-projection matrices. Object/skin transforms may also qualify.'}


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('directory',type=Path);p.add_argument('--output',type=Path)
    a=p.parse_args()
    try:
        result=inventory(a.directory);text=json.dumps(result,ensure_ascii=False,indent=2)+'\n'
        if a.output: a.output.write_text(text)
        else: print(text)
    except (ValueError,OSError,KeyError,TypeError) as e:p.exit(1,f'Inventory failed: {e}\n')

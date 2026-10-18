"""CPU replay of verified complete scene inputs; no GPU/visibility acceptance."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import tempfile
from analyze_rt_scene import analyze


def replay(directory, executable):
    directory=Path(directory);evidence=analyze(directory)
    if not evidence['pass']:raise ValueError(evidence['errors'])
    samples=[json.loads(l) for l in (directory/'world-runtime.jsonl').read_text().splitlines() if l.strip()]
    samples=[s for s in samples if s.get('event')=='scene_input_sample' and s['geometry_hresult']==0 and s['shader'] in ('NekoSky','VertexLitGeneric')]
    if not samples:raise ValueError('No complete relevant inputs')
    with tempfile.TemporaryDirectory(prefix='rt-scene-replay-') as temp:
        path=Path(temp)/'inputs.bin'
        with path.open('wb') as f:
            f.write(struct.pack('<2I',0x3152534c,len(samples)))
            for s in samples:
                payload={i['file'].split(str(s['ordinal'])+'-',1)[1]:(directory/i['file']).read_bytes() for i in s['files']}
                decl=[struct.unpack_from('<HH4B',payload['declaration.bin'],i) for i in range(0,len(payload['declaration.bin']),8)]
                def offset(usage,kind):
                    found=[e[1] for e in decl if e[0]==0 and e[2]==kind and e[4:]==(usage,0)]
                    if len(found)>1:raise ValueError('duplicate semantic')
                    return found[0] if found else -1
                pos,uv,weight,indices=offset(0,2),offset(5,1),offset(1,6),offset(2,4)
                if min(pos,uv)<0:raise ValueError('missing position/UV')
                f.write(struct.pack('<5I2i',s['ordinal'],int(s['shader']!='NekoSky'),s['stride'],pos,uv,weight,indices))
                f.write(hashlib.sha256(payload['vs.bin']).hexdigest().encode('ascii'))
                f.write(payload['constants.bin']);f.write(struct.pack('<32f',*s['view'],*s['projection']))
                for name in ('vs.bin','ps.bin','vertices.bin'):
                    raw=payload[name];f.write(struct.pack('<I',len(raw)//4 if name!='vertices.bin' else len(raw)));f.write(raw)
        result=json.loads(subprocess.check_output([str(executable),str(path)],text=True))
    if len(result)!=len(samples):raise ValueError('incomplete replay')
    return {'scope':'captured input CPU replay, no GPU/BLAS/visibility/performance acceptance','samples':result,'all_inputs_reconstructed':all(s['valid'] for s in result),'all_sky_uvs_match_independent_formula':all(s['max_uv_error']<1e-5 for s in result if s['kind']==0)}


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('directory',type=Path);parser.add_argument('--executable',required=True,type=Path);parser.add_argument('--output',required=True,type=Path);args=parser.parse_args()
    report=replay(args.directory,args.executable.resolve());args.output.write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps({k:v for k,v in report.items() if k!='samples'},indent=2))
    if not report['all_inputs_reconstructed'] or not report['all_sky_uvs_match_independent_formula']:raise SystemExit(1)

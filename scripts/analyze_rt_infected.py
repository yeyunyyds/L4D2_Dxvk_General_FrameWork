"""Review bounded near-target and diffuse dependency evidence, not visibility."""
import hashlib
import json
import math
from pathlib import Path
import struct


def analyze(directory):
    root=Path(directory);errors=[];events=[];recipes=[];used=0;files=set();verified=set();omitted=set()
    def check(value,reason):
        if not value:raise ValueError(reason)
    def rows(name,limit):
        path=root/name
        if not path.is_file():return []
        check(path.stat().st_size<=limit,'infected_event_budget')
        return [json.loads(s) for s in path.read_text().splitlines() if s.strip()]
    def digest_file(name,digest,evidence=True):
        check(name=='model-textures/'+digest+'.dds' and len(digest)==64 and all(c in '0123456789abcdef' for c in digest),'infected_unsafe_dependency')
        if not evidence:omitted.add(name);return
        path=root/name;check(path.is_file() and 148<path.stat().st_size<=24*1024*1024+148,'infected_missing_dependency')
        if name not in verified:
            raw=path.read_bytes();check(hashlib.sha256(raw).hexdigest()==digest,'infected_dependency_digest')
            h=struct.unpack_from('<37I',raw);check(h[0]==0x20534444 and h[1]==124 and h[32] in (71,72,74,75,77,78,87,91) and h[33:37]==(3,0,1,0),'infected_dependency_dds')
            verified.add(name)
    try:
        for name in ('world-runtime.jsonl','preview-runtime.jsonl'):events.extend(rows(name,8*1024*1024))
        if not any(e.get('event')=='world_coverage_revision' and e.get('value')=='21' for e in events):return {'schema':1,'pass':True,'status':'not_requested'}
        recipes=rows('infected-materials.jsonl',1024*1024);check(len(recipes)<=128,'infected_recipe_budget')
        for e in recipes:
            check(e['event']=='infected_diffuse_bake' and type(e['frame']) is int and e['frame']>=241,'infected_recipe_identity')
            p=e['parameters'];check(len(p)==20 and all(type(x) in (int,float) and math.isfinite(x) and abs(x)<=1e4 for x in p) and p[1]-p[0]>1e-6,'infected_recipe_constants')
            check([x['slot'] for x in e['inputs']]==[0,4,5],'infected_recipe_samplers')
            digest_file(e['output_file'],e['output_sha256'],e['output_evidence_saved'])
            if e['output_evidence_saved']:
                raw=(root/e['output_file']).read_bytes();h=struct.unpack_from('<37I',raw)
                check(h[32]==87 and 0<h[3]<=256 and 0<h[4]<=256 and h[7]==max(h[3:5]).bit_length(),'infected_output_layout')
                check(all(raw[i]==255 for i in range(151,len(raw),4)),'infected_output_not_opaque')
            for s in e['inputs']:
                check(type(s['srgb']) is bool and s['wrap_u'] in (0,1,2) and s['wrap_v'] in (0,1,2) and s['filter'] in (0,1),'infected_dependency_sampler')
                digest_file(s['file'],s['sha256'],s['evidence_saved'])
        unknown=rows('unknown-model-inputs.jsonl',1024*1024);unknown_used=0;unknown_files=set()
        check(len(unknown)<=32,'unknown_model_row_budget')
        for sample in unknown:
            check(sample['event']=='unknown_model_input' and sample['shader'] in ('VertexLitGeneric','NekoToon','Infected'),'unknown_model_identity')
            for item in sample['files']:
                name=item['file'];check(Path(name).name==name and name.startswith('unknown-'+str(sample['ordinal'])+'-') and name not in unknown_files,'unknown_model_path');unknown_files.add(name)
                path=root/name;check(path.is_file() and 0<item['size']<=4194304 and path.stat().st_size==item['size'],'unknown_model_payload');raw=path.read_bytes();unknown_used+=len(raw)
                check(hashlib.sha256(raw).hexdigest()==item['sha256'],'unknown_model_digest')
        check(unknown_used<=16*1024*1024,'unknown_model_payload_budget')
        targets=[e for e in events if e.get('event')=='infected_near_target'];check(len(targets)<=256,'infected_target_row_budget')
        for e in targets:
            check(type(e['ready']) is bool and type(e['submitted']) is bool and (e['ready'] or not e['submitted']),'infected_target_status')
            check(type(e['distance_source_units']) in (int,float) and math.isfinite(e['distance_source_units']) and -1<=e['distance_source_units']<=1024,'infected_target_distance')
            check(len(e['center_clip'])==4 and len(e['world_center'])==3 and all(math.isfinite(x) for x in e['center_clip']+e['world_center']),'infected_target_projection')
            for key in ('vertices','indices'):
                name=e['vertex_file' if key=='vertices' else 'index_file']
                if not name:continue
                check(Path(name).name==name and name==f"target-{e['ordinal']}-{key}.bin" and name not in files,'infected_target_path');files.add(name)
                path=root/name;check(path.is_file() and 0<path.stat().st_size<=1048576,'infected_target_payload');raw=path.read_bytes();used+=len(raw)
                check(hashlib.sha256(raw).hexdigest()==e[key+'_sha256'],'infected_target_digest')
                if key=='vertices':check(len(raw)%32==0 and all(math.isfinite(x) and abs(x)<=1e7 for at in range(0,len(raw),32) for x in struct.unpack_from('<8f',raw,at)),'infected_target_vertices')
            check(used==e['payload_bytes'] and used<=16*1024*1024,'infected_target_payload_accounting')
        summaries=[e for e in events if e.get('event')=='live_shader_summary']
        check(len(summaries)==1,'infected_missing_final_shader_summary')
        if summaries:
            check(summaries[0]['near_target_rows']==len(targets) and summaries[0]['near_target_payload_bytes']==used and summaries[0]['infected_bakes']==len(recipes),'infected_final_accounting')
        return {'schema':1,'pass':not errors,'status':'collected_requires_image_review','recipes':len(recipes),'unknown_model_inputs':len(unknown),'unknown_payload_bytes':unknown_used,'verified_dependency_files':len(verified),'omitted_dependency_files':len(omitted-verified),'near_target_rows':len(targets),'near_submitted_rows':sum(e['submitted'] for e in targets),'payload_bytes':used,'shader_summary':summaries,'errors':errors,'gpu_visibility':'unrun','limits':'File integrity, diffuse dependency graph and projected center only; no triangle occlusion, actual appearance or full material coverage acceptance.'}
    except (ValueError,KeyError,TypeError,OSError,struct.error,json.JSONDecodeError) as exc:
        return {'schema':1,'pass':False,'status':'incomplete','errors':[str(exc)],'gpu_visibility':'unrun'}

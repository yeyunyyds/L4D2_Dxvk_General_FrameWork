"""Validate exported base textures and fallback accounting, without a GPU-load claim."""
import hashlib
from pathlib import Path
import struct


def analyze(directory,events):
    directory=Path(directory);errors=[];files={};bindings=[]
    def check(value,why):
        if not value:raise ValueError(why)
    if not any(e.get('event')=='model_texture_policy' and e.get('code')==0 for e in events):errors.append('missing_model_texture_policy')
    lights=[(e.get('after_present'),e.get('value'),e.get('code')) for e in events if e.get('event')=='model_light_config_requested']
    if lights!=[(300,'30,30,30',0),(540,'3000,3000,3000',0)]:errors.append('texture_light_sequence')
    for e in events:
        if e.get('event')!='model_texture_binding':continue
        bindings.append(e)
        try:
            check(type(e.get('frame')) is int and 181<=e['frame']<=540 and type(e.get('material_handle')) is int and e['material_handle']>0,'invalid_texture_binding_identity')
            check(type(e.get('textured')) is bool and type(e.get('srgb')) is bool,'invalid_texture_binding_flags')
            check(e.get('alpha_test') in range(8) and e.get('alpha_ref') in range(256) and e.get('filter') in (0,1) and e.get('wrap_u') in (0,1,2) and e.get('wrap_v') in (0,1,2),'invalid_texture_alpha_sampler')
            if not e['textured']:
                check(not e.get('file') and not e.get('sha256') and e.get('reason')!='sampler0_base_level_dds_dx10','invalid_texture_fallback')
                continue
            digest=e['sha256'];check(isinstance(digest,str) and len(digest)==64 and all(x in '0123456789abcdef' for x in digest),'invalid_texture_digest')
            name=e['file'];check(name=='model-textures/'+digest+'.dds','unsafe_texture_path')
            check(0<e['width']<=4096 and 0<e['height']<=4096,'invalid_texture_dimensions')
            formats={int.from_bytes(b'DXT1','little'):(71,72,8),int.from_bytes(b'DXT3','little'):(74,75,16),int.from_bytes(b'DXT5','little'):(77,78,16),21:(87,91,0),22:(87,91,0)}
            check(e['format'] in formats,'unsupported_exported_texture_format');linear,srgb,block=formats[e['format']]
            path=directory/name;check(path.is_file() and 148<path.stat().st_size<=16*1024*1024+148,'missing_or_oversized_texture')
            if name not in files:
                raw=path.read_bytes();check(hashlib.sha256(raw).hexdigest()==digest,'texture_file_digest');files[name]=len(raw)
                check(len(files)<=128 and sum(files.values())<=64*1024*1024,'texture_export_budget')
            else:raw=path.read_bytes()[:148]
            h=struct.unpack_from('<37I',raw)
            row=((e['width']+3)//4)*block if block else e['width']*4;rows=(e['height']+3)//4 if block else e['height']
            check(h[0]==0x20534444 and h[1]==124 and h[2]==(0x1007|(0x80000 if block else 8)) and h[3:5]==(e['height'],e['width']) and h[5]==(row*rows if block else row) and h[7]==1,'invalid_dds_dimensions_pitch')
            check(h[19:22]==(32,4,0x30315844) and h[27]==0x1000 and h[32:37]==(srgb if e['srgb'] else linear,3,0,1,0),'invalid_dds_dx10_color_space')
            check(files[name]==148+row*rows,'texture_payload_size')
        except (ValueError,KeyError,TypeError,OSError,struct.error) as exc:errors.append(str(exc))
    frames=[e for e in events if e.get('event')=='api_mesh_frame'];previous={}
    for e in frames:
        try:
            for k in ('material_updates','textured_submissions','texture_fallback_submissions','texture_exports','texture_bytes','texture_held','texture_read_failures','texture_budget_deferred'):
                check(type(e.get(k)) is int and e[k]>=0,'invalid_texture_counter')
                if k!='texture_held':check(e[k]>=previous.get(k,0),'regressing_texture_counter')
            check(e['textured_submissions']+e['texture_fallback_submissions']==e['submitted'],'texture_submission_accounting')
            check(e['texture_exports']<=128 and e['texture_held']<=128 and e['texture_bytes']<=64*1024*1024,'texture_runtime_budget')
            if e['frame']==600:check(e['texture_held']==0,'texture_refs_not_released')
            previous=e
        except (ValueError,KeyError,TypeError) as exc:errors.append(str(exc))
    if frames:
        if frames[-1].get('material_updates')!=len(bindings):errors.append('texture_binding_count')
        if sum(files.values())>frames[-1].get('texture_bytes',0):errors.append('texture_export_byte_accounting')
    handles={e.get('material_handle') for e in bindings}
    for e in events:
        if e.get('event') not in ('api_model_sample','api_model_previous_sample'):continue
        if type(e.get('textured')) is not bool or e.get('material_handle') not in handles:errors.append('model_sample_texture_binding')
    last=frames[-1] if frames else {};fallback=last.get('texture_fallback_submissions',0)
    coverage='partial_with_synthetic_fallback' if fallback else 'exported_for_all_submitted_models' if last.get('textured_submissions',0) else 'unrun'
    return {'schema':1,'pass':not errors,'errors':sorted(set(errors)),'coverage':coverage,'bindings':len(bindings),'unique_dds_files':len(files),'verified_dds_bytes':sum(files.values()),'textured_submissions':last.get('textured_submissions',0),'fallback_submissions':fallback,'gpu_texture_acceptance':'unrun','limits':'Validated static sampler0 base-level DDS and API intent; original PS/Toon/tints, animated textures, mips and actual GPU preload/appearance remain separate.'}

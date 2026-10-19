"""Validate consumer snapshots embedded in physical Draw RPCs; no game RT claim."""
from collections import Counter
import hashlib
import json
import math
from pathlib import Path
import struct

RECORD = struct.Struct('<IIQIIQ')
SNAPSHOT = struct.Struct('<IIQQQII48fII4I2f')
MATERIAL = struct.Struct('<QII64s')
PIPELINE = struct.Struct('<12I2f4I')
PIPELINE_V7 = struct.Struct('<12I2f5I')  # original CULLMODE follows declaration count
PIPELINE_V8 = struct.Struct('<12I2f8I')  # adds original lighting, fog, reserved category42
STATUS = {1:'valid',2:'module_missing',3:'version_mismatch',4:'read_failed',5:'invalid_stack',
          6:'changed_during_read',7:'invalid_matrices',8:'not_sampled'}


def nonsingular(values):
    a=[list(values[i:i+4]) for i in range(0,16,4)]
    for i in range(4):
        pivot=max(range(i,4),key=lambda j:abs(a[j][i]))
        if abs(a[pivot][i])<1e-20:return False
        a[i],a[pivot]=a[pivot],a[i]
        for j in range(i+1,4):
            ratio=a[j][i]/a[i][i]
            a[j]=[a[j][k]-ratio*a[i][k] for k in range(4)]
    return True


def affine_rigid(m):
    if not all(math.isfinite(x) for x in m) or sum(abs(m[i]) for i in (3,7,11))+abs(m[15]-1)>1e-5:return False
    if any(abs(sum(m[i*4+k]*m[j*4+k] for k in range(3))-float(i==j))>.01 for i in range(3) for j in range(3)):return False
    determinant=m[0]*(m[5]*m[10]-m[6]*m[9])-m[1]*(m[4]*m[10]-m[6]*m[8])+m[2]*(m[4]*m[9]-m[5]*m[8])
    return abs(determinant-1)<.02


STATIC_SHADERS=('VertexLitGeneric','UnlitGeneric','LightmappedGeneric','LightmappedGeneric_DX9',
                'WorldVertexTransition','WorldVertexTransition_DX9','WorldTwoTextureBlend')


def static_affine(m):
    if not all(math.isfinite(x) and abs(x)<=1e7 for x in m) or sum(abs(m[i]) for i in (3,7,11))+abs(m[15]-1)>1e-5:return False
    det=m[0]*(m[5]*m[10]-m[6]*m[9])-m[1]*(m[4]*m[10]-m[6]*m[8])+m[2]*(m[4]*m[9]-m[5]*m[8])
    return 1e-8<det<1e12


def static_layout(declaration,stride,allow_rigid_bone=False):
    sizes=(4,8,12,16,4,4,4,8,4,4,8,4,8,4,4,4,8)
    if len(declaration)%8 or not 24<=len(declaration)<=520 or declaration[-8:]!=bytes.fromhex('ff00000011000000') or not 0<stride<=128 or stride%4:return False
    position=uv=False;bone_fields=set()
    for at in range(0,len(declaration)-8,8):
        stream,offset,kind,method,usage,index=struct.unpack_from('<HH4B',declaration,at)
        if method or kind>=len(sizes) or usage==9:return False
        if usage in (1,2):
            if not allow_rigid_bone or stream or index or offset%4 or offset+4>stride or kind!=(6 if usage==1 else 4) or usage in bone_fields:return False
            bone_fields.add(usage);continue
        if stream:
            if not ((stream,offset,kind,usage,index)==(1,0,4,10,1) or (stream,offset,kind,usage,index)==(2,0,3,0,1)
                or stream==2 and offset==16 and kind in (2,5) and (usage,index)==(3,1) or (stream,offset,kind,usage,index)==(3,0,0,0,2)):return False
            continue
        if offset%4 or offset+sizes[kind]>stride:return False
        if usage==0:
            if position or index or kind!=2:return False
            position=True
        elif usage==5:
            if index>7 or (kind>3 if index else kind!=1):return False
            if not index:
                if uv:return False
                uv=True
        elif usage not in (3,6,7,10) or index:return False
    return position and uv and (not bone_fields or bone_fields=={1,2})


def analyze(path):
    path = Path(path)
    if path.stat().st_size > 258 * 1024 * 1024:
        raise ValueError('Draw trace exceeds budget')
    data = path.read_bytes()
    offset = 0
    errors, pending, header, footer = [], None, None, None
    counts, statuses, kinds = Counter(), Counter(), Counter()
    shaders, material_flags, decisions, conversions = Counter(), Counter(), Counter(), Counter()
    pending_conversion, pending_world_safe = False, False
    client_threads, ordinals = {}, set()
    host_thread, last_tick = None, 0
    cameras, layouts, attribution = {}, {}, {}
    static_groups={};static_omitted=0;pending_static=None
    attribution_omitted_draws = 0
    pending_attribution = None
    viewport_mismatch = 0
    while offset < len(data):
        if len(data) - offset < RECORD.size:
            errors.append('truncated_record'); break
        event, size, uid, thread, reserved, tick = RECORD.unpack_from(data, offset)
        if size < RECORD.size or size > 4096 or offset + size > len(data) or reserved or footer:
            errors.append('invalid_record_or_data_after_footer'); break
        payload = data[offset + RECORD.size:offset + size]; offset += size
        if host_thread is None:
            host_thread = thread
        if not thread or thread != host_thread or tick < last_tick:
            errors.append('host_thread_or_clock_changed')
        last_tick = tick; counts[event] += 1
        try:
            if event == 0:
                if header or counts.total() != 1 or len(payload) not in (28,36):
                    raise ValueError('invalid_header')
                version, session, device, frequency = struct.unpack_from('<IQQQ', payload)
                if (version,len(payload)) not in ((1,28),(2,36),(3,36),(4,36),(5,36),(6,36),(7,36),(8,36),(9,36),(10,36),(11,36),(12,36),(13,36)) or not all((session, device, frequency)):
                    raise ValueError('invalid_identity')
                header = {'session':session,'device':device,'frequency':frequency}
                if version in (2,3,4,5,6,7,8,9,10,11,12,13):
                    target,control=struct.unpack_from('<2I',payload,28)
                    if (target,control)!=((600 if version>=12 else 420 if version==11 else 240),30):raise ValueError('invalid_world_profile_boundaries')
                    header.update(profile='world_pilot',target_presents=target,control_presents=control)
                    if version==3:header['coverage_revision']=6
                    if version==4:header['coverage_revision']=7
                    if version==5:header['coverage_revision']=8
                    if version==6:header['coverage_revision']=9
                    if version==7:header['coverage_revision']=10
                    if version==8:header['coverage_revision']=11
                    if version==9:header['coverage_revision']=12
                    if version==10:header['coverage_revision']=13
                    if version==11:header['coverage_revision']=14
                    if version==12:header['coverage_revision']=15
                    if version==13:header['coverage_revision']=16
            elif event == 1:
                if not header or pending is not None or len(payload) < SNAPSHOT.size + 32 + PIPELINE.size:
                    raise ValueError('unpaired_or_short_draw')
                fields = SNAPSHOT.unpack_from(payload)
                magic, version, session, ordinal, client_tick, client_thread, status = fields[:7]
                if magic != 0x314f444c or version not in (1,2) or session != header['session'] or not ordinal or not client_thread or status not in STATUS:
                    raise ValueError('invalid_snapshot_identity')
                previous = client_threads.get(client_thread, 0)
                if ordinal in ordinals or ordinal <= previous:
                    raise ValueError('duplicate_or_reordered_client_draw')
                ordinals.add(ordinal); client_threads[client_thread] = ordinal
                statuses[STATUS[status]] += 1
                matrices = fields[7:55]
                snapshot_size=SNAPSHOT.size+(MATERIAL.size if version==2 else 0)
                vertex_format,flags,shader=0,0,''
                if version==2:
                    vertex_format,flags,reserved,raw_name=MATERIAL.unpack_from(payload,SNAPSHOT.size)
                    if reserved or flags&~3 or raw_name[-1]:raise ValueError('invalid_material_wire')
                    shader=raw_name.split(b'\0',1)[0].decode('ascii')
                    if flags&2 and (not flags&1 or not shader):raise ValueError('invalid_material_identity')
                    material_flags[flags]+=1
                    if shader:shaders[shader]+=1
                if status == 1 and (not all(math.isfinite(x) for x in matrices)
                                    or not nonsingular(matrices[16:32]) or not nonsingular(matrices[32:48])):
                    raise ValueError('nonfinite_valid_matrix')
                kind, topology, *args = struct.unpack_from('<8I', payload, snapshot_size)
                if kind not in (1,2,3,4) or topology not in range(1,7):
                    raise ValueError('invalid_draw_arguments')
                kinds[kind] += 1
                state_at = snapshot_size + 32
                v8=header.get('coverage_revision') in (8,9,10,11,12,13,14,15,16)
                v7=header.get('coverage_revision') in (7,8,9,10,11,12,13,14,15,16)
                pipeline=PIPELINE_V8 if v8 else PIPELINE_V7 if v7 else PIPELINE
                state = pipeline.unpack_from(payload, state_at)
                failures, stream_offset, stride, stream_frequency = state[:4]
                alpha, test, depth, depth_write = state[4:8]
                host_vp = state[8:14]
                width, height, fmt, elements = state[14:18]
                cull=state[18] if v7 else None
                if v7 and not failures and cull not in (1,2,3):raise ValueError('invalid_cull_mode')
                if elements > 65 or len(payload) != state_at + pipeline.size + elements * 8:
                    raise ValueError('invalid_vertex_declaration_size')
                client_vp = fields[57:63]
                world_identity=all(abs(v-float(i%5==0))<=1e-6 for i,v in enumerate(matrices[:16]))
                p=matrices[32:48];v=matrices[16:32]
                rigid=all(abs(sum(v[i*4+k]*v[j*4+k] for k in range(3))-float(i==j))<=.01 for i in range(3) for j in range(3))
                camera=(rigid and abs(p[11]+1)<=1e-5 and abs(p[15])<=1e-5 and p[0]>0 and p[5]>0
                        and p[10]<-1 and 4<=p[14]/p[10]<=32)
                expanded=header.get('coverage_revision') in (6,7) and counts[3]>=120
                unlit_hud_enabled=expanded or v7 and counts[3]>=30
                lightmapped=shader in ('LightmappedGeneric','LightmappedGeneric_DX9')
                tangent=v7 and (expanded or v8 and counts[3]>=30) and vertex_format in (0x480033,0x2480033) and lightmapped
                material_safe=(vertex_format==0x480003 and lightmapped) or (unlit_hud_enabled and vertex_format==0x80503 and shader=='UnlitGeneric') or tangent
                pending_world_safe=(status==1 and fields[55] and fields[56] and flags==3 and material_safe and not failures
                    and kind in (1,2) and topology in (4,5)
                    and world_identity and camera and fields[57:59]==(0,0) and fields[61:63]==(0.,1.) and fmt==113
                    and (width,height)==fields[59:61] and not alpha and not test and depth==depth_write==1)
                if status == 1 and tuple(client_vp) != tuple(host_vp):
                    viewport_mismatch += 1
                declaration = payload[state_at + pipeline.size:]
                core=bytes.fromhex('000000000200000000000c000200030000001800010005000000200001000501')
                terminator=bytes.fromhex('ff00000011000000')
                auxiliary=bytes.fromhex('02000000030000010200100002000301')
                layout_safe=(stride in (40,48) and stream_frequency==1 and stream_offset%4==0
                    and (declaration==core+terminator or stride==48 and declaration==core+auxiliary+terminator))
                unlit_decl=bytes.fromhex('000000000200000000000c000200030000001800010005000100000004000a01020000000300000102001000020003010300000000000002ff00000011000000')
                if vertex_format==0x80503:
                    layout_safe=unlit_hud_enabled and stride==32 and stream_frequency==1 and stream_offset%4==0 and declaration==unlit_decl
                if tangent:
                    tangent2=core+bytes.fromhex('00002800020006000000340002000700')+auxiliary+terminator
                    tangent3=core+bytes.fromhex('0000280001000502000030000200060000003c0002000700')+auxiliary+terminator
                    expected_decl,expected_stride=(tangent2,64) if vertex_format==0x480033 else (tangent3,80)
                    layout_safe=stride==expected_stride and stream_frequency==1 and stream_offset%4==0 and declaration==expected_decl
                pending_world_safe=pending_world_safe and layout_safe
                post_decl=bytes.fromhex('000000000200000000000c000100050002000000030000010200100002000301ff00000011000000')
                rigid_decl=bytes.fromhex('000000000200000000000c0005000300000010000100050000000c00050006000100000004000a01020000000300000102001000050003010300000000000002ff00000011000000')
                rigid_alt=bytes.fromhex('000000000200000000000c0005000300000010000100050000000c0005000600020000000300000102001000050003010300000000000002ff00000011000000')
                scene_common=(v8 and status==1 and fields[55] and fields[56] and flags==3 and not failures and camera and affine_rigid(matrices[16:32])
                    and fields[57:59]==(0,0) and fields[61:63]==(0.,1.) and fmt==113 and (width,height)==fields[59:61]
                    and kind in (1,2) and topology in (4,5) and stride==32 and stream_frequency==1 and stream_offset%4==0 and not alpha and not test)
                pending_sky_safe=(scene_common and counts[3]>=120 and world_identity and shader=='NekoSky' and vertex_format==0x80001
                    and not depth and not depth_write and declaration==post_decl)
                pending_rigid_safe=(scene_common and counts[3]>=180 and affine_rigid(matrices[:16]) and shader=='VertexLitGeneric' and depth==depth_write==1
                    and ((vertex_format==0x80503 and declaration==rigid_decl) or (vertex_format==0xa0403 and declaration==rigid_alt)))
                all_identity=all(abs(v-float(i%5==0))<=1e-6 for part in (matrices[:16],matrices[16:32],matrices[32:48]) for i,v in enumerate(part))
                model_phase_safe=(header.get('coverage_revision')!=14 or shader not in ('VertexLitGeneric','NekoToon') or (240<=counts[3]<360 if shader=='NekoToon' else 180<=counts[3]<240 or 300<=counts[3]<360))
                if header.get('coverage_revision')==15 and shader in ('VertexLitGeneric','NekoToon'):
                    model_phase_safe=(180<=counts[3]<540 if shader=='NekoToon' else 360<=counts[3]<540)
                if header.get('coverage_revision')==16 and shader in ('VertexLitGeneric','NekoToon'):
                    model_phase_safe=(180<=counts[3]<540 if shader=='NekoToon' else 240<=counts[3]<540)
                pending_bulk_safe=(model_phase_safe and header.get('coverage_revision') in (10,11,12,13,14,15,16) and counts[3]>=180 and status==1 and fields[55] and fields[56] and flags==3
                    and (shader in STATIC_SHADERS or header.get('coverage_revision') in (13,14,15,16) and shader=='NekoToon') and static_affine(matrices[:16]) and affine_rigid(matrices[16:32]) and camera
                    and fields[57:59]==(0,0) and fields[61:63]==(0.,1.) and fmt==113 and (width,height)==fields[59:61]
                    and not failures and kind in (1,2) and topology in (4,5) and not alpha and depth==depth_write==1
                    and stream_frequency==1 and stream_offset%4==0 and static_layout(declaration,stride,header.get('coverage_revision') in (11,12,13,14,15,16) and shader=='VertexLitGeneric' or header.get('coverage_revision') in (13,14,15,16) and shader=='NekoToon'))
                pending_hud_safe=(unlit_hud_enabled and status==1 and fields[55] and fields[56] and flags==3 and shader=='Neko_Engine_Post' and vertex_format==0x80001
                    and kind in (1,2) and topology==4 and stride==32 and stream_frequency==1 and stream_offset%4==0 and declaration==post_decl
                    and all_identity and fields[57:59]==(0,0) and fields[61:63]==(0.,1.) and fmt==21
                    and (width,height)==fields[59:61] and not alpha and not test and not depth and not depth_write)
                layout_key = hashlib.sha256(struct.pack('<II', stride, stream_frequency) + declaration).hexdigest()[:16]
                if layout_key not in layouts:
                    layouts[layout_key] = {'stride':stride,'frequency':stream_frequency,
                        'declaration_hex':declaration.hex(),'draws':0,'opaque_depth_draws':0,'query_failure_draws':0}
                layout = layouts[layout_key]; layout['draws'] += 1
                layout['opaque_depth_draws'] += int(not alpha and not test and depth == 1 and depth_write == 1)
                layout['query_failure_draws'] += int(bool(failures))
                pending_static=None
                if status==1 and flags==3 and fields[55] and fields[56] and (shader in STATIC_SHADERS or header.get('coverage_revision') in (12,13,14,15,16) and shader=='NekoToon') and camera and fmt==113:
                    phase='raster' if counts[3]<30 else 'baseline_world' if counts[3]<120 else 'expanded_sky' if v8 and counts[3]<180 else 'expanded_models' if v8 else 'expanded_world'
                    bones=any(declaration[i+6] in (1,2) for i in range(0,max(0,len(declaration)-8),8))
                    key=(phase,shader,vertex_format,layout_key,alpha,test,depth,depth_write,static_affine(matrices[:16]))
                    if key not in static_groups and len(static_groups)<1024:
                        static_groups[key]={'phase':phase,'shader':shader,'vertex_format':hex(vertex_format),'stride':stride,
                            'declaration_hex':declaration.hex(),'has_bone_fields':bones,'affine_world':key[-1],
                            'alpha_blend':alpha,'alpha_test':test,'depth':depth,'depth_write':depth_write,'draws':0,'converted':0,'reports':{}}
                    pending_static=static_groups.get(key)
                    if pending_static is None:static_omitted+=1
                    else:pending_static['draws']+=1
                pending_attribution=None
                old_group=world_identity and depth==depth_write==1 and (lightmapped or shader=='UnlitGeneric')
                scene_group=v8 and affine_rigid(matrices[:16]) and (shader=='VertexLitGeneric' and depth==depth_write==1 or shader=='NekoSky' and not depth and not depth_write)
                if header.get('profile') and status==1 and flags==3 and camera and fmt==113 and not alpha and not test and (old_group or scene_group):
                    phase='raster' if counts[3]<30 else 'baseline_world' if counts[3]<120 else 'expanded_world'
                    if v8 and counts[3]>=120:phase='expanded_sky' if counts[3]<180 else 'expanded_models'
                    # Same material may use several layouts/cull states. Keep
                    # separate groups; do not invent a Source object identity.
                    group_key=(phase,fields[55],fields[56],vertex_format,shader,layout_key,cull)
                    if group_key not in attribution and len(attribution)<256:
                        attribution[group_key]={'phase':phase,'material':fields[55],'table':fields[56],
                            'vertex_format':hex(vertex_format),'shader':shader,'stride':stride,'frequency':stream_frequency,
                            'declaration_hex':declaration.hex(),'cull_mode':cull,'first_ordinal':ordinal,'first_rpc_uid':uid,
                            'first_frame':counts[3]+1,'first_draw_kind':kind,'first_topology':topology,'first_args':args,
                            'draws':0,'converted':0,'decisions':{}}
                        if v8:attribution[group_key].update(lighting=state[19],fog=state[20],original_category=state[21])
                    pending_attribution=attribution.get(group_key)
                    if pending_attribution is None:attribution_omitted_draws+=1
                    else:
                        pending_attribution['draws']+=1
                        pending_attribution['last_ordinal']=ordinal
                        pending_attribution['last_frame']=counts[3]+1
                if status == 1:
                    key_bytes = payload[40+64:40+192] + struct.pack('<6I', *fields[57:61], width, height)
                    key = hashlib.sha256(key_bytes).hexdigest()[:16]
                    if key not in cameras and len(cameras) < 1024:
                        cameras[key] = {'view':list(matrices[16:32]),'projection':list(matrices[32:48]),
                            'client_viewport':list(client_vp),'host_viewport':list(host_vp),
                            'render_target':[width,height,fmt],'draws':0}
                    if key in cameras:
                        cameras[key]['draws'] += 1
                pending = uid
                pending_conversion=False
            elif event == 6:
                if pending!=uid or pending_conversion or len(payload)!=32 or not header.get('profile'):
                    raise ValueError('conversion_uid_or_scope_mismatch')
                decision,code,capture,apply,restore,converted,called,physical=struct.unpack('<8I',payload)
                if decision>(12 if header.get('coverage_revision') in (10,11,12,13,14,15,16) else 11 if header.get('coverage_revision') in (8,9) else 9) or code>10 or converted not in (0,1) or called not in (0,1):raise ValueError('invalid_conversion_report')
                decisions[decision]+=1;conversions['reports']+=1;pending_conversion=True
                if pending_attribution is not None:
                    pending_attribution['converted']+=converted
                    bucket=pending_attribution['decisions'];bucket[str(decision)]=bucket.get(str(decision),0)+1
                if pending_static is not None:
                    pending_static['converted']+=converted
                    key=f'{decision}:{code}';pending_static['reports'][key]=pending_static['reports'].get(key,0)+1
                if decision==9:
                    if not pending_hud_safe or converted or code!=9 or called or physical:
                        raise ValueError('unsafe_or_failed_hud_replacement')
                    conversions['hud_replaced']+=1
                if converted:
                    safe=pending_world_safe if decision==8 else pending_sky_safe if decision==10 else pending_rigid_safe if decision==11 else pending_bulk_safe if decision==12 else False
                    if code not in (9,10) or not called or counts[3]<header['control_presents'] or not safe:
                        raise ValueError('unsafe_world_conversion_or_control_modified')
                    if decision in (10,11,12,13):
                        # Scene scopes never start RT without a prior world Draw.
                        if conversions['converted']-conversions.get('sky_converted',0)-conversions.get('rigid_converted',0)-conversions.get('bulk_static_converted',0)<=0:raise ValueError('scene_before_successful_world')
                        conversions['sky_converted' if decision==10 else 'rigid_converted' if decision==11 else 'bulk_static_converted']+=1
                    conversions['converted']+=1
                if any(x&0x80000000 for x in (capture,apply,restore)) or code in (7,8,10):
                    conversions['state_failures']+=1
            elif event in (2,3,4,5):
                if len(payload) != 4:
                    raise ValueError('invalid_hresult_record')
                result, = struct.unpack('<I', payload)
                if event == 2:
                    if pending is None or pending != uid:
                        raise ValueError('draw_result_uid_mismatch')
                    if header.get('profile') and not pending_conversion:raise ValueError('missing_world_conversion_report')
                    pending = None
                elif pending is not None:
                    raise ValueError('draw_result_missing_at_boundary')
                if result & 0x80000000:
                    errors.append('physical_api_failure')
                if event == 5:
                    errors.append('unobserved_draw_inside_segment')
            elif event == 8:
                if len(payload) != 52 or pending is not None:
                    raise ValueError('invalid_footer_or_pending_draw')
                draws, valid, presents, resets, failures, unobserved, reason = struct.unpack('<6QI', payload)
                footer = dict(draws=draws,valid=valid,presents=presents,resets=resets,failures=failures,unobserved=unobserved,reason=reason)
                if (draws, valid, presents, resets, unobserved) != (counts[1],statuses['valid'],counts[3],counts[4],counts[5]):
                    raise ValueError('footer_counter_mismatch')
            else:
                raise ValueError('unknown_event')
        except (ValueError, struct.error, UnicodeDecodeError) as error:
            errors.append(str(error))
    if not header or not footer:
        errors.append('missing_header_or_footer')
    if footer and (footer['reason'] or footer['failures'] or footer['unobserved'] or footer['presents'] != (header or {}).get('target_presents',120)):
        errors.append('incomplete_or_failed_segment')
    if viewport_mismatch:
        errors.append('client_host_viewport_mismatch')
    pipeline_failures=sum(layout['query_failure_draws'] for layout in layouts.values())
    if pipeline_failures:
        errors.append('pipeline_query_failure')
    if not statuses['valid'] or any(k != 'valid' and v for k,v in statuses.items()):
        errors.append('source_state_incomplete')
    return {'schema':1,'scope':'draw_rpc_consumer_observation_not_source_queue_or_game_rt',
            'pass':not errors,'errors':sorted(set(errors)),'header':header,'footer':footer,
            'status_counts':dict(statuses),'draw_kinds':dict(kinds),'host_thread':host_thread,
            'client_draw_threads':sorted(client_threads),'viewport_mismatches':viewport_mismatch,
            'pipeline_query_failure_draws':pipeline_failures,
            'camera_groups':cameras,'vertex_layouts':layouts,
            'material_decode_flags':dict(material_flags),'shader_names':dict(shaders),
            'world_decisions':dict(decisions),'world_conversion':dict(conversions),
            'world_material_attribution':{'groups':list(attribution.values()),'limit':256,'omitted_draws':attribution_omitted_draws,
                'scope':'main-camera candidate opaque HDR world draws; original cull state, material/layout identity and RPC linkage; no screen-space or per-object identity proof'},
            'static_family_inventory':{'groups':list(static_groups.values()),'limit':1024,'omitted_draws':static_omitted,
                'scope':'static shader families and, in v12, audit-only NekoToon at main-camera HDR targets; includes bones, cutout and rejected draws; counts are not pixel, object or whole-map coverage'},
            'world_normalization':'pass' if header and header.get('profile') and conversions['converted'] and not conversions['state_failures'] and not errors else 'unrun_or_incomplete',
            'source_queue_execution':'unrun','source_parallel_preparation':'unrun','static_world_classification':'unverified',
            'game_rt':'unrun','performance':'unrun'}


if __name__ == '__main__':
    import argparse
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('trace',type=Path);parser.add_argument('--output',type=Path)
    args=parser.parse_args()
    if args.trace.is_dir():
        traces={p.name:analyze(p) for p in args.trace.glob('rt-draw-host-*.bin')}
        result={'schema':1,'scope':'batch_consumer_observation','pass':bool(traces) and all(t['pass'] for t in traces.values()),'traces':traces}
    else:
        result=analyze(args.trace)
    if args.output:
        args.output.write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps({k:v for k,v in result.items() if k not in ('camera_groups','vertex_layouts','world_material_attribution')},indent=2))
    raise SystemExit(0 if result['pass'] else 1)

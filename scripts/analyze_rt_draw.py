"""Validate consumer snapshots embedded in physical Draw RPCs; no game RT claim."""
from collections import Counter
import hashlib
import json
import math
from pathlib import Path
import struct

RECORD = struct.Struct('<IIQIIQ')
SNAPSHOT = struct.Struct('<IIQQQII48fII4I2f')
PIPELINE = struct.Struct('<12I2f4I')
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


def analyze(path):
    path = Path(path)
    if path.stat().st_size > 130 * 1024 * 1024:
        raise ValueError('Draw trace exceeds budget')
    data = path.read_bytes()
    offset = 0
    errors, pending, header, footer = [], None, None, None
    counts, statuses, kinds = Counter(), Counter(), Counter()
    client_threads, ordinals = {}, set()
    host_thread, last_tick = None, 0
    cameras, layouts = {}, {}
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
                if header or counts.total() != 1 or len(payload) != 28:
                    raise ValueError('invalid_header')
                version, session, device, frequency = struct.unpack('<IQQQ', payload)
                if version != 1 or not all((session, device, frequency)):
                    raise ValueError('invalid_identity')
                header = {'session':session,'device':device,'frequency':frequency}
            elif event == 1:
                if not header or pending is not None or len(payload) < SNAPSHOT.size + 32 + PIPELINE.size:
                    raise ValueError('unpaired_or_short_draw')
                fields = SNAPSHOT.unpack_from(payload)
                magic, version, session, ordinal, client_tick, client_thread, status = fields[:7]
                if magic != 0x314f444c or version != 1 or session != header['session'] or not ordinal or not client_thread or status not in STATUS:
                    raise ValueError('invalid_snapshot_identity')
                previous = client_threads.get(client_thread, 0)
                if ordinal in ordinals or ordinal <= previous:
                    raise ValueError('duplicate_or_reordered_client_draw')
                ordinals.add(ordinal); client_threads[client_thread] = ordinal
                statuses[STATUS[status]] += 1
                matrices = fields[7:55]
                if status == 1 and (not all(math.isfinite(x) for x in matrices)
                                    or not nonsingular(matrices[16:32]) or not nonsingular(matrices[32:48])):
                    raise ValueError('nonfinite_valid_matrix')
                kind, topology, *args = struct.unpack_from('<8I', payload, SNAPSHOT.size)
                if kind not in (1,2,3,4) or topology not in range(1,7):
                    raise ValueError('invalid_draw_arguments')
                kinds[kind] += 1
                state_at = SNAPSHOT.size + 32
                state = PIPELINE.unpack_from(payload, state_at)
                failures, stream_offset, stride, stream_frequency = state[:4]
                alpha, test, depth, depth_write = state[4:8]
                host_vp = state[8:14]
                width, height, fmt, elements = state[14:]
                if elements > 65 or len(payload) != state_at + PIPELINE.size + elements * 8:
                    raise ValueError('invalid_vertex_declaration_size')
                client_vp = fields[57:63]
                if status == 1 and tuple(client_vp) != tuple(host_vp):
                    viewport_mismatch += 1
                declaration = payload[state_at + PIPELINE.size:]
                layout_key = hashlib.sha256(struct.pack('<II', stride, stream_frequency) + declaration).hexdigest()[:16]
                if layout_key not in layouts:
                    layouts[layout_key] = {'stride':stride,'frequency':stream_frequency,
                        'declaration_hex':declaration.hex(),'draws':0,'opaque_depth_draws':0,'query_failure_draws':0}
                layout = layouts[layout_key]; layout['draws'] += 1
                layout['opaque_depth_draws'] += int(not alpha and not test and depth == 1 and depth_write == 1)
                layout['query_failure_draws'] += int(bool(failures))
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
            elif event in (2,3,4,5):
                if len(payload) != 4:
                    raise ValueError('invalid_hresult_record')
                result, = struct.unpack('<I', payload)
                if event == 2:
                    if pending is None or pending != uid:
                        raise ValueError('draw_result_uid_mismatch')
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
        except (ValueError, struct.error) as error:
            errors.append(str(error))
    if not header or not footer:
        errors.append('missing_header_or_footer')
    if footer and (footer['reason'] or footer['failures'] or footer['unobserved'] or footer['presents'] != 120):
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
    print(json.dumps({k:v for k,v in result.items() if k not in ('camera_groups','vertex_layouts')},indent=2))
    raise SystemExit(0 if result['pass'] else 1)

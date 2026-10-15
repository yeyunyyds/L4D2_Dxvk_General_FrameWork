"""Check RT metadata/physical-command correspondence; never infer game RT quality."""

import argparse
from collections import Counter
import json
from pathlib import Path
import struct

RECORD = struct.Struct('<IIQIIQ')
PACKET = struct.Struct('<6I8Q2IQ')
MAX_PACKET = 1024 * 1024


def analyze(path):
    data = Path(path).read_bytes()
    offset = 0
    events = Counter()
    errors = []
    packets = 0
    generation = None
    identity = None
    sequence = 0
    pending = None
    draws = {}
    ended = None
    presented = []
    last_tick = 0
    consumer = None
    footer = None
    frequency = None
    saw_reset = False
    while offset < len(data):
        if len(data) - offset < RECORD.size:
            errors.append('truncated_record_header'); break
        event, size, uid, thread, reserved, tick = RECORD.unpack_from(data, offset)
        if size < RECORD.size or size > RECORD.size + MAX_PACKET or size > len(data) - offset or reserved:
            errors.append('invalid_record_size_or_flags'); break
        payload = data[offset + RECORD.size:offset + size]
        offset += size
        events[event] += 1
        if consumer is None:
            consumer = thread
        if consumer != thread:
            errors.append('consumer_thread_changed')
        if tick < last_tick:
            errors.append('nonmonotonic_consumer_clock')
        last_tick = tick
        if event == 0:
            if len(payload) != 8 or frequency is not None:
                errors.append('invalid_clock_header')
            else:
                frequency = struct.unpack('<Q', payload)[0]
        elif event == 1:
            if len(payload) < PACKET.size:
                errors.append('truncated_packet'); continue
            fields = PACKET.unpack_from(payload)
            magic, version, header_size, total_size, kind, flags = fields[:6]
            session, run, device, gen, map_id, frame, view, seq = fields[6:14]
            producer, pad, producer_tick = fields[14:]
            if magic != 0x5452444c or version != 1 or header_size != 104 or total_size != len(payload) or flags or pad or not 1 <= kind <= 10:
                errors.append('invalid_packet_header'); continue
            key = (session, run, device)
            if identity is None:
                identity = key
                if kind != 1:
                    errors.append('missing_hello')
            if key != identity or not all(key) or not producer or seq != sequence + 1:
                errors.append('packet_identity_or_sequence')
            if generation is not None:
                if kind == 8 and gen != generation[0] + 1:
                    errors.append('reset_generation')
                elif kind == 10 and map_id != generation[1] + 1:
                    errors.append('map_generation')
                elif kind not in (8, 10) and (gen, map_id) != generation:
                    errors.append('stale_generation')
            sequence = seq; generation = (gen, map_id); packets += 1
            if kind == 5:
                if pending is not None:
                    errors.append('metadata_draw_not_consumed')
                pending = (frame, view, uid)
            elif kind == 7:
                ended = frame
            elif kind in (8, 10):
                pending = None; ended = None
                if kind == 8:
                    if not saw_reset:
                        errors.append('metadata_reset_without_physical_reset')
                    saw_reset = False
        elif event == 2:
            if pending is None or uid <= pending[2] or uid in draws:
                errors.append('physical_draw_without_unique_preceding_metadata')
            else:
                draws[uid] = {'frame': pending[0], 'view': pending[1], 'result': None}
            pending = None
        elif event == 3:
            if len(payload) != 16:
                errors.append('present_payload'); continue
            frame, hresult, error = struct.unpack('<QII', payload)
            if frame != ended or hresult & 0x80000000 or error:
                errors.append('present_frame_or_result')
            else:
                presented.append(frame)
            ended = None
        elif event == 4:
            if len(payload) != 4:
                errors.append('reset_payload')
            else:
                hresult, = struct.unpack('<I', payload)
                saw_reset = not bool(hresult & 0x80000000)
        elif event == 5:
            if len(payload) != 4:
                errors.append('draw_result_payload'); continue
            hresult, = struct.unpack('<I', payload)
            if uid in draws:
                if draws[uid]['result'] is not None or hresult & 0x80000000:
                    errors.append('duplicate_or_failed_draw_result')
                draws[uid]['result'] = hresult
        elif event == 7:
            errors.append('host_rejected_metadata')
        elif event == 8:
            if len(payload) != 40 or footer is not None:
                errors.append('invalid_footer')
            else:
                footer = struct.unpack('<4Q2I', payload)
        elif event == 9:
            errors.append('trace_budget_exhausted')
        else:
            errors.append('unknown_trace_event')
    if not frequency or footer is None or not packets or not draws or not presented:
        errors.append('missing_required_trace_evidence')
    if pending is not None or ended is not None or any(d['result'] is None for d in draws.values()):
        errors.append('unfinished_frame_or_draw')
    if footer:
        if footer[0] != len(draws) or footer[2] != len(presented) or footer[3] or footer[4] or footer[5]:
            errors.append('footer_counts_or_failure')
    return {'schema': 1, 'scope': 'metadata_transport_observer_only',
            'result': 'pass' if not errors else 'fail', 'errors': sorted(set(errors)),
            'packets': packets, 'labelled_draws': len(draws), 'successful_presents': len(presented),
            'presented_metadata_frame_ids': presented, 'events': dict(sorted(events.items())),
            'consumer_thread': consumer, 'qpc_frequency': frequency,
            'source_game_queue': 'unrun', 'image_quality': 'unrun', 'b580_rt': 'unrun',
            'performance_acceptance': 'unrun'}


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('trace', type=Path)
    p.add_argument('--output', type=Path)
    a = p.parse_args()
    report = analyze(a.trace)
    text = json.dumps(report, indent=2) + '\n'
    if a.output:
        a.output.write_text(text)
    print(text, end='')
    raise SystemExit(0 if report['result'] == 'pass' else 1)

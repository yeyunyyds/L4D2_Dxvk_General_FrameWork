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
    active_draw = None
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
            ordered = seq > sequence if kind == 8 and saw_reset else seq == sequence + 1
            if key != identity or not all(key) or not gen or not map_id or not producer or not ordered or (kind == 1 and packets):
                errors.append('packet_identity_or_sequence')
            if generation is not None:
                if kind == 8 and (gen != generation[0] + 1 or map_id != generation[1]):
                    errors.append('reset_generation')
                elif kind == 10 and (map_id != generation[1] + 1 or gen != generation[0]):
                    errors.append('map_generation')
                elif kind not in (8, 10) and (gen, map_id) != generation:
                    errors.append('stale_generation')
            sequence = seq; generation = (gen, map_id); packets += 1
            if kind == 5:
                if pending is not None:
                    errors.append('metadata_draw_not_consumed')
                if len(payload) < 104 + 80:
                    errors.append('truncated_draw_metadata'); continue
                draw_id, = struct.unpack_from('<Q', payload, 104)
                pending = (frame, view, uid, draw_id, gen, map_id)
            elif kind == 7:
                ended = frame
            elif kind in (8, 10):
                pending = None; ended = None
                if kind == 8:
                    if not saw_reset:
                        errors.append('metadata_reset_without_physical_reset')
                    saw_reset = False
        elif event == 2:
            if pending is None or uid == pending[2] or active_draw is not None:
                errors.append('physical_draw_without_unique_preceding_metadata')
            else:
                key = (pending[0], pending[3], pending[4], pending[5])
                if key in draws:
                    errors.append('duplicate_draw_identity')
                draws[key] = {'frame': pending[0], 'view': pending[1], 'result': None}
                active_draw = (key, uid)
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
            if active_draw is not None:
                key, expected_uid = active_draw
                if uid != expected_uid or draws[key]['result'] is not None or hresult & 0x80000000:
                    errors.append('duplicate_or_failed_draw_result')
                draws[key]['result'] = hresult
                active_draw = None
        elif event == 6:
            if len(payload) != 4 or pending is not None or ended is not None:
                errors.append('unlabelled_present_payload_or_open_frame')
        elif event == 7:
            errors.append('host_rejected_metadata')
        elif event == 8:
            if len(payload) != 40 or footer is not None:
                errors.append('invalid_footer')
            else:
                footer = struct.unpack('<4Q2I', payload)
        elif event == 9:
            errors.append('trace_budget_exhausted')
        elif event == 10:
            if len(payload) < PACKET.size:
                errors.append('truncated_skipped_packet')
            else:
                fields = PACKET.unpack_from(payload)
                if fields[0] != 0x5452444c or fields[1] != 1 or fields[3] != len(payload) or tuple(fields[6:9]) != identity:
                    errors.append('invalid_skipped_packet')
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
            'unlabelled_presents': events[6],
            'skipped_metadata_packets': events[10],
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

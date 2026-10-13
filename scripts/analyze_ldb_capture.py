"""Check bounded Host observations and exported images; never infer world/HUD labels."""
import argparse
import hashlib
import json
from pathlib import Path
import struct


def image_files(directory, event):
    prefix = event['prefix']
    if Path(prefix).name != prefix or prefix in ('.', '..') or '\\' in prefix:
        raise ValueError('Unsafe snapshot prefix')
    width, height = event['color']['width'], event['color']['height']
    if event['depth']['width'] != width or event['depth']['height'] != height:
        raise ValueError('Color/depth extent mismatch')
    result = {}
    for suffix, channels, magic, maximum in [('color.ppm', 3, b'P6', b'255'), ('depth.pgm', 2, b'P5', b'65535')]:
        path = directory / f'{prefix}-{suffix}'
        content = path.read_bytes()
        parts = content.split(b'\n', 3)
        if len(parts) != 4 or parts[:3] != [magic, f'{width} {height}'.encode(), maximum] or len(parts[3]) != width * height * channels:
            raise ValueError(f'Invalid/incomplete image: {path.name}')
        result[suffix] = hashlib.sha256(content).hexdigest()
    raw = (directory / f'{prefix}-depth.f32').read_bytes()
    if len(raw) != width * height * 4:
        raise ValueError('Incomplete raw depth image')
    result['depth.f32'] = hashlib.sha256(raw).hexdigest()
    return result


def analyze(directory):
    directory = Path(directory)
    events = [json.loads(line) for line in (directory / 'events.jsonl').read_text().splitlines() if line.strip()]
    if not events or events[0].get('event') != 'session' or events[-1].get('event') != 'completed':
        raise ValueError('Capture did not complete; inspect diagnostic_error/device_destroyed')
    if any(e.get('device_id') != events[0]['device_id'] or e.get('sequence') != i for i, e in enumerate(events)):
        raise ValueError('Mixed device log or missing/duplicated events')
    start = events[0]['start_frame']
    count = events[0]['frames_requested']
    presents = [e for e in events if e['event'] == 'present_result']
    if [e['frame'] for e in presents] != list(range(start, start + count)) or any(e['hresult'] != 0 for e in presents):
        raise ValueError('Missing or failed Present')
    generation = 0
    snapshots = []
    skips = []
    prefixes = set()
    for event in events:
        if event['event'] == 'reset_result' and event['hresult'] == 0:
            generation += 1
        if event['generation'] != generation:
            raise ValueError('Reset generation mismatch')
        if event['event'] == 'diagnostic_error':
            raise ValueError('Diagnostic failure')
        if event['event'] == 'snapshot':
            if not start <= event['frame'] < start + count or event['prefix'] in prefixes:
                raise ValueError('Out-of-range or duplicated snapshot')
            if event.get('source_layout_restored') is not True or event.get('gpu_copy_completed') is not True:
                raise ValueError('Snapshot copy/restoration not complete')
            prefixes.add(event['prefix'])
            snapshots.append({**event, 'files_sha256': image_files(directory, event)})
        if event['event'] == 'snapshot_skipped':
            skips.append(event)
    if not snapshots:
        raise ValueError('No usable color/depth snapshots; metadata alone is insufficient')
    for present in presents:
        if present['snapshots'] != sum(e['frame'] == present['frame'] for e in snapshots):
            raise ValueError('Snapshot count differs from Present summary')
    return {'status': 'passed_observation_structure_and_files', 'directory': str(directory), 'frames': len(presents),
            'snapshots': snapshots, 'skips': skips, 'successful_resets': generation,
            'draw_events_truncated': any(e.get('draw_events_truncated') is True for e in events),
            'semantic_labels': 'unknown', 'gpu_fg_execution': 'unrun'}


def verify_fixture(root, mode, frames):
    root = Path(root)
    if mode == 'disabled':
        if root.exists():
            raise ValueError('Disabled observer created output')
        return {'status': 'passed', 'mode': 'disabled', 'output_created': False}
    folders = [p for p in root.iterdir() if p.is_dir()]
    if len(folders) != 1:
        raise ValueError('Fixture must produce one device session')
    directory = folders[0]
    if mode == 'waiting':
        events = [json.loads(line) for line in (directory / 'events.jsonl').read_text().splitlines()]
        allowed = {'session', 'reset_result', 'device_destroyed'}
        if not events or events[-1]['event'] != 'device_destroyed' or any(e['event'] not in allowed for e in events):
            raise ValueError('Untriggered observer captured frames or completed early')
        if list(directory.glob('*-color.ppm')) or list(directory.glob('*-depth.f32')):
            raise ValueError('Untriggered observer exported images')
        return {'status': 'passed', 'mode': mode, 'snapshots': 0, 'premature_completion': False}
    report = analyze(directory)
    events = [json.loads(line) for line in (directory / 'events.jsonl').read_text().splitlines()]
    shift = 1 if mode == 'triggered' else 0
    expected_frames = frames - shift
    if report['frames'] != expected_frames or report['successful_resets'] != 1 or len(report['snapshots']) != expected_frames * 2:
        raise ValueError('Fixture frame/reset/boundary snapshot count differs')
    if not any(e['event'] == 'reset_result' and e['hresult'] != 0 and e['generation'] == 0 for e in events):
        raise ValueError('Failed Reset generation case not observed')
    for event in report['snapshots']:
        width, height = event['color']['width'], event['color']['height']
        physical_frame = event['frame'] + shift
        expected = (640, 360) if physical_frame < frames // 2 else (576, 296)
        if (width, height) != expected:
            raise ValueError('Fixture extent incorrect across Reset')
        rgb = (directory / (event['prefix'] + '-color.ppm')).read_bytes().split(b'\n', 3)[3]
        raw = (directory / (event['prefix'] + '-depth.f32')).read_bytes()
        for x, y, color, depth in [(width//2, height//2, (32,220,64), 0.25), (2,2,(16+physical_frame%100,32,192),1.0)]:
            offset = y * width + x
            actual = tuple(rgb[offset*3:offset*3+3])
            if any(abs(a-b) > 1 for a,b in zip(actual,color)) or abs(struct.unpack_from('<f',raw,offset*4)[0] - depth) > 0.0001:
                raise ValueError('Exported fixture pixels do not match rendered content')
    if mode == 'triggered' and sum(e['event'] == 'triggered' for e in events) != 1:
        raise ValueError('Trigger activation missing or duplicated')
    return {'status': 'passed', 'mode': mode, 'frames': expected_frames, 'snapshots': len(report['snapshots']),
            'successful_resets': 1, 'exported_color_depth_pixels': 'verified', 'device_and_swapchain_present': 'exercised',
            'l4d2_game_capture': 'unrun', 'full_host_build': 'unrun'}


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('directory', type=Path)
    a = p.parse_args()
    try:
        print(json.dumps(analyze(a.directory), ensure_ascii=False, indent=2))
    except (ValueError, OSError, KeyError, TypeError) as e:
        p.exit(1, f'Capture check failed: {e}\n')

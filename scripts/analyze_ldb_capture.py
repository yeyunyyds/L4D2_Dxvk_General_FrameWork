"""Check bounded Host observations and exported images; never infer world/HUD labels."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import math
from array import array
import sys


def image_files(directory, event):
    prefix = event['prefix']
    if Path(prefix).name != prefix or prefix in ('.', '..') or '\\' in prefix:
        raise ValueError('Unsafe snapshot prefix')
    export=event.get('export', event['color'])
    width, height = export['width'], export['height']
    if event['depth']['width'] != event['color']['width'] or event['depth']['height'] != event['color']['height']:
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
    if event['color'].get('format') == 97:
        if event.get('raw_hdr_color') is not True or event.get('color_preview') != 'reinhard_linear_no_gamma':
            raise ValueError('Missing HDR export metadata')
        hdr = (directory / f'{prefix}-color.rgba16f').read_bytes()
        if len(hdr) != width * height * 8:
            raise ValueError('Incomplete raw RGBA16F image')
        result['color.rgba16f'] = hashlib.sha256(hdr).hexdigest()
    values = array('f'); values.frombytes(raw)
    if sys.byteorder != 'little': values.byteswap()
    finite_count = non_clear = 0
    minimum, maximum_value = math.inf, -math.inf
    for value in values:
        if math.isfinite(value):
            finite_count += 1
            minimum = min(minimum, value); maximum_value = max(maximum_value, value)
            if value != 1.0: non_clear += 1
    result['depth_content'] = {'pixels': len(values), 'finite_pixels': finite_count,
        'min': minimum if finite_count else None, 'max': maximum_value if finite_count else None,
        'non_clear_pixels': non_clear, 'all_depth_equals_1': finite_count == len(values) and non_clear == 0}
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
    previous_qpc=-1
    draw_sequences={}
    metadata_files={}
    for event in events:
        if events[0].get('capture_schema',1)>=2:
            if type(event.get('cpu_qpc')) is not int or event['cpu_qpc']<previous_qpc: raise ValueError('Missing or nonmonotonic CPU timestamp')
            previous_qpc=event['cpu_qpc']
        if event['event']=='draw' and 'state' in event:
            draw_sequences[event['sequence']]=event
            state=event['state']
            sizes={'vs_f32':state['vs_registers']*16,'ps_f32':state['ps_registers']*16,'vs_i32':256,'ps_i32':256,'vs_bool32':64,'ps_bool32':64,'legacy_transforms':192}
            for key,size in sizes.items():
                name=state.get(key)
                if name is None: continue
                if Path(name).name!=name or not name.startswith('state-') or not name.endswith('.bin'): raise ValueError('Unsafe metadata filename')
                if name not in metadata_files: metadata_files[name]=(directory/name).read_bytes()
                if len(metadata_files[name])!=size: raise ValueError('Metadata blob has incorrect length')
            name=state.get('vertex_declaration')
            if name is not None:
                if Path(name).name!=name or not name.startswith('state-') or not name.endswith('.bin'): raise ValueError('Unsafe declaration filename')
                if name not in metadata_files:metadata_files[name]=(directory/name).read_bytes()
                if not 8<=len(metadata_files[name])<=520 or len(metadata_files[name])%8:raise ValueError('Invalid vertex declaration size')
            for key in ('vs_shader','ps_shader'):
                name=state.get(key)
                if name is None: continue
                if Path(name).name!=name or not name.startswith('state-') or not name.endswith('.bin'): raise ValueError('Unsafe shader filename')
                if name not in metadata_files: metadata_files[name]=(directory/name).read_bytes()
                if len(metadata_files[name])%4 or len(metadata_files[name])<8: raise ValueError('Invalid shader bytecode size')
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
            if 'export' in event:
                ex=event['export'];stride=ex['stride']
                if not 1<=stride<=8 or ex['offset']!=stride//2 or ex['width']!=(event['color']['width']+stride-1)//stride or ex['height']!=(event['color']['height']+stride-1)//stride: raise ValueError('Invalid sampled-image mapping')
            if events[0].get('capture_schema',1)>=2 and event.get('last_draw_sequence') not in draw_sequences and event.get('last_draw_sequence')!=2**64-1:
                raise ValueError('Snapshot references an unknown draw sequence')
            if events[0].get('capture_schema',1)>=2 and event.get('last_draw_sequence') in draw_sequences:
                producer=draw_sequences[event['last_draw_sequence']]
                if producer['frame']!=event['frame'] or producer['generation']!=event['generation'] or producer['hresult']!=0: raise ValueError('Snapshot metadata belongs to another frame/generation')
            files = image_files(directory, event)
            content = files.pop('depth_content')
            snapshots.append({**event, 'files_sha256': files, 'depth_content': content})
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
            'metadata_blobs':len(metadata_files),
            'non_clear_depth_snapshots': [e['prefix'] for e in snapshots if e['depth_content']['non_clear_pixels'] > 0],
            'world_depth_verified': False, 'semantic_labels': 'unknown', 'gpu_fg_execution': 'unrun'}


def verify_fixture(root, mode, frames, hdr_scene=False):
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
    if report['frames'] != expected_frames or report['successful_resets'] != 1 or len(report['snapshots']) != expected_frames * (4 if hdr_scene else 2):
        raise ValueError('Fixture frame/reset/boundary snapshot count differs')
    if not any(e['event'] == 'reset_result' and e['hresult'] != 0 and e['generation'] == 0 for e in events):
        raise ValueError('Failed Reset generation case not observed')
    for event in report['snapshots']:
        export=event.get('export',event['color'])
        width,height=export['width'],export['height']
        original=(event['color']['width'],event['color']['height'])
        physical_frame = event['frame'] + shift
        expected = (640, 360) if physical_frame < frames // 2 else (576, 296)
        if original != expected:
            raise ValueError('Fixture extent incorrect across Reset')
        rgb = (directory / (event['prefix'] + '-color.ppm')).read_bytes().split(b'\n', 3)[3]
        raw = (directory / (event['prefix'] + '-depth.f32')).read_bytes()
        if event['color']['format'] == 97:
            references = [e for e in events if e['event'] == 'depth_range_before' and e.get('snapshot_prefix') == event['prefix']]
            if len(references) != 1:
                raise ValueError('HDR snapshot is not associated with a depth-range boundary')
            entering = references[0]['next']['max_z'] == 0.1
            expected_color = (4.0,0.5,0.25,1.0) if entering else (0.125,2.0,0.5,0.75)
            old_range = references[0]['previous']
            geometry_z = 0.25 if entering else 0.05
            expected_depth = old_range['min_z'] + geometry_z * (old_range['max_z'] - old_range['min_z'])
            producer=next(e for e in events if e['sequence']==event['last_draw_sequence'])
            blob=(directory/producer['state']['vs_f32']).read_bytes()
            expected_matrix=(1,0,0,float(physical_frame),0,2,0,0,0,0,3,0,0,0,0,1)
            if struct.unpack_from('<16f',blob,12*16)!=expected_matrix: raise ValueError('Actual constants after StateBlock Apply were not captured')
            hdr = (directory / (event['prefix'] + '-color.rgba16f')).read_bytes()
            offset = (height//2 * width + width//2)
            actual = struct.unpack_from('<4e',hdr,offset*8)
            if any(abs(a-b) > 0.001 for a,b in zip(actual,expected_color)):
                raise ValueError(f'Raw HDR values were clipped, reordered or corrupted: {actual}')
            if abs(struct.unpack_from('<f',raw,offset*4)[0] - expected_depth) > 0.0001:
                raise ValueError('HDR depth offset or near geometry is incorrect')
            expected_preview = tuple(round(max(v,0)/(1+max(v,0))*255) for v in expected_color[:3])
            if tuple(rgb[offset*3:offset*3+3]) != expected_preview:
                raise ValueError('HDR preview differs from declared mapping')
            if struct.unpack_from('<f',raw,(2*width+2)*4)[0] != 1.0:
                raise ValueError('HDR background depth differs from clear value')
        else:
            for x, y, color, depth in [(width//2, height//2, (32,220,64), 0.25), (2,2,(16+physical_frame%100,32,192),1.0)]:
                offset = y * width + x
                actual = tuple(rgb[offset*3:offset*3+3])
                if any(abs(a-b) > 1 for a,b in zip(actual,color)) or abs(struct.unpack_from('<f',raw,offset*4)[0] - depth) > 0.0001:
                    raise ValueError('Exported fixture pixels do not match rendered content')
    if mode == 'triggered' and sum(e['event'] == 'triggered' for e in events) != 1:
        raise ValueError('Trigger activation missing or duplicated')
    return {'status': 'passed', 'mode': mode, 'frames': expected_frames, 'snapshots': len(report['snapshots']),
            'successful_resets': 1, 'exported_color_depth_pixels': 'verified', 'device_and_swapchain_present': 'exercised',
            'l4d2_game_capture': 'unrun', 'full_host_build': 'unrun', 'hdr_values_and_depth_range': 'verified' if hdr_scene else 'unrun'}


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('directory', type=Path)
    a = p.parse_args()
    try:
        print(json.dumps(analyze(a.directory), ensure_ascii=False, indent=2))
    except (ValueError, OSError, KeyError, TypeError) as e:
        p.exit(1, f'Capture check failed: {e}\n')

"""Replay existing v15 model captures on the GPU; never modify the game."""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path, PurePosixPath
import struct
import subprocess
import sys
import time
import uuid
import zipfile
import zlib

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts'))
MAX_INPUT = 32 * 1024 * 1024
MAX_RESULT = 28 * 1024 * 1024
MATERIALS = ('rough', 'metal', 'cutout', 'alpha', 'additive', 'emissive', 'glass')
ROUTES = ('raster', 'ephemeral', 'retire-present', 'persistent', 'api', 'api-safe-uv')


def sha(data):
    return hashlib.sha256(data).hexdigest()


class Capture:
    """Read only selected bounded members. Do not extract an untrusted archive."""
    def __init__(self, path):
        self.path = Path(path).resolve()
        self.archive = None
        if self.path.is_dir():
            candidates = list(self.path.glob('world-runtime.jsonl')) + list(self.path.glob('*/world-runtime.jsonl'))
            if len(candidates) != 1:
                raise ValueError('Select the result folder containing world-runtime.jsonl')
            self.folder = candidates[0].parent
        else:
            self.archive = zipfile.ZipFile(self.path)
            names = self.archive.namelist()
            if len(names) > 20000 or len(names) != len(set(names)):
                raise ValueError('Duplicate members or too many archive entries')
            candidates = [n for n in names if PurePosixPath(n).name == 'world-runtime.jsonl']
            if len(candidates) != 1:
                raise ValueError('ZIP must contain exactly one world-runtime.jsonl')
            self.prefix = candidates[0][:-len('world-runtime.jsonl')]

    def close(self):
        if self.archive:
            self.archive.close()

    def read(self, name, limit):
        if not name or '/' in name or '\\' in name or ':' in name or name in ('.', '..'):
            raise ValueError('Unsafe capture filename')
        if self.archive:
            info = self.archive.getinfo(self.prefix + name)
            if info.file_size > limit or info.is_dir():
                raise ValueError('Capture member exceeds budget')
            with self.archive.open(info) as source:
                data = source.read(limit + 1)
        else:
            path = self.folder / name
            if path.is_symlink() or path.resolve().parent != self.folder.resolve() or path.stat().st_size > limit:
                raise ValueError('Capture member exceeds budget or redirects outside folder')
            data = path.read_bytes()
        if len(data) > limit:
            raise ValueError('Capture member exceeds budget')
        return data


def matrix(values):
    if len(values) != 16 or any(not math.isfinite(x) or abs(x) > 1e7 for x in values):
        raise ValueError('Invalid matrix')
    return struct.pack('<16f', *values)


def scene(capture, events, frame):
    selected = [e for e in events if e.get('event') == 'model_rebuild_sample' and e.get('frame') == frame]
    if not 0 < len(selected) <= 64:
        raise ValueError('No complete model Draw set at frame ' + str(frame))
    # The v15 collection reserves payloads for every converted Draw in these
    # two readback frames. Refuse partial sets, duplicate ordinals and cameras.
    if len({e['ordinal'] for e in selected}) != len(selected):
        raise ValueError('Duplicate Draw ordinal')
    view, projection = matrix(selected[0]['view']), matrix(selected[0]['projection'])
    result = bytearray(b'LDBRPLY1' + struct.pack('<4I', 1, 640, 400, len(selected)) + view + projection)
    summary = []
    for e in selected:
        if (not e.get('upload_verified') or not e.get('binding_verified') or not e.get('owned_draw')
                or e.get('payload_omitted') or e.get('rebuilt_stride') != 32
                or matrix(e['view']) != view or matrix(e['projection']) != projection):
            raise ValueError('Incomplete/unverified Draw or inconsistent camera')
        ordinal = e['ordinal']
        names = {f['file']: f for f in e['files']}
        def payload(suffix, expected):
            name = f'model-{ordinal}-{suffix}.bin'
            entry = names[name]
            data = capture.read(name, 4 * 1024 * 1024)
            if len(data) != entry['size'] or sha(data) != entry['sha256'] or sha(data) != expected:
                raise ValueError('Payload hash/size mismatch: ' + name)
            return data
        vb = payload('vertices', e['vertices_sha256'])
        ib = payload('indices', e['indices_sha256']) if e['kind'] == 2 else b''
        n = len(vb) // 32
        if not 0 < n <= 32768 or len(vb) != n * 32:
            raise ValueError('Invalid vertex range')
        for values in struct.iter_unpack('<8f', vb):
            if any(not math.isfinite(x) or abs(x) > 1e7 for x in values[:5]):
                raise ValueError('Invalid vertex data')
        if e['kind'] == 2:
            if e['index_bytes'] not in (2, 4) or len(ib) % e['index_bytes']:
                raise ValueError('Invalid index format')
            indices = [x[0] for x in struct.iter_unpack('<H' if e['index_bytes'] == 2 else '<I', ib)]
            if e['draw_args'][:4] != [0, 0, n, 0]:
                raise ValueError('Draw must use compact normalized indices')
            primitives = e['draw_args'][4]
        elif e['kind'] == 1:
            primitives = e['draw_args'][1]
            indices = list(range(n))
            if e['draw_args'][0] != 0:
                raise ValueError('Draw must start at zero')
        else:
            raise ValueError('Unsupported Draw kind')
        if e['topology'] == 4:
            if len(indices) != primitives * 3:
                raise ValueError('Triangle list length mismatch')
        elif e['topology'] == 5:
            if len(indices) != primitives + 2:
                raise ValueError('Triangle strip length mismatch')
            indices = [v for i in range(primitives) for v in
                       ((indices[i+1], indices[i], indices[i+2]) if i % 2 else (indices[i], indices[i+1], indices[i+2]))]
        else:
            raise ValueError('Unsupported topology')
        if not indices or len(indices) > 393216 or max(indices) >= n:
            raise ValueError('Index outside vertices/budget')
        world = e['applied_world']
        if any(world[i] != 0 for i in (3, 7, 11)) or world[15] != 1:
            raise ValueError('Non-affine world')
        result += struct.pack('<QII', ordinal, n, len(indices)) + matrix(world) + vb + struct.pack('<' + str(len(indices)) + 'I', *indices)
        if len(result) > MAX_INPUT:
            raise ValueError('Replay scene exceeds budget')
        summary.append({'ordinal': ordinal, 'vertices': n, 'triangles': len(indices)//3,
                        'vertices_sha256': sha(vb), 'indices_sha256': sha(ib)})
    return bytes(result), {'frame': frame, 'parts': summary, 'sha256': sha(result), 'bytes': len(result)}


def checker_dds():
    # A8R8G8B8 uncompressed DDS, opaque/transparent checker with nonzero RGB.
    header = [124, 0x100f, 8, 8, 32, 0, 0] + [0]*11
    header += [32, 0x41, 0, 32, 0x00ff0000, 0x0000ff00, 0x000000ff, 0xff000000]
    header += [0x1000, 0, 0, 0, 0]
    return b'DDS ' + struct.pack('<31I', *header) + b''.join(
        struct.pack('<I', (0xff000000 if (x//2+y//2)%2 else 0) | 0xbdbdbd)
        for y in range(8) for x in range(8))


def image_metrics(path):
    data = path.read_bytes()
    prefix = b'P6\n640 400\n255\n'
    if not data.startswith(prefix) or len(data) != len(prefix) + 640*400*3:
        raise ValueError('Incomplete final-color readback')
    pixels = data[len(prefix):]
    from analyze_rt_world import png
    png(path.with_suffix('.png'), 640, 400, pixels)
    signal = cyan = 0
    for r, g, b in zip(pixels[0::3], pixels[1::3], pixels[2::3]):
        signal += max(r, g, b) > 16
        cyan += r < 16 and g > 240 and b > 240
    return {'sha256': sha(data), 'nonblack_pixels': signal, 'cyan_pixels': cyan}


def package_results(folder):
    # Never include the user's model input again. Bound each archive below the
    # attachment limit, including incompressible readbacks or failure dumps.
    files = sorted(p for p in folder.rglob('*') if p.is_file() and p.suffix in
                   ('.json', '.jsonl', '.log', '.png', '.ppm', '.rgba32f', '.txt', '.dmp'))
    outputs, group, used = [], [], 0
    omissions = []
    for p in files:
        raw_size = p.stat().st_size
        if raw_size > 96*1024*1024:
            omissions.append({'file': str(p.relative_to(folder)), 'size': raw_size, 'reason': 'single_file_read_budget'})
            continue
        compressor = zlib.compressobj(6, zlib.DEFLATED, -15)
        size = 0
        with p.open('rb') as source:
            for block in iter(lambda: source.read(1024*1024), b''):
                size += len(compressor.compress(block))
        size += len(compressor.flush())
        if size > MAX_RESULT - 65536:
            omissions.append({'file': str(p.relative_to(folder)), 'size': raw_size, 'reason': 'single_file_zip_budget'})
            continue
        if used + size + 4096 > MAX_RESULT - 65536 and group:
            outputs.append(group);group=[];used=0
        group.append(p);used += size+4096
    if group:
        outputs.append(group)
    paths = []
    for index, group in enumerate(outputs, 1):
        path = folder.parent / (folder.name + f'-{index:02d}.zip')
        with zipfile.ZipFile(path, 'x', zipfile.ZIP_DEFLATED) as archive:
            for p in group:
                archive.write(p, str(p.relative_to(folder)))
            archive.writestr('archive-part.json', json.dumps({'part': index, 'parts': len(outputs), 'omissions': omissions}))
        if path.stat().st_size >= 32*1024*1024:
            raise ValueError('Result part exceeds attachment budget')
        paths.append(path)
    return paths


def verify_package(root):
    from rt_preflight import check_file
    manifest = json.loads((root/'replay-manifest.json').read_text(encoding='utf-8'))
    if manifest.get('schema') != 1 or manifest.get('scope') != 'frozen_geometry_gpu_replay':
        raise ValueError('Unsupported replay package')
    for name, entry in manifest['files'].items():
        from rt_preflight import relative
        relative(name)
        if not check_file(root/name, entry):
            raise ValueError('Package file differs: ' + name)
    return manifest


def runtime(root):
    from rt_preflight import check_file, ensure_runtime
    catalog = json.loads((root/'integration/rt/world-runtime.json').read_text())
    for package in [root] + [p for p in list(root.parent.iterdir())[:128] if p.is_dir() and p != root]:
        candidate = package/'cache/runtime'
        if (candidate/'d3d9.dll').is_file() and all(check_file(candidate/name, entry) for name, entry in catalog['runtime_files'].items()):
            print('Using verified runtime: ' + str(candidate), flush=True)
            return candidate
    return ensure_runtime(root, catalog)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture', nargs='?', type=Path)
    parser.add_argument('--verify-only', action='store_true')
    parser.add_argument('--prepare-only', action='store_true')
    args = parser.parse_args()
    if args.verify_only:
        print(json.dumps(verify_package(ROOT), ensure_ascii=False));return 0
    if not args.capture:
        print('Drag the PREVIOUS v15 result ZIP or extracted result folder here, then press Enter.', flush=True)
        args.capture = Path(input().strip().strip('"'))
    if os.name != 'nt' and not args.prepare_only:
        raise OSError('GPU replay requires Windows and a Vulkan RT GPU')
    manifest = verify_package(ROOT) if not args.prepare_only else None
    output = ROOT/'results'/('rt-replay-'+time.strftime('%Y%m%d-%H%M%S')+'-'+uuid.uuid4().hex[:8])
    output.mkdir(parents=True)
    def save(name, value):
        (output/name).write_text(json.dumps(value, indent=2, ensure_ascii=False)+'\n', encoding='utf-8')
    review = {'schema': 1, 'identity': manifest['identity'] if manifest else 'prepare-only', 'image_acceptance': 'unreviewed',
              'source_threading': 'unchanged_not_measured', 'scenes': {}, 'cases': [],
              'limits': 'neutral frozen pose; API flat normals and triangle expansion differ from legacy; safe-UV repairs only degenerate UVs; material variants are API diagnostics, not full game material conversion'}
    capture = None
    try:
        capture = Capture(args.capture)
        events = [json.loads(line) for line in capture.read('world-runtime.jsonl', 4*1024*1024).decode('utf-8-sig').splitlines() if line.strip()]
        for name, frame in [('neko', 240), ('combined', 420)]:
            data, summary = scene(capture, events, frame)
            (output/(name+'.bin')).write_bytes(data);review['scenes'][name]=summary
        capture.close();capture=None
        (output/'checker.dds').write_bytes(checker_dds())
        if args.prepare_only:
            save('review.json', review);print(output);return 0
        dependencies = runtime(ROOT)
        schedule = [(name, route, 'neutral') for name in review['scenes'] for route in ROUTES]
        schedule += [('combined', 'api-safe-uv', material) for material in MATERIALS]
        started=time.monotonic()
        for index, (name, route, material) in enumerate(schedule, 1):
            if time.monotonic()-started > 1200:
                raise TimeoutError('20 minute suite budget reached')
            case_name=f'{index:02d}-{name}-{route}-{material}'
            print(f'[{index}/{len(schedule)}] {case_name}', flush=True)
            case_directory=output/case_name
            case_directory.mkdir()
            destination=case_directory/'capture'
            environment=dict(os.environ, DXVK_LOG_PATH=str(case_directory), DXVK_STATE_CACHE_PATH=str(ROOT/'cache/replay-shaders'))
            with (output/(case_name+'.log')).open('w',encoding='utf-8') as log:
                command=[str(ROOT/'bin/rt-replay.exe'),str(dependencies/'d3d9.dll'),str(output/(name+'.bin')),str(destination),route,material,str(output/'checker.dds')]
                try:
                    process=subprocess.run(command,cwd=case_directory,env=environment,stdout=log,stderr=subprocess.STDOUT,timeout=180)
                    code=process.returncode
                except subprocess.TimeoutExpired:
                    code='timeout'
            item={'name':case_name,'scene':name,'route':route,'material':material,'exit_code':code}
            status=destination/'status.json'
            if status.is_file():
                item['status']=json.loads(status.read_text(encoding='utf-8'))
            for image in ('early.ppm','final.ppm'):
                if (destination/image).is_file():
                    item[image]=image_metrics(destination/image)
            review['cases'].append(item);save('review.json',review)
            if index==1 and code!=0 and item.get('status',{}).get('failed_at') in ('load_runtime','find_api','initialize_api','create_device'):
                raise RuntimeError('Runtime/device prerequisite failed; preserve evidence instead of repeating every case')
        review['execution']='complete' if all(c['exit_code']==0 for c in review['cases']) else 'completed_with_failures'
    except Exception as error:
        review['execution']='fail';review['error']=type(error).__name__+': '+str(error)
        print(review['error'],flush=True)
    finally:
        if capture:
            capture.close()
    save('review.json',review)
    for path in package_results(output):
        print('UPLOAD: '+str(path),flush=True)
    return 0 if review.get('execution')=='complete' else 1


if __name__=='__main__':
    raise SystemExit(main())

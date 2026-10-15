"""One Windows entry for the real custom-runtime smoke and game ABI evidence.

This is a prerequisite check, not the R0/R1/R2 game RT suite. It reads an
already-running game, and only terminates the diagnostic process it created.
"""

import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import subprocess
import sys
import time
import urllib.request
import uuid
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
from rt_game_probe import WindowsProcess, collect
from rt_suite import save, stop

ROOT = Path(__file__).resolve().parents[1]
PAYLOAD_BASE = ('https://raw.githubusercontent.com/yeyunyyds/'
                'L4D2_Dxvk_General_FrameWork/zip-host-capture/downloads/')
MAX_PART = 95 * 1024 * 1024
MAX_UNPACKED = 3 * 1024 * 1024 * 1024
MAX_RESULTS = 128 * 1024 * 1024


def digest(path):
    result = hashlib.sha256()
    with Path(path).open('rb') as source:
        for data in iter(lambda: source.read(1024 * 1024), b''):
            result.update(data)
    return result.hexdigest()


def relative(name):
    path = PurePosixPath(name)
    if (not name or '\\' in name or ':' in name or path.is_absolute() or
            any(x in ('', '.', '..') or x.endswith((' ', '.')) for x in name.split('/'))):
        raise ValueError('Unsafe package member: ' + name)
    return path


def check_file(path, entry):
    return (path.is_file() and not path.is_symlink() and
            path.stat().st_size == entry['size'] and digest(path) == entry['sha256'])


def ensure_runtime(root, manifest, payload_cache=None):
    """Hash every dependency; download valid ZIP parts only when necessary."""
    if manifest.get('schema') != 1 or manifest.get('scope') != 'rt_prerequisites':
        raise ValueError('Unsupported prerequisite package')
    runtime = root / 'cache/runtime'
    files = manifest['runtime_files']
    total = 0
    for name, entry in files.items():
        relative(name)
        if not isinstance(entry['size'], int) or entry['size'] < 0:
            raise ValueError('Invalid dependency size')
        total += entry['size']
    if total > MAX_UNPACKED or not {'d3d9.dll', 'rt-runtime-smoke.exe'} <= files.keys():
        raise ValueError('Invalid runtime file catalog')
    if all(check_file(runtime / name, entry) for name, entry in files.items()):
        return runtime
    cache = payload_cache or root / 'cache/downloads'
    cache.mkdir(parents=True, exist_ok=True)
    runtime.mkdir(parents=True, exist_ok=True)
    seen = set()
    for part in manifest['payloads']:
        name = part['name']
        if relative(name).name != name or not name.startswith('rt-prereq-payload-') or not name.endswith('.zip'):
            raise ValueError('Invalid payload name')
        if not 0 < part['size'] <= MAX_PART:
            raise ValueError('Invalid payload size')
        path = cache / name
        if not check_file(path, part):
            temporary = path.with_suffix('.download')
            print('Downloading runtime dependency bundle:', name, flush=True)
            try:
                with urllib.request.urlopen(PAYLOAD_BASE + name, timeout=30) as source, temporary.open('wb') as target:
                    written, started = 0, time.monotonic()
                    while True:
                        data = source.read(256 * 1024)
                        if not data:
                            break
                        written += len(data)
                        if written > part['size'] or time.monotonic() - started > 600:
                            raise ValueError('Payload download exceeded size/time budget')
                        target.write(data)
                if not check_file(temporary, part):
                    raise ValueError('Payload SHA256/size mismatch')
                temporary.replace(path)
            finally:
                temporary.unlink(missing_ok=True)
        with zipfile.ZipFile(path) as archive:
            for info in archive.infolist():
                relative(info.filename)
                entry = files.get(info.filename)
                if (info.filename in seen or entry is None or info.is_dir() or
                        info.file_size != entry['size'] or info.external_attr >> 16 & 0o170000 == 0o120000):
                    raise ValueError('Unexpected/duplicate payload member')
                seen.add(info.filename)
                target = runtime / info.filename
                # Resolve the full destination before writing; reject redirected parents.
                if not target.resolve().is_relative_to(runtime.resolve()):
                    raise ValueError('Payload destination leaves runtime directory')
                target.parent.mkdir(parents=True, exist_ok=True)
                temporary = target.with_name(target.name + '.unpacking')
                with archive.open(info) as source, temporary.open('wb') as output:
                    written = 0
                    while True:
                        block = source.read(1024 * 1024)
                        if not block:
                            break
                        written += len(block)
                        if written > entry['size']:
                            raise ValueError('Unpacked dependency exceeded size budget')
                        output.write(block)
                if not check_file(temporary, entry):
                    temporary.unlink(missing_ok=True)
                    raise ValueError('Unpacked dependency SHA256 mismatch')
                temporary.replace(target)
    if seen != files.keys() or not all(check_file(runtime / n, e) for n, e in files.items()):
        raise ValueError('Incomplete runtime dependencies')
    return runtime


def run_owned(argv, directory, timeout=180, byte_limit=64 * 1024 * 1024):
    """Bound the owned diagnostic; never terminate the game by process name."""
    directory.mkdir(parents=True, exist_ok=False)
    entry = {'execution': 'fail', 'argv': list(map(str, argv))}
    child = None
    started = time.monotonic()
    try:
        with (directory / 'stdout.log').open('wb') as stdout, (directory / 'stderr.log').open('wb') as stderr:
            options = {'start_new_session': True} if os.name != 'nt' else {'creationflags': subprocess.CREATE_NEW_PROCESS_GROUP}
            environment = os.environ.copy()
            environment.update(LDB_FG_ENABLE='0', LDB_FG_HOTKEYS='0', DXVK_LOG_PATH=str(directory))
            # A private working directory isolates config, shader caches and logs.
            child = subprocess.Popen(list(map(str, argv)), cwd=directory, env=environment,
                                     stdout=stdout, stderr=stderr, **options)
            entry['pid'] = child.pid
            while child.poll() is None:
                size = sum(p.stat().st_size for p in directory.rglob('*') if p.is_file() and not p.is_symlink())
                if time.monotonic() - started > timeout or size > byte_limit:
                    entry['reason'] = 'timeout' if time.monotonic() - started > timeout else 'output_budget'
                    stop(child)
                    break
                time.sleep(0.1)
            entry['exit_code'] = child.wait()
            entry['execution'] = 'pass' if entry['exit_code'] == 0 and 'reason' not in entry else 'fail'
            child = None
    finally:
        if child is not None:
            stop(child)
        entry['duration_seconds'] = time.monotonic() - started
        save(directory / 'process.json', entry)
    return entry


def game_pids():
    """Enumerate processes without reading unrelated command lines."""
    from ctypes import wintypes as w
    class Entry(ctypes.Structure):
        _fields_ = [('dwSize', w.DWORD), ('cntUsage', w.DWORD), ('pid', w.DWORD),
                    ('heap', ctypes.c_size_t), ('module', w.DWORD), ('threads', w.DWORD),
                    ('parent', w.DWORD), ('priority', w.LONG), ('flags', w.DWORD),
                    ('exe', w.WCHAR * 260)]
    api = ctypes.WinDLL('kernel32', use_last_error=True)
    api.CreateToolhelp32Snapshot.argtypes = [w.DWORD, w.DWORD]
    api.CreateToolhelp32Snapshot.restype = w.HANDLE
    api.CloseHandle.argtypes = [w.HANDLE]
    for name in ('Process32FirstW', 'Process32NextW'):
        getattr(api, name).argtypes = [w.HANDLE, ctypes.POINTER(Entry)]
        getattr(api, name).restype = w.BOOL
    handle = api.CreateToolhelp32Snapshot(2, 0)
    if handle == ctypes.c_void_p(-1).value:
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        value = Entry(); value.dwSize = ctypes.sizeof(value)
        ok = api.Process32FirstW(handle, ctypes.byref(value))
        result = []
        while ok:
            if value.exe.casefold() == 'left4dead2.exe':
                result.append(value.pid)
            ok = api.Process32NextW(handle, ctypes.byref(value))
        if ctypes.get_last_error() != 18:
            raise ctypes.WinError(ctypes.get_last_error())
        return result
    finally:
        api.CloseHandle(handle)


def wait_game(timeout=180):
    started = time.monotonic()
    required = {'client.dll', 'engine.dll', 'materialsystem.dll', 'shaderapidx9.dll'}
    while time.monotonic() - started < timeout:
        pids = game_pids()
        if len(pids) > 1:
            raise OSError('More than one left4dead2.exe is running; target is ambiguous')
        if pids:
            with WindowsProcess(pids[0]) as process:
                if required <= {m['name'] for m in process.modules()}:
                    return pids[0]
        time.sleep(0.5)
    raise TimeoutError('Game modules not ready within 180 seconds; start L4D2 and load a map')


def verify_load(runtime):
    if os.name != 'nt':
        raise OSError('Runtime loader verification requires Windows x64')
    library = ctypes.WinDLL(str(runtime / 'd3d9.dll'), winmode=0x1100)
    getattr(library, 'remixapi_InitializeLibrary')
    return {'dependency_load': 'pass', 'api_export': 'present', 'gpu_execution': 'unrun'}


def pack_results(directory, report, limit=MAX_RESULTS):
    archive = directory.with_suffix('.zip')
    skipped = []
    remaining = limit
    members = []
    for path in sorted(directory.rglob('*')):
        if not path.is_file() or path.is_symlink() or path.name == 'summary.json':
            continue
        size = path.stat().st_size
        if size > remaining:
            skipped.append({'path': path.relative_to(directory).as_posix(), 'size': size})
        else:
            members.append(path); remaining -= size
    report['omitted_files'] = skipped
    if skipped:
        report['collection'] = 'incomplete'
    save(directory / 'summary.json', report)
    with zipfile.ZipFile(archive, 'x', zipfile.ZIP_DEFLATED) as output:
        output.write(directory / 'summary.json', 'summary.json')
        for path in members:
            output.write(path, path.relative_to(directory).as_posix())
    return archive


def run(root=ROOT, pid=None, payload_cache=None, verify_only=False):
    directory = root / 'results' / ('rt-preflight-' + time.strftime('%Y%m%d-%H%M%S') + '-' + uuid.uuid4().hex[:8])
    directory.mkdir(parents=True)
    report = {'schema': 1, 'scope': 'rt_prerequisites', 'collection': 'complete',
              'runtime_preparation': 'unrun', 'runtime_smoke': 'unrun', 'game_binding': 'unrun',
              'game_rt': 'unrun', 'source_threading': 'unrun', 'performance': 'unrun',
              'b580_rt': 'unrun', 'verify_only': verify_only, 'errors': []}
    try:
        manifest = json.loads((root / 'preflight-manifest.json').read_text(encoding='utf-8'))
        report['build_commit'] = manifest['build_commit']
        save(directory / 'package-manifest.json', manifest)
        try:
            runtime = ensure_runtime(root, manifest, payload_cache)
            report['runtime_preparation'] = 'pass'
            if verify_only:
                report['runtime_load'] = verify_load(runtime)
            else:
                print('Running static RT diagnostic (up to 180 seconds)...', flush=True)
                stage = directory / 'runtime'
                entry = run_owned([runtime / 'rt-runtime-smoke.exe', runtime / 'd3d9.dll', stage / 'smoke'], stage)
                report['runtime_process'] = entry
                status = stage / 'smoke/status.json'
                observed = json.loads(status.read_text()) if status.is_file() else {}
                passed = (entry['execution'] == 'pass' and observed.get('result') == 'static_hit_and_color_readback'
                          and (stage / 'smoke/final-color.ppm').is_file())
                report['runtime_smoke'] = 'static_hit_and_color_readback' if passed else 'fail'
                # Verify the vendor observed by the actual runtime, not Windows' GPU list.
                events = stage / 'smoke/events.jsonl'
                adapters = [json.loads(line) for line in events.read_text().splitlines()
                            if '"vendor_id"' in line] if events.is_file() else []
                report['runtime_adapters'] = adapters
                # Intel vendor alone does not identify a B580. Preserve device ID for review.
                report['b580_rt'] = 'pending_adapter_and_image_review' if passed else 'fail_or_unrun'
        except Exception as error:
            report['errors'].append({'stage': 'runtime', 'error': str(error)})
            report['runtime_preparation'] = 'fail' if report['runtime_preparation'] != 'pass' else 'pass'
        if not verify_only:
            try:
                print('Reading loaded L4D2 modules; load a map if the game is not ready...', flush=True)
                target = pid or wait_game()
                audit = json.loads((root / 'research/rt-source-audit.json').read_text())
                probe = collect(target, audit)
                save(directory / 'game-binding.json', probe)
                report['game_binding'] = probe['execution']
            except Exception as error:
                report['errors'].append({'stage': 'game_binding', 'error': str(error)})
                report['game_binding'] = 'fail'
    except (Exception, KeyboardInterrupt) as error:
        report['errors'].append({'stage': 'session', 'error': str(error) or type(error).__name__})
        report['collection'] = 'incomplete'
    finally:
        archive = pack_results(directory, report)
    return report, archive


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pid', type=int)
    parser.add_argument('--verify-package', action='store_true', help='CI: verify dependencies/export, do not run GPU/game stages')
    parser.add_argument('--payload-cache', type=Path)
    args = parser.parse_args()
    result, archive = run(pid=args.pid, payload_cache=args.payload_cache, verify_only=args.verify_package)
    print('Result ZIP:', archive, flush=True)
    print('Runtime:', result['runtime_smoke'], '| Game evidence:', result['game_binding'], flush=True)
    raise SystemExit(1 if result['errors'] or result['runtime_smoke'] == 'fail' or result['game_binding'] == 'fail' else 0)

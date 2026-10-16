"""One Windows entry for the real custom-runtime smoke and game ABI evidence.

This is a prerequisite check, not the R0/R1/R2 game RT suite. It reads an
already-running game, and only terminates the diagnostic process it created.
"""

import argparse
import ctypes
import hashlib
import http.client
import json
import os
from pathlib import Path, PurePosixPath
import shutil
import subprocess
import sys
import time
import urllib.request
import uuid
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
from rt_game_probe import WindowsProcess, collect
from rt_suite import save, stop
from rt_visual import evaluate_matrix

ROOT = Path(__file__).resolve().parents[1]
MAX_PART = 95 * 1024 * 1024
MAX_UNPACKED = 3 * 1024 * 1024 * 1024
MAX_RESULTS = 128 * 1024 * 1024


def message(value):
    print(value, flush=True)


def download_payload(path, part, notify=message):
    ref = part.get('ref', 'zip-host-capture')
    if ref != 'zip-host-capture' and (len(ref) != 40 or any(c not in '0123456789abcdef' for c in ref)):
        raise ValueError('Invalid dependency source revision')
    location = 'yeyunyyds/L4D2_Dxvk_General_FrameWork/'
    urls = [f'https://raw.githubusercontent.com/{location}{ref}/downloads/{part["name"]}',
            f'https://github.com/{location}raw/{ref}/downloads/{part["name"]}']
    temporary = path.with_suffix('.download')
    errors = []
    try:
        for attempt, url in enumerate(urls, 1):
            notify(f'Downloading {part["name"]} ({part["size"] / 1048576:.1f} MiB), source {attempt}/{len(urls)}: {url}')
            try:
                request = urllib.request.Request(url, headers={'User-Agent': 'LDB-RT-Preflight/2'})
                with urllib.request.urlopen(request, timeout=30) as source, temporary.open('wb') as target:
                    written, started = 0, time.monotonic()
                    updated = started
                    while True:
                        data = source.read(256 * 1024)
                        if not data:
                            break
                        written += len(data)
                        now = time.monotonic()
                        if written > part['size'] or now - started > 600:
                            raise ValueError('Payload download exceeded size/time budget')
                        target.write(data)
                        if now - updated >= 2 or written == part['size']:
                            notify(f'  {written / 1048576:.1f}/{part["size"] / 1048576:.1f} MiB ({100 * written / part["size"]:.0f}%)')
                            updated = now
                if not check_file(temporary, part):
                    raise ValueError('Payload SHA256/size mismatch')
                temporary.replace(path)
                notify('  Download verified (SHA256 and size).')
                return
            except (OSError, ValueError, http.client.HTTPException) as error:
                detail = f'{type(error).__name__}: {error}; URL={url}'
                errors.append(detail)
                notify('  Download failed: ' + detail)
        raise ValueError('All dependency sources failed: ' + ' | '.join(errors))
    finally:
        temporary.unlink(missing_ok=True)


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


def ensure_runtime(root, manifest, payload_cache=None, notify=message):
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
        notify('Runtime files already verified; no download needed.')
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
            bundled = next((p for p in (root / 'payloads' / name, root / name) if check_file(p, part)), None)
            if bundled is not None:
                shutil.copyfile(bundled, path)
                notify('Using bundled dependency: ' + name)
            else:
                download_payload(path, part, notify)
        else:
            notify('Using verified cached dependency: ' + name)
        notify('Extracting and verifying: ' + name)
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
                if check_file(target, entry):
                    continue
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


def run_owned(argv, directory, timeout=360, byte_limit=64 * 1024 * 1024, notify=message):
    """Bound the owned diagnostic; never terminate the game by process name."""
    directory.mkdir(parents=True, exist_ok=False)
    entry = {'execution': 'fail', 'argv': list(map(str, argv))}
    child = None
    started = time.monotonic()
    updated = started
    try:
        with (directory / 'stdout.log').open('wb') as stdout, (directory / 'stderr.log').open('wb') as stderr:
            options = {'start_new_session': True} if os.name != 'nt' else {'creationflags': subprocess.CREATE_NEW_PROCESS_GROUP}
            environment = os.environ.copy()
            environment.update(LDB_FG_ENABLE='0', LDB_FG_HOTKEYS='0', DXVK_LOG_PATH=str(directory),
                               DXVK_METRICS_PATH=str(directory), RTX_TEST_TRACE='1',
                               DXVK_DUMP_SCREENSHOT_FRAME='90',
                               RTX_AUTOMATION_DISABLE_BLOCKING_DIALOG_BOXES='1')
            # A private working directory isolates config, shader caches and logs.
            child = subprocess.Popen(list(map(str, argv)), cwd=directory, env=environment,
                                     stdout=stdout, stderr=stderr, **options)
            entry['pid'] = child.pid
            while child.poll() is None:
                now = time.monotonic()
                if now - updated >= 15:
                    progress = directory / 'smoke/progress.json'
                    try:
                        state = json.loads(progress.read_text()) if progress.is_file() else {}
                    except (OSError, ValueError):
                        state = {}
                    notify(f'RT diagnostic: {now-started:.0f}s; frames={state.get("frames", "initializing")}; '
                           f'case={state.get("case", "initializing")}; pick callbacks={state.get("pick_callback_count", 0)}; '
                           f'readbacks={state.get("readback_successes", 0)}')
                    updated = now
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


def game_pids(diagnostics=None):
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
        related = []
        while ok:
            name = value.exe.casefold()
            if 'left4dead' in name or 'l4d2' in name or name == 'hl2.exe':
                related.append({'pid': value.pid, 'exe': value.exe})
            if name == 'left4dead2.exe':
                result.append(value.pid)
            ok = api.Process32NextW(handle, ctypes.byref(value))
        if ctypes.get_last_error() != 18:
            raise ctypes.WinError(ctypes.get_last_error())
        if diagnostics is not None:
            diagnostics['related_processes'] = related
            diagnostics['game_pids'] = result
        return result
    finally:
        api.CloseHandle(handle)


def wait_game(timeout=180, diagnostics=None, notify=message):
    started = time.monotonic()
    diagnostics = diagnostics if diagnostics is not None else {}
    core = {'client.dll', 'engine.dll'}
    previous = None
    pids = []
    while time.monotonic() - started < timeout:
        pids = game_pids(diagnostics)
        diagnostics['elapsed_seconds'] = round(time.monotonic() - started, 2)
        if len(pids) > 1:
            raise OSError('More than one left4dead2.exe is running; target is ambiguous')
        if pids:
            with WindowsProcess(pids[0]) as process:
                modules = process.modules()
                diagnostics['modules'] = modules
                names = {m['name'] for m in modules}
                diagnostics['missing_core_modules'] = sorted(core - names)
                state = f'Found left4dead2.exe PID {pids[0]}; modules={len(modules)}; missing core={sorted(core - names)}'
                if state != previous:
                    notify(state); previous = state
                if core <= names:
                    notify('Reading game evidence. Missing renderer/material modules will be recorded, not treated as an unloaded map.')
                    return pids[0]
        elif previous != 'absent':
            notify('No left4dead2.exe found; waiting up to ' + str(timeout) + ' seconds. Related processes: ' + str(diagnostics.get('related_processes', [])))
            previous = 'absent'
        time.sleep(0.5)
    if pids:
        notify('Core modules remain incomplete; collecting available evidence from PID ' + str(pids[0]))
        return pids[0]
    raise TimeoutError(f'No left4dead2.exe detected within {timeout} seconds; see game-discovery.json for related process names')


def verify_load(runtime):
    if os.name != 'nt':
        raise OSError('Runtime loader verification requires Windows x64')
    # Runtime dependencies can retain loaded modules after the top-level DLL
    # handle is released. Keep this load-only check in its own process so no
    # runtime DLL (or worker) survives into collection or package cleanup.
    code = '''import ctypes, sys
library = ctypes.WinDLL(sys.argv[1], winmode=0x1100)
getattr(library, 'remixapi_InitializeLibrary')
'''
    process = subprocess.run([sys.executable, '-I', '-c', code, str((runtime / 'd3d9.dll').resolve())],
                             capture_output=True, text=True, timeout=30)
    if process.returncode:
        raise OSError('Runtime dependency/export check failed: ' + process.stderr[-4000:])
    return {'dependency_load': 'pass', 'api_export': 'present', 'gpu_execution': 'unrun'}


def smoke_program(root, manifest, runtime):
    """A small rebuilt diagnostic can replace the old EXE without repacking DLLs."""
    diagnostic = manifest.get('diagnostic')
    if diagnostic is None:
        return runtime / 'rt-runtime-smoke.exe'
    name = relative(diagnostic['path'])
    if name.as_posix() != 'bin/rt-runtime-smoke.exe' or not check_file(root / name, diagnostic):
        raise ValueError('Bundled diagnostic SHA256/size/path mismatch')
    return root / name


def smoke_evidence(stage, entry):
    status = stage / 'smoke/status.json'
    observed = json.loads(status.read_text()) if status.is_file() else {}
    visual = evaluate_matrix(stage / 'smoke')
    save(stage / 'visual-acceptance.json', visual)
    passed = (entry['execution'] == 'pass' and observed.get('result') == 'static_visible_triangle_readback'
              and observed.get('cleanup') == 'complete' and observed.get('pick_hit') is True
              and observed.get('stable_image_signal') is True and visual['pass']
              and not (stage / 'smoke/crash.json').exists() and not (stage / 'smoke/crash.dmp').exists())
    trace = stage / 'frame_trace.jsonl'
    traces = [json.loads(line) for line in trace.read_text().splitlines() if line.strip()] if trace.is_file() else []
    warnings = []
    for path in stage.glob('*.log'):
        for line in path.read_text(encoding='utf-8', errors='replace').splitlines():
            if 'common device objects were not disposed of' in line:
                warnings.append({'file': path.name, 'message': line})
    return passed, {'diagnostic_status': observed, 'visual_acceptance': visual, 'resource_warnings': warnings,
                    'cleanup_scope': 'owned window destruction and Shutdown returned; resource warnings reported separately',
                    'readiness_trace': traces,
                    'readiness_scope': 'bounded snapshot around runtime frame 90, not a full run timeline',
                    'status_file_present': status.is_file(), 'final_color_present': (stage / 'smoke/final-color.ppm').is_file(),
                    'crash_report_present': (stage / 'smoke/crash.json').is_file(),
                    'minidump_present': (stage / 'smoke/crash.dmp').is_file()}


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
    def notify(value):
        message(value)
        with (directory / 'progress.log').open('a', encoding='utf-8') as output:
            output.write(value + '\n')

    def failure(stage, error):
        detail = f'{type(error).__name__}: {error}'
        report['errors'].append({'stage': stage, 'error': detail})
        notify(f'FAILED [{stage}]: {detail}')

    try:
        manifest = json.loads((root / 'preflight-manifest.json').read_text(encoding='utf-8'))
        report['build_commit'] = manifest['build_commit']
        save(directory / 'package-manifest.json', manifest)
        try:
            runtime = ensure_runtime(root, manifest, payload_cache, notify=notify)
            program = smoke_program(root, manifest, runtime)
            report['runtime_preparation'] = 'pass'
            if verify_only:
                report['runtime_load'] = verify_load(runtime)
            else:
                notify('Running 13 automatic RT comparisons in one window (up to 6 minutes total)...')
                stage = directory / 'runtime'
                entry = run_owned([program, runtime / 'd3d9.dll', stage / 'smoke'], stage, notify=notify)
                report['runtime_process'] = entry
                passed, details = smoke_evidence(stage, entry)
                report['runtime_details'] = details
                report['runtime_smoke'] = 'static_visible_triangle_and_motion' if passed else 'fail'
                report['static_visual_acceptance'] = 'pass' if passed else 'fail'
                visual = details['visual_acceptance']
                report['emission_path'] = 'pass' if passed else 'fail_or_unrun'
                report['lighting_path'] = 'pass' if (visual.get('lighting_path', {}).get('pass')
                    and entry.get('exit_code') in (0, 1) and 'reason' not in entry
                    and details['diagnostic_status'].get('cleanup') == 'complete'
                    and not details['crash_report_present'] and not details['minidump_present']) else 'fail_or_unrun'
                report['resource_warning_review'] = 'required' if details['resource_warnings'] else 'none_observed'
                notify('Static RT diagnostic: ' + report['runtime_smoke'])
                # Verify the vendor observed by the actual runtime, not Windows' GPU list.
                events = stage / 'smoke/events.jsonl'
                adapters = [json.loads(line) for line in events.read_text().splitlines()
                            if '"vendor_id"' in line] if events.is_file() else []
                report['runtime_adapters'] = adapters
                # Intel vendor alone does not identify a B580. Preserve device ID for review.
                report['b580_rt'] = 'pending_adapter_and_image_review' if passed else 'fail_or_unrun'
        except Exception as error:
            failure('runtime', error)
            report['runtime_preparation'] = 'fail' if report['runtime_preparation'] != 'pass' else 'pass'
        if not verify_only:
            discovery = {'explicit_pid': pid}
            try:
                notify('Checking the running L4D2 process and its modules...')
                target = pid or wait_game(diagnostics=discovery, notify=notify)
                audit = json.loads((root / 'research/rt-source-audit.json').read_text())
                probe = collect(target, audit)
                save(directory / 'game-binding.json', probe)
                report['game_binding'] = probe['execution']
                notify('Game evidence: ' + probe['execution'])
            except Exception as error:
                failure('game_binding', error)
                report['game_binding'] = 'fail'
            finally:
                save(directory / 'game-discovery.json', discovery)
    except (Exception, KeyboardInterrupt) as error:
        failure('session', error)
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
    raise SystemExit(1 if result['errors'] or result['collection'] != 'complete' or result['runtime_smoke'] == 'fail'
                     or (not args.verify_package and result['game_binding'] != 'snapshot_collected') else 0)

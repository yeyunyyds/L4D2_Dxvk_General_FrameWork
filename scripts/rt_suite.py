"""Run a developer RT foundation suite and retain one bounded result ZIP.

Game launch/replay and installation are intentionally not enabled by this runner.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import signal
import subprocess
import time
import uuid
import zipfile


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def save(path, value):
    temporary = path.with_suffix(path.suffix + '.tmp')
    temporary.write_text(json.dumps(value, indent=2, ensure_ascii=False) + '\n', encoding='utf-8')
    temporary.replace(path)


def stop(process):
    if process.poll() is not None:
        return
    if os.name == 'nt':
        subprocess.run(['taskkill', '/PID', str(process.pid), '/T', '/F'],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=10, check=False)
    else:
        os.killpg(process.pid, signal.SIGTERM)
        try:
            process.wait(timeout=2)
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGKILL)
    process.wait(timeout=10)


def validate(manifest):
    if manifest.get('schema') != 1 or manifest.get('scope') != 'developer_foundation':
        raise ValueError('Only schema=1 developer_foundation suites are implemented')
    stages = manifest.get('stages')
    if not isinstance(stages, list) or not stages:
        raise ValueError('Suite has no stages')
    known = set()
    for stage in stages:
        name = stage.get('id', '')
        if not name or any(c not in 'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-' for c in name) or name in known:
            raise ValueError('Invalid or duplicate stage ID')
        if not set(stage.get('depends_on', [])) <= known:
            raise ValueError('Stage dependency must refer to an earlier stage')
        argv = stage.get('argv', [])
        if not argv or not all(isinstance(item, str) for item in argv):
            raise ValueError('Stage argv must be a nonempty string list')
        executable = Path(argv[0])
        if not executable.is_absolute() or not executable.is_file():
            raise ValueError('Stage executable must be an existing absolute path')
        if sha256(executable) != stage.get('executable_sha256'):
            raise ValueError('Stage executable SHA256 mismatch')
        for item in stage.get('inputs', []):
            if sha256(item['path']) != item['sha256']:
                raise ValueError('Stage input SHA256 mismatch')
        if not 0 < stage.get('timeout_seconds', 120) <= 3600:
            raise ValueError('Invalid stage timeout')
        known.add(name)


def run(manifest, output):
    output = Path(output).resolve()
    archive = output.with_name(output.name + '.zip')
    if output.exists() or archive.exists():
        raise FileExistsError('Preserve existing output and ZIP; choose a new session directory')
    output.mkdir(parents=True)
    report = {'schema': 1, 'session': uuid.uuid4().hex, 'scope': 'developer_foundation',
              'execution': 'incomplete', 'source_game_queue': 'unrun', 'b580_rt': 'unrun',
              'image_quality': 'unrun', 'performance_acceptance': 'unrun', 'stages': []}
    child = None
    try:
        validate(manifest)
        save(output / 'session-manifest.json', {
            'schema': 1, 'session': report['session'], 'scope': manifest['scope'],
            'stages': [{'id': s['id'], 'argv': s['argv'], 'executable_sha256': s['executable_sha256'],
                        'depends_on': s.get('depends_on', [])} for s in manifest['stages']]})
        completed = {}
        for stage in manifest['stages']:
            entry = {'id': stage['id'], 'execution': 'unrun'}
            report['stages'].append(entry)
            if any(completed.get(name) != 'pass' for name in stage.get('depends_on', [])):
                entry['reason'] = 'dependency_failed'; completed[stage['id']] = 'unrun'; continue
            directory = output / 'runs' / stage['id']
            directory.mkdir(parents=True)
            argv = [s.replace('${RUN_DIR}', str(directory)).replace('${SESSION_DIR}', str(output)) for s in stage['argv']]
            environment = os.environ.copy()
            environment.update(LDB_FG_ENABLE='0', LDB_FG_HOTKEYS='0', LDB_RT_LOG_DIR=str(directory))
            started = time.monotonic()
            with (directory / 'stdout.log').open('wb') as stdout, (directory / 'stderr.log').open('wb') as stderr:
                options = {'start_new_session': True} if os.name != 'nt' else {'creationflags': subprocess.CREATE_NEW_PROCESS_GROUP}
                child = subprocess.Popen(argv, cwd=directory, env=environment, stdout=stdout, stderr=stderr, **options)
                entry['pid'] = child.pid
                limit = int(manifest.get('max_log_bytes_per_stage', 64 * 1024 * 1024))
                while child.poll() is None:
                    elapsed = time.monotonic() - started
                    written = sum(p.stat().st_size for p in directory.glob('*.log'))
                    if elapsed > stage.get('timeout_seconds', 120) or written > limit:
                        entry['reason'] = 'timeout' if elapsed > stage.get('timeout_seconds', 120) else 'log_budget'
                        stop(child)
                        break
                    time.sleep(0.05)
                entry['exit_code'] = child.wait()
                entry['execution'] = 'pass' if entry['exit_code'] == 0 and 'reason' not in entry else 'fail'
                child = None
            entry['duration_seconds'] = time.monotonic() - started
            completed[stage['id']] = entry['execution']
            save(directory / 'run-manifest.json', entry)
            save(output / 'summary.json', report)
        report['execution'] = 'pass' if all(x['execution'] == 'pass' for x in report['stages']) else 'fail'
    except (Exception, KeyboardInterrupt) as error:
        report['execution'] = 'fail'
        report['error'] = str(error) or type(error).__name__
    finally:
        if child is not None:
            stop(child)
        save(output / 'summary.json', report)
        with zipfile.ZipFile(archive, 'x', zipfile.ZIP_DEFLATED) as package:
            for file in sorted(output.rglob('*')):
                if file.is_symlink():
                    continue
                if file.is_file():
                    package.write(file, str(file.relative_to(output)))
    return report, archive


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--manifest', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    result, archive = run(json.loads(a.manifest.read_text()), a.output)
    print(json.dumps({'execution': result['execution'], 'archive': str(archive)}))
    raise SystemExit(0 if result['execution'] == 'pass' else 1)

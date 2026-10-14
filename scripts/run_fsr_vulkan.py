"""Validate official FSR on software Vulkan, including two expected-failure controls."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import uuid

from prepare_research import ROOT


def run(args):
    build = args.build.resolve()
    executable = build/'fsr_vulkan_smoke'
    metadata = json.loads((build/'build.json').read_text())
    actual_hash = hashlib.sha256(executable.read_bytes()).hexdigest()
    if actual_hash != metadata['exe_sha256']:
        raise RuntimeError('Executable does not match build evidence')
    output = args.output.resolve()/uuid.uuid4().hex
    output.mkdir(parents=True)
    runtime = ROOT/'.research/software-runtime'
    env = os.environ.copy()
    env.update(VK_ICD_FILENAMES=str(runtime/'lvp.json'),
               VK_LAYER_PATH=str(runtime/'root/usr/share/vulkan/explicit_layer.d'),
               MESA_SHADER_CACHE_DIR=str(build/'cache'))
    library = str(runtime/'root/usr/lib/x86_64-linux-gnu')
    env['LD_LIBRARY_PATH'] = library + (os.pathsep+env['LD_LIBRARY_PATH'] if env.get('LD_LIBRARY_PATH') else '')
    outcomes = []
    for case, option, expected_error in [
        ('functional', None, None),
        ('negative-current', '--negative-current', 'Official generated object failed independent midpoint check'),
        ('negative-format', '--negative-format', 'FSR resource contract mismatch'),
    ]:
        command = [str(executable), str(output/case)] + ([option] if option else [])
        result = subprocess.run(command, env=env, capture_output=True, text=True, timeout=180)
        (output/(case+'.stdout')).write_text(result.stdout)
        (output/(case+'.stderr')).write_text(result.stderr)
        validation_error = 'VUID-' in result.stderr or 'SYNC-HAZARD-' in result.stderr
        if expected_error:
            passed = result.returncode == 1 and expected_error in result.stderr and not validation_error
        else:
            messages = [json.loads(line) for line in result.stdout.splitlines() if line.startswith('{')]
            passed = result.returncode == 0 and bool(messages) and messages[-1].get('status') == 'passed'
            passed = passed and messages[-1].get('source_frames') == 24 and not validation_error
        outcome = {'case':case, 'command':command, 'exit_code':result.returncode,
                   'expected_failure':expected_error is not None,
                   'expected_error':expected_error, 'passed':passed}
        outcomes.append(outcome)
        print(f'{case}: {"passed" if passed else "FAILED"}', flush=True)
    frames = [json.loads(line) for line in (output/'functional/frames.jsonl').read_text().splitlines()]
    report = {'status':'passed' if all(x['passed'] for x in outcomes) else 'failed',
              'sdk_commit':metadata['sdk_commit'], 'exe_sha256':actual_hash,
              'profile':metadata['profile'], 'host_adaptations':metadata['host_adaptations'],
              'device':'Mesa llvmpipe; software Vulkan, not Arc B580',
              'validation':'KHRONOS core + synchronization; mandatory',
              'shader_passes':len(metadata['shaders']), 'frames':frames, 'cases':outcomes,
              'b580_game_execution':'unrun', 'game_backend_switched':False,
              'frame_pacing_and_monitor_output':'unrun'}
    (output/'report.json').write_text(json.dumps(report, indent=2)+'\n')
    print(output/'report.json')
    return 0 if report['status'] == 'passed' else 1


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, default=ROOT/'.research/fsr-vulkan')
    parser.add_argument('--output', type=Path, default=ROOT/'.research/fsr-vulkan/runs')
    raise SystemExit(run(parser.parse_args()))

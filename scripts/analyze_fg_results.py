"""Summarize a whole FG session without treating Present calls as display FPS."""
import argparse
from collections import Counter, defaultdict
import json
from pathlib import Path


def distribution(values):
    values = sorted(values)
    if not values:
        return None
    def percentile(p):
        index = (len(values)-1)*p
        low = int(index)
        return values[low] + (values[min(low+1, len(values)-1)]-values[low])*(index-low)
    return dict(samples=len(values), median=percentile(.5), p90=percentile(.9), p99=percentile(.99))


def analyze(path):
    counts = Counter()
    by_mode = Counter()
    groups = defaultdict(lambda: defaultdict(list))
    summaries, errors, changes, captures, rejected = [], [], [], [], []
    previous = None
    frequency = None
    backend = 'custom'
    local_effect_policy = 'whole_frame_rejection'
    presenter = {}
    presenter_inputs = {'packets': 0, 'valid': 0, 'gpu_ready': 0,
                        'present_ticket': 0, 'rejected_events': 0}
    complete = False
    successful = 0
    extra_calls = 0
    with path.open(encoding='utf-8') as source:
        for line in source:
            event = json.loads(line)
            kind = event['event']
            counts[kind] += 1
            if kind == 'session':
                frequency = event['qpc_frequency']
                backend = event.get('backend','custom')
                local_effect_policy = event.get('local_effect_policy','whole_frame_rejection')
                presenter = {
                    'requested': event.get('presenter_requested', False),
                    'device_api': event.get('presenter_device_api', False),
                    'owner_match': event.get('presenter_owner_match', False),
                    'device_flags': event.get('presenter_device_flags', 0),
                    'feature_flags': event.get('presenter_feature_flags', 0),
                    'graphics_family': event.get('presenter_graphics_family'),
                    'graphics_queues': event.get('presenter_graphics_queues', 0),
                    'reserved_queues': event.get('presenter_reserved_queues', 0),
                    'input_api': event.get('presenter_input_api', 0),
                }
            elif kind == 'presenter_input':
                presenter_inputs['packets'] += 1
                presenter_inputs['valid'] += int(event.get('valid', False))
                presenter_inputs['gpu_ready'] += int(event.get('gpu_ready_token', False))
                presenter_inputs['present_ticket'] += int(event.get('present_ticket', False))
                presenter_inputs['motion_source'] = event.get('motion_source')
            elif kind == 'presenter_input_rejected':
                presenter_inputs['rejected_events'] += 1
            elif kind == 'summary':
                summaries.append(event)
                complete = True
            elif kind == 'fg_error':
                errors.append(event)
            elif kind == 'mode':
                changes.append(event)
            elif kind == 'capture':
                captures.append(event)
            elif kind == 'frame_rejected':
                rejected.append(event)
            elif kind == 'generated_present':
                by_mode[event['mode']] += 1
                successful += int(0 <= event['hresult'] < 0x80000000)
                extra_calls += int(event['mode'] == 'paired')
            elif kind == 'frame_metrics':
                if event['captured']:
                    previous = None
                    continue
                group = ('disabled' if not event['enabled'] else
                         event['mode'] if event['generated'] else 'enabled_fallback')
                data = groups[group]
                for key in ('host_work_ms', 'gpu_dispatch_ms', 'fence_wait_ms',
                            'gpu_submissions', 'generated_present_ms', 'logical_present_ms', 'pacing_wait_ms', 'paired_start_gap_ms'):
                    value = event.get(key, -1)
                    if value >= 0:
                        data[key].append(value)
                if (previous and previous[0] == group and frequency and
                        event['generation'] == previous[1]['generation'] and
                        event['frame'] == previous[1]['frame']+1):
                    value = (event['arrival_qpc']-previous[1]['arrival_qpc'])*1000/frequency
                    if value > 0:
                        data['source_arrival_interval_ms'].append(value)
                previous = (group, event)
    logical = summaries[-1]['logical_presents'] if summaries else None
    if summaries:
        presenter_inputs['accepted_total'] = summaries[-1].get('presenter_inputs_accepted', 0)
        presenter_inputs['rejected_total'] = summaries[-1].get('presenter_inputs_rejected', 0)
    return dict(log=path.name, log_complete=complete, event_counts=dict(counts),
                backend=backend, local_effect_policy=local_effect_policy, gpu_dispatch_scope='preflight_only' if backend.startswith('fsr') else 'generation',
                presenter_device=presenter,
                presenter_inputs=presenter_inputs,
                generated_by_mode=dict(by_mode), successful_generated_presents=successful,
                backend_calls=logical+extra_calls if logical is not None else None,
                summaries=summaries, errors=errors, mode_changes=changes, captures=captures,
                rejected_candidates=rejected, rejected_candidate_count=len(rejected),
                metrics={group: {key: distribution(values) for key, values in data.items()}
                         for group, data in groups.items()},
                interpretation=['Capture frames and adjacent arrival intervals are excluded from metrics.',
                                'Rejected candidates restore the original before Present; candidate PPMs do not prove presentation.',
                                'GPU dispatch uses device timestamps; -1 means unavailable.',
                                'For official FSR, GPU dispatch timestamps and motion counts describe the conservative preflight, not the SDK kernels.',
                                'Presenter device fields only prove x64 DXVK ownership and queue availability; they do not prove replacement-swapchain FG ran.',
                                'Presenter input packets prove color/depth/camera/HUD-less collection. gpu_ready=false or present_ticket=false means the asynchronous replacement-swapchain handoff is not active yet.',
                                'Fence waits cover FG submissions including stage copies; Host work covers the Present wrapper only.',
                                'Source arrival intervals and backend calls do not measure monitor delivery, FPS gain or added input latency.',
                                'On/off samples can contain different scenes and are not a controlled benchmark.',
                                'Missing summary means the session did not close normally or reached the bounded log limit.'])


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    logs = sorted(args.directory.rglob('fg-events.jsonl'))
    if not logs:
        parser.error('No fg-events.jsonl found')
    result = {'sessions': [analyze(path) for path in logs]}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, ensure_ascii=False, indent=2)+'\n', encoding='utf-8')
    print(args.output)

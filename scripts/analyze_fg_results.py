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


def analyze_layer(path):
    data = json.loads(path.read_text(encoding='utf-8'))
    live = [value for value in data.get('swapchains', {}).values()
            if isinstance(value, dict) and value.get('live') and
            value.get('frame_input_attempts', 0) > 0]
    swapchain = max(live, key=lambda value: value.get('frame_input_attempts', 0),
                    default={})
    attempts = swapchain.get('frame_input_attempts', 0)
    dispatches = swapchain.get('interpolation_dispatches', 0)
    ffx_presents = swapchain.get('present_count_ffx', 0)
    host_accepted = data.get('host_frame_accepted', 0)
    host_consumed = data.get('host_frame_consumed', 0)
    host_control = (host_accepted > 0 and host_consumed > 0 and
                    swapchain.get('host_camera_control_active', False))
    algorithm = attempts > 0 and dispatches > 0 and dispatches <= attempts
    ratio = ffx_presents / attempts if attempts else 0
    extra_present = (1.8 <= ratio <= 2.2 and
                     data.get('native_present_failures', 0) == 0)
    content = swapchain.get('generated_content_distinctness', 'unverified')
    host_gpu = (host_control and
                data.get('host_scene_depth_image_consumed', False) and
                swapchain.get('host_depth_resource_connected', False) and
                swapchain.get('motion_input_source') == 1)
    if host_gpu and algorithm and extra_present:
        level = 'host_depth_motion_algorithm_and_extra_present_confirmed'
    elif host_control and algorithm and extra_present:
        level = 'host_camera_algorithm_and_extra_present_confirmed'
    elif algorithm and extra_present:
        level = 'estimated_input_algorithm_and_extra_present_only'
    else:
        level = 'frame_generation_not_confirmed'
    return {
        'log': path.name,
        'host_control': {
            'api_version': data.get('host_frame_api_version', 0),
            'submitted': data.get('host_frame_submitted', 0),
            'accepted': host_accepted,
            'consumed': host_consumed,
            'rejected': data.get('host_frame_rejected', 0),
            'stale_at_present': data.get('host_frame_stale_at_present', 0),
            'active': host_control,
            'gpu_inputs_active': host_gpu,
            'scene_depth_semantic_observed': data.get('host_scene_depth_semantic_observed', False),
            'scene_depth_image_consumed': data.get('host_scene_depth_image_consumed', False),
        },
        'frame_generation': {
            'input_attempts': attempts,
            'interpolation_dispatches': dispatches,
            'native_present_successes': data.get('native_present_successes', 0),
            'native_present_failures': data.get('native_present_failures', 0),
            'ffx_present_count': ffx_presents,
            'output_presents_per_input': ratio,
            'input_rate_hz_ema': swapchain.get('input_present_rate_hz_ema'),
            'estimated_output_rate_hz': swapchain.get('estimated_output_rate_hz'),
            'motion_input_source': swapchain.get('motion_input_source'),
            'algorithm_dispatch_confirmed': algorithm,
            'extra_present_submission_confirmed': extra_present,
            'generated_content_distinctness': content,
            'physical_scanout_confirmed': False,
        },
        'acceptance_level': level,
        'interpretation': [
            'Host GPU input status requires ABI consumption, the exact Host depth resource, and engine motion input in the Layer.',
            'Interpolation dispatch plus extra successful native Presents proves algorithm execution and output submission.',
            'Generated content remains unverified until a GPU content-difference probe reports distinct midpoint pixels.',
            'Present submission counts and EMA rates do not prove physical monitor scanout or perceived FPS.',
        ],
    }


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
    host_provider = {'packets': 0, 'accepted': 0, 'gpu_inputs_ready': 0,
                     'max_motion_pixels': 0}
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
                    'topology_api': event.get('presenter_topology_api', False),
                    'queue_families': event.get('presenter_queue_families', []),
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
            elif kind == 'host_layer_frame':
                host_provider['packets'] += 1
                host_provider['accepted'] += int(event.get('accepted', False))
                host_provider['gpu_inputs_ready'] += int(event.get('gpu_inputs_ready', False))
                host_provider['max_motion_pixels'] = max(
                    host_provider['max_motion_pixels'],
                    event.get('gpu_motion_pixels', 0))
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
                host_provider=host_provider,
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
    layer_logs = sorted(args.directory.rglob('asteria-layer.json'))
    if not logs and not layer_logs:
        parser.error('No fg-events.jsonl or asteria-layer.json found')
    result = {'sessions': [analyze(path) for path in logs],
              'layers': [analyze_layer(path) for path in layer_logs]}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, ensure_ascii=False, indent=2)+'\n', encoding='utf-8')
    print(args.output)

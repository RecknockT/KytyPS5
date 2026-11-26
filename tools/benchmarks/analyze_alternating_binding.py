"""Validate six alternating same-process binding-reuse captures; never infer FPS gains from hits."""
import json
import statistics
import sys
from pathlib import Path


def analyze(root):
    root = Path(root)
    run = json.loads((root / 'run.json').read_text(encoding='utf-8-sig'))
    log = (root / 'stdout.log').read_text(encoding='utf-8-sig', errors='replace')
    replay = json.loads((root / 'replay.json').read_text(encoding='utf-8-sig'))
    manual = bool(run.get('manual_arm_file')) and not any(c['action'] in ('down', 'up') for c in replay)
    manual_ready = manual and Path(run['manual_arm_file']).is_file() and (
        'StartLevel aerial_garden' in log or 'Level has started: aerial_garden' in log)
    if 'load gate passed sky_garden_started' not in log and not manual_ready:
        raise ValueError('Sky Garden gate or manual level confirmation is missing')
    samples = []
    reference_config = None
    for index in range(6):
        enabled = index % 2 == 1
        label = f"binding_{'on' if enabled else 'off'}_{index // 2 + 1}"
        data = json.loads((root / (label + '.json')).read_text(encoding='utf-8-sig'))
        config = dict(data['configuration'])
        if config.pop('binding_reuse') != enabled:
            raise ValueError(f'{label}: effective mode differs from requested mode')
        if reference_config is None:
            reference_config = config
        if config != reference_config:
            raise ValueError(f'{label}: another configuration changed')
        meta, frame, work = data['metadata'], data['frametime_summary'], data['graphics_workload']
        if meta['capture_state'] != 'Completed' or not 29 <= meta['real_duration_seconds'] <= 32:
            raise ValueError(f'{label}: incomplete capture')
        if work['shaders_compiled'] or work['pipelines_created']:
            raise ValueError(f'{label}: compilation contaminated the capture')
        if work['draws_per_frame'] < 4000:
            raise ValueError(f'{label}: workload does not match the reference scene')
        samples.append(dict(label=label, enabled=enabled, frames=frame['total_frames'],
                            fps=frame['total_frames'] / meta['real_duration_seconds'],
                            p95_ms=frame['p95_frame_ms'], draws=work['draws_per_frame'],
                            descriptor_ms=data['pipeline_stage_timings']['DescriptorPrep']['ms_per_frame'],
                            render_state_ms=data['pipeline_stage_timings']['RenderStatePrep']['ms_per_frame'],
                            priority_wait_ms=data['gpu_waits_breakdown']['PriorityOperation']['thread_accumulated_ms_per_frame'],
                            readback_wait_ms=data['gpu_waits_breakdown']['BufferReadback']['thread_accumulated_ms_per_frame']))
    if max(s['draws'] for s in samples) / min(s['draws'] for s in samples) > 1.05:
        raise ValueError('Draw workload varies by over 5%; scene stability is not established')
    groups = {}
    for enabled in (False, True):
        group = [s for s in samples if s['enabled'] == enabled]
        groups['on' if enabled else 'off'] = {
            key: statistics.median(s[key] for s in group)
            for key in ('fps', 'p95_ms', 'draws', 'descriptor_ms', 'render_state_ms',
                        'priority_wait_ms', 'readback_wait_ms')
        }
    pairs = [100 * (samples[i + 1]['fps'] / samples[i]['fps'] - 1) for i in (0, 2, 4)]
    return dict(run=str(root), sha=run.get('sha'), binary_sha256=run.get('executableHash', run.get('sha256')),
                pid=run['pid'], samples=samples, medians=groups, paired_fps_change_percent=pairs,
                median_fps_change_percent=100 * (groups['on']['fps'] / groups['off']['fps'] - 1),
                all_pairs_improved=all(p > 0 for p in pairs),
                note='One process, alternating captures. Accumulated waits overlap; do not sum them.')


if __name__ == '__main__':
    result = analyze(sys.argv[1])
    output = json.dumps(result, indent=2) + '\n'
    (Path(sys.argv[1]) / 'binding_comparison.json').write_text(output, encoding='utf-8')
    print(output)

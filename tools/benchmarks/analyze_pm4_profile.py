"""Report PM4 opcode host timings and instrumentation cost from a manual 3+3 run."""
import json
import re
import statistics
import sys
from pathlib import Path


def rank_opcodes(captures, names):
    totals = {}
    frames = sum(j['frametime_summary']['total_frames'] for j in captures)
    if not frames:
        raise ValueError('No measured frames')
    for capture in captures:
        profile = capture['pm4_opcode_profile']
        if not profile['enabled']:
            raise ValueError('Profiling was not enabled')
        for row in profile['opcodes']:
            if not (0 <= row['host_wait_union_ns'] <= row['excluded_union_ns'] <= row['inclusive_ns']):
                raise ValueError('Invalid nested interval accounting')
            if row['exclusive_ns'] != row['inclusive_ns'] - row['excluded_union_ns']:
                raise ValueError('Exclusive interval mismatch')
            opcode = row['opcode']
            result = totals.setdefault(opcode, dict(opcode=opcode, name=names.get(opcode, 'UNKNOWN'),
                calls=0, inclusive_ns=0, exclusive_ns=0, host_wait_union_ns=0, excluded_union_ns=0))
            for key in ('calls', 'inclusive_ns', 'exclusive_ns', 'host_wait_union_ns', 'excluded_union_ns'):
                result[key] += row[key]
    rows = sorted(totals.values(), key=lambda row: row['exclusive_ns'], reverse=True)
    for row in rows:
        row['opcode_hex'] = f"0x{row['opcode']:02X}"
        row['calls_per_frame'] = row['calls'] / frames
        row['exclusive_ms_per_frame'] = row['exclusive_ns'] / 1e6 / frames
        row['inclusive_ms_per_frame'] = row['inclusive_ns'] / 1e6 / frames
        row['host_wait_ms_per_frame'] = row['host_wait_union_ns'] / 1e6 / frames
        row['exclusive_avg_ns'] = row['exclusive_ns'] / row['calls'] if row['calls'] else 0
    return rows


def analyze(root):
    root = Path(root)
    run = json.loads((root/'run.json').read_text(encoding='utf-8-sig'))
    log = (root/'stdout.log').read_text(encoding='utf-8-sig', errors='replace')
    if 'StartLevel aerial_garden' not in log and 'Level has started: aerial_garden' not in log:
        raise ValueError('Sky Garden is not confirmed')
    groups = {False: [], True: []}
    samples = []
    config = None
    for index in range(6):
        enabled = index % 2 == 1
        label = f"pm4_{'on' if enabled else 'off'}_{index//2+1}"
        j = json.loads((root/(label+'.json')).read_text(encoding='utf-8-sig'))
        cfg = dict(j['configuration'])
        if cfg.pop('profile_pm4') != enabled or j['pm4_opcode_profile']['enabled'] != enabled:
            raise ValueError(f'{label}: wrong effective mode')
        if config is None:
            config = cfg
        if config != cfg:
            raise ValueError('Other options changed')
        if j['metadata']['capture_state'] != 'Completed' or not 29 <= j['metadata']['real_duration_seconds'] <= 32:
            raise ValueError(f'{label}: incomplete capture')
        work = j['graphics_workload']
        if work['shaders_compiled'] or work['pipelines_created'] or work['draws_per_frame'] < 4000:
            raise ValueError(f'{label}: wrong workload or shader compilation')
        groups[enabled].append(j)
        samples.append(dict(label=label, fps=j['frametime_summary']['total_frames']/j['metadata']['real_duration_seconds'],
            p95_ms=j['frametime_summary']['p95_frame_ms'], draws=work['draws_per_frame'],
            discarded_scopes=j['pm4_opcode_profile']['discarded_scopes']))
    header = (Path(__file__).resolve().parents[2]/'src/graphics/guest_gpu/pm4.h').read_text()
    names = {int(value,16):name for name,value in re.findall(r'constexpr uint32_t (IT_\w+)\s*=\s*(0x[0-9A-Fa-f]+)',header)}
    ranking = rank_opcodes(groups[True], names)
    off = statistics.median(s['fps'] for s in samples[::2])
    on = statistics.median(s['fps'] for s in samples[1::2])
    stable = max(s['draws'] for s in samples)/min(s['draws'] for s in samples) <= 1.05
    return dict(run=str(root), binary_sha256=run['sha256'], config=config, samples=samples,
        draw_workload_within_five_percent=stable, median_off_fps=off, median_on_fps=on,
        instrumentation_fps_change_percent=100*(on/off-1) if stable else None,
        instrumentation_frame_time_change_ms=(1000/on-1000/off) if stable else None,
        top_five=ranking[:5], all_opcodes=ranking,
        note='Diagnostic host wall-time ranking, not FPS improvement or physical GPU time. Inclusive child times overlap; NOP groups custom subopcodes. Capture-boundary discarded scopes are excluded.')


def analyze_ranking_only(root):
    """Keep clean opcode profiles; deliberately make no FPS-cost comparison."""
    root = Path(root)
    log = (root/'stdout.log').read_text(encoding='utf-8-sig', errors='replace')
    if 'StartLevel aerial_garden' not in log and 'Level has started: aerial_garden' not in log:
        raise ValueError('Sky Garden is not confirmed')
    clean, excluded, labels = [], [], []
    config = None
    for index in range(1,4):
        label = f'pm4_on_{index}'
        data = json.loads((root/(label+'.json')).read_text(encoding='utf-8-sig'))
        if data['metadata']['capture_state'] != 'Completed' or not 29 <= data['metadata']['real_duration_seconds'] <= 32:
            raise ValueError(f'{label}: incomplete capture')
        if not data['configuration']['profile_pm4'] or not data['pm4_opcode_profile']['enabled']:
            raise ValueError(f'{label}: profiling not enabled')
        if config is None:
            config = data['configuration']
        if config != data['configuration']:
            raise ValueError('Other options changed')
        work = data['graphics_workload']
        if work['draws_per_frame'] < 4000:
            raise ValueError(f'{label}: workload does not match reference scene')
        if work['shaders_compiled'] or work['pipelines_created']:
            excluded.append(dict(label=label, shaders=work['shaders_compiled'], pipelines=work['pipelines_created']))
        else:
            clean.append(data)
            labels.append(label)
    if not clean:
        raise ValueError('No clean profiled captures')
    header = (Path(__file__).resolve().parents[2]/'src/graphics/guest_gpu/pm4.h').read_text(encoding='utf-8')
    names = {int(value,16):name for name,value in re.findall(r'constexpr uint32_t (IT_\w+)\s*=\s*(0x[0-9A-Fa-f]+)',header)}
    ranking = rank_opcodes(clean,names)
    return dict(run=str(root),ranking_only=True,clean_profiled_captures=labels,excluded_captures=excluded,
        instrumentation_fps_cost_established=False,
        note='No FPS improvement or instrumentation-cost conclusion. Exclusive host wall time still includes uninstrumented work and waits; compute preparation is not separately covered in this version.',
        top_five=ranking[:5],all_opcodes=ranking)


if __name__ == '__main__':
    ranking_only = '--ranking-only' in sys.argv[2:]
    result = analyze_ranking_only(sys.argv[1]) if ranking_only else analyze(sys.argv[1])
    text = json.dumps(result, indent=2)+'\n'
    filename = 'pm4_ranking_only.json' if ranking_only else 'pm4_comparison.json'
    (Path(sys.argv[1])/filename).write_text(text,encoding='utf-8')
    print(text)

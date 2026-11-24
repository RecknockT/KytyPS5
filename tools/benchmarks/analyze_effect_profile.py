"""Compare cumulative diagnostic snapshots over the final stationary frames.
GPU samples are measured intervals, not isolated shader costs or additive utilization.
"""
import argparse
import json
from pathlib import Path


def analyze(directory, target_frames=600):
    lines = [line for line in (directory / 'effects.jsonl').read_text().splitlines() if line]
    snapshots = []
    incomplete_tail = False
    for index, line in enumerate(lines):
        try:
            snapshots.append(json.loads(line))
        except json.JSONDecodeError:
            if index != len(lines)-1:
                raise
            incomplete_tail = True
    if len(snapshots) < 2:
        raise ValueError('Insufficient complete snapshots')
    end = snapshots[-1]
    earlier = [s for s in snapshots[:-1] if 0 < s['frame'] <= end['frame'] - target_frames]
    if not earlier:
        raise ValueError('Insufficient stationary frames for analysis')
    begin = earlier[-1]
    frames = end['frame'] - begin['frame']
    seconds = (end['host_ns'] - begin['host_ns']) / 1e9
    key = lambda r: (r['kind'], r['shader'], r['pixel'], r['targets'])
    previous = {key(r): r for r in begin['rows']}
    readback_available = all('readback_ns' in r for r in end['rows'])
    rows = []
    for r in end['rows']:
        old = previous.get(key(r), {})
        delta = {field: r.get(field, 0) - old.get(field, 0) for field in ('calls', 'cpu_ns', 'prep_ns', 'gpu_samples', 'gpu_sample_ns', 'readback_ns', 'readback_count')}
        if any(value < 0 for value in delta.values()):
            raise ValueError('Counters decreased inside the analysis interval')
        if delta['calls'] == 0:
            continue
        sections = {name: (value-old.get('sections_ns',{}).get(name,0))/frames/1e6 for name,value in r.get('sections_ns',{}).items()}
        rows.append(dict(sections_ms_per_frame=sections, kind=r['kind'], shader=r['shader'], pixel=r['pixel'], targets=r['targets'],
            calls_per_frame=delta['calls']/frames, cpu_ms_per_frame=delta['cpu_ns']/frames/1e6,
            prep_ms_per_frame=delta['prep_ns']/frames/1e6, readback_ms_per_frame=delta['readback_ns']/frames/1e6, readbacks_per_frame=delta['readback_count']/frames, gpu_samples=delta['gpu_samples'],
            gpu_sample_avg_us=delta['gpu_sample_ns']/delta['gpu_samples']/1e3 if delta['gpu_samples'] else None))
    rows.sort(key=lambda r: r['cpu_ms_per_frame'], reverse=True)
    result = dict(incomplete_tail=incomplete_tail, readback_attribution_available=readback_available, begin_frame=begin['frame'], end_frame=end['frame'], frames=frames, seconds=seconds,
        timestamp_bits=end['timestamp_bits'], completed_batches=end['completed_batches']-begin['completed_batches'],
        dropped=end['dropped']-begin['dropped'],
        submission_interval_ms_per_frame=(end['submission_interval_ns']-begin['submission_interval_ns'])/frames/1e6,
        cpu_ms_per_frame=sum(r['cpu_ms_per_frame'] for r in rows),
        draw_cpu_ms_per_frame=sum(r['cpu_ms_per_frame'] for r in rows if r['kind']==0),
        readback_ms_per_frame=sum(r['readback_ms_per_frame'] for r in rows) if readback_available else None,
        compute_cpu_ms_per_frame=sum(r['cpu_ms_per_frame'] for r in rows if r['kind']!=0), rows=rows)
    if not readback_available:
        for row in rows:
            row['readback_ms_per_frame'] = None
            row['readbacks_per_frame'] = None
    (directory / 'effects_summary.json').write_text(json.dumps(result, indent=2)+'\n')
    print(json.dumps({k:v for k,v in result.items() if k!='rows'},indent=2))
    print('Top CPU shader groups (kind 0 draw, 1/2 compute):')
    for r in rows[:12]:
        readback = f"{r['readback_ms_per_frame']:.3f}" if readback_available else 'n/a'
        gpu = f"{r['gpu_sample_avg_us']:.3f}" if r['gpu_sample_avg_us'] is not None else 'n/a'
        print(f"{r['kind']} {r['shader']}/{r['pixel']} targets={r['targets']:x}: {r['calls_per_frame']:.1f} calls/frame, CPU={r['cpu_ms_per_frame']:.3f} ms/frame, readback={readback} ms/frame, GPU sample={gpu} us (n={r['gpu_samples']})")
    return result

if __name__ == '__main__':
    parser=argparse.ArgumentParser()
    parser.add_argument('directory',type=Path)
    parser.add_argument('--frames',type=int,default=600)
    args=parser.parse_args()
    analyze(args.directory,args.frames)

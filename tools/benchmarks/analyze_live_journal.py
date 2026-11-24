"""Read completed JSONL records while a live play session is still running."""
import argparse
import json
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument('directory', type=Path)
parser.add_argument('--seconds', type=float, default=20)
args = parser.parse_args()
if args.seconds <= 0:
    parser.error('--seconds must be positive')
records = []
ignored_tail = False
raw = (args.directory / 'audit.json.live.jsonl').read_bytes()
for index, line in enumerate(raw.splitlines(keepends=True)):
    if not line.endswith(b'\n'):
        ignored_tail = True
        continue
    try:
        row = json.loads(line)
    except (ValueError, UnicodeDecodeError) as error:
        raise ValueError(f'Invalid completed record {index + 1}') from error
    if row.get('type') in ('sample', 'final'):
        records.append(row)
if not records:
    print(json.dumps({'samples': 0, 'ignored_incomplete_tail': ignored_tail}))
    raise SystemExit(0)
last = records[-1]
window = [r for r in records if r['elapsed_seconds'] > last['elapsed_seconds'] - args.seconds]
seconds = sum(r['interval_seconds'] for r in window)
# Completed flips remain observable when F2 disables the overlay's interval sampler.
frames = round(sum(r['completed_flip_hz'] * r['interval_seconds'] for r in window))
overlay_intervals = sum(r['frame_intervals'] for r in window)
def summed_object(key):
    names = {name for row in window for name in row[key]}
    return {name: sum(row[key].get(name) or 0 for row in window) for name in sorted(names)}
work = summed_object('work')
waits = summed_object('thread_accumulated_wait_ms')
stages = summed_object('host_stage_ms')
print(json.dumps({
    'samples': len(window), 'elapsed_seconds': last['elapsed_seconds'],
    'window_seconds': seconds, 'frames': frames, 'fps': frames / seconds,
    'overlay_frame_intervals': overlay_intervals,
    'completed_flip_hz': sum(r['completed_flip_hz'] * r['interval_seconds'] for r in window) / seconds,
    'seconds_without_frame_interval_sample': last['seconds_without_frame'] if last['debug_counters_enabled'] else None,
    'overlay_interval_metrics_available': all(r['debug_counters_enabled'] for r in window),
    'worst_frame_ms': max(r['worst_frame_ms'] for r in window),
    'hitches_100ms': sum(r['hitches_100ms'] for r in window),
    'audio_media_seconds_per_wall_second': sum(r['audio_media_seconds'] for r in window) / seconds,
    'work': work,
    'draws_per_frame': work.get('draws', 0) / frames if frames else None,
    'thread_accumulated_wait_ms_per_frame': {k: v / frames for k, v in waits.items()} if frames else None,
    'host_stage_ms_per_frame_inclusive': {k: v / frames for k, v in stages.items()} if frames else None,
    'simulation_speed_measured': False, 'ignored_incomplete_tail': ignored_tail,
}, indent=2))

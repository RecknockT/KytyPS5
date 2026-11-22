import json
import glob

files = [
    'stage8_1_wide_optimized.json', 'stage8_1_wide_baseline.json',
    'stage8_1_landing_optimized.json', 'stage8_1_landing_baseline.json',
    'stage8_1_landing_shaders.json', 'stage8_1_landing_memory.json'
]

for fn in files:
    try:
        with open(fn, 'r') as fp:
            d = json.load(fp)
        print(f"=== {fn} ===")
        print(f"Frames: {d.get('total_frames')}, Duration: {d.get('capture_real_duration_ms')} ms, FPS: {d.get('effective_fps')}, Avg FT: {d.get('avg_frame_ms')} ms, Max FT: {d.get('max_frame_ms')} ms, P95: {d.get('p95_frame_ms')} ms")
        print(f"Draws/F: {d.get('draws_per_frame')}, Disp/F: {d.get('dispatches_per_frame')}")
        print(f"CPU ms: {d.get('gpu_thread_cpu_time_ms')}, Wall ms: {d.get('gpu_thread_wall_clock_ms')}, CPU %: {d.get('gpu_thread_cpu_utilization_pct')}")
        print(f"PM4 incl: {d.get('pm4_dispatch_inclusive_ms')}, PM4 excl: {d.get('pm4_dispatch_exclusive_ms')}")
        waits = d.get('gpu_wait_reasons', {})
        for k, v in waits.items():
            if isinstance(v, dict) and v.get('count', 0) > 0:
                print(f"  Wait {k}: count={v.get('count')}, total_ms={v.get('total_ms')}, max_ms={v.get('max_ms')}")
        stages = d.get('pipeline_stages', {})
        print("  Pipeline stages:")
        for sname, sdata in stages.items():
            if isinstance(sdata, dict) and sdata.get('total_ms', 0) > 0:
                print(f"    {sname}: total={sdata.get('total_ms')} ms ({sdata.get('percent')}%), per_frame={sdata.get('per_frame_ms')} ms")
    except Exception as e:
        print(f"Error {fn}: {e}")

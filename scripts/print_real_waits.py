import json
for k, fn in [('Wide Opt', 'stage8_1_wide_optimized.json'), ('Wide Base', 'stage8_1_wide_baseline.json'), ('Landing Opt', 'stage8_1_landing_optimized.json'), ('Landing Base', 'stage8_1_landing_baseline.json'), ('Landing Shaders', 'stage8_1_landing_shaders.json'), ('Landing Memory', 'stage8_1_landing_memory.json')]:
    with open(fn) as f:
        d = json.load(f)
    print(f"=== {k} ({fn}) ===")
    for reason, data in d['gpu_timeline_waits'].items():
        if data['count'] > 0:
            print(f"  {reason:<20}: count={data['count']:<6} total={data.get('thread_accumulated_ms', 0):>10.2f} ms ({data.get('thread_accumulated_ms_per_frame', 0):>6.2f} ms/f) avg={data.get('avg_latency_us', 0):>8.1f} us max={data.get('max_ms', 0):>8.2f} ms")

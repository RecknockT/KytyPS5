import json

files = {
    'Wide Opt': 'stage8_1_wide_optimized.json',
    'Wide Base': 'stage8_1_wide_baseline.json',
    'Landing Opt': 'stage8_1_landing_optimized.json',
    'Landing Base': 'stage8_1_landing_baseline.json',
    'Landing Shaders': 'stage8_1_landing_shaders.json',
    'Landing Memory': 'stage8_1_landing_memory.json'
}

data = {}
for k, v in files.items():
    with open(v) as fp:
        data[k] = json.load(fp)

print(f"{'Metric':<32} | " + " | ".join(f"{k:>12}" for k in files.keys()))
print("-" * 120)

metrics = [
    ("Effective FPS", lambda d: f"{d['frametime_summary']['effective_fps']:.2f}"),
    ("Avg Frametime (ms)", lambda d: f"{d['frametime_summary']['avg_frame_ms']:.2f}"),
    ("P95 Frametime (ms)", lambda d: f"{d['frametime_summary']['p95_frame_ms']:.2f}"),
    ("Max Frametime (ms)", lambda d: f"{d['frametime_summary']['max_frame_ms']:.2f}"),
    ("Total Frames", lambda d: f"{d['frametime_summary']['total_frames']}"),
    ("Duration (s)", lambda d: f"{d['metadata']['real_duration_seconds']:.2f}"),
    ("Draws / Frame", lambda d: f"{d['graphics_workload']['draws_per_frame']:.1f}"),
    ("Dispatches / Frame", lambda d: f"{d['graphics_workload']['dispatches_per_frame']:.1f}"),
    ("Submits / Frame", lambda d: f"{d['vulkan_submits']['submits_per_frame']:.1f}"),
    ("GPU Thread CPU %", lambda d: f"{d['cpu_gpu_separation']['gpu_thread_cpu_utilization_pct']:.1f}%"),
    ("PM4 Excl ms/frame", lambda d: f"{d['cpu_gpu_separation']['pm4_dispatch_exclusive_ms_per_frame']:.2f}"),
    ("DescriptorPrep ms/f", lambda d: f"{d['pipeline_stage_timings']['DescriptorPrep']['ms_per_frame']:.2f}"),
    ("RenderStatePrep ms/f", lambda d: f"{d['pipeline_stage_timings']['RenderStatePrep']['ms_per_frame']:.2f}"),
    ("CommandRec ms/f", lambda d: f"{d['pipeline_stage_timings']['CommandRecording']['ms_per_frame']:.2f}"),
    ("Barriers ms/f", lambda d: f"{d['pipeline_stage_timings']['BarriersTransitions']['ms_per_frame']:.2f}"),
    ("BufferPrep ms/f", lambda d: f"{d['pipeline_stage_timings']['BufferPrep']['ms_per_frame']:.2f}"),
]

for name, fn in metrics:
    row = [fn(data[k]) for k in files.keys()]
    print(f"{name:<32} | " + " | ".join(f"{v:>12}" for v in row))

print("\n=== GPU WAITS BREAKDOWN ===")
wait_keys = ['PriorityOperation', 'BufferReadback', 'StreamBufferWait', 'FenceWait', 'TimelineSemaphore', 'HostReadbackWait', 'StallBudget']
for wk in wait_keys:
    counts = [str(data[k]['gpu_waits_breakdown'].get(wk, {}).get('count', 0)) for k in files.keys()]
    totals = [f"{data[k]['gpu_waits_breakdown'].get(wk, {}).get('total_ms', 0):.1f}ms" for k in files.keys()]
    maxs = [f"{data[k]['gpu_waits_breakdown'].get(wk, {}).get('max_ms', 0):.1f}ms" for k in files.keys()]
    print(f"\nWait: {wk}")
    print(f"{'  Counts':<32} | " + " | ".join(f"{v:>12}" for v in counts))
    print(f"{'  Totals':<32} | " + " | ".join(f"{v:>12}" for v in totals))
    print(f"{'  Maxs':<32} | " + " | ".join(f"{v:>12}" for v in maxs))

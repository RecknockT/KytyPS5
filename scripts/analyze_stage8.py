import json

files = [
    ('Wide Opt', 'stage8_camera_wide.json'),
    ('Wide Baseline', 'stage8_camera_wide_baseline.json'),
    ('Wide Stages', 'stage8_camera_wide_stages.json'),
    ('Ground Normal', 'stage8_camera_ground_normal.json'),
    ('Ground ExtBuf', 'stage8_camera_ground_extbuf.json'),
    ('Ground Hitches', 'stage8_camera_ground_hitches.json')
]

for label, fname in files:
    try:
        with open(fname, 'r') as f:
            d = json.load(f)
    except Exception as e:
        print(f"Error loading {fname}: {e}")
        continue

    meta = d.get('metadata', {})
    fs = d.get('frametime_summary', {})
    cg = d.get('cpu_gpu_separation', {})
    pst = d.get('pipeline_stage_timings', {})
    sb = d.get('stream_buffer_audit', {})
    hitches = d.get('stuttering_analysis', {}).get('detailed_hitches', [])

    print(f"=== {label} ({fname}) ===")
    print(f"  Git: {meta.get('git_commit')} | Config: {meta.get('config')}")
    print(f"  Duration: {meta.get('real_duration_ms')} ms | Frames: {fs.get('total_frames')} | FPS: {fs.get('effective_fps'):.2f}")
    print(f"  Frametimes: avg={fs.get('avg_frame_ms'):.2f} ms, p50={fs.get('p50_frame_ms'):.2f} ms, p95={fs.get('p95_frame_ms'):.2f} ms, max={fs.get('max_frame_ms'):.2f} ms")
    print(f"  CPU/GPU Sep: wall={cg.get('gpu_thread_wall_clock_ms')} ms, cpu={cg.get('gpu_thread_cpu_time_ms')} ms, util={cg.get('gpu_thread_cpu_utilization_pct')}%, wait={cg.get('gpu_thread_wait_time_ms')} ms, gpu_exec={cg.get('gpu_execution_time_ms')} ms, queries={cg.get('gpu_queries_recorded')}")
    print("  Stages (ms/frame):")
    for sname, sdata in pst.items():
        if isinstance(sdata, dict) and sdata.get('ms_per_frame', 0) > 0.05:
            print(f"    {sname}: {sdata.get('ms_per_frame'):.2f} ms/frame (total {sdata.get('total_ms'):.1f} ms, count={sdata.get('count')})")
    print(f"  StreamBuffer: allocs={sb.get('total_allocations')}, wraps={sb.get('total_wraparounds')}, waits={sb.get('wait_events')}, wait_total={sb.get('wait_total_ms')} ms, wait_max={sb.get('wait_max_ms')} ms")
    print(f"  Hitches count: {len(hitches)}")
    for h in hitches:
        print(f"    Hitch frame={h.get('frame_index')}, frame_ms={h.get('frame_ms')}, blocker={h.get('dominant_blocker')}, dur={h.get('blocker_duration_ms')} ms, conf={h.get('correlation_confidence')}, tick={h.get('blocker_tick')}")
    print()

import json

files = [
    ('Wide (Etapa 7)', 'stage7_camera_wide.json'),
    ('Wide Baseline (Control)', 'stage7_camera_wide_baseline.json'),
    ('Ground Normal (Etapa 7)', 'stage7_camera_ground.json'),
    ('Ground ExtBuf (Etapa 7)', 'stage7_camera_ground_extbuf.json'),
]

for label, path in files:
    with open(path, 'r') as f:
        d = json.load(f)
    fm = d.get('frametime_metrics_ms', {})
    gw = d.get('graphics_workload', {})
    gw_breakdown = d.get('gpu_waits_breakdown', {})
    stutter = d.get('stuttering_analysis', {})
    cfg = d.get('configuration', {})
    
    print(f"=== {label} ===")
    print(f"  FPS: {fm.get('effective_fps', 0):.4f} | Avg frame: {fm.get('avg_frame_ms', 0):.4f} ms | P50: {fm.get('p50_frame_ms', 0):.4f} ms | P95: {fm.get('p95_frame_ms', 0):.4f} ms | P99: {fm.get('p99_frame_ms', 0):.4f} ms | Max: {fm.get('max_frame_ms', 0):.4f} ms")
    print(f"  Total frames: {fm.get('total_frames', 0)} | Draws/frame: {gw.get('draws_per_frame', 0):.2f} | Dispatches/frame: {gw.get('dispatches_per_frame', 0):.2f} | Inst/frame: {gw.get('instances_per_frame', 0):.2f}")
    print(f"  Vulkan submits/frame: {gw.get('vulkan_submits_per_frame', 0):.2f} | Write faults/frame: {gw.get('write_faults_per_frame', 0):.2f} | CPU->GPU MB/frame: {gw.get('cpu_to_gpu_mb_per_frame', 0):.2f}")
    print(f"  Hitches >=50ms: {stutter.get('hitches_50ms_plus', 0)} | >=100ms: {stutter.get('hitches_100ms_plus', 0)} | >=250ms: {stutter.get('hitches_250ms_plus', 0)} | >=500ms: {stutter.get('hitches_500ms_plus', 0)}")
    print(f"  StreamBuffer: wait_events={gw_breakdown.get('stream_buffer_wait_events', 0)}, max_ms={gw_breakdown.get('stream_buffer_wait_max_ms', 0):.2f}, avg_ms_per_frame={gw_breakdown.get('stream_buffer_wait_avg_ms_per_frame', 0):.2f}")
    print(f"  BufferReadback: wait_events={gw_breakdown.get('buffer_readback_wait_events', 0)}, max_ms={gw_breakdown.get('buffer_readback_wait_max_ms', 0):.2f}, avg_ms_per_frame={gw_breakdown.get('buffer_readback_wait_avg_ms_per_frame', 0):.2f}")
    print(f"  PriorityOp: wait_events={gw_breakdown.get('priority_operation_wait_events', 0)}, max_ms={gw_breakdown.get('priority_operation_wait_max_ms', 0):.2f}, avg_ms_per_frame={gw_breakdown.get('priority_operation_wait_avg_ms_per_frame', 0):.2f}")
    print(f"  MRU hits: {gw.get('pipeline_cache_hits', 0)} / {gw.get('pipeline_cache_lookups', 0)} ({gw.get('pipeline_cache_hit_pct', 0):.2f}%)")
    print(f"  Redundant binds: pipeline={gw.get('redundant_pipeline_binds', 0)}, vertex={gw.get('redundant_vertex_binds', 0)}, index={gw.get('redundant_index_binds', 0)}")
    print()

import json

def analyze(fn):
    with open(fn, 'r') as fp:
        d = json.load(fp)
    print(f"================ {fn} ================")
    print("Meta:", d.get('metadata'))
    print("Frametime summary:", d.get('frametime_summary'))
    print("Frametime metrics:", d.get('frametime_metrics_ms'))
    print("CPU/GPU sep:", d.get('cpu_gpu_separation'))
    print("Workload:", d.get('graphics_workload'))
    print("Pipeline stages:", d.get('pipeline_stage_timings'))
    print("GPU timeline waits:", d.get('gpu_timeline_waits'))
    print("GPU waits breakdown:", d.get('gpu_waits_breakdown'))
    print("Stream buffer audit:", d.get('stream_buffer_audit'))
    print("Stuttering:", d.get('stuttering_analysis'))
    print("Phase segmentation:", d.get('phase_segmentation'))

analyze('stage8_1_wide_optimized.json')
analyze('stage8_1_wide_baseline.json')
analyze('stage8_1_landing_optimized.json')
analyze('stage8_1_landing_baseline.json')

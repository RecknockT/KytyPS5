import csv

for fn in ['stage8_1_landing_optimized_frames.csv', 'stage8_1_landing_baseline_frames.csv']:
    print(f"=== {fn} ===")
    with open(fn, 'r') as fp:
        reader = csv.DictReader(fp)
        rows = list(reader)
    print(f"Total rows (frames): {len(rows)}")
    slow = [r for r in rows if float(r.get('frame_ms', 0)) > 200.0]
    print(f"Frames > 200ms: {len(slow)}")
    for r in sorted(slow, key=lambda x: float(x['frame_ms']), reverse=True)[:10]:
        print(f"  Frame {r.get('frame_index')}: {float(r.get('frame_ms')):.1f} ms, phase={r.get('phase')}, draws={r.get('draws')}, disp={r.get('dispatches')}, sub={r.get('submits')}, faults={r.get('write_faults')}, pm4_us={r.get('pm4_dispatch_us')}, desc_us={r.get('descriptor_prep_us')}, rstate_us={r.get('render_state_prep_us')}, sync_us={r.get('sync_waits_us')}, sbuf_us={r.get('stream_buffer_wait_us')}")
    phases = {}
    for r in rows:
        ph = r.get('phase', 'Unknown')
        phases.setdefault(ph, []).append(float(r.get('frame_ms', 0)))
    print("  By phase:")
    for ph, ftimes in phases.items():
        avg_ft = sum(ftimes) / len(ftimes) if ftimes else 0
        fps = 1000.0 / avg_ft if avg_ft > 0 else 0
        print(f"    {ph:<16}: frames={len(ftimes):<4} avg_ft={avg_ft:>7.2f} ms ({fps:>5.2f} FPS) max_ft={max(ftimes):>7.2f} ms")

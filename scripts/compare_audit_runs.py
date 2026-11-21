#!/usr/bin/env python3
"""
KytyPS5 - Audit Profile Comparison and Validation Tool (Etapa 4.2)

Validates JSON/CSV audit profiles, verifies consistency, normalizes metrics per-frame
and per-second, compares multiple runs of each configuration (Run 1 vs Run 2),
evaluates variance, analyzes stuttering and hitches, detects scene load differences,
and computes comparative deltas against baseline.
"""

import sys
import os
import json
import csv
import argparse
from pathlib import Path
from typing import Dict, List, Any, Optional

def load_profile(json_path: Path) -> Dict[str, Any]:
    """Loads and validates an audit JSON profile and its companion CSV."""
    if not json_path.exists():
        raise FileNotFoundError(f"JSON file not found: {json_path}")

    with open(json_path, 'r', encoding='utf-8') as f:
        data = json.load(f)

    csv_path = json_path.with_suffix('.csv')
    csv_exists = csv_path.exists()
    csv_valid = False
    csv_rows = 0

    if csv_exists:
        try:
            with open(csv_path, 'r', encoding='utf-8') as f:
                reader = csv.reader(f)
                for row in reader:
                    if row and not row[0].startswith('#'):
                        csv_rows += 1
            csv_valid = csv_rows > 5
        except Exception:
            csv_valid = False

    data['_csv_path'] = str(csv_path)
    data['_csv_exists'] = csv_exists
    data['_csv_valid'] = csv_valid
    data['_filename'] = json_path.name
    return data

def extract_metrics(p: Dict[str, Any]) -> Dict[str, Any]:
    """Extracts and normalizes metrics from both legacy and new profile schemas."""
    meta = p.get('metadata', {})
    summary = p.get('frametime_summary', p.get('frametime_metrics_ms', {}))
    wt = p.get('write_tracking', p.get('write_tracking_metrics', {}))
    tx = p.get('memory_transfers', p.get('memory_transfers_mb', {}))
    opt = p.get('optimizations', p.get('optimization_metrics', {}))
    waits_raw = p.get('gpu_timeline_waits', p.get('gpu_waits_breakdown', {}))
    submits_raw = p.get('vulkan_submits', p.get('vk_queue_submits_breakdown', {}))
    stutter = p.get('stuttering_analysis', {})
    phases = p.get('phase_segmentation', {})

    total_frames = summary.get('total_frames', 0)
    real_duration_s = meta.get('real_duration_seconds', 0.0)
    capture_phase = meta.get('capture_phase', 'Unknown')
    trigger_reason = meta.get('trigger_reason', 'Unknown')
    if real_duration_s <= 0.0:
        real_duration_ms = meta.get('real_duration_ms', 0)
        real_duration_s = real_duration_ms / 1000.0 if real_duration_ms > 0 else 0.0

    frames_divisor = float(total_frames) if total_frames > 0 else 1.0

    # FPS calculations
    avg_ms = summary.get('avg_frame_ms', 0.0)
    frametime_fps = 1000.0 / avg_ms if avg_ms > 0 else 0.0

    if real_duration_s > 0.0:
        effective_fps = summary.get('effective_fps', total_frames / real_duration_s)
        sec_divisor = real_duration_s
    else:
        sec_divisor = (total_frames * avg_ms) / 1000.0 if avg_ms > 0 else 1.0
        effective_fps = frametime_fps
        real_duration_s = sec_divisor

    # Faults and VEH handler time
    total_faults = wt.get('total_faults', 0)
    total_reprotects = wt.get('total_reprotects', 0)
    faults_per_frame = wt.get('faults_per_frame', total_faults / frames_divisor)
    faults_per_sec = wt.get('faults_per_second', total_faults / sec_divisor)
    reprotects_per_frame = wt.get('reprotects_per_frame', total_reprotects / frames_divisor)
    handler_ns = wt.get('measured_user_handler_total_ns', wt.get('total_handler_ns', 0))
    handler_ms = wt.get('thread_accumulated_handler_ms', handler_ns / 1000000.0)
    handler_ms_per_frame = handler_ms / frames_divisor

    # Transfers
    cpu_to_gpu_mb = tx.get('cpu_to_gpu_upload_mb', 0.0)
    cpu_to_gpu_mb_frame = tx.get('cpu_to_gpu_upload_mb_per_frame', cpu_to_gpu_mb / frames_divisor)
    cpu_to_gpu_mb_sec = tx.get('cpu_to_gpu_upload_mb_per_second', cpu_to_gpu_mb / sec_divisor)

    gpu_to_cpu_mb = tx.get('gpu_to_cpu_readback_mb', 0.0)
    gpu_to_cpu_mb_frame = tx.get('gpu_to_cpu_readback_mb_per_frame', gpu_to_cpu_mb / frames_divisor)
    gpu_to_cpu_mb_sec = tx.get('gpu_to_cpu_readback_mb_per_second', gpu_to_cpu_mb / sec_divisor)

    # Submits: handle dict or list
    total_submits = submits_raw.get('total_submits', 0) if isinstance(submits_raw, dict) else 0
    if total_submits == 0 and isinstance(submits_raw, dict):
        total_submits = sum(v if isinstance(v, int) else v.get('count', 0) for v in submits_raw.values() if isinstance(v, (int, dict)))
    submits_per_frame = total_submits / frames_divisor
    cadence_submits = 0
    explicit_submits = 0

    by_reason = submits_raw.get('by_reason', submits_raw) if isinstance(submits_raw, dict) else {}
    if isinstance(by_reason, dict):
        cad_entry = by_reason.get('Cadence', 0)
        cadence_submits = cad_entry if isinstance(cad_entry, int) else cad_entry.get('count', 0)
        exp_entry = by_reason.get('ExplicitFlush', 0)
        explicit_submits = exp_entry if isinstance(exp_entry, int) else exp_entry.get('count', 0)

    # Waits: handle dict or list
    waits_dict: Dict[str, Dict[str, Any]] = {}
    if isinstance(waits_raw, dict):
        waits_dict = waits_raw
    elif isinstance(waits_raw, list):
        for item in waits_raw:
            if isinstance(item, dict) and 'reason' in item:
                waits_dict[item['reason']] = item

    readback_wait = waits_dict.get('BufferReadback', {})
    rb_count = readback_wait.get('count', 0)
    rb_ms = readback_wait.get('thread_accumulated_ms', readback_wait.get('total_ms', 0.0))
    rb_lat_us = readback_wait.get('avg_latency_us', readback_wait.get('avg_us', 0.0))
    rb_max_ms = readback_wait.get('max_ms', 0.0)

    priority_wait = waits_dict.get('PriorityOperation', {})
    po_count = priority_wait.get('count', 0)
    po_ms = priority_wait.get('thread_accumulated_ms', priority_wait.get('total_ms', 0.0))
    po_lat_us = priority_wait.get('avg_latency_us', priority_wait.get('avg_us', 0.0))
    po_max_ms = priority_wait.get('max_ms', 0.0)

    stream_wait = waits_dict.get('StreamBufferWait', {})
    sb_count = stream_wait.get('count', 0)
    sb_ms = stream_wait.get('thread_accumulated_ms', stream_wait.get('total_ms', 0.0))
    sb_lat_us = stream_wait.get('avg_latency_us', stream_wait.get('avg_us', 0.0))
    sb_max_ms = stream_wait.get('max_ms', 0.0)

    # Top page hotspot check
    top_pages = p.get('top_hot_pages', [])
    top_page_addr = top_pages[0].get('page_vaddr', 'N/A') if top_pages else 'N/A'
    top_page_faults = top_pages[0].get('faults', 0) if top_pages else 0
    top_page_reprotects = top_pages[0].get('reprotects', 0) if top_pages else 0

    return {
        'filename': p.get('_filename', 'unknown'),
        'capture_id': meta.get('capture_id', 'legacy_session'),
        'state': meta.get('capture_state', 'Completed' if total_frames > 0 else 'Unknown'),
        'csv_valid': p.get('_csv_valid', False),
        'duration_s': real_duration_s,
        'frames': total_frames,
        'effective_fps': effective_fps,
        'frametime_fps': frametime_fps,
        'avg_ms': avg_ms,
        'min_ms': summary.get('min_frame_ms', 0.0),
        'p50_ms': summary.get('p50_frame_ms', 0.0),
        'p90_ms': summary.get('p90_frame_ms', 0.0),
        'p95_ms': summary.get('p95_frame_ms', 0.0),
        'p99_ms': summary.get('p99_frame_ms', 0.0),
        'max_ms': summary.get('max_frame_ms', 0.0),
        'total_faults': total_faults,
        'faults_per_frame': faults_per_frame,
        'faults_per_sec': faults_per_sec,
        'total_reprotects': total_reprotects,
        'reprotects_per_frame': reprotects_per_frame,
        'handler_ms_frame': handler_ms_per_frame,
        'upload_mb_frame': cpu_to_gpu_mb_frame,
        'upload_mb_sec': cpu_to_gpu_mb_sec,
        'readback_mb_frame': gpu_to_cpu_mb_frame,
        'readback_mb_sec': gpu_to_cpu_mb_sec,
        'total_submits': total_submits,
        'submits_frame': submits_per_frame,
        'cadence_submits': cadence_submits,
        'explicit_submits': explicit_submits,
        'rb_waits_frame': rb_count / frames_divisor,
        'rb_ms_frame': rb_ms / frames_divisor,
        'rb_lat_us': rb_lat_us,
        'rb_max_ms': rb_max_ms,
        'po_waits_frame': po_count / frames_divisor,
        'po_ms_frame': po_ms / frames_divisor,
        'po_lat_us': po_lat_us,
        'po_max_ms': po_max_ms,
        'sb_count': sb_count,
        'sb_ms': sb_ms,
        'sb_lat_us': sb_lat_us,
        'sb_max_ms': sb_max_ms,
        'partial_refreshes': opt.get('texture_partial_refreshes', 0),
        'partial_fallbacks': opt.get('texture_partial_fallbacks', 0),
        'cadence_held_back': opt.get('cadence_held_back', 0),
        'hitches_50ms': stutter.get('hitches_ge_50ms', 0),
        'hitches_100ms': stutter.get('hitches_ge_100ms', 0),
        'hitches_500ms': stutter.get('hitches_ge_500ms', 0),
        'hitches_1000ms': stutter.get('hitches_ge_1000ms', 0),
        'top_page_addr': top_page_addr,
        'top_page_faults': top_page_faults,
        'top_page_reprotects': top_page_reprotects,
        'capture_phase': capture_phase,
        'trigger_reason': trigger_reason,
        'phases': phases,
    }

def print_markdown_table(headers: List[str], rows: List[List[str]]) -> str:
    """Formats headers and rows into a standard GitHub Markdown table."""
    widths = [len(h) for h in headers]
    for row in rows:
        for i, val in enumerate(row):
            widths[i] = max(widths[i], len(str(val)))

    header_line = "| " + " | ".join(f"{h:<{widths[i]}}" for i, h in enumerate(headers)) + " |"
    sep_line = "| " + " | ".join("-" * widths[i] for i in range(len(headers))) + " |"
    data_lines = [
        "| " + " | ".join(f"{val:<{widths[i]}}" for i, val in enumerate(row)) + " |"
        for row in rows
    ]
    return "\n".join([header_line, sep_line] + data_lines)

def main():
    parser = argparse.ArgumentParser(description="KytyPS5 Audit Profile Analysis & Comparison Tool (Etapa 4.2)")
    parser.add_argument("files", nargs="*", help="List of JSON profile paths to compare")
    parser.add_argument("--dir", help="Directory containing JSON audit files")
    parser.add_argument("--output", help="Optional output Markdown file")
    args = parser.parse_args()

    import glob
    files: List[Path] = []
    for f in args.files:
        if any(c in f for c in ['*', '?', '[']):
            matched = [Path(p) for p in glob.glob(f)]
            if matched:
                files.extend(sorted(matched))
            else:
                files.append(Path(f))
        else:
            files.append(Path(f))
    if args.dir:
        d = Path(args.dir)
        files.extend(sorted(d.glob("stage4_1_*.json")))
        if not files:
            files.extend(sorted(d.glob("audit_stage4_*.json")))
        if not files:
            files.extend(sorted(d.glob("audit_*.json")))

    if not files:
        for root in [Path("."), Path("_Build/windows")]:
            files.extend(sorted(root.glob("stage4_1_*.json")))
        if not files:
            for root in [Path("."), Path("_Build/windows")]:
                files.extend(sorted(root.glob("audit_stage4_*.json")))
        if not files:
            for root in [Path("."), Path("_Build/windows")]:
                files.extend(sorted(root.glob("audit_*.json")))

    # Deduplicate while preserving order
    seen = set()
    dedup_files = []
    for f in files:
        resolved = f.resolve()
        if resolved not in seen:
            seen.add(resolved)
            dedup_files.append(f)
    files = dedup_files

    if not files:
        print("[ERROR] No audit JSON files found. Pass file paths or run benchmark scripts.")
        return 1

    print(f"Loaded {len(files)} audit profile(s) for validation and comparison.\n")

    metrics_list = []
    validation_issues = []

    for f in files:
        try:
            raw = load_profile(f)
            m = extract_metrics(raw)
            metrics_list.append(m)

            if not m['csv_valid']:
                validation_issues.append(f"[INFO] {f.name}: Companion CSV missing or in legacy format ({raw['_csv_path']})")
            if m['state'] == 'Interrupted':
                validation_issues.append(f"[WARN] {f.name}: Capture session was INTERRUPTED before target duration.")
            if m['frames'] < 100:
                validation_issues.append(f"[WARN] {f.name}: Very low frame count ({m['frames']} frames).")

            # Check consistency between frametime metrics and phase segmentation
            phases = m.get('phases', {})
            gp = phases.get('gameplay', {})
            gp_frames = gp.get('frame_count', 0)
            gp_total_ms = gp.get('total_frame_ms', 0.0)
            if gp_frames > 0 and abs(gp_frames - m['frames']) > 5:
                validation_issues.append(
                    f"[WARN] {f.name}: Inconsistencia de instrumentación: frametime_metrics_ms ({m['frames']} frames) "
                    f"!= phase_gameplay ({gp_frames} frames) [delta={gp_frames - m['frames']}]. "
                    f"Frames posteriores al fin de captura se filtraron en phase_segmentation (pre-Etapa 4.3)."
                )
            if gp_total_ms > 0 and abs(gp_total_ms / 1000.0 - m['duration_s']) > 5.0:
                validation_issues.append(
                    f"[WARN] {f.name}: Discrepancia de duración de fase: duración de sesión es {m['duration_s']:.1f}s "
                    f"pero phase_gameplay totaliza {gp_total_ms / 1000.0:.1f}s (+{gp_total_ms / 1000.0 - m['duration_s']:.1f}s). "
                    f"Evidencia retraso en cierre de ventana (pre-Etapa 4.3)."
                )
        except Exception as e:
            validation_issues.append(f"[ERROR] Failed to load {f.name}: {e}")

    if validation_issues:
        print("--- Validation Notes & Warnings ---")
        for issue in validation_issues:
            print(issue)
        print()

    has_stage4_1 = any('stage4_1' in m['filename'] for m in metrics_list)
    warning_banner = ""
    if has_stage4_1:
        warning_banner = (
            "> [!WARNING] **ADVERTENCIA METODOLÓGICA (Resultados Exploratorios - Etapa 4.1)**:\n"
            "> Las capturas de la Etapa 4.1 NO se realizaron bajo condiciones equivalentes garantizadas\n"
            "> (algunas ejecuciones iniciaron desde el inicio de la app/carga y otras en gameplay interactivo).\n"
            "> Estos datos se conservan como referencia histórica exploratoria y NO como evidencia concluyente de causalidad.\n"
        )
        print(warning_banner)

    # 1. Summary Table: Frametimes & Percentiles
    summary_headers = [
        "Capture File", "Phase", "State", "Duration", "Frames", "Eff. FPS", "Avg ms", "P50 ms", "P95 ms", "P99 ms", "Max ms"
    ]
    summary_rows = []
    for m in metrics_list:
        summary_rows.append([
            m['filename'],
            m.get('capture_phase', 'Unknown'),
            m['state'],
            f"{m['duration_s']:.1f}s",
            str(m['frames']),
            f"{m['effective_fps']:.2f}",
            f"{m['avg_ms']:.2f}",
            f"{m['p50_ms']:.2f}",
            f"{m['p95_ms']:.2f}",
            f"{m['p99_ms']:.2f}",
            f"{m['max_ms']:.2f}",
        ])

    table1 = print_markdown_table(summary_headers, summary_rows)
    print("### 1. Resumen de Frametimes y Percentiles")
    print(table1)
    print()

    # 2. Normalized Per-Frame Metrics
    norm_headers = [
        "Capture File", "Faults/f", "VEH ms/f", "Upload MB/f", "Readback MB/f",
        "Submits/f", "Rb Wait ms/f", "Rb Lat (us)", "Rb Max ms"
    ]
    norm_rows = []
    for m in metrics_list:
        norm_rows.append([
            m['filename'],
            f"{m['faults_per_frame']:.1f}",
            f"{m['handler_ms_frame']:.2f}",
            f"{m['upload_mb_frame']:.2f}",
            f"{m['readback_mb_frame']:.2f}",
            f"{m['submits_frame']:.2f}",
            f"{m['rb_ms_frame']:.2f}",
            f"{m['rb_lat_us']:.0f}",
            f"{m['rb_max_ms']:.1f}",
        ])

    table2 = print_markdown_table(norm_headers, norm_rows)
    print("### 2. Métricas Normalizadas por Frame")
    print(table2)
    print()

    # 3. GPU Scheduler Stalls & Stuttering Analysis
    stall_headers = [
        "Capture File", "Cadence Submits", "Cadence Held", "SB Waits", "SB Avg (ms)", "SB Max ms", "PO Max ms", "Max Frame ms"
    ]
    stall_rows = []
    for m in metrics_list:
        sb_avg_ms = (m['sb_lat_us'] / 1000.0) if m['sb_count'] > 0 else 0.0
        stall_rows.append([
            m['filename'],
            str(m['cadence_submits']),
            str(m['cadence_held_back']),
            str(m['sb_count']),
            f"{sb_avg_ms:.1f}",
            f"{m['sb_max_ms']:.1f}",
            f"{m['po_max_ms']:.1f}",
            f"{m['max_ms']:.1f}",
        ])

    table3 = print_markdown_table(stall_headers, stall_rows)
    print("### 3. Diagnóstico de Stuttering y Esperas GPU (StreamBuffer / Scheduler)")
    print(table3)
    print()

    # 4. Check Run 1 vs Run 2 pairing and variance
    by_config: Dict[str, List[Dict[str, Any]]] = {}
    for m in metrics_list:
        name = m['filename']
        base_name = name.replace('_run1', '').replace('_run2', '').replace('.json', '')
        by_config.setdefault(base_name, []).append(m)

    var_headers = [
        "Configuración", "Run 1 Avg ms", "Run 2 Avg ms", "Diff %", "Run 1 FPS", "Run 2 FPS", "Faults Diff %", "Detección Carga Escena", "Evaluación"
    ]
    var_rows = []
    for cfg, runs in by_config.items():
        if len(runs) >= 2:
            r1, r2 = runs[0], runs[1]
            diff_ms = abs(r2['avg_ms'] - r1['avg_ms'])
            mean_ms = (r1['avg_ms'] + r2['avg_ms']) / 2.0
            diff_pct = (diff_ms / mean_ms) * 100.0 if mean_ms > 0 else 0.0

            fault_diff_pct = abs(r1['faults_per_frame'] - r2['faults_per_frame']) / max(r1['faults_per_frame'], 1.0) * 100.0
            up_diff_pct = abs(r1['upload_mb_frame'] - r2['upload_mb_frame']) / max(r1['upload_mb_frame'], 0.01) * 100.0

            scene_load = "Carga Equivalente"
            if fault_diff_pct > 25.0 or up_diff_pct > 50.0:
                scene_load = "[ALERTA] Discrepancia Escena"
            elif fault_diff_pct > 10.0 or up_diff_pct > 25.0:
                scene_load = "Variación Moderada"

            eval_str = "Excelente (< 2%)" if diff_pct < 2.0 else ("Aceptable (< 5%)" if diff_pct < 5.0 else "[ALERTA] Alta varianza (> 5%)")
            var_rows.append([
                cfg,
                f"{r1['avg_ms']:.2f}",
                f"{r2['avg_ms']:.2f}",
                f"{diff_pct:.2f}%",
                f"{r1['effective_fps']:.2f}",
                f"{r2['effective_fps']:.2f}",
                f"{fault_diff_pct:.1f}%",
                scene_load,
                eval_str,
            ])
        else:
            var_rows.append([cfg, f"{runs[0]['avg_ms']:.2f}", "N/A", "-", f"{runs[0]['effective_fps']:.2f}", "-", "-", "-", "1 captura"])

    table4 = print_markdown_table(var_headers, var_rows)
    print("### 4. Comparación entre Repeticiones (Run 1 vs Run 2) y Variabilidad")
    print(table4)
    print()

    # 5. Baseline delta comparison
    baseline_run = None
    for m in metrics_list:
        if 'baseline' in m['filename'].lower():
            baseline_run = m
            break

    table5 = ""
    if baseline_run:
        delta_headers = ["Configuración", "FPS Delta", "Frametime Delta", "Faults Delta", "Upload Delta", "Readback Wait Delta"]
        delta_rows = []
        base_fps = baseline_run['effective_fps']
        base_ms = baseline_run['avg_ms']
        base_faults = baseline_run['faults_per_frame']
        base_up = baseline_run['upload_mb_frame']
        base_rb = baseline_run['rb_ms_frame']

        for m in metrics_list:
            fps_d = ((m['effective_fps'] - base_fps) / base_fps) * 100.0 if base_fps > 0 else 0.0
            ms_d = ((m['avg_ms'] - base_ms) / base_ms) * 100.0 if base_ms > 0 else 0.0
            fault_d = ((m['faults_per_frame'] - base_faults) / base_faults) * 100.0 if base_faults > 0 else 0.0
            up_d = ((m['upload_mb_frame'] - base_up) / base_up) * 100.0 if base_up > 0 else 0.0
            rb_d = ((m['rb_ms_frame'] - base_rb) / base_rb) * 100.0 if base_rb > 0 else 0.0

            delta_rows.append([
                m['filename'],
                f"{fps_d:+.2f}%",
                f"{ms_d:+.2f}%",
                f"{fault_d:+.2f}%",
                f"{up_d:+.2f}%",
                f"{rb_d:+.2f}%",
            ])
        table5 = print_markdown_table(delta_headers, delta_rows)
        print(f"### 5. Deltas Relativos respecto a Baseline ({baseline_run['filename']})")
        print(table5)
        print()

    if args.output:
        out_path = Path(args.output)
        with open(out_path, 'w', encoding='utf-8') as out:
            out.write("# KytyPS5 Benchmark Analysis Report (Etapa 4.2)\n\n")
            if warning_banner:
                out.write(warning_banner + "\n\n")
            out.write("### 1. Resumen de Frametimes y Percentiles\n")
            out.write(table1 + "\n\n")
            out.write("### 2. Métricas Normalizadas por Frame\n")
            out.write(table2 + "\n\n")
            out.write("### 3. Diagnóstico de Stuttering y Esperas GPU (StreamBuffer / Scheduler)\n")
            out.write(table3 + "\n\n")
            out.write("### 4. Comparación entre Repeticiones (Run 1 vs Run 2) y Variabilidad\n")
            out.write(table4 + "\n\n")
            if table5:
                out.write("### 5. Deltas Relativos respecto a Baseline\n")
                out.write(table5 + "\n\n")
        print(f"Report written to {out_path}")

    return 0

if __name__ == '__main__':
    sys.exit(main())

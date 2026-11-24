import csv,json,statistics,sys
from pathlib import Path
root=Path(sys.argv[1])
mode=sys.argv[2] if len(sys.argv)>2 else 'binding_reuse'
assert mode in ('binding_reuse','gpu_submit_cadence','tile_layout_cache','resource_prep_reuse','read_buffer_reuse','compact_image_readback')
def pct(values,p):
    s=sorted(values);i=(len(s)-1)*p;a=int(i);b=min(a+1,len(s)-1)
    return s[a]+(s[b]-s[a])*(i-a)
def read(label):
    j=json.loads((root/(label+'.json')).read_text(encoding='utf-8-sig'))
    rows=list(csv.DictReader((root/(label+'_frames.csv')).open(encoding='utf-8-sig')))
    assert j['metadata']['capture_state']=='Completed'
    assert len(rows)==j['frametime_summary']['total_frames']
    assert j['configuration'][mode]==label.startswith('candidate')
    return j,rows
samples={name:read(name) for name in ['control1','candidate1','candidate2','control2']}
configs=[{k:v for k,v in j['configuration'].items() if k!=mode} for j,_ in samples.values()]
assert all(c==configs[0] for c in configs),'Other configuration changed'
def group(names):
    js=[samples[n][0] for n in names];rows=[r for n in names for r in samples[n][1]]
    n=len(rows);duration=sum(j['metadata']['real_duration_seconds'] for j in js)
    optimkeys=['compact_readback_copies','compact_readback_fallbacks','readback_prefill_avoided_bytes','readback_staging_saved_bytes','read_buffer_hits','read_buffer_misses','resource_prep_hits','resource_prep_misses','tile_layout_hits','tile_layout_misses','binding_range_reuses','binding_lookup_reuses','binding_sampler_reuses','binding_upload_reuses','binding_upload_reuse_bytes']
    return {'frames':n,'seconds':duration,'fps':n/duration,'p95_ms':pct([float(r['frame_ms']) for r in rows],.95),'p99_ms':pct([float(r['frame_ms']) for r in rows],.99),'draws_per_frame':sum(float(r['draws']) for r in rows)/n,'descriptor_ms_per_frame':sum(float(r['descriptor_prep_us']) for r in rows)/n/1000,'render_state_ms_per_frame':sum(float(r['render_state_prep_us']) for r in rows)/n/1000,'stream_alloc_kib_per_frame':sum(float(r['stream_buffer_alloc_kb']) for r in rows)/n,'gpu_thread_busy_ms_per_frame':sum(j['graphics_workload']['gpu_thread_busy_ms'] for j in js)/n,'shaders':sum(j['graphics_workload']['shaders_compiled'] for j in js),'pipelines':sum(j['graphics_workload']['pipelines_created'] for j in js),'reuse_counts':{k:sum(j['optimizations'].get(k,0) for j in js) for k in optimkeys}}
a=group(['control1','control2']);b=group(['candidate1','candidate2'])
for names, metrics in [(['control1','control2'], a), (['candidate1','candidate2'], b)]:
    js=[samples[name][0] for name in names]
    n=metrics['frames']
    metrics['gpu_thread_cpu_ms_per_frame']=sum(j['cpu_gpu_separation']['gpu_thread_cpu_time_ms'] for j in js)/n
    metrics['readback_waits_per_frame']=sum(j['gpu_timeline_waits']['BufferReadback']['count'] for j in js)/n
    metrics['readback_thread_ms_per_frame']=sum(j['gpu_timeline_waits']['BufferReadback']['thread_accumulated_ms'] for j in js)/n
    metrics['submits_per_frame']=sum(j['vulkan_submits']['total_submits'] for j in js)/n
    metrics['submits_by_reason_per_frame']={reason:sum(j['vulkan_submits']['by_reason'][reason]['count'] for j in js)/n for reason in js[0]['vulkan_submits']['by_reason']}
    assert metrics['shaders']==0 and metrics['pipelines']==0, 'New shaders/pipelines during static comparison'

run=json.loads((root/'run.json').read_text(encoding='utf-8-sig'))
binary_hash=run.get('executableHash',run.get('sha256'))
assert isinstance(binary_hash,str) and len(binary_hash)==64, 'Missing tested executable hash'
result={'mode':mode,'run':str(root),'pid':run['pid'],'binary_sha256':binary_hash,'one_process':True,'order':'ABBA','control':a,'candidate':b,'fps_change_percent':100*(b['fps']/a['fps']-1),'draws_change_percent':100*(b['draws_per_frame']/a['draws_per_frame']-1),'samples':{name:{'fps':j['frametime_summary']['effective_fps'],'p95_ms':j['frametime_summary']['p95_frame_ms'],'draws':j['graphics_workload']['draws_per_frame'],'capture_id':j['metadata']['capture_id']} for name,(j,rows) in samples.items()}}
(root/'comparison.json').write_text(json.dumps(result,indent=2)+'\n')

print(json.dumps(result,indent=2))

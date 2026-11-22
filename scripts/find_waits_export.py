with open('src/common/auditProfiler.cpp') as f:
    lines = f.readlines()
for i, l in enumerate(lines):
    if 'gpu_waits_breakdown' in l:
        for j in range(max(0, i-5), min(len(lines), i+30)):
            print(f"{j+1}: {lines[j]}", end='')
        break

"""Run in the Linux container, after the Release build; compare paired trials."""
import csv
import json
import statistics
import subprocess
from pathlib import Path

root = Path(__file__).resolve().parents[1]
binary = root / 'build/risk-Release/risk_benchmark'
out = root / 'results/risk-controls'
count = 200000
seed = 20261001
repeats = 7

def run(mode, workload):
    result = subprocess.run([str(binary), mode, workload, str(count), str(seed)],
                            text=True, capture_output=True, check=True)
    if result.stderr.strip():
        raise RuntimeError(result.stderr)
    row = json.loads(result.stdout)
    assert row['orders'] == count
    return row

rows = []
for workload in ('crossing', 'resting'):
    # One untabulated warm-up per mode, each still a fresh process.
    run('bare', workload)
    run('risk', workload)
    for repeat in range(repeats):
        modes = ('bare', 'risk') if repeat % 2 == 0 else ('risk', 'bare')
        paired = []
        for mode in modes:
            row = run(mode, workload)
            row['repeat'] = repeat + 1
            rows.append(row)
            paired.append(row)
        assert paired[0]['workload_hash'] == paired[1]['workload_hash']
        assert paired[0]['result_hash'] == paired[1]['result_hash']
        print(workload, repeat + 1, 'outcomes match', flush=True)

(out / 'benchmark_raw.json').write_text(json.dumps(rows, indent=2) + '\n')
with (out / 'benchmark_raw.csv').open('w') as stream:
    writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
    writer.writeheader()
    writer.writerows(rows)
metrics = ('throughput_orders_s','p50_ns','p95_ns','p99_ns','p99_9_ns',
           'cpu_seconds','cpu_percent','peak_rss_kib')
summary = {}
for workload in ('crossing', 'resting'):
    summary[workload] = {}
    for mode in ('bare','risk'):
        selected = [r for r in rows if r['workload']==workload and r['mode']==mode]
        assert len({r['result_hash'] for r in selected}) == 1
        summary[workload][mode] = {
            metric: statistics.median(r[metric] for r in selected) for metric in metrics
        }
        summary[workload][mode]['throughput_min'] = min(r['throughput_orders_s'] for r in selected)
        summary[workload][mode]['throughput_max'] = max(r['throughput_orders_s'] for r in selected)
    base, risk = summary[workload]['bare'], summary[workload]['risk']
    summary[workload]['throughput_reduction_percent'] = 100*(1-risk['throughput_orders_s']/base['throughput_orders_s'])
    summary[workload]['p99_overhead_ns'] = risk['p99_ns']-base['p99_ns']
(out / 'benchmark_summary.json').write_text(json.dumps(summary, indent=2)+'\n')
print(json.dumps(summary, indent=2))

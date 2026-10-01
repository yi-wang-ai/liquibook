import subprocess,json,pathlib,statistics
out=pathlib.Path('/workspace/results/wal');rows=[]
modes=['none','buffered','group100','sync1']
for repeat in range(3):
 for mode in (modes if repeat%2==0 else modes[::-1]):
  r=json.loads(subprocess.check_output(['/workspace/build/wal-Release/wal_benchmark',mode,'5000'],text=True));r['repeat']=repeat;rows.append(r);print(mode,repeat,flush=True)
assert len({r['state_hash'] for r in rows})==1
(out/'benchmark-raw.json').write_text(json.dumps(rows,indent=2))
summary={m:{k:statistics.median(r[k] for r in rows if r['mode']==m) for k in ('throughput','durable_throughput','submit_p99_ns','durable_ack_p99_ns','durable_acks','recovery_seconds')} for m in modes}
(out/'benchmark-summary.json').write_text(json.dumps(summary,indent=2));print(json.dumps(summary,indent=2))

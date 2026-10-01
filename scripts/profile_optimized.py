import pathlib, subprocess, json, statistics
root=pathlib.Path('/workspace'); d=root/'build/profile/8'; d.mkdir(exist_ok=True)
(d/'risk_gateway.h').write_text((root/'extensions/risk/risk_gateway.h').read_text())
(d/'bench.cpp').write_text((root/'build/profile/6/bench.cpp').read_text())
subprocess.run(['g++','-O3','-std=c++11','-DNDEBUG','-I/workspace/src',str(d/'bench.cpp'),'-o',str(d/'bench')],check=True)
rows=[]
for workload in ('crossing','resting'):
 for repeat in range(7):
  for level in ((6,8) if repeat%2==0 else (8,6)):
   r=json.loads(subprocess.check_output([str(root/'build/profile'/str(level)/'bench'),'risk',workload,'1000000','20261001'],text=True))
   r.update(level=level,repeat=repeat);rows.append(r)
 for repeat in range(7):
  pair=[r for r in rows if r['workload']==workload and r['repeat']==repeat]
  assert len({r['result_hash'] for r in pair})==1
(root/'results/risk-profile/optimized.json').write_text(json.dumps(rows,indent=2))
print({w:{l:statistics.median(r['throughput_orders_s'] for r in rows if r['workload']==w and r['level']==l) for l in (6,8)} for w in ('crossing','resting')})

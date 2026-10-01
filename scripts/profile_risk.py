"""Benchmark-only ablations; never compiled into the production gateway."""
import json, pathlib, subprocess, statistics
root=pathlib.Path('/workspace'); out=root/'results/risk-profile'; out.mkdir(exist_ok=True)
header=subprocess.check_output(['git','show','risk-controls-verified:extensions/risk/risk_gateway.h'],text=True)
bench=(root/'benchmarks/risk_benchmark.cpp').read_text().replace('#include "../extensions/risk/risk_gateway.h"','#include "risk_gateway.h"')
# Separate instrumentation run: count C++ allocations and large allocations
# (unordered container bucket reallocations are visible among large allocations).
counter='''#include <cstdlib>
#include <new>
static unsigned long long allocs=0, bytes=0, large_allocs=0;
void* operator new(std::size_t n) { ++allocs; bytes+=n; if(n>4096)++large_allocs; if(void* p=std::malloc(n))return p; throw std::bad_alloc(); }
void operator delete(void* p) noexcept { std::free(p); }
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete[](void* p) noexcept { ::operator delete(p); }
'''
variants={}
for level in range(1,8):
 s=header
 if level<2:
  a=s.index('    // Division avoids'); b=s.index('    // Widen before',a); s=s[:a]+s[b:]
 if level<3:
  s=s.replace('    if (accepted_ids_.count(order.id)) return {RejectReason::DuplicateOrderId};','').replace('    accepted_ids_.insert(order.id);','')
 if level<4:
  a=s.index('    // Widen before'); b=s.index('    if (now <',a); s=s[:a]+s[b:]
 if level<5:
  a=s.index('    if (now <'); b=s.index('    return {RejectReason::None};',a); s=s[:a]+s[b:]
  s=s.replace('try { admit(state, now); }','try { (void)now; }')
 if level<6:
  s=s.replace('    (order.buy ? state.reserved_buy : state.reserved_sell) += order.quantity;','').replace('    release(id, buy, quantity);','')
 if level==7:
  s=s.replace('    auto first = state.admissions.begin();\n    if (now >= window)\n      first = std::upper_bound(first, state.admissions.end(), now - window);\n    if (static_cast<size_t>(state.admissions.end() - first) >= limits.max_orders_per_second)', '''    if (state.admissions.size() >= limits.max_orders_per_second &&
        (now < window || state.admissions.front() > now - window))''')
 variants[level]=s
for level,s in variants.items():
 d=root/'build/profile'/str(level); d.mkdir(parents=True,exist_ok=True)
 (d/'risk_gateway.h').write_text(s); (d/'bench.cpp').write_text(bench)
 subprocess.run(['g++','-std=c++11','-O3','-DNDEBUG','-g','-fno-omit-frame-pointer','-I/workspace/src',str(d/'bench.cpp'),'-o',str(d/'bench')],check=True)
d=root/'build/profile/6'
instrumented=counter+bench.replace('  rusage before{},after{};','  allocs=bytes=large_allocs=0;\n  rusage before{},after{};').replace('  uint64_t digest=', '  std::cerr << "allocations=" << allocs << " bytes=" << bytes << " large_allocations=" << large_allocs << "\\n";\n  uint64_t digest=')
(d/'count.cpp').write_text(instrumented)
subprocess.run(['g++','-std=c++11','-O3','-I/workspace/src',str(d/'count.cpp'),'-o',str(d/'count')],check=True)
rows=[]
for workload in ('crossing','resting'):
 for repeat in range(5):
  levels=list(range(8))
  if repeat%2:levels.reverse()
  for level in levels:
   binary=root/'build/profile'/str(max(1,level))/'bench'
   r=json.loads(subprocess.check_output([str(binary),'bare' if level==0 else 'risk',workload,'1000000','20261001'],text=True))
   r.update(level=level,repeat=repeat); rows.append(r)
 for mode in ('bare','risk'):
  r=subprocess.run([str(d/'count'),mode,workload,'1000000','20261001'],text=True,capture_output=True,check=True)
  (out/(workload+'-'+mode+'-allocations.txt')).write_text(r.stderr)
for workload in ('crossing','resting'):
 hashes={r['result_hash'] for r in rows if r['workload']==workload}; assert len(hashes)==1
(out/'raw.json').write_text(json.dumps(rows,indent=2))
summary={w:{str(l):statistics.median(r['throughput_orders_s'] for r in rows if r['workload']==w and r['level']==l) for l in range(8)} for w in ('crossing','resting')}
(out/'summary.json').write_text(json.dumps(summary,indent=2))
print(json.dumps(summary,indent=2))

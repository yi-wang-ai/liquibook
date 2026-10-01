from pathlib import Path
import subprocess
root=Path('/workspace')
prefix='''#include <cstdlib>
#include <new>
static unsigned long long allocs=0, bytes=0, large_allocs=0, order_rehashes=0, id_rehashes=0, account_lookups=0;
void* operator new(std::size_t n) {++allocs;bytes+=n;if(n>4096)++large_allocs;if(void*p=std::malloc(n))return p;throw std::bad_alloc();}
void operator delete(void*p) noexcept {std::free(p);}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete[](void*p) noexcept {::operator delete(p);}
'''
for level in (6,8):
 d=root/'build/profile'/str(level)
 header=(d/'risk_gateway.h').read_text().replace('accounts_.at(', '(++account_lookups, accounts_).at(').replace('accounts_.find(', '(++account_lookups, accounts_).find(')
 header=header.replace('    accepted_ids_.insert(order.id);','    auto buckets=accepted_ids_.bucket_count(); accepted_ids_.insert(order.id); id_rehashes += buckets != accepted_ids_.bucket_count();')
 header=header.replace('    orders_.emplace(request.id, std::move(stored));','    auto buckets=orders_.bucket_count(); orders_.emplace(request.id, std::move(stored)); order_rehashes += buckets != orders_.bucket_count();')
 (d/'count_header.h').write_text(header)
 bench=(d/'bench.cpp').read_text().replace('"risk_gateway.h"','"count_header.h"')
 bench=bench.replace('    orders_.emplace(request.id,std::move(order));','    auto buckets=orders_.bucket_count(); orders_.emplace(request.id,std::move(order)); order_rehashes += buckets != orders_.bucket_count();')
 bench=bench.replace('  rusage before{},after{};', '  allocs=bytes=large_allocs=order_rehashes=id_rehashes=account_lookups=0;\n  rusage before{},after{};')
 bench=bench.replace('  uint64_t digest=', '  std::cerr << "allocations " << allocs << " bytes " << bytes << " large_allocations " << large_allocs << " order_rehashes " << order_rehashes << " id_rehashes " << id_rehashes << " account_lookups " << account_lookups << "\\n";\n  uint64_t digest=')
 (d/'counters.cpp').write_text(prefix+bench)
 subprocess.run(['g++','-O3','-std=c++11','-I/workspace/src',str(d/'counters.cpp'),'-o',str(d/'counters')],check=True)
 for mode in ('bare','risk'):
  for workload in ('crossing','resting'):
   r=subprocess.run([str(d/'counters'),mode,workload,'1000000','20261001'],text=True,capture_output=True,check=True)
   (root/'results/risk-profile'/f'counters-{level}-{mode}-{workload}.txt').write_text(r.stderr)

#include "../test/fix/harness.h"
#include <cmath>
#include <iostream>
int main(int argc,char** argv){
 uint64_t count=argc>1?std::stoull(argv[1]):5000;
 auto config=fix_settings();for(auto& s:config.risk)for(auto& a:s.accounts){a.limits.max_long_position=1000000;a.limits.max_orders_per_second=1000000;}
 Network n(config,false);std::vector<uint64_t> latency;
 for(int i=0;i<100;++i)n.submit(n.order("W"+std::to_string(i),"1","1"));
 auto start=steady_now();
 for(uint64_t i=0;i<count;++i){
  auto m=n.order("B"+std::to_string(i),"1","1");auto id=value(m,11);auto tick=steady_now();
  n.send(m);auto r=n.client.wait(id);auto elapsed=steady_now()-tick;
  if(value(r,150)!="0")throw std::runtime_error("benchmark rejected");latency.push_back(elapsed);
 }
 auto elapsed=steady_now()-start;n.service->drain();
 if(n.service->sequence()!=count+100)throw std::runtime_error("benchmark sequence mismatch");
 std::sort(latency.begin(),latency.end());auto p=[&](double quantile){return latency[static_cast<size_t>(std::ceil(quantile*count))-1];};
 std::cout<<std::setprecision(12)<<"{\"requests\":"<<count<<",\"requests_s\":"<<double(count)*1e9/elapsed
  <<",\"p50_ns\":"<<p(.5)<<",\"p95_ns\":"<<p(.95)<<",\"p99_ns\":"<<p(.99)<<",\"p99_9_ns\":"<<p(.999)<<"}\n";
}

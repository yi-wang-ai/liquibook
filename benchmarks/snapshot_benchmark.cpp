#include "../extensions/durability/wal.h"
#include <iostream>
#include <iomanip>
using namespace exchange;
using namespace exchange::durable;
int main(){
 char name[]="/tmp/liquibook-snapshot-bench-XXXXXX";auto p=mkdtemp(name);if(!p)return 2;
 std::string dir=p,path=dir+"/wal",snap=dir+"/snapshot";
 RiskLimits l;l.max_long_position=1000000;l.max_short_position=1000000;
 Config config={{1,{{1,l,0}}}};
 std::string expected;double creation;
 {
  Exchange x(path,config,Durability::Buffered);
  for(uint64_t i=0;i<52000;++i){
   Command c;c.symbol=1;c.account=1;c.order=i%1000;c.buy=true;c.quantity=10;
   c.price=90+(i/1000)%2;c.time=i*1000000000ULL;
   if(i>=1000)c.type=CommandType::ReplaceOrder;
   if(!x.execute(c).result)throw std::runtime_error("benchmark rejected command");
   if(i==50999){auto start=steady_now();x.save_snapshot(snap);creation=(steady_now()-start)/1e6;}
  }
  x.flush();expected=x.canonical_state();
 }
 struct stat ws{},ss{};stat(path.c_str(),&ws);stat(snap.c_str(),&ss);
 std::cout<<"trial,full_replay_ms,snapshot_tail_ms,snapshot_create_ms,wal_bytes,snapshot_bytes\n";
 for(int trial=0;trial<7;++trial){
  double elapsed[2];
  for(int j=0;j<2;++j){
   int mode=(trial+j)%2;auto start=steady_now();
   Exchange x(path,config,Durability::Buffered,100,mode?snap:"");
   elapsed[mode]=(steady_now()-start)/1e6;
   if(x.canonical_state()!=expected)throw std::runtime_error("state mismatch");
  }
  std::cout<<trial<<","<<std::setprecision(10)<<elapsed[0]<<","<<elapsed[1]<<","<<creation<<","<<ws.st_size<<","<<ss.st_size<<"\n";
 }
 unlink(path.c_str());unlink(snap.c_str());rmdir(dir.c_str());
}

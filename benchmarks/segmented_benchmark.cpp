#include "../extensions/durability/segmented.h"
#include <iostream>
using namespace exchange;
using namespace exchange::durable;
int main(int argc,char** argv){
 if(argc!=3)return 2;
 uint64_t count=std::stoull(argv[1]),percent=std::stoull(argv[2]),at=count/100*percent;
 if(count<1000||percent>=100||!at)return 2;
 char name[]="/tmp/liquibook-segmented-bench-XXXXXX";auto p=mkdtemp(name);if(!p)return 2;std::string dir=p;
 RiskLimits limits;limits.max_long_position=1000000;limits.max_short_position=1000000;
 Config config={{1,{{1,limits,0}}}};std::string expected;double creation=0;
 {
  SegmentedExchange x(dir,config,100000,Durability::Buffered);
  for(uint64_t i=0;i<count;++i){
   Command c;c.account=1;c.order=i%1000;c.quantity=10;c.price=90+(i/1000)%2;c.time=i*1000000000ULL;
   if(i>=1000)c.type=CommandType::ReplaceOrder;
   if(!x.execute(c).result)throw std::runtime_error("benchmark rejected");
   if(i+1==at){auto start=steady_now();x.save_snapshot(false);creation=(steady_now()-start)/1e6;}
  }
  x.flush();expected=x.canonical_state();
 }
 std::cout<<"commands,snapshot_percent,trial,mode,snapshot_ms,tail_ms,total_ms,bytes_read,records_decoded,snapshot_create_ms,state_hash\n";
 for(int trial=0;trial<3;++trial){
  for(int j=0;j<2;++j){
   bool full=(trial+j)%2==0;
   SegmentedExchange x(dir,config,100000,Durability::Buffered,100,full);
   if(x.canonical_state()!=expected)throw std::runtime_error("recovered state mismatch");
   const auto& s=x.recovery_stats();
   if(s.records_decoded!=(full?count:count-at))throw std::runtime_error("decoded record count mismatch");
   std::cout<<count<<","<<percent<<","<<trial<<","<<(full?"full":"snapshot")<<","
     <<std::setprecision(12)<<s.snapshot_ms<<","<<s.tail_ms<<","<<s.total_ms<<","
     <<s.bytes_read<<","<<s.records_decoded<<","<<creation<<","<<x.hash()<<std::endl;
  }
 }
 DIR* d=opendir(dir.c_str());if(!d)return 3;
 while(auto e=readdir(d)){std::string n=e->d_name;if(n!="."&&n!="..")if(unlink((dir+"/"+n).c_str()))return 3;}
 closedir(d);if(rmdir(dir.c_str()))return 3;
}

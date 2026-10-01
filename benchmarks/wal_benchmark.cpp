#include "../extensions/durability/wal.h"
#include <iostream>
#include <iomanip>
#include <cmath>
using namespace exchange;using namespace exchange::durable;
int main(int argc,char** argv){
 try{
  if(argc!=3)throw std::invalid_argument("MODE COUNT");
  std::string mode=argv[1];size_t n=std::stoul(argv[2]);
  if(!n||n>1000000||n%2)throw std::invalid_argument("invalid count");
  Durability policy;
  if(mode=="none")policy=Durability::NoWal;else if(mode=="buffered")policy=Durability::Buffered;
  else if(mode=="group100")policy=Durability::Group;else if(mode=="sync1")policy=Durability::EveryCommand;
  else throw std::invalid_argument("invalid mode");
  char tmp[]="/tmp/lb-wal-bench-XXXXXX";auto dir=mkdtemp(tmp);if(!dir)throw std::runtime_error("mkdtemp");
  std::string path=std::string(dir)+"/wal-000001.log";
  RiskLimits limits;limits.max_order_qty=1000;limits.max_orders_per_second=10000000;limits.max_long_position=10000000;limits.max_short_position=10000000;
  Config config={{1,{{1,limits,0},{2,limits,0}}}};
  std::vector<Command> commands(n);
  for(size_t i=0;i<n;++i){auto& c=commands[i];c.order=i;c.account=i%2+1;c.buy=i%2==0;c.quantity=10;c.price=100;c.time=i*10000;}
  std::vector<uint64_t> submit(n),acks;acks.reserve(n);
  std::vector<std::chrono::steady_clock::time_point> starts(n);
  std::string state;double seconds,durable_seconds,recovery=0;uint64_t hash=0;
  {
    Exchange x(path,config,policy);uint64_t watermark=0;
    auto begin=std::chrono::steady_clock::now();
    for(size_t i=0;i<n;++i){starts[i]=std::chrono::steady_clock::now();auto receipt=x.execute(commands[i]);auto end=std::chrono::steady_clock::now();
      if(!receipt.result)throw std::runtime_error("rejected benchmark order");
      submit[i]=std::chrono::duration_cast<std::chrono::nanoseconds>(end-starts[i]).count();
      while(watermark<receipt.durable_sequence){acks.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(end-starts[watermark]).count());++watermark;}
    }
    seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-begin).count();
    const auto durable=x.flush();auto end=std::chrono::steady_clock::now();
    while(watermark<durable){acks.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(end-starts[watermark]).count());++watermark;}
    durable_seconds=std::chrono::duration<double>(end-begin).count();
    state=x.canonical_state();hash=x.hash();
  }
  if(policy!=Durability::NoWal){auto start=std::chrono::steady_clock::now();Exchange x(path,config);recovery=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();if(state!=x.canonical_state())throw std::runtime_error("recovery mismatch");}
  std::sort(submit.begin(),submit.end());std::sort(acks.begin(),acks.end());
  auto p99=[](const std::vector<uint64_t>& v){return v[static_cast<size_t>(std::ceil(.99*v.size()))-1];};
  std::cout<<std::setprecision(12)<<"{\"mode\":\""<<mode<<"\",\"orders\":"<<n<<",\"throughput\":"<<n/seconds
    <<",\"durable_throughput\":"<<(acks.empty()?0:n/durable_seconds)<<",\"submit_p99_ns\":"<<p99(submit)
    <<",\"durable_ack_p99_ns\":"<<(acks.empty()?0:p99(acks))<<",\"durable_acks\":"<<acks.size()
    <<",\"recovery_seconds\":"<<recovery<<",\"state_hash\":\""<<hash<<"\"}\n";
  unlink(path.c_str());rmdir(dir);
 }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}

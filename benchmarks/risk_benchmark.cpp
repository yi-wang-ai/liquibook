#include "../extensions/risk/risk_gateway.h"
#include <sys/resource.h>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <random>
#include <vector>
using namespace exchange;

// Same DTO, ownership map, and order lifecycle callbacks as the risk gateway.
// This path calls unchanged Liquibook with no risk validation or reservations.
class BareGateway : private liquibook::book::OrderListener<OrderRecord*> {
  std::unordered_map<OrderId,std::unique_ptr<OrderRecord>> orders_;
  liquibook::book::OrderBook<OrderRecord*> book_;
  Position positions_[3] = {};
  void on_accept(OrderRecord* const&) override {}
  void on_reject(OrderRecord* const&,const char*) override { throw std::logic_error("bare reject"); }
  void on_fill(OrderRecord* const& a, OrderRecord* const& b, Quantity q,
               liquibook::book::Price) override {
    a->apply_fill(q); b->apply_fill(q);
    positions_[a->account] += a->buy ? static_cast<Position>(q) : -static_cast<Position>(q);
    positions_[b->account] += b->buy ? static_cast<Position>(q) : -static_cast<Position>(q);
  }
  void on_cancel(OrderRecord* const&) override { throw std::logic_error("unexpected cancel"); }
  void on_cancel_reject(OrderRecord* const&,const char*) override {}
  void on_replace(OrderRecord* const&,const int64_t&,liquibook::book::Price) override {}
  void on_replace_reject(OrderRecord* const&,const char*) override {}
public:
  BareGateway() { book_.set_order_listener(this); }
  RiskResult submit(const OrderRequest& request, Timestamp) {
    std::unique_ptr<OrderRecord> order(new OrderRecord(request));
    auto* ptr=order.get();
    orders_.emplace(request.id,std::move(order));
    book_.add(ptr);
    return {RejectReason::None};
  }
  const OrderRecord& order(OrderId id) const { return *orders_.at(id); }
  Position position(AccountId id) const { return positions_[id]; }
};

static Position position(const BareGateway& gateway, AccountId id) { return gateway.position(id); }
static Position position(const RiskGateway& gateway, AccountId id) { return gateway.account(id).position; }
static double cpu_seconds(const rusage& r) {
  return r.ru_utime.tv_sec + r.ru_utime.tv_usec/1e6 + r.ru_stime.tv_sec + r.ru_stime.tv_usec/1e6;
}
static uint64_t mix(uint64_t hash,uint64_t value) {
  return (hash ^ value) * 1099511628211ULL;
}
template<class Gateway> void run(Gateway& gateway,const std::vector<OrderRequest>& orders,
                                 const std::string& mode,const std::string& workload,
                                 uint64_t seed,uint64_t workload_hash) {
  std::vector<uint64_t> latency;
  latency.reserve(orders.size());
  rusage before{},after{};
  getrusage(RUSAGE_SELF,&before);
  const auto start=std::chrono::steady_clock::now();
  for(size_t i=0;i<orders.size();++i) {
    const auto tick=std::chrono::steady_clock::now();
    if(!gateway.submit(orders[i],i*10000ULL)) throw std::logic_error("benchmark order rejected");
    const auto done=std::chrono::steady_clock::now();
    latency.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(done-tick).count());
  }
  const double seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
  getrusage(RUSAGE_SELF,&after);
  uint64_t digest=1469598103934665603ULL;
  for(const auto& request:orders) {
    const auto& order=gateway.order(request.id);
    digest=mix(digest,order.id);
    digest=mix(digest,order.remaining);
    digest=mix(digest,order.filled);
    digest=mix(digest,order.limit);
    digest=mix(digest,static_cast<uint64_t>(order.state));
  }
  digest=mix(digest,static_cast<uint64_t>(position(gateway,1)));
  digest=mix(digest,static_cast<uint64_t>(position(gateway,2)));
  std::sort(latency.begin(),latency.end());
  auto percentile=[&](double fraction) { return latency[static_cast<size_t>(std::ceil(fraction*latency.size()))-1]; };
  const double cpu=cpu_seconds(after)-cpu_seconds(before);
  std::cout << std::setprecision(12)
    << "{\"mode\":\"" << mode << "\",\"workload\":\"" << workload
    << "\",\"orders\":" << orders.size() << ",\"seed\":" << seed
    << ",\"workload_hash\":\"" << workload_hash << "\",\"result_hash\":\"" << digest
    << "\",\"wall_seconds\":" << seconds << ",\"throughput_orders_s\":" << orders.size()/seconds
    << ",\"p50_ns\":" << percentile(.50) << ",\"p95_ns\":" << percentile(.95)
    << ",\"p99_ns\":" << percentile(.99) << ",\"p99_9_ns\":" << percentile(.999)
    << ",\"cpu_seconds\":" << cpu << ",\"cpu_percent\":" << 100*cpu/seconds
    << ",\"peak_rss_kib\":" << after.ru_maxrss << "}\n";
}
int main(int argc,char** argv) {
  try {
    if(argc!=5) throw std::invalid_argument("MODE WORKLOAD COUNT SEED required");
    const std::string mode=argv[1], workload=argv[2];
    const uint64_t count=std::stoull(argv[3]), seed=std::stoull(argv[4]);
    if((mode!="bare" && mode!="risk") || (workload!="crossing" && workload!="resting") ||
       count<2 || count>1000000 || count%2) throw std::invalid_argument("invalid benchmark arguments");
    std::mt19937_64 random(seed);
    std::vector<OrderRequest> requests;
    requests.reserve(count);
    uint64_t hash=1469598103934665603ULL;
    for(uint64_t i=0;i<count;i+=2) {
      const int64_t qty=1+random()%10, price=10000+random()%100;
      const AccountId seller=1+(i/2)%2, buyer=3-seller;
      requests.push_back({i,seller,false,qty,workload=="crossing"?price:10100});
      requests.push_back({i+1,buyer,true,qty,workload=="crossing"?price:9900});
    }
    for(const auto& r:requests) {
      hash=mix(hash,r.id); hash=mix(hash,r.account); hash=mix(hash,r.buy);
      hash=mix(hash,r.quantity); hash=mix(hash,r.price);
    }
    if(mode=="bare") { BareGateway gateway; run(gateway,requests,mode,workload,seed,hash); }
    else {
      RiskGateway gateway;
      RiskLimits limits;
      limits.max_order_qty=100; limits.max_order_notional=2000000;
      limits.max_long_position=1000000000; limits.max_short_position=1000000000;
      limits.min_price=1; limits.max_price=20000; limits.max_orders_per_second=1000000;
      gateway.add_account(1,limits); gateway.add_account(2,limits);
      run(gateway,requests,mode,workload,seed,hash);
    }
  } catch(const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}

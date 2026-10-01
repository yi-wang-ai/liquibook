#include "../test/fix/harness.h"
#include "../extensions/audit/audit.h"
#include <cmath>
#include <iostream>

namespace au=exchange::audit;
static void reset_database(const char* conninfo){
  PGconn* db=PQconnectdb(conninfo);if(PQstatus(db)!=CONNECTION_OK)throw std::runtime_error(PQerrorMessage(db));
  PGresult* result=PQexec(db,"DROP TABLE IF EXISTS order_events,trades,risk_events,orders CASCADE");
  if(PQresultStatus(result)!=PGRES_COMMAND_OK)throw std::runtime_error(PQresultErrorMessage(result));PQclear(result);PQfinish(db);
}
int main(int argc,char** argv){
  uint64_t count=argc>1?std::stoull(argv[1]):5000;const char* conninfo=argc>2?argv[2]:"dbname=audit host=/var/run/postgresql connect_timeout=1";
  try{
    reset_database(conninfo);auto config=fix_settings();for(auto& symbol:config.risk)for(auto& account:symbol.accounts){account.limits.max_long_position=10000000;account.limits.max_orders_per_second=10000000;}
    char root[]="/tmp/liquibook-postgres-benchmark-XXXXXX";if(!mkdtemp(root))throw std::runtime_error("mkdtemp");
    au::Store audit(std::string(root)+"/audit",conninfo,fx::encode_settings(config));
    Network network(config,false,[&audit](const fx::Request& q,Timestamp time,uint64_t ordinal,uint64_t before,uint64_t after,const std::vector<fx::Response>& responses){audit.enqueue(au::derive(q,time,ordinal,before,after,responses));});
    std::vector<uint64_t> latency;latency.reserve(count);
    for(int i=0;i<100;++i)network.submit(network.order("W"+std::to_string(i),"1","1"));
    auto start=steady_now();
    for(uint64_t i=0;i<count;++i){auto message=network.order("B"+std::to_string(i),"1","1");auto id=value(message,11);auto tick=steady_now();network.send(message);auto response=network.client.wait(id);latency.push_back(steady_now()-tick);if(value(response,150)!="0")throw std::runtime_error("benchmark rejected");}
    auto request_elapsed=steady_now()-start;network.service->drain();auto drain_start=steady_now();if(!audit.wait_idle(60000))throw std::runtime_error("audit drain timeout");auto total_elapsed=steady_now()-start;auto drain_elapsed=steady_now()-drain_start;auto metrics=audit.metrics();
    std::sort(latency.begin(),latency.end());auto percentile=[&](double q){return latency[static_cast<size_t>(std::ceil(q*count))-1];};
    std::cout<<std::setprecision(12)<<"{\"requests\":"<<count<<",\"requests_s\":"<<double(count)*1e9/request_elapsed
      <<",\"p50_ns\":"<<percentile(.5)<<",\"p95_ns\":"<<percentile(.95)<<",\"p99_ns\":"<<percentile(.99)<<",\"p99_9_ns\":"<<percentile(.999)
      <<",\"audit_events\":"<<metrics.committed<<",\"audit_events_s\":"<<double(metrics.committed)*1e9/total_elapsed
      <<",\"audit_batches\":"<<metrics.batches<<",\"average_batch\":"<<(metrics.batches?double(metrics.committed)/metrics.batches:0)
      <<",\"max_queue_depth\":"<<metrics.max_queue_depth<<",\"max_commit_us\":"<<metrics.max_commit_us<<",\"drain_ms\":"<<double(drain_elapsed)/1e6<<"}\n";
    return 0;
  }catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}
}

#include "../test/fix/harness.h"
#include "../extensions/audit/audit.h"
#include <iostream>

namespace au=exchange::audit;
static uint64_t row_count(const char* conninfo){
  PGconn* db=PQconnectdb(conninfo);if(PQstatus(db)!=CONNECTION_OK)throw std::runtime_error(PQerrorMessage(db));PGresult* result=PQexec(db,"SELECT count(*) FROM orders");
  if(PQresultStatus(result)!=PGRES_TUPLES_OK)throw std::runtime_error(PQresultErrorMessage(result));auto count=std::stoull(PQgetvalue(result,0,0));PQclear(result);PQfinish(db);return count;
}
int main(int argc,char** argv){
  unsigned seconds=argc>1?std::stoul(argv[1]):10;const char* conninfo=argc>2?argv[2]:"dbname=audit host=/var/run/postgresql connect_timeout=1";
  try{
    PGconn* db=PQconnectdb(conninfo);if(PQstatus(db)!=CONNECTION_OK)throw std::runtime_error(PQerrorMessage(db));PGresult* result=PQexec(db,"DROP TABLE IF EXISTS order_events,trades,risk_events,orders CASCADE");PQclear(result);PQfinish(db);
    auto config=fix_settings();for(auto& symbol:config.risk)for(auto& account:symbol.accounts){account.limits.max_long_position=1000000000;account.limits.max_orders_per_second=1000000000;}
    char root[]="/tmp/liquibook-audit-outage-XXXXXX";if(!mkdtemp(root))throw std::runtime_error("mkdtemp");au::Store audit(std::string(root)+"/audit",conninfo,fx::encode_settings(config));
    Network network(config,false,[&audit](const fx::Request& q,Timestamp time,uint64_t ordinal,uint64_t before,uint64_t after,const std::vector<fx::Response>& responses){audit.enqueue(au::derive(q,time,ordinal,before,after,responses));});
    network.submit(network.order("WARMUP","1","1"));if(!audit.wait_idle(10000))throw std::runtime_error("warmup audit timeout");
    if(std::system("pg_ctlcluster 16 main stop --force >/dev/null 2>&1")!=0)throw std::runtime_error("stop PostgreSQL");
    uint64_t submitted=0;uint64_t started=steady_now();uint64_t deadline=started+uint64_t(seconds)*1000000000ULL;
    while(steady_now()<deadline){auto id="O"+std::to_string(submitted);auto message=network.order(id,"1","1");network.send(message);auto response=network.client.wait(id);if(value(response,150)!="0")throw std::runtime_error("outage request rejected");++submitted;}
    auto engine_elapsed=steady_now()-started;network.service->drain();auto backlog=audit.metrics();
    auto recovery_start=steady_now();if(std::system("pg_ctlcluster 16 main start >/dev/null 2>&1")!=0)throw std::runtime_error("start PostgreSQL");if(!audit.wait_idle(120000))throw std::runtime_error("backlog drain timeout");auto recovery_elapsed=steady_now()-recovery_start;auto final=audit.metrics();auto rows=row_count(conninfo);
    if(rows!=submitted+1)throw std::runtime_error("database row count mismatch");
    std::cout<<std::setprecision(12)<<"{\"outage_s\":"<<double(engine_elapsed)/1e9<<",\"accepted_requests\":"<<submitted<<",\"requests_s\":"<<double(submitted)*1e9/engine_elapsed
      <<",\"engine_sequence\":"<<network.service->sequence()<<",\"backlog_events\":"<<(backlog.memory_depth+backlog.durable_depth)
      <<",\"max_queue_depth\":"<<final.max_queue_depth<<",\"recovery_s\":"<<double(recovery_elapsed)/1e9
      <<",\"recovery_events_s\":"<<(recovery_elapsed?double(final.committed-backlog.committed)*1e9/recovery_elapsed:0)<<",\"orders_verified\":"<<rows<<"}\n";
    return 0;
  }catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}
}

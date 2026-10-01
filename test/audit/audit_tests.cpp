#define BOOST_TEST_MODULE PostgresAuditTests
#include <boost/test/unit_test.hpp>
#include "../../extensions/audit/audit.h"
#include "../../extensions/fix/service.h"
#include <sys/wait.h>
#include <signal.h>
#include <cstdlib>

using namespace exchange;
namespace fx=exchange::fix;
namespace au=exchange::audit;
using exchange::durable::Bytes;

static const char* connection(){auto value=std::getenv("AUDIT_TEST_CONNINFO");return value?value:"dbname=audit host=/var/run/postgresql";}
static void sql(const std::string& statement){
  PGconn* db=PQconnectdb(connection());BOOST_REQUIRE_MESSAGE(PQstatus(db)==CONNECTION_OK,PQerrorMessage(db));
  PGresult* result=PQexec(db,statement.c_str());BOOST_REQUIRE_MESSAGE(PQresultStatus(result)==PGRES_COMMAND_OK,PQresultErrorMessage(result));PQclear(result);PQfinish(db);
}
static std::string scalar(const std::string& statement){
  PGconn* db=PQconnectdb(connection());BOOST_REQUIRE_MESSAGE(PQstatus(db)==CONNECTION_OK,PQerrorMessage(db));
  PGresult* result=PQexec(db,statement.c_str());BOOST_REQUIRE_MESSAGE(PQresultStatus(result)==PGRES_TUPLES_OK,PQresultErrorMessage(result));
  BOOST_REQUIRE_EQUAL(PQntuples(result),1);std::string value=PQgetvalue(result,0,0);PQclear(result);PQfinish(db);return value;
}
static fx::Settings settings(){
  fx::Settings s;RiskLimits limits;limits.max_long_position=100000;limits.max_short_position=100000;limits.max_orders_per_second=100000;
  s.risk={{1,{{1,limits,0},{2,limits,0}}},{2,{{1,limits,0},{2,limits,0}}}};
  s.symbols={{"ESZ6",{1,2}},{"TEST",{2,2}}};s.clients={{"C1",1},{"C2",2}};return s;
}
struct Fixture {
  std::string dir;fx::Settings config=settings();uint64_t clock=1;
  Fixture(){char name[]="/tmp/liquibook-audit-test-XXXXXX";auto p=mkdtemp(name);if(!p)throw std::runtime_error("mkdtemp");dir=p;if(mkdir((dir+"/gateway").c_str(),0700))throw std::runtime_error("mkdir gateway");sql("DROP TABLE IF EXISTS order_events,trades,risk_events,orders CASCADE");}
  ~Fixture(){erase(dir);}
  static void erase(const std::string& path){DIR* d=opendir(path.c_str());if(!d)return;while(auto e=readdir(d)){std::string n=e->d_name;if(n=="."||n=="..")continue;auto p=path+"/"+n;struct stat st{};if(!lstat(p.c_str(),&st)&&S_ISDIR(st.st_mode))erase(p);else unlink(p.c_str());}closedir(d);rmdir(path.c_str());}
  fx::Request order(std::string id="A",std::string side="1",std::string qty="10",std::string price="100.00",std::string client="C1"){
    fx::Request q;q.client=client;q.type="D";q.id=id;q.symbol="ESZ6";q.side=side;q.quantity=qty;q.order_type="2";q.price=price;q.tif="1";q.account=client=="C1"?"1":"2";return q;
  }
  fx::Request cancel(std::string id,std::string original,std::string client="C1"){auto q=order(id,"1","10","100.00",client);q.type="F";q.original=original;q.order_type.clear();q.price.clear();q.tif.clear();return q;}
  fx::Request replace(std::string id,std::string original,std::string qty="20"){auto q=order(id,"1",qty,"101.00");q.type="G";q.original=original;return q;}
  struct Running {
    au::Store audit;fx::Service service;
    Running(Fixture& f):audit(f.dir+"/audit",connection(),fx::encode_settings(f.config)),
      service(f.dir+"/gateway",f.config,[](const fx::Response&){return true;},[&f]{return f.clock++;},
       [this](const fx::Request& q,Timestamp t,uint64_t o,uint64_t b,uint64_t a,const std::vector<fx::Response>& r){audit.enqueue(au::derive(q,t,o,b,a,r));}){}
    void submit(const fx::Request& q){service.enqueue(q);service.drain();}
    void flush(){BOOST_REQUIRE(audit.wait_idle(10000));}
  };
};
BOOST_FIXTURE_TEST_CASE(new_order_categories,Fixture){Running r(*this);r.submit(order());r.flush();BOOST_CHECK_EQUAL(scalar("SELECT count(*) FROM orders"),"1");BOOST_CHECK_EQUAL(scalar("SELECT count(*) FROM order_events"),"1");BOOST_CHECK_EQUAL(scalar("SELECT count(*) FROM risk_events"),"1");BOOST_CHECK_EQUAL(scalar("SELECT count(*) FROM trades"),"0");}
BOOST_FIXTURE_TEST_CASE(full_fill_trade,Fixture){Running r(*this);r.submit(order("S","2","10","100.00","C2"));r.submit(order("B"));r.flush();BOOST_CHECK_EQUAL(scalar("SELECT count(*) FROM trades"),"1");BOOST_CHECK_EQUAL(scalar("SELECT buy_order_id||':'||sell_order_id||':'||price_ticks||':'||quantity FROM trades"),"2:1:10000:10");BOOST_CHECK_EQUAL(scalar("SELECT string_agg(status,',' ORDER BY order_id) FROM orders"),"2,2");}
BOOST_FIXTURE_TEST_CASE(partial_fill_state,Fixture){Running r(*this);r.submit(order("S","2","4","100.00","C2"));r.submit(order("B"));r.flush();BOOST_CHECK_EQUAL(scalar("SELECT status||':'||original_qty FROM orders WHERE order_id=2"),"1:10");BOOST_CHECK_EQUAL(scalar("SELECT leaves_qty||':'||cumulative_qty FROM order_events WHERE order_id=2 AND event_type='F'"),"6:4");}
BOOST_FIXTURE_TEST_CASE(cancel_updates_read_model,Fixture){Running r(*this);r.submit(order());r.submit(cancel("C","A"));r.flush();BOOST_CHECK_EQUAL(scalar("SELECT status||':'||cl_ord_id FROM orders"),"4:C");}
BOOST_FIXTURE_TEST_CASE(replace_updates_read_model,Fixture){Running r(*this);r.submit(order());r.submit(replace("R","A"));r.flush();BOOST_CHECK_EQUAL(scalar("SELECT status||':'||cl_ord_id||':'||original_qty||':'||price_ticks FROM orders"),"0:R:20:10100");}
BOOST_FIXTURE_TEST_CASE(rejection_only_risk_event,Fixture){Running r(*this);auto q=order();q.quantity="0";r.submit(q);r.flush();BOOST_CHECK_EQUAL(scalar("SELECT decision||':'||reject_reason FROM risk_events"),"REJECTED:InvalidQuantity");BOOST_CHECK_EQUAL(scalar("SELECT count(*) FROM orders"),"0");}
BOOST_FIXTURE_TEST_CASE(multiple_symbols_accounts,Fixture){Running r(*this);r.submit(order());auto q=order("B","1","3","90.00","C2");q.symbol="TEST";r.submit(q);r.flush();BOOST_CHECK_EQUAL(scalar("SELECT string_agg(client_id||':'||symbol,',' ORDER BY order_id) FROM orders"),"C1:ESZ6,C2:TEST");}
BOOST_FIXTURE_TEST_CASE(global_event_order_columns,Fixture){Running r(*this);r.submit(order("S","2","10","100.00","C2"));r.submit(order("B"));r.flush();BOOST_CHECK_EQUAL(scalar("SELECT string_agg(ordinal||'.'||event_index,',' ORDER BY ordinal,event_index) FROM order_events"),"1.1,2.1,2.2,2.3");}
BOOST_FIXTURE_TEST_CASE(restart_replay_is_idempotent,Fixture){
  {Running r(*this);r.submit(order());r.flush();}
  {Running r(*this);r.flush();BOOST_CHECK_EQUAL(r.service.sequence(),1);}
  BOOST_CHECK_EQUAL(scalar("SELECT count(*) FROM orders"),"1");BOOST_CHECK_EQUAL(scalar("SELECT count(*) FROM risk_events"),"1");BOOST_CHECK_EQUAL(scalar("SELECT count(*) FROM order_events"),"1");
}
BOOST_FIXTURE_TEST_CASE(independent_duplicate_spool_is_idempotent,Fixture){
  auto records=au::derive(order(),1,1,0,1,{});BOOST_REQUIRE_EQUAL(records.size(),1);
  {au::Store first(dir+"/one",connection(),Bytes{1});first.enqueue(records);BOOST_REQUIRE(first.wait_idle(10000));}
  {au::Store second(dir+"/two",connection(),Bytes{2});second.enqueue(records);BOOST_REQUIRE(second.wait_idle(10000));}
  BOOST_CHECK_EQUAL(scalar("SELECT count(*) FROM risk_events"),"1");
}
BOOST_FIXTURE_TEST_CASE(connection_termination_retries_batch,Fixture){
  Running r(*this);r.submit(order());sql("DO $$ DECLARE connection record; BEGIN FOR connection IN SELECT pid FROM pg_stat_activity WHERE datname=current_database() AND pid<>pg_backend_pid() LOOP PERFORM pg_terminate_backend(connection.pid); END LOOP; END $$");r.submit(order("B"));r.flush();BOOST_CHECK_EQUAL(scalar("SELECT count(*) FROM orders"),"2");BOOST_CHECK(r.audit.metrics().reconnects>=1);
}
BOOST_FIXTURE_TEST_CASE(batch_transaction_rolls_back_and_recovers,Fixture){
  Running r(*this);r.submit(order());r.flush();sql("CREATE OR REPLACE FUNCTION reject_audit() RETURNS trigger LANGUAGE plpgsql AS $$ BEGIN RAISE EXCEPTION 'injected'; END $$; CREATE TRIGGER reject_audit BEFORE INSERT ON risk_events FOR EACH ROW EXECUTE FUNCTION reject_audit()");
  r.submit(order("B"));usleep(300000);BOOST_CHECK_EQUAL(scalar("SELECT count(*) FROM orders"),"1");sql("DROP TRIGGER reject_audit ON risk_events; DROP FUNCTION reject_audit()");r.flush();BOOST_CHECK_EQUAL(scalar("SELECT count(*) FROM orders"),"2");
}
BOOST_FIXTURE_TEST_CASE(database_outage_does_not_stop_engine,Fixture){
  Running r(*this);BOOST_REQUIRE(std::system("pg_ctlcluster 16 main stop --force >/dev/null 2>&1")==0);for(int i=0;i<20;++i)r.submit(order("O"+std::to_string(i),"1","1"));BOOST_CHECK_EQUAL(r.service.sequence(),20);usleep(300000);BOOST_CHECK(r.audit.metrics().durable_depth>0||r.audit.metrics().memory_depth>0);
  BOOST_REQUIRE(std::system("pg_ctlcluster 16 main start >/dev/null 2>&1")==0);r.flush();BOOST_CHECK_EQUAL(scalar("SELECT count(*) FROM orders"),"20");
}
BOOST_FIXTURE_TEST_CASE(backlog_order_preserved,Fixture){Running r(*this);for(int i=0;i<250;++i)r.submit(order("O"+std::to_string(i),"1","1"));r.flush();BOOST_CHECK_EQUAL(scalar("SELECT min(created_ordinal)||':'||max(created_ordinal)||':'||count(*) FROM orders"),"1:250:250");}
BOOST_FIXTURE_TEST_CASE(metrics_report_batches_and_depth,Fixture){Running r(*this);for(int i=0;i<25;++i)r.submit(order("O"+std::to_string(i),"1","1"));r.flush();auto m=r.audit.metrics();BOOST_CHECK(m.enqueued>=75);BOOST_CHECK_EQUAL(m.enqueued,m.committed);BOOST_CHECK(m.batches>0);BOOST_CHECK(m.max_queue_depth>0);BOOST_CHECK_EQUAL(m.dropped,0);}
BOOST_FIXTURE_TEST_CASE(exclusive_spool_writer,Fixture){au::Store first(dir+"/audit",connection(),Bytes{1});BOOST_CHECK_THROW(au::Store second(dir+"/audit",connection(),Bytes{1}),std::runtime_error);}
BOOST_FIXTURE_TEST_CASE(binding_mismatch_fails,Fixture){{au::Store first(dir+"/audit",connection(),Bytes{1});}BOOST_CHECK_THROW(au::Store second(dir+"/audit",connection(),Bytes{2}),std::runtime_error);}
BOOST_FIXTURE_TEST_CASE(truncated_spool_tail_is_discarded,Fixture){
  {au::Store store(dir+"/audit",connection(),Bytes{1});}
  int fd=open((dir+"/audit/audit.spool").c_str(),O_WRONLY|O_APPEND);BOOST_REQUIRE(fd>=0);uint8_t bytes[3]={1,2,3};BOOST_REQUIRE(write(fd,bytes,3)==3);close(fd);
  au::Store recovered(dir+"/audit",connection(),Bytes{1});BOOST_REQUIRE(recovered.wait_idle(10000));
}
BOOST_FIXTURE_TEST_CASE(corrupt_spool_fails_closed,Fixture){
  {au::Store store(dir+"/audit",connection(),Bytes{1});}
  auto path=dir+"/audit/audit.spool";int fd=open(path.c_str(),O_RDWR);BOOST_REQUIRE(fd>=0);auto bytes=durable::read_file(fd);bytes.back()^=1;BOOST_REQUIRE(lseek(fd,0,SEEK_SET)==0);durable::write_all(fd,bytes.data(),bytes.size());close(fd);
  BOOST_CHECK_THROW(au::Store recovered(dir+"/audit",connection(),Bytes{1}),std::runtime_error);
}
BOOST_FIXTURE_TEST_CASE(process_crash_recovers_from_adapter_history,Fixture){
  auto child=fork();BOOST_REQUIRE(child>=0);if(child==0){try{Running r(*this);r.submit(order());kill(getpid(),SIGKILL);}catch(...){_exit(9);}}
  int status=0;BOOST_REQUIRE(waitpid(child,&status,0)==child);BOOST_REQUIRE(WIFSIGNALED(status)&&WTERMSIG(status)==SIGKILL);
  Running recovered(*this);recovered.flush();BOOST_CHECK_EQUAL(recovered.service.sequence(),1);BOOST_CHECK_EQUAL(scalar("SELECT count(*) FROM orders"),"1");
}
BOOST_FIXTURE_TEST_CASE(offlined_destructor_returns_with_durable_spool,Fixture){
  BOOST_REQUIRE(std::system("pg_ctlcluster 16 main stop --force >/dev/null 2>&1")==0);auto start=steady_now();{au::Store store(dir+"/audit",connection(),Bytes{1});au::Record record;record.event_id="1:risk:0";record.category=au::Category::Risk;store.enqueue({record});usleep(200000);}BOOST_CHECK(steady_now()-start<3000000000ULL);BOOST_REQUIRE(std::system("pg_ctlcluster 16 main start >/dev/null 2>&1")==0);
  au::Store recovered(dir+"/audit",connection(),Bytes{1});BOOST_REQUIRE(recovered.wait_idle(10000));BOOST_CHECK_EQUAL(scalar("SELECT count(*) FROM risk_events"),"1");
}

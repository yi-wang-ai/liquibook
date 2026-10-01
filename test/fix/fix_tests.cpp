#define BOOST_TEST_MODULE FixGatewayTests
#include <boost/test/unit_test.hpp>
#include "harness.h"
#include <sys/wait.h>
#include <signal.h>
BOOST_FIXTURE_TEST_CASE(new_accepted,Network){auto r=submit(order("A"));BOOST_CHECK_EQUAL(value(r,150),"0");BOOST_CHECK_EQUAL(value(r,151),"10");}
BOOST_FIXTURE_TEST_CASE(full_fill,Network){
 submit(order("A"));submit(order("B","2","10","100.00","2"),"C2");auto a=client.wait("A","F");auto b=client.wait("B","F","C2");
 BOOST_CHECK_EQUAL(value(a,39),"2");BOOST_CHECK_EQUAL(value(b,31),"100.00");BOOST_CHECK_EQUAL(value(a,6),"100.00000000");
}
BOOST_FIXTURE_TEST_CASE(partial_fill,Network){
 submit(order("A"));submit(order("B","2","4","100.00","2"),"C2");auto a=client.wait("A","F");BOOST_CHECK_EQUAL(value(a,39),"1");BOOST_CHECK_EQUAL(value(a,151),"6");
}
BOOST_FIXTURE_TEST_CASE(cancel_success,Network){submit(order("A"));auto r=submit(cancel("C","A"));BOOST_CHECK_EQUAL(value(r,150),"4");BOOST_CHECK_EQUAL(value(r,151),"0");}
BOOST_FIXTURE_TEST_CASE(cancel_unknown,Network){auto r=submit(cancel("C","NONE"));BOOST_CHECK_EQUAL(r.getHeader().getField(35),"9");BOOST_CHECK_EQUAL(value(r,102),"1");}
BOOST_FIXTURE_TEST_CASE(replace_success,Network){submit(order("A"));auto r=submit(replace("R","A"));BOOST_CHECK_EQUAL(value(r,150),"5");BOOST_CHECK_EQUAL(value(r,38),"20");}
BOOST_FIXTURE_TEST_CASE(replace_invalid_quantity,Network){submit(order("A"));auto r=submit(replace("R","A","0"));BOOST_CHECK_EQUAL(r.getHeader().getField(35),"9");BOOST_CHECK_EQUAL(service->sequence(),1);}
BOOST_FIXTURE_TEST_CASE(duplicate_clordid,Network){
 submit(order("A"));send(order("A"));service->drain();
 // Wait for the distinct rejection, not the earlier acceptance.
 auto r=client.wait("A","8");BOOST_CHECK_EQUAL(value(r,58),"DuplicateClOrdID");BOOST_CHECK_EQUAL(service->sequence(),1);
}
BOOST_FIXTURE_TEST_CASE(invalid_symbol,Network){auto m=order("A");m.setField(55,"UNKNOWN");auto r=submit(m);BOOST_CHECK_EQUAL(value(r,58),"UnknownSymbol");}
BOOST_FIXTURE_TEST_CASE(negative_price,Network){auto r=submit(order("A","1","10","-1"));BOOST_CHECK_EQUAL(value(r,150),"8");}
BOOST_FIXTURE_TEST_CASE(off_tick_price,Network){auto r=submit(order("A","1","10","100.001"));BOOST_CHECK_EQUAL(value(r,150),"8");}
BOOST_FIXTURE_TEST_CASE(zero_quantity,Network){auto r=submit(order("A","1","0"));BOOST_CHECK_EQUAL(value(r,58),"InvalidQuantity");}
BOOST_FIXTURE_TEST_CASE(fractional_quantity,Network){auto r=submit(order("A","1","1.5"));BOOST_CHECK_EQUAL(value(r,150),"8");}
BOOST_FIXTURE_TEST_CASE(quantity_limit,Network){auto r=submit(order("A","1","1001"));BOOST_CHECK_EQUAL(value(r,58),"OrderQuantityLimit");}
BOOST_FIXTURE_TEST_CASE(notional_limit,Network){auto r=submit(order("A","1","1000","1000.00"));BOOST_CHECK_EQUAL(value(r,58),"OrderNotionalLimit");}
BOOST_FIXTURE_TEST_CASE(position_limit,Network){
 submit(order("A","1","1000"));submit(order("B","1","1000"));auto r=submit(order("C","1","1"));BOOST_CHECK_EQUAL(value(r,58),"PositionLimit");
}
BOOST_AUTO_TEST_CASE(rate_limit){
 auto s=fix_settings();s.risk[0].accounts[0].limits.max_orders_per_second=1;Network n(s);n.submit(n.order("A"));auto r=n.submit(n.order("B"));BOOST_CHECK_EQUAL(value(r,58),"RateLimit");
}
BOOST_FIXTURE_TEST_CASE(ioc_explicitly_rejected,Network){auto m=order("A");m.setField(59,"3");auto r=submit(m);BOOST_CHECK_EQUAL(value(r,58),"OnlyGTCLimitSupported");BOOST_CHECK_EQUAL(service->sequence(),0);}
BOOST_FIXTURE_TEST_CASE(market_order_rejected,Network){auto m=order("A");m.setField(40,"1");auto r=submit(m);BOOST_CHECK_EQUAL(value(r,150),"8");}
BOOST_FIXTURE_TEST_CASE(unauthorized_account,Network){auto r=submit(order("A","1","10","100.00","2"));BOOST_CHECK_EQUAL(value(r,58),"UnauthorizedAccount");}
BOOST_FIXTURE_TEST_CASE(two_clients_same_clordid,Network){
 submit(order("A"));auto r=submit(order("A","2","10","101.00","2"),"C2");BOOST_CHECK_EQUAL(value(r,150),"0");BOOST_CHECK_EQUAL(service->sequence(),2);
}
BOOST_FIXTURE_TEST_CASE(cross_account_cancel,Network){submit(order("A"));auto m=cancel("C","A");m.setField(1,"2");auto r=submit(m,"C2");BOOST_CHECK_EQUAL(value(r,58),"UnknownOrder");}
BOOST_FIXTURE_TEST_CASE(stale_origclordid,Network){submit(order("A"));submit(replace("R","A"));auto r=submit(cancel("C","A"));BOOST_CHECK_EQUAL(value(r,58),"OriginalOrderMismatch");}
BOOST_FIXTURE_TEST_CASE(replace_chain_cancel,Network){submit(order("A"));submit(replace("R","A"));auto m=cancel("C","R");m.setField(38,"20");auto r=submit(m);BOOST_CHECK_EQUAL(value(r,150),"4");}
BOOST_FIXTURE_TEST_CASE(partial_fill_replace_cancel,Network){
 submit(order("A"));submit(order("B","2","4","100.00","2"),"C2");client.wait("A","F");
 auto r=submit(replace("R","A","12","99.00"));BOOST_CHECK_EQUAL(value(r,151),"8");
 auto m=cancel("C","R");m.setField(38,"12");r=submit(m);BOOST_CHECK_EQUAL(value(r,14),"4");BOOST_CHECK_EQUAL(value(r,6),"100.00000000");
}
BOOST_FIXTURE_TEST_CASE(replace_crosses_book,Network){
 submit(order("A","1","10","99.00"));submit(order("B","2","10","100.00","2"),"C2");submit(replace("R","A","10","100.00"));
 auto r=client.wait("R","F");BOOST_CHECK_EQUAL(value(r,39),"2");
}
BOOST_FIXTURE_TEST_CASE(disconnect_reconnect,Network){
 submit(order("A"));auto before=session()->getExpectedSenderNum();reconnect();auto r=submit(cancel("C","A"));BOOST_CHECK_EQUAL(value(r,150),"4");BOOST_CHECK(session()->getExpectedSenderNum()>before);
}
BOOST_FIXTURE_TEST_CASE(sequence_gap_recovery,Network){
 submit(order("A"));session()->setNextSenderMsgSeqNum(session()->getExpectedSenderNum()+1);
 auto r=submit(order("B"));BOOST_CHECK_EQUAL(value(r,150),"0");BOOST_CHECK_EQUAL(service->sequence(),2);
 bool resend=false;{std::lock_guard<std::mutex> lock(client.mutex);for(const auto& e:client.admin)if(e.message.getHeader().getField(35)=="2")resend=true;}
 BOOST_CHECK(resend);
}
BOOST_FIXTURE_TEST_CASE(direct_command_equivalence,Network){
 submit(order("A"));submit(order("B","2","4","100.00","2"),"C2");submit(replace("R","A","12","99.00"));
 auto m=cancel("C","R");m.setField(38,"12");submit(m);
 durable::Exchange direct("",config.risk,durable::Durability::NoWal);
 durable::Command c;c.order=1;c.account=1;c.quantity=10;c.price=10000;c.time=1000000000;BOOST_REQUIRE(direct.execute(c).result);
 c.order=2;c.account=2;c.buy=false;c.quantity=4;BOOST_REQUIRE(direct.execute(c).result);
 c.order=1;c.account=1;c.buy=true;c.quantity=12;c.price=9900;c.type=durable::CommandType::ReplaceOrder;BOOST_REQUIRE(direct.execute(c).result);
 c.type=durable::CommandType::CancelOrder;BOOST_REQUIRE(direct.execute(c).result);BOOST_CHECK_EQUAL(service->state(),direct.canonical_state());
}
BOOST_FIXTURE_TEST_CASE(rejected_order_unchanged,Network){submit(order("A"));auto before=service->state();submit(order("B","1","0"));BOOST_CHECK_EQUAL(before,service->state());}
BOOST_FIXTURE_TEST_CASE(multiple_symbols,Network){submit(order("A"));auto m=order("B");m.setField(55,"TEST");submit(m);BOOST_CHECK_EQUAL(service->sequence(),2);}
BOOST_FIXTURE_TEST_CASE(invalid_clordid,Network){auto r=submit(order(std::string(65,'A')));BOOST_CHECK_EQUAL(value(r,58),"InvalidClOrdID");}
struct ProtocolDisk {
 std::string dir;fx::Settings settings=fix_settings();
 ProtocolDisk(){char n[]="/tmp/liquibook-fix-protocol-XXXXXX";auto p=mkdtemp(n);if(!p)throw std::runtime_error("mkdtemp");dir=p;}
 ~ProtocolDisk(){erase_owned_tree(dir);}
 fx::Request request(std::string id="A"){
  fx::Request q;q.client="C1";q.type="D";q.id=id;q.symbol="ESZ6";q.side="1";q.quantity="10";q.order_type="2";q.price="100.00";q.tif="1";q.account="1";return q;
 }
};
BOOST_FIXTURE_TEST_CASE(adapter_restart_and_pending_outbox,ProtocolDisk){
 std::string before;
 {fx::Service s(dir,settings,[](const fx::Response&){return false;},[]{return 1;});
  s.enqueue(request());auto r=request("R");r.type="G";r.original="A";r.quantity="12";s.enqueue(r);s.drain();before=s.state();}
 std::vector<fx::Response> sent;
 fx::Service recovered(dir,settings,[&](const fx::Response& r){sent.push_back(r);return true;},[]{return 1;});
 BOOST_CHECK_EQUAL(recovered.state(),before);recovered.retry_delivery();recovered.drain();BOOST_REQUIRE_EQUAL(sent.size(),2);
 auto q=request("C");q.type="F";q.original="R";q.quantity="12";recovered.enqueue(q);recovered.drain();
 BOOST_CHECK_EQUAL(value(sent.back().message,150),"4");BOOST_CHECK_EQUAL(recovered.sequence(),3);
}
BOOST_FIXTURE_TEST_CASE(possdup_is_idempotent,ProtocolDisk){
 std::vector<fx::Response> sent;fx::Service s(dir,settings,[&](const fx::Response& r){sent.push_back(r);return true;},[]{return 1;});
 auto q=request();s.enqueue(q);s.drain();q.duplicate=true;s.enqueue(q);s.drain();
 BOOST_REQUIRE_EQUAL(sent.size(),2);BOOST_CHECK_EQUAL(value(sent[0].message,17),value(sent[1].message,17));BOOST_CHECK_EQUAL(s.sequence(),1);
}
BOOST_FIXTURE_TEST_CASE(adapter_crash_reconciliation,ProtocolDisk){
 for(const std::string stage:{"after_adapter_intent","after_business_commit","after_report_send"}){
  ProtocolDisk disk;auto child=fork();BOOST_REQUIRE(child>=0);
  if(child==0){try{fx::Service s(disk.dir,disk.settings,[](const fx::Response&){return true;},[]{return 1;});
   s.set_hook([&](const char* name){if(stage==name)kill(getpid(),SIGKILL);});s.enqueue(disk.request());s.drain();_exit(7);}catch(...){_exit(9);}}
  int status=0;BOOST_REQUIRE(waitpid(child,&status,0)==child);BOOST_REQUIRE(WIFSIGNALED(status)&&WTERMSIG(status)==SIGKILL);
  std::vector<fx::Response> reports;fx::Service recovered(disk.dir,disk.settings,[&](const fx::Response& r){reports.push_back(r);return true;},[]{return 1;});
  recovered.retry_delivery();recovered.drain();BOOST_CHECK_EQUAL(recovered.sequence(),1);BOOST_REQUIRE_EQUAL(reports.size(),1);BOOST_CHECK_EQUAL(value(reports[0].message,17),"1-1");
 }
}
BOOST_FIXTURE_TEST_CASE(concurrent_clients_single_execution_path,Network){
 auto a=[&]{for(int i=0;i<20;++i)send(order("A"+std::to_string(i),"1","1"));};
 auto b=[&]{for(int i=0;i<20;++i)send(order("B"+std::to_string(i),"1","1","99.00","2"),"C2");};
 std::thread first(a),second(b);first.join();second.join();client.wait("A19");client.wait("B19","","C2");service->drain();
 BOOST_CHECK_EQUAL(service->sequence(),40);
}
BOOST_FIXTURE_TEST_CASE(rejected_replace_preserves_order_status,Network){
 submit(order("A"));auto r=submit(replace("R","A","1.5"));BOOST_CHECK_EQUAL(value(r,39),"0");BOOST_CHECK_EQUAL(service->sequence(),1);
}
BOOST_FIXTURE_TEST_CASE(weighted_average_multiple_fills,Network){
 submit(order("S1","2","4","99.00","2"),"C2");submit(order("S2","2","6","100.00","2"),"C2");
 submit(order("B","1","10","101.00"));auto report=client.wait("B","F","C1","10");
 BOOST_CHECK_EQUAL(value(report,6),"99.60000000");BOOST_CHECK_EQUAL(value(report,31),"100.00");BOOST_CHECK_EQUAL(value(report,39),"2");
}
BOOST_FIXTURE_TEST_CASE(changed_possdup_payload_rejected,ProtocolDisk){
 std::vector<fx::Response> sent;fx::Service s(dir,settings,[&](const fx::Response& r){sent.push_back(r);return true;},[]{return 1;});
 auto q=request();s.enqueue(q);s.drain();q.duplicate=true;q.quantity="20";s.enqueue(q);s.drain();
 BOOST_CHECK_EQUAL(s.sequence(),1);BOOST_CHECK_EQUAL(value(sent.back().message,58),"DuplicateClOrdID");
}
BOOST_FIXTURE_TEST_CASE(adapter_journal_bound_to_business_stream,ProtocolDisk){
 ProtocolDisk other;
 {fx::Service a(dir,settings,[](const fx::Response&){return true;},[]{return 1;});a.enqueue(request("A"));a.drain();}
 {fx::Service b(other.dir,other.settings,[](const fx::Response&){return true;},[]{return 1;});b.enqueue(other.request("B"));b.drain();}
 int from=open((dir+"/adapter.journal").c_str(),O_RDONLY);BOOST_REQUIRE(from>=0);auto bytes=durable::read_file(from);close(from);
 int to=open((other.dir+"/adapter.journal").c_str(),O_WRONLY|O_TRUNC);BOOST_REQUIRE(to>=0);durable::write_all(to,bytes.data(),bytes.size());close(to);
 BOOST_CHECK_THROW(fx::Service wrong(other.dir,other.settings,[](const fx::Response&){return true;}),std::runtime_error);
}

#define BOOST_TEST_MODULE WalRecoveryTests
#include <boost/test/unit_test.hpp>
#include "../../extensions/durability/wal.h"
#include <fstream>
#include <sys/wait.h>
#include <signal.h>
using namespace exchange;
using namespace exchange::durable;
struct Fixture {
  std::string dir,path;Config config;
  Fixture(){
    char name[]="/tmp/liquibook-wal-XXXXXX";auto p=mkdtemp(name);if(!p)throw std::runtime_error("mkdtemp");
    dir=p;path=dir+"/wal-000001.log";RiskLimits limits;
    limits.max_long_position=100000;limits.max_short_position=100000;limits.max_orders_per_second=100000;
    config={{1,{{1,limits,0},{2,limits,0}}},{2,{{1,limits,0},{2,limits,0}}}};
  }
  ~Fixture(){unlink(path.c_str());unlink((dir+"/snapshot.bin").c_str());unlink((dir+"/snapshot.bin.tmp").c_str());rmdir(dir.c_str());}
  Command order(uint64_t id,bool buy=true,int64_t qty=100,int64_t price=100,uint64_t account=1,uint32_t symbol=1){
    Command c;c.order=id;c.buy=buy;c.quantity=qty;c.price=price;c.account=account;c.symbol=symbol;return c;
  }
  Command cancel(uint64_t id){auto c=order(id);c.type=CommandType::CancelOrder;return c;}
  Command replace(uint64_t id,int64_t qty,int64_t price=100){auto c=order(id,true,qty,price);c.type=CommandType::ReplaceOrder;return c;}
  void replay(const std::vector<Command>& commands){
    std::string state;uint64_t hash;
    {Exchange x(path,config);for(const auto& c:commands)BOOST_REQUIRE(x.execute(c).acknowledged);state=x.canonical_state();hash=x.hash();}
    Exchange recovered(path,config);
    BOOST_CHECK_EQUAL(recovered.canonical_state(),state);BOOST_CHECK_EQUAL(recovered.hash(),hash);
    BOOST_CHECK_EQUAL(recovered.sequence(),commands.size());BOOST_CHECK_EQUAL(recovered.durable_sequence(),commands.size());
  }
  Bytes bytes(){int fd=open(path.c_str(),O_RDONLY);auto b=read_file(fd);close(fd);return b;}
  void write(const Bytes& b){int fd=open(path.c_str(),O_WRONLY|O_TRUNC);write_all(fd,b.data(),b.size());close(fd);}
  void seed(){Exchange x(path,config);BOOST_REQUIRE(x.execute(order(1)).acknowledged);}
};
BOOST_FIXTURE_TEST_CASE(empty_log,Fixture){replay({});}
BOOST_FIXTURE_TEST_CASE(single_order,Fixture){replay({order(1)});}
BOOST_FIXTURE_TEST_CASE(multiple_orders,Fixture){replay({order(1),order(2),order(3,false,50,110,2)});}
BOOST_FIXTURE_TEST_CASE(partial_fills,Fixture){replay({order(1),order(2,false,40,100,2)});}
BOOST_FIXTURE_TEST_CASE(cancel_after_partial_fill,Fixture){replay({order(1),order(2,false,40,100,2),cancel(1)});}
BOOST_FIXTURE_TEST_CASE(replace_after_partial_fill,Fixture){replay({order(1),order(2,false,40,100,2),replace(1,150),order(3,false,110,100,2)});}
BOOST_FIXTURE_TEST_CASE(replace_triggers_fill,Fixture){replay({order(1,true,100,90),order(2,false,40,100,2),replace(1,100,100)});}
BOOST_FIXTURE_TEST_CASE(reservation_replay,Fixture){
  replay({order(1),order(2,true,200,90),cancel(1)});
  Exchange x(path,config);BOOST_CHECK_EQUAL(x.book(1).account(1).reserved_buy,200);
}
BOOST_FIXTURE_TEST_CASE(position_replay,Fixture){
  replay({order(1),order(2,false,40,100,2)});
  Exchange x(path,config);BOOST_CHECK_EQUAL(x.book(1).account(1).position,40);BOOST_CHECK_EQUAL(x.book(1).account(2).position,-40);
}
BOOST_FIXTURE_TEST_CASE(multiple_symbols_global_sequence,Fixture){
  replay({order(1),order(1,true,100,100,1,2),order(2,false,40,100,2,2)});
}
BOOST_FIXTURE_TEST_CASE(terminal_duplicate_id_replay,Fixture){
  replay({order(1),cancel(1)});Exchange x(path,config);auto before=x.canonical_state();
  BOOST_CHECK(x.execute(order(1)).result.reason==RejectReason::DuplicateOrderId);BOOST_CHECK_EQUAL(before,x.canonical_state());
}
BOOST_FIXTURE_TEST_CASE(rate_window_replay,Fixture){
  for(auto& symbol:config)for(auto& account:symbol.accounts)account.limits.max_orders_per_second=1;
  replay({order(1)});Exchange x(path,config);
  BOOST_CHECK(x.execute(order(2)).result.reason==RejectReason::RateLimit);
  auto c=order(2);c.time=1000000000;BOOST_CHECK(x.execute(c).acknowledged);
}
BOOST_FIXTURE_TEST_CASE(rejected_commands_not_logged,Fixture){
  Exchange x(path,config);auto before=bytes();auto c=order(1);c.quantity=0;
  BOOST_CHECK(!x.execute(c).result);BOOST_CHECK_EQUAL(x.sequence(),0);BOOST_CHECK(bytes()==before);
}
BOOST_FIXTURE_TEST_CASE(checksum_corruption_fails_closed,Fixture){
  seed();auto b=bytes();b[b.size()-5]^=1;write(b);BOOST_CHECK_THROW(Exchange x(path,config),std::runtime_error);
}
BOOST_FIXTURE_TEST_CASE(truncated_final_payload,Fixture){
  seed();auto prefix=bytes();auto extra=frame(1,2,encode_command(order(2)));prefix.insert(prefix.end(),extra.begin(),extra.end()-5);write(prefix);
  Exchange x(path,config);BOOST_CHECK_EQUAL(x.sequence(),1);BOOST_CHECK(x.execute(order(2)).acknowledged);
}
BOOST_FIXTURE_TEST_CASE(truncated_final_header,Fixture){
  seed();auto b=bytes();b.push_back(0x4c);b.push_back(0x57);write(b);
  Exchange x(path,config);BOOST_CHECK_EQUAL(x.sequence(),1);BOOST_CHECK_EQUAL(bytes().size(),b.size()-2);
}
BOOST_FIXTURE_TEST_CASE(duplicate_sequence,Fixture){
  seed();auto b=bytes();auto extra=frame(1,1,encode_command(order(2)));b.insert(b.end(),extra.begin(),extra.end());write(b);
  BOOST_CHECK_THROW(Exchange x(path,config),std::runtime_error);
}
BOOST_FIXTURE_TEST_CASE(missing_sequence,Fixture){
  seed();auto b=bytes();auto extra=frame(1,3,encode_command(order(2)));b.insert(b.end(),extra.begin(),extra.end());write(b);
  BOOST_CHECK_THROW(Exchange x(path,config),std::runtime_error);
}
BOOST_FIXTURE_TEST_CASE(unsupported_version,Fixture){
  seed();auto b=bytes();b[4]=2;write(b);BOOST_CHECK_THROW(Exchange x(path,config),std::runtime_error);
}
BOOST_FIXTURE_TEST_CASE(invalid_record_length,Fixture){
  seed();auto b=bytes();auto parsed=scan(b);b[parsed.records[0].end_offset+8]=44;write(b);
  BOOST_CHECK_THROW(Exchange x(path,config),std::runtime_error);
}
BOOST_FIXTURE_TEST_CASE(configuration_mismatch,Fixture){
  seed();config[0].accounts[0].limits.max_order_qty=42;
  BOOST_CHECK_THROW(Exchange x(path,config),std::runtime_error);
}
BOOST_FIXTURE_TEST_CASE(large_wal,Fixture){
  std::string state;
  {Exchange x(path,config,Durability::Buffered);for(int i=0;i<10000;++i)BOOST_REQUIRE(x.execute(order(i,true,1)).result);x.flush();state=x.canonical_state();}
  Exchange recovered(path,config);BOOST_CHECK_EQUAL(recovered.sequence(),10000);BOOST_CHECK_EQUAL(recovered.canonical_state(),state);
}
BOOST_FIXTURE_TEST_CASE(sigkill_at_state_boundaries,Fixture){
  const std::vector<std::string> stages={"after_risk","during_wal_write","after_wal_write","after_sync","during_fill","during_cancel","during_replace","after_match"};
  for(const auto& stage:stages){
    unlink(path.c_str());
    Command target=order(2,false,40,100,2);
    if(stage=="during_cancel")target=cancel(1);
    if(stage=="during_replace")target=replace(1,150);
    pid_t child=fork();BOOST_REQUIRE(child>=0);
    if(child==0){
      try{Exchange x(path,config);x.execute(order(1));
        x.set_hook([&](const char* actual,uint64_t seq){if(stage==actual && seq==2)kill(getpid(),SIGKILL);});
        x.execute(target);_exit(7);
      }catch(...){_exit(9);}
    }
    int status=0;BOOST_REQUIRE(waitpid(child,&status,0)==child);
    BOOST_REQUIRE_MESSAGE(WIFSIGNALED(status)&&WTERMSIG(status)==SIGKILL,stage);
    Exchange expected("",config,Durability::NoWal);expected.execute(order(1));
    if(stage!="after_risk"&&stage!="during_wal_write")expected.execute(target);
    Exchange recovered(path,config);
    BOOST_CHECK_MESSAGE(recovered.canonical_state()==expected.canonical_state(),stage);
  }
}
BOOST_FIXTURE_TEST_CASE(group_acknowledgments,Fixture){
  Exchange x(path,config,Durability::Group,3);
  auto a=x.execute(order(1));auto b=x.execute(order(2));auto c=x.execute(order(3));
  BOOST_CHECK(!a.acknowledged&&!b.acknowledged);BOOST_CHECK_EQUAL(b.durable_sequence,0);
  BOOST_CHECK(c.acknowledged);BOOST_CHECK_EQUAL(c.durable_sequence,3);
  BOOST_CHECK(!x.execute(order(4)).acknowledged);BOOST_CHECK_EQUAL(x.flush(),4);
}
BOOST_FIXTURE_TEST_CASE(buffered_requires_flush,Fixture){
  Exchange x(path,config,Durability::Buffered);
  BOOST_CHECK(!x.execute(order(1)).acknowledged);BOOST_CHECK_EQUAL(x.durable_sequence(),0);BOOST_CHECK_EQUAL(x.flush(),1);
}
BOOST_FIXTURE_TEST_CASE(no_wal_never_claims_durable_ack,Fixture){
  Exchange x(path,config,Durability::NoWal);BOOST_CHECK(!x.execute(order(1)).acknowledged);
  BOOST_CHECK_EQUAL(x.sequence(),1);BOOST_CHECK_EQUAL(x.flush(),0);BOOST_CHECK(access(path.c_str(),F_OK)!=0);
}
BOOST_FIXTURE_TEST_CASE(exclusive_writer,Fixture){
  Exchange first(path,config);BOOST_CHECK_THROW(Exchange second(path,config),std::runtime_error);
}
BOOST_FIXTURE_TEST_CASE(failure_halts_writer_and_never_acknowledges,Fixture){
  {Exchange x(path,config);x.set_hook([](const char* s,uint64_t){if(std::string(s)=="after_wal_write")throw std::runtime_error("injected failure");});
    BOOST_CHECK_THROW(x.execute(order(1)),std::runtime_error);BOOST_CHECK_THROW(x.execute(order(2)),std::runtime_error);}
  Exchange x(path,config);BOOST_CHECK_EQUAL(x.sequence(),1);
}
BOOST_AUTO_TEST_CASE(checksum_known_vector){
  const std::string input="123456789";BOOST_CHECK_EQUAL(crc32(reinterpret_cast<const uint8_t*>(input.data()),input.size()),0xcbf43926U);
}
BOOST_FIXTURE_TEST_CASE(sigkill_after_many_commands,Fixture){
  pid_t child=fork();BOOST_REQUIRE(child>=0);
  if(child==0){
    try{Exchange x(path,config);x.set_hook([](const char* stage,uint64_t seq){if(seq==53&&std::string(stage)=="after_match")kill(getpid(),SIGKILL);});
      for(int i=0;i<100;++i)x.execute(order(i,true,1));_exit(7);
    }catch(...){_exit(9);}
  }
  int status=0;BOOST_REQUIRE(waitpid(child,&status,0)==child);
  BOOST_REQUIRE(WIFSIGNALED(status)&&WTERMSIG(status)==SIGKILL);
  Exchange expected("",config,Durability::NoWal);
  for(int i=0;i<53;++i)expected.execute(order(i,true,1));
  Exchange recovered(path,config);BOOST_CHECK_EQUAL(recovered.canonical_state(),expected.canonical_state());
}

#define BOOST_TEST_MODULE SnapshotTests
#include <boost/test/unit_test.hpp>
#include "../../extensions/durability/wal.h"
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

BOOST_FIXTURE_TEST_CASE(empty_snapshot,Fixture){
 {Exchange x(path,config);x.save_snapshot(dir+"/snapshot.bin");}
 Exchange x(path,config,Durability::EveryCommand,100,dir+"/snapshot.bin");BOOST_CHECK_EQUAL(x.sequence(),0);
}
BOOST_FIXTURE_TEST_CASE(full_state_and_tail,Fixture){
 std::string state;
 {Exchange x(path,config);x.execute(order(90));x.execute(order(2,false,40,100,2));x.execute(order(3,true,20,90));
  x.execute(order(4,false,10,110,2,2));x.save_snapshot(dir+"/snapshot.bin");
  x.execute(replace(90,150));x.execute(cancel(3));state=x.canonical_state();}
 {Exchange x(path,config,Durability::EveryCommand,100,dir+"/snapshot.bin");BOOST_CHECK_EQUAL(x.canonical_state(),state);}
 Exchange x(path,config);BOOST_CHECK_EQUAL(x.canonical_state(),state);
}
BOOST_FIXTURE_TEST_CASE(fifo_after_restore,Fixture){
 std::string state;
 {Exchange x(path,config);x.execute(order(90));x.execute(order(2));x.execute(order(70));
  x.execute(replace(90,110));x.save_snapshot(dir+"/snapshot.bin");}
 {Exchange x(path,config,Durability::EveryCommand,100,dir+"/snapshot.bin");
  BOOST_REQUIRE(x.execute(order(100,false,150,100,2)).result);state=x.canonical_state();}
 Exchange full(path,config);BOOST_CHECK_EQUAL(full.canonical_state(),state);
}
BOOST_FIXTURE_TEST_CASE(terminal_ids_preserved,Fixture){
 {Exchange x(path,config);x.execute(order(1));x.execute(cancel(1));x.save_snapshot(dir+"/snapshot.bin");}
 Exchange x(path,config,Durability::EveryCommand,100,dir+"/snapshot.bin");
 BOOST_CHECK(x.execute(order(1)).result.reason==RejectReason::DuplicateOrderId);
}
BOOST_FIXTURE_TEST_CASE(rate_history_preserved,Fixture){
 config[0].accounts[0].limits.max_orders_per_second=1;
 {Exchange x(path,config);x.execute(order(1));x.save_snapshot(dir+"/snapshot.bin");}
 Exchange x(path,config,Durability::EveryCommand,100,dir+"/snapshot.bin");
 BOOST_CHECK(x.execute(order(2)).result.reason==RejectReason::RateLimit);
 auto c=order(2);c.time=1000000000;BOOST_CHECK(x.execute(c).result);
}
BOOST_FIXTURE_TEST_CASE(snapshot_checksum_corruption,Fixture){
 auto snap=dir+"/snapshot.bin";
 {Exchange x(path,config);x.execute(order(1));x.save_snapshot(snap);}
 int fd=open(snap.c_str(),O_RDWR);auto b=read_file(fd);b[b.size()-5]^=1;
 BOOST_REQUIRE(lseek(fd,0,SEEK_SET)==0);write_all(fd,b.data(),b.size());close(fd);
 BOOST_CHECK_THROW(Exchange x(path,config,Durability::EveryCommand,100,snap),std::runtime_error);
}
BOOST_FIXTURE_TEST_CASE(truncated_snapshot,Fixture){
 auto snap=dir+"/snapshot.bin";{Exchange x(path,config);x.save_snapshot(snap);}
 BOOST_REQUIRE(truncate(snap.c_str(),17)==0);
 BOOST_CHECK_THROW(Exchange x(path,config,Durability::EveryCommand,100,snap),std::runtime_error);
}
BOOST_FIXTURE_TEST_CASE(different_wal_prefix,Fixture){
 auto snap=dir+"/snapshot.bin";{Exchange x(path,config);x.execute(order(1));x.save_snapshot(snap);}
 unlink(path.c_str());{Exchange x(path,config);x.execute(order(2));}
 BOOST_CHECK_THROW(Exchange x(path,config,Durability::EveryCommand,100,snap),std::runtime_error);
}
BOOST_FIXTURE_TEST_CASE(snapshot_ahead_of_wal,Fixture){
 auto snap=dir+"/snapshot.bin";{Exchange x(path,config);x.execute(order(1));x.save_snapshot(snap);}
 auto b=bytes();b.resize(scan(b).records[0].end_offset);write(b);
 BOOST_CHECK_THROW(Exchange x(path,config,Durability::EveryCommand,100,snap),std::runtime_error);
}
BOOST_FIXTURE_TEST_CASE(orphan_temporary_ignored,Fixture){
 auto snap=dir+"/snapshot.bin";std::string state;
 {Exchange x(path,config);x.execute(order(1));x.save_snapshot(snap);x.execute(order(2));state=x.canonical_state();}
 int fd=open((snap+".tmp").c_str(),O_CREAT|O_WRONLY,0600);uint8_t bad=0;write_all(fd,&bad,1);close(fd);
 Exchange x(path,config,Durability::EveryCommand,100,snap);BOOST_CHECK_EQUAL(x.canonical_state(),state);
}
BOOST_FIXTURE_TEST_CASE(sigkill_snapshot_publication,Fixture){
 auto snap=dir+"/snapshot.bin";
 for(const std::string stage:{"snapshot_after_write","snapshot_after_sync","snapshot_after_rename"}){
  unlink(path.c_str());unlink(snap.c_str());unlink((snap+".tmp").c_str());
  {Exchange x(path,config);x.execute(order(1));x.save_snapshot(snap);}
  auto child=fork();BOOST_REQUIRE(child>=0);
  if(child==0){try{Exchange x(path,config);x.execute(order(2));
   x.set_hook([&](const char* actual,uint64_t){if(stage==actual)kill(getpid(),SIGKILL);});
   x.save_snapshot(snap);_exit(7);}catch(...){_exit(9);}}
  int status=0;BOOST_REQUIRE(waitpid(child,&status,0)==child);BOOST_REQUIRE(WIFSIGNALED(status)&&WTERMSIG(status)==SIGKILL);
  std::string state;{Exchange full(path,config);state=full.canonical_state();}
  Exchange x(path,config,Durability::EveryCommand,100,snap);BOOST_CHECK_EQUAL(x.canonical_state(),state);
 }
}
BOOST_FIXTURE_TEST_CASE(wal_alias_rejected,Fixture){
 Exchange x(path,config);x.execute(order(1));auto original=bytes();
 BOOST_CHECK_THROW(x.save_snapshot(path),std::runtime_error);
 BOOST_CHECK_THROW(x.save_snapshot(dir+"/./wal-000001.log"),std::runtime_error);
 BOOST_REQUIRE(link(path.c_str(),(dir+"/snapshot.bin.tmp").c_str())==0);
 BOOST_CHECK_THROW(x.save_snapshot(dir+"/snapshot.bin"),std::runtime_error);BOOST_CHECK(bytes()==original);
}
BOOST_FIXTURE_TEST_CASE(partial_wal_tail_with_snapshot,Fixture){
 auto snap=dir+"/snapshot.bin";std::string state;
 {Exchange x(path,config);x.execute(order(1));x.save_snapshot(snap);state=x.canonical_state();}
 auto b=bytes();b.push_back(0x4c);write(b);
 Exchange x(path,config,Durability::EveryCommand,100,snap);BOOST_CHECK_EQUAL(x.canonical_state(),state);
}
BOOST_FIXTURE_TEST_CASE(no_wal_snapshot_rejected,Fixture){
 Exchange x("",config,Durability::NoWal);BOOST_CHECK_THROW(x.save_snapshot(dir+"/snapshot.bin"),std::runtime_error);
}

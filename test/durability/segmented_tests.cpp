#define BOOST_TEST_MODULE SegmentedRecoveryTests
#include <boost/test/unit_test.hpp>
#include "../../extensions/durability/segmented.h"
#include <signal.h>
#include <sys/wait.h>
using namespace exchange;
using namespace exchange::durable;
struct SegFixture {
 std::string dir;Config config;uint64_t cap=4;
 SegFixture(){
  char name[]="/tmp/liquibook-segmented-test-XXXXXX";auto p=mkdtemp(name);if(!p)throw std::runtime_error("mkdtemp");dir=p;
  RiskLimits l;l.max_long_position=100000;l.max_short_position=100000;
  config={{1,{{1,l,0},{2,l,0}}},{2,{{1,l,0},{2,l,0}}}};
 }
 ~SegFixture(){DIR* d=opendir(dir.c_str());if(d){while(auto e=readdir(d)){std::string n=e->d_name;if(n!="."&&n!="..")unlink((dir+"/"+n).c_str());}closedir(d);}rmdir(dir.c_str());}
 std::string file(const char* prefix,uint64_t n,const char* suffix){
  std::ostringstream out;out<<dir<<"/"<<prefix<<std::setfill('0')<<std::setw(20)<<n<<suffix;return out.str();
 }
 std::string wal(uint64_t id){return file("wal-",id,".log");}
 std::string snap(uint64_t seq){return file("snapshot-",seq,".bin");}
 Command order(uint64_t id){Command c;c.order=id;c.account=1;c.quantity=10;c.price=90;c.time=id*1000000000ULL;return c;}
 void fill(SegmentedExchange& x,uint64_t begin,uint64_t end){for(auto i=begin;i<=end;++i)BOOST_REQUIRE(x.execute(order(i)).result);}
 void corrupt(const std::string& p){int fd=open(p.c_str(),O_RDWR);BOOST_REQUIRE(fd>=0);auto b=read_file(fd);BOOST_REQUIRE(!b.empty());b.back()^=1;BOOST_REQUIRE(lseek(fd,0,SEEK_SET)==0);write_all(fd,b.data(),b.size());close(fd);}
 void compare(uint64_t at,uint64_t end){
  std::string expected;{SegmentedExchange x(dir,config,cap,Durability::Buffered);fill(x,1,at);x.save_snapshot(false);fill(x,at+1,end);x.flush();expected=x.canonical_state();}
  {SegmentedExchange full(dir,config,cap,Durability::Buffered,100,true);BOOST_CHECK_EQUAL(full.canonical_state(),expected);BOOST_CHECK_EQUAL(full.recovery_stats().records_decoded,end);}
  SegmentedExchange x(dir,config,cap);BOOST_CHECK_EQUAL(x.canonical_state(),expected);BOOST_CHECK_EQUAL(x.recovery_stats().records_decoded,end-at);
 }
};
BOOST_FIXTURE_TEST_CASE(boundary_snapshot,SegFixture){compare(8,11);}
BOOST_FIXTURE_TEST_CASE(middle_snapshot,SegFixture){compare(6,11);}
BOOST_FIXTURE_TEST_CASE(empty_tail_boundary,SegFixture){compare(8,8);}
BOOST_FIXTURE_TEST_CASE(empty_tail_middle,SegFixture){compare(6,6);}
BOOST_FIXTURE_TEST_CASE(several_tail_segments,SegFixture){compare(3,23);}
BOOST_FIXTURE_TEST_CASE(empty_snapshot,SegFixture){compare(0,0);}
BOOST_FIXTURE_TEST_CASE(no_snapshot_full_replay,SegFixture){
 std::string state;{SegmentedExchange x(dir,config,cap);fill(x,1,13);state=x.canonical_state();}
 SegmentedExchange x(dir,config,cap);BOOST_CHECK_EQUAL(x.canonical_state(),state);BOOST_CHECK_EQUAL(x.recovery_stats().records_decoded,13);
}
BOOST_FIXTURE_TEST_CASE(retention_preserves_fallback,SegFixture){
 std::string state;{SegmentedExchange x(dir,config,cap);fill(x,1,8);x.save_snapshot();fill(x,9,16);x.save_snapshot();fill(x,17,20);state=x.canonical_state();}
 BOOST_CHECK(access(wal(1).c_str(),F_OK)!=0);BOOST_CHECK(access(wal(2).c_str(),F_OK)!=0);BOOST_CHECK(access(wal(3).c_str(),F_OK)==0);
 corrupt(snap(16));
 SegmentedExchange x(dir,config,cap);BOOST_CHECK_EQUAL(x.canonical_state(),state);BOOST_CHECK_EQUAL(x.recovery_stats().snapshot_sequence,8);
}
BOOST_FIXTURE_TEST_CASE(single_corrupt_snapshot_falls_back_to_full,SegFixture){
 compare(8,12);corrupt(snap(8));SegmentedExchange x(dir,config,cap);BOOST_CHECK_EQUAL(x.sequence(),12);BOOST_CHECK_EQUAL(x.recovery_stats().records_decoded,12);
}
BOOST_FIXTURE_TEST_CASE(all_snapshots_corrupt_after_retention_fail_closed,SegFixture){
 {SegmentedExchange x(dir,config,cap);fill(x,1,8);x.save_snapshot();fill(x,9,16);x.save_snapshot();}
 corrupt(snap(8));corrupt(snap(16));BOOST_CHECK_THROW(SegmentedExchange x(dir,config,cap),std::runtime_error);
}
BOOST_FIXTURE_TEST_CASE(truncated_first_tail_record,SegFixture){
 std::string expected;{SegmentedExchange x(dir,config,cap);fill(x,1,6);x.save_snapshot(false);expected=x.canonical_state();fill(x,7,7);}
 BOOST_REQUIRE(truncate(wal(2).c_str(),56+2*69+30)==0);
 SegmentedExchange x(dir,config,cap);BOOST_CHECK_EQUAL(x.canonical_state(),expected);BOOST_REQUIRE(x.execute(order(7)).result);
}
BOOST_FIXTURE_TEST_CASE(truncated_first_record_at_boundary,SegFixture){
 {SegmentedExchange x(dir,config,cap);fill(x,1,8);x.save_snapshot(false);fill(x,9,9);}
 BOOST_REQUIRE(truncate(wal(3).c_str(),56+30)==0);
 SegmentedExchange x(dir,config,cap);BOOST_CHECK_EQUAL(x.sequence(),8);BOOST_REQUIRE(x.execute(order(9)).result);
}
BOOST_FIXTURE_TEST_CASE(corrupt_tail_fails_without_fallback,SegFixture){
 compare(6,11);corrupt(wal(3));BOOST_CHECK_THROW(SegmentedExchange x(dir,config,cap),std::runtime_error);
}
BOOST_FIXTURE_TEST_CASE(missing_tail_segment,SegFixture){
 compare(6,19);BOOST_REQUIRE(unlink(wal(3).c_str())==0);BOOST_CHECK_THROW(SegmentedExchange x(dir,config,cap),std::runtime_error);
}
BOOST_FIXTURE_TEST_CASE(truncated_sealed_segment,SegFixture){
 compare(6,19);BOOST_REQUIRE(truncate(wal(3).c_str(),100)==0);BOOST_CHECK_THROW(SegmentedExchange x(dir,config,cap),std::runtime_error);
}
BOOST_FIXTURE_TEST_CASE(corrupt_prefix_is_not_read,SegFixture){
 compare(8,11);corrupt(wal(1));
 {SegmentedExchange x(dir,config,cap);BOOST_CHECK_EQUAL(x.sequence(),11);BOOST_CHECK_EQUAL(x.recovery_stats().records_decoded,3);}
 BOOST_CHECK_THROW(SegmentedExchange x(dir,config,cap,Durability::Buffered,100,true),std::runtime_error);
}
BOOST_FIXTURE_TEST_CASE(config_and_capacity_mismatch,SegFixture){
 {SegmentedExchange x(dir,config,cap);}
 BOOST_CHECK_THROW(SegmentedExchange x(dir,config,5),std::runtime_error);
 config[0].accounts[0].limits.max_order_qty=2;BOOST_CHECK_THROW(SegmentedExchange x(dir,config,cap),std::runtime_error);
}
BOOST_FIXTURE_TEST_CASE(exclusive_directory_writer,SegFixture){
 SegmentedExchange x(dir,config,cap);BOOST_CHECK_THROW(SegmentedExchange y(dir,config,cap),std::runtime_error);
}
BOOST_FIXTURE_TEST_CASE(group_ack_across_rotation,SegFixture){
 SegmentedExchange x(dir,config,cap,Durability::Group,3);
 BOOST_CHECK(!x.execute(order(1)).acknowledged);BOOST_CHECK(!x.execute(order(2)).acknowledged);
 BOOST_CHECK(x.execute(order(3)).acknowledged);BOOST_CHECK(!x.execute(order(4)).acknowledged);
 BOOST_CHECK(!x.execute(order(5)).acknowledged);BOOST_CHECK_EQUAL(x.durable_sequence(),4);
 BOOST_CHECK(x.execute(order(6)).acknowledged);BOOST_CHECK_EQUAL(x.flush(),6);
}
BOOST_FIXTURE_TEST_CASE(rejected_order_does_not_log,SegFixture){
 SegmentedExchange x(dir,config,cap);auto c=order(1);c.quantity=0;auto before=x.canonical_state();
 BOOST_CHECK(!x.execute(c).result);BOOST_CHECK_EQUAL(x.sequence(),0);BOOST_CHECK_EQUAL(x.canonical_state(),before);
}
BOOST_FIXTURE_TEST_CASE(lifecycle_fifo_and_multisymbol,SegFixture){
 std::string state;
 {SegmentedExchange x(dir,config,cap);
  auto a=order(90);a.time=0;BOOST_REQUIRE(x.execute(a).result);a.order=2;BOOST_REQUIRE(x.execute(a).result);
  auto sell=a;sell.order=3;sell.account=2;sell.buy=false;sell.quantity=4;BOOST_REQUIRE(x.execute(sell).result);
  auto replace=a;replace.order=90;replace.type=CommandType::ReplaceOrder;replace.quantity=15;BOOST_REQUIRE(x.execute(replace).result);
  a.symbol=2;BOOST_REQUIRE(x.execute(a).result);x.save_snapshot(false);
  sell.order=4;sell.quantity=12;BOOST_REQUIRE(x.execute(sell).result);
  auto cancel=a;cancel.type=CommandType::CancelOrder;BOOST_REQUIRE(x.execute(cancel).result);state=x.canonical_state();}
 {SegmentedExchange x(dir,config,cap);BOOST_CHECK_EQUAL(x.canonical_state(),state);}
 SegmentedExchange full(dir,config,cap,Durability::Buffered,100,true);BOOST_CHECK_EQUAL(full.canonical_state(),state);
}
BOOST_FIXTURE_TEST_CASE(snapshot_and_retention_sigkill,SegFixture){
 for(const std::string stage:{"snapshot_after_write","snapshot_after_sync","snapshot_after_rename","snapshot_after_directory_sync","snapshot_verified","before_segment_deletion","during_segment_deletion","after_segment_deletion"}){
  // Each iteration uses a fresh owned fixture so previous retirement cannot mask errors.
  SegFixture f;std::string expected;
  {SegmentedExchange x(f.dir,f.config,f.cap);f.fill(x,1,8);x.save_snapshot();f.fill(x,9,16);expected=x.canonical_state();}
  auto child=fork();BOOST_REQUIRE(child>=0);
  if(child==0){try{SegmentedExchange x(f.dir,f.config,f.cap);
   x.set_hook([&](const char* s,uint64_t){if(stage==s)kill(getpid(),SIGKILL);});x.save_snapshot();_exit(7);}catch(...){_exit(9);}}
  int status=0;BOOST_REQUIRE(waitpid(child,&status,0)==child);BOOST_REQUIRE_MESSAGE(WIFSIGNALED(status)&&WTERMSIG(status)==SIGKILL,stage);
  SegmentedExchange x(f.dir,f.config,f.cap);BOOST_CHECK_MESSAGE(x.canonical_state()==expected,stage);
 }
}
BOOST_FIXTURE_TEST_CASE(rotation_and_append_sigkill,SegFixture){
 for(const std::string stage:{"after_segment_rotation","after_risk","during_wal_write","after_wal_write","after_sync","after_match"}){
  SegFixture f;{SegmentedExchange x(f.dir,f.config,f.cap);f.fill(x,1,4);}
  auto child=fork();BOOST_REQUIRE(child>=0);
  if(child==0){try{SegmentedExchange x(f.dir,f.config,f.cap);x.set_hook([&](const char* s,uint64_t){if(stage==s)kill(getpid(),SIGKILL);});x.execute(f.order(5));_exit(7);}catch(...){_exit(9);}}
  int status=0;BOOST_REQUIRE(waitpid(child,&status,0)==child);BOOST_REQUIRE(WIFSIGNALED(status)&&WTERMSIG(status)==SIGKILL);
  SegmentedExchange x(f.dir,f.config,f.cap);bool written=stage=="after_wal_write"||stage=="after_sync"||stage=="after_match";
  BOOST_CHECK_EQUAL(x.sequence(),written?5:4);BOOST_CHECK(x.execute(f.order(6)).result);
 }
}
BOOST_FIXTURE_TEST_CASE(three_generations_retire_old_snapshot,SegFixture){
 {SegmentedExchange x(dir,config,cap);fill(x,1,8);x.save_snapshot();fill(x,9,16);x.save_snapshot();fill(x,17,24);x.save_snapshot();}
 BOOST_CHECK(access(snap(8).c_str(),F_OK)!=0);BOOST_CHECK(access(wal(4).c_str(),F_OK)!=0);
 corrupt(snap(24));SegmentedExchange x(dir,config,cap);BOOST_CHECK_EQUAL(x.recovery_stats().snapshot_sequence,16);BOOST_CHECK_EQUAL(x.sequence(),24);
}
BOOST_FIXTURE_TEST_CASE(missing_all_segments_fails_closed,SegFixture){
 {SegmentedExchange x(dir,config,cap);fill(x,1,2);}
 BOOST_REQUIRE(unlink(wal(1).c_str())==0);BOOST_CHECK_THROW(SegmentedExchange x(dir,config,cap),std::runtime_error);
}
BOOST_FIXTURE_TEST_CASE(recovery_counters_exclude_later_snapshot_saves,SegFixture){
 SegmentedExchange x(dir,config,cap);fill(x,1,8);auto before=x.recovery_stats().bytes_read;
 x.save_snapshot();BOOST_CHECK_EQUAL(x.recovery_stats().bytes_read,before);
 BOOST_CHECK_EQUAL(x.recovery_stats().records_decoded,0);
}
BOOST_FIXTURE_TEST_CASE(recovered_logical_clock_advances,SegFixture){
 compare(6,11);
 for(bool full:{false,true}){
  SegmentedExchange x(dir,config,cap,Durability::Buffered,100,full);
  auto before=x.now();BOOST_CHECK(before>=11000000000ULL);
  usleep(2000);BOOST_CHECK(x.now()>before);
 }
}

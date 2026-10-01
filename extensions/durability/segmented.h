#pragma once
#include "wal.h"
#include <dirent.h>
#include <iomanip>
#include <set>

namespace exchange { namespace durable {
struct RecoveryStats {
 uint64_t bytes_read=0,records_decoded=0,snapshot_sequence=0;
 double snapshot_ms=0,tail_ms=0,total_ms=0;
};
class SegmentedExchange {
 Config config_;
 std::string dir_;
 Durability mode_;
 uint64_t capacity_,segment_=1,durable_=0;
 uint32_t group_;
 int lock_=-1,fd_=-1;
 Bytes identity_;
 std::unique_ptr<Exchange> engine_;
 RecoveryStats stats_;
 bool halted_=false;
 bool recording_recovery_=true;
 std::function<void(const char*,uint64_t)> hook_;
 static constexpr uint64_t header_size=56,record_size=69;
 std::string name(const char* prefix,uint64_t n,const char* suffix)const{
  std::ostringstream out;out<<dir_<<"/"<<prefix<<std::setw(20)<<std::setfill('0')<<n<<suffix;return out.str();
 }
 std::string wal(uint64_t n)const{return name("wal-",n,".log");}
 std::string snapshot(uint64_t n)const{return name("snapshot-",n,".bin");}
 void fire(const char* stage){if(hook_)hook_(stage,engine_->sequence());}
 std::vector<uint64_t> inventory(const char* prefix,const char* suffix)const;
 Bytes read(const std::string&,uint64_t offset=0,uint64_t size=UINT64_MAX);
 Bytes unpack(const Bytes&,uint16_t type,uint64_t* sequence=nullptr)const;
 void publish(const std::string&,const Bytes&,bool snapshot_hooks=false);
 Bytes segment_header(uint64_t id)const;
 void check_header(uint64_t id);
 void create_segment(uint64_t id);
 void open_current();
 void recover(bool full);
 std::unique_ptr<Exchange> load_snapshot(uint64_t seq,uint64_t& id,uint64_t& offset);
public:
 SegmentedExchange(const std::string& directory,const Config&,uint64_t capacity=100000,
                   Durability mode=Durability::EveryCommand,uint32_t group=100,bool full_replay=false);
 ~SegmentedExchange(){if(fd_>=0)close(fd_);if(lock_>=0)close(lock_);}
 SegmentedExchange(const SegmentedExchange&)=delete;
 SegmentedExchange& operator=(const SegmentedExchange&)=delete;
 Receipt execute(const Command&);
 uint64_t flush();
 // Retains the newest two valid snapshots and the WAL required by the older one.
 void save_snapshot(bool retire=true);
 void set_hook(std::function<void(const char*,uint64_t)> hook){hook_=std::move(hook);}
 uint64_t sequence()const{return engine_->sequence();}
 uint64_t durable_sequence()const{return durable_;}
 Timestamp now()const{return engine_->now();}
 std::string canonical_state()const{return engine_->canonical_state();}
 uint64_t hash()const{return engine_->hash();}
 const RiskGateway& book(uint32_t id)const{return engine_->book(id);}
 const RecoveryStats& recovery_stats()const{return stats_;}
};
} }

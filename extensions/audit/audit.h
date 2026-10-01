#pragma once
#include "../fix/adapter.h"
#include <libpq-fe.h>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <set>
#include <thread>

namespace exchange { namespace audit {
enum class Category:uint8_t { Order=1, OrderEvent=2, Trade=3, Risk=4 };
struct Record {
  std::string event_id,client,cl_ord_id,symbol,side,event_type,decision,reason;
  Category category=Category::Risk;
  uint64_t ordinal=0,event_index=0,sequence=0,timestamp=0,order_id=0,buy_order_id=0,sell_order_id=0;
  int64_t quantity=0,price=0,leaves=0,cumulative=0;
};
struct Metrics {
  uint64_t enqueued=0,committed=0,batches=0,reconnects=0,max_queue_depth=0,dropped=0;
  uint64_t last_commit_us=0,max_commit_us=0;
  size_t memory_depth=0,durable_depth=0;
};
std::vector<Record> derive(const fix::Request&,Timestamp,uint64_t ordinal,
                           uint64_t before,uint64_t after,const std::vector<fix::Response>&);
durable::Bytes encode(const Record&);
Record decode(const durable::Bytes&);

class Store {
  std::string path_,conninfo_;
  int fd_=-1;
  std::mutex mutex_;
  std::condition_variable wake_,idle_;
  std::deque<Record> incoming_;
  std::map<uint64_t,Record> pending_;
  std::set<std::string> known_;
  uint64_t next_=1;
  bool stopping_=false,busy_=false;
  std::thread worker_;
  std::atomic<uint64_t> enqueued_{0},committed_{0},batches_{0},reconnects_{0},max_depth_{0},dropped_{0},last_commit_us_{0},max_commit_us_{0};
  void recover(const durable::Bytes& binding);
  void append(uint16_t,uint64_t,const durable::Bytes&);
  void run();
public:
  Store(const std::string& directory,const std::string& conninfo,const durable::Bytes& binding);
  ~Store();
  Store(const Store&)=delete;Store& operator=(const Store&)=delete;
  void enqueue(std::vector<Record>) noexcept;
  bool wait_idle(uint64_t timeout_ms);
  Metrics metrics();
};
} }

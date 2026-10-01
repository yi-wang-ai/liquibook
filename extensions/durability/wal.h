#pragma once
#include "../risk/risk_gateway.h"
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <map>
#include <vector>

namespace exchange { namespace durable {
using Bytes=std::vector<uint8_t>;
inline void put(Bytes& b,uint64_t n,size_t width) {
  for(size_t i=0;i<width;++i)b.push_back(static_cast<uint8_t>(n>>(8*i)));
}
struct Reader {
  const Bytes& bytes; size_t pos=0;
  explicit Reader(const Bytes& b):bytes(b){}
  uint64_t get(size_t width) {
    if(width>8 || pos>bytes.size() || bytes.size()-pos<width)throw std::runtime_error("truncated payload");
    uint64_t n=0;for(size_t i=0;i<width;++i)n|=uint64_t(bytes[pos++])<<(8*i);return n;
  }
  int64_t signed64() {uint64_t n=get(8); int64_t value;std::memcpy(&value,&n,8);return value;}
  void end() const {if(pos!=bytes.size())throw std::runtime_error("unexpected payload bytes");}
};
inline uint32_t crc32(const uint8_t* p,size_t n) {
  uint32_t c=~0U;for(size_t i=0;i<n;++i){c^=p[i];for(int j=0;j<8;++j)c=(c>>1)^(0xedb88320U & (0U-(c&1)));}return ~c;
}
inline uint64_t state_hash(const std::string& s) {
  uint64_t hash=1469598103934665603ULL;for(unsigned char c:s)hash=(hash^c)*1099511628211ULL;return hash;
}
struct AccountConfig {AccountId id; RiskLimits limits; Position position;};
struct SymbolConfig {uint32_t id;std::vector<AccountConfig> accounts;};
using Config=std::vector<SymbolConfig>;
inline Bytes encode_config(const Config& config) {
  Bytes b;put(b,config.size(),4);
  for(const auto& symbol:config){put(b,symbol.id,4);put(b,symbol.accounts.size(),4);
    for(const auto& a:symbol.accounts){put(b,a.id,8);put(b,a.position,8);const auto& l=a.limits;
      put(b,l.max_order_qty,8);put(b,l.max_order_notional,8);put(b,l.max_long_position,8);
      put(b,l.max_short_position,8);put(b,l.min_price,8);put(b,l.max_price,8);put(b,l.max_orders_per_second,4);
    }
  }return b;
}
enum class CommandType:uint16_t {NewOrder=1,CancelOrder=2,ReplaceOrder=3};
struct Command {
  CommandType type=CommandType::NewOrder;
  uint32_t symbol=1;OrderId order=0;AccountId account=0;
  bool buy=true;int64_t price=0;int64_t quantity=0;Timestamp time=0;
};
inline Bytes encode_command(const Command& c) {
  Bytes b;put(b,c.symbol,4);put(b,c.order,8);put(b,c.account,8);put(b,c.buy,1);
  put(b,c.price,8);put(b,c.quantity,8);put(b,c.time,8);return b;
}
inline Command decode_command(uint16_t type,const Bytes& b) {
  if(type<1||type>3||b.size()!=45)throw std::runtime_error("invalid command schema");
  Reader r(b);Command c;c.type=static_cast<CommandType>(type);c.symbol=r.get(4);c.order=r.get(8);
  c.account=r.get(8);auto side=r.get(1);if(side>1)throw std::runtime_error("invalid side");
  c.buy=side;c.price=r.signed64();c.quantity=r.signed64();c.time=r.get(8);r.end();return c;
}
struct Record {uint16_t type;uint64_t sequence;Bytes payload;size_t end_offset;};
inline Bytes frame(uint16_t type,uint64_t seq,const Bytes& payload) {
  Bytes b;put(b,0x3142574c,4);put(b,1,2);put(b,type,2);put(b,payload.size(),4);put(b,seq,8);
  b.insert(b.end(),payload.begin(),payload.end());put(b,crc32(b.data(),b.size()),4);return b;
}
struct Scan {std::vector<Record> records;size_t valid_bytes=0;bool partial_tail=false;};
inline Scan scan(const Bytes& bytes) {
  Scan result;size_t offset=0;
  while(offset<bytes.size()){
    if(bytes.size()-offset<20){result.partial_tail=true;break;}
    Bytes header(bytes.begin()+offset,bytes.begin()+offset+20);Reader r(header);
    if(r.get(4)!=0x3142574c||r.get(2)!=1)throw std::runtime_error("invalid WAL magic/version");
    const uint16_t type=r.get(2);const uint32_t length=r.get(4);const uint64_t seq=r.get(8);
    if(type>3 || (type!=0 && length!=45) || length>16*1024*1024)throw std::runtime_error("invalid WAL record length/type");
    if(bytes.size()-offset<24ULL+length){result.partial_tail=true;break;}
    const size_t end=offset+24+length;
    Bytes checksum(bytes.begin()+end-4,bytes.begin()+end);Reader check(checksum);
    if(check.get(4)!=crc32(bytes.data()+offset,20+length))throw std::runtime_error("WAL checksum mismatch");
    if(result.records.empty()) {
      if(type!=0||seq!=0)throw std::runtime_error("missing WAL configuration");
    } else if(type==0||result.records.back().sequence==UINT64_MAX||seq!=result.records.back().sequence+1)throw std::runtime_error("WAL sequence gap/duplicate");
    result.records.push_back({type,seq,Bytes(bytes.begin()+offset+20,bytes.begin()+end-4),end});
    offset=end;result.valid_bytes=end;
  }return result;
}
inline void io_error(const char* action){throw std::runtime_error(std::string(action)+": "+std::strerror(errno));}
inline void write_all(int fd,const uint8_t* data,size_t size) {
  while(size){const auto n=::write(fd,data,size);if(n<0&&errno==EINTR)continue;if(n<=0)io_error("write");data+=n;size-=n;}
}
inline void sync_file(int fd){if(::fsync(fd)!=0)io_error("fsync");}
inline Bytes read_file(int fd) {
  struct stat st{};if(fstat(fd,&st))io_error("stat");
  if(!S_ISREG(st.st_mode)||st.st_size<0||st.st_size>512LL*1024*1024)throw std::runtime_error("unsupported WAL size/type");
  Bytes bytes(static_cast<size_t>(st.st_size));size_t n=0;
  while(n<bytes.size()){auto got=pread(fd,bytes.data()+n,bytes.size()-n,n);if(got<0&&errno==EINTR)continue;if(got<=0)io_error("read");n+=got;}return bytes;
}
inline void sync_parent(const std::string& path) {
  auto slash=path.find_last_of('/');const auto parent=slash==std::string::npos?".":path.substr(0,slash);
  int fd=open(parent.empty()?"/":parent.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC);
  if(fd<0)io_error("open directory");
  try{sync_file(fd);}catch(...){close(fd);throw;}close(fd);
}
enum class Durability {NoWal,Buffered,Group,EveryCommand};
struct Receipt {
  RiskResult result;uint64_t applied_sequence;uint64_t durable_sequence;bool acknowledged;
};
class Exchange {
  friend class SegmentedExchange;
  Bytes checkpoint_state() const;
  void load_checkpoint_state(const Bytes&,uint64_t);
  Config config_;
  std::map<uint32_t,std::unique_ptr<RiskGateway>> books_;
  int fd_=-1;std::string path_;Durability mode_;uint32_t group_;
  uint64_t sequence_=0,durable_=0,pending_=0,logical_floor_=0;
  Timestamp clock_origin_=steady_now();
  Timestamp clock_base_=0;bool halted_=false;
  std::function<void(const char*,uint64_t)> hook_;
  void restore_snapshot(const std::string&,const Scan&,const Bytes&);
  void fire(const char* stage){if(hook_)hook_(stage,pending_);}
  RiskGateway& gateway(uint32_t symbol) {
    auto it=books_.find(symbol);if(it==books_.end())throw std::invalid_argument("unknown symbol");return *it->second;
  }
  RiskResult validate(const Command& c) {
    auto& g=gateway(c.symbol);
    switch(c.type){
      case CommandType::NewOrder:return g.validate({c.order,c.account,c.buy,c.quantity,c.price},c.time);
      case CommandType::CancelOrder:return g.validate_cancel(c.account,c.order);
      case CommandType::ReplaceOrder:return g.validate_replace(c.account,c.order,c.quantity,c.price,c.time);
    }throw std::invalid_argument("invalid command type");
  }
  RiskResult apply(const Command& c) {
    auto& g=gateway(c.symbol);
    switch(c.type){
      case CommandType::NewOrder:return g.submit({c.order,c.account,c.buy,c.quantity,c.price},c.time);
      case CommandType::CancelOrder:return g.cancel(c.account,c.order);
      case CommandType::ReplaceOrder:return g.replace(c.account,c.order,c.quantity,c.price,c.time);
    }throw std::invalid_argument("invalid command type");
  }
  void append(const Bytes& bytes) {
    if(lseek(fd_,0,SEEK_END)<0)io_error("seek");
    auto end=lseek(fd_,0,SEEK_CUR);
    if(end<0 || uint64_t(end)+bytes.size()>512ULL*1024*1024)throw std::runtime_error("WAL size limit");
    if(hook_){size_t half=bytes.size()/2;write_all(fd_,bytes.data(),half);fire("during_wal_write");write_all(fd_,bytes.data()+half,bytes.size()-half);}
    else write_all(fd_,bytes.data(),bytes.size());
  }
public:
  Exchange(const std::string& path,const Config& config,Durability mode=Durability::EveryCommand,uint32_t group=100,const std::string& snapshot="")
      :config_(config),path_(path),mode_(mode),group_(group) {
    if(!group)throw std::invalid_argument("zero group size");
    for(const auto& symbol:config){
      std::unique_ptr<RiskGateway> g(new RiskGateway);
      for(const auto& a:symbol.accounts)g->add_account(a.id,a.limits,a.position);
      if(!books_.emplace(symbol.id,std::move(g)).second)throw std::invalid_argument("duplicate symbol");
    }
    if(books_.empty())throw std::invalid_argument("empty configuration");
    if(mode==Durability::NoWal)return;
    fd_=open(path.c_str(),O_RDWR|O_CREAT|O_CLOEXEC|O_NOFOLLOW,0600);
    if(fd_<0)io_error("open WAL");
    try{
      if(flock(fd_,LOCK_EX|LOCK_NB))io_error("lock WAL");
      Bytes data=read_file(fd_);
      if(data.empty()){
        if(!snapshot.empty())throw std::runtime_error("snapshot requires existing WAL");
        append(frame(0,0,encode_config(config)));sync_file(fd_);sync_parent(path);
      } else {
        auto parsed=scan(data);
        if(parsed.records.empty()||parsed.records.front().payload!=encode_config(config))throw std::runtime_error("configuration mismatch/truncated header");
        if(!snapshot.empty())restore_snapshot(snapshot,parsed,data);
        for(size_t i=1;i<parsed.records.size();++i){
          const auto& record=parsed.records[i];if(record.sequence<=sequence_)continue;const auto c=decode_command(record.type,record.payload);
          if(!validate(c)||!apply(c))throw std::runtime_error("invalid command during replay");
          sequence_=record.sequence;logical_floor_=std::max(logical_floor_,c.time);
        }
        if(parsed.partial_tail){if(ftruncate(fd_,parsed.valid_bytes))io_error("truncate tail");}
        // Valid pending records may survive a process crash. Persist the recovered prefix.
        sync_file(fd_);durable_=sequence_;clock_base_=logical_floor_;
      }
    }catch(...){close(fd_);fd_=-1;throw;}
  }
  ~Exchange(){if(fd_>=0)close(fd_);}
  Exchange(const Exchange&)=delete;Exchange& operator=(const Exchange&)=delete;
  void set_hook(std::function<void(const char*,uint64_t)> hook) {
    hook_=std::move(hook);
    for(auto& entry:books_)entry.second->set_event_hook([this](const char* stage){fire(stage);});
  }
  Receipt execute(const Command& c) {
    if(halted_)throw std::runtime_error("exchange halted");
    auto result=validate(c);
    if(!result)return {result,sequence_,durable_,false};
    if(sequence_==UINT64_MAX)throw std::overflow_error("sequence exhausted");
    pending_=sequence_+1;
    fire("after_risk");
    try{
      if(mode_!=Durability::NoWal){
        append(frame(static_cast<uint16_t>(c.type),pending_,encode_command(c)));fire("after_wal_write");
        if(mode_==Durability::EveryCommand || (mode_==Durability::Group && pending_%group_==0)){
          sync_file(fd_);durable_=pending_;fire("after_sync");
        }
      }
      if(!apply(c))throw std::runtime_error("risk result changed before apply");
      sequence_=pending_;logical_floor_=std::max(logical_floor_,c.time);fire("after_match");
      return {{RejectReason::None},sequence_,durable_,mode_!=Durability::NoWal && durable_>=sequence_};
    }catch(...){halted_=true;throw;}
  }
  void save_snapshot(const std::string& path);
  uint64_t flush() {
    if(halted_)throw std::runtime_error("exchange halted");
    if(mode_==Durability::NoWal)return 0;
    try{sync_file(fd_);durable_=sequence_;return durable_;}catch(...){halted_=true;throw;}
  }
  Timestamp now()const {
    const auto elapsed=steady_now()-clock_origin_;
    if(elapsed>UINT64_MAX-clock_base_)throw std::overflow_error("logical clock exhausted");
    return std::max(logical_floor_,clock_base_+elapsed);
  }
  uint64_t sequence()const{return sequence_;}
  uint64_t durable_sequence()const{return durable_;}
  const RiskGateway& book(uint32_t symbol)const{return *books_.at(symbol);}
  std::string canonical_state() const {
    std::ostringstream out;out.imbue(std::locale::classic());out<<sequence_<<':'<<logical_floor_<<'|';
    for(const auto& entry:books_)out<<entry.first<<'{'<<entry.second->canonical_state()<<'}';
    return out.str();
  }
  uint64_t hash()const{return state_hash(canonical_state());}
};
} }

#include "wal.h"
#include <set>

namespace exchange { namespace durable {
static uint64_t byte_hash(const Bytes& bytes,size_t length) {
  if(length>bytes.size())throw std::runtime_error("invalid prefix length");
  uint64_t hash=1469598103934665603ULL;for(size_t i=0;i<length;++i)hash=(hash^bytes[i])*1099511628211ULL;return hash;
}
static Bytes take(Reader& r,size_t size){
  if(size>r.bytes.size()-r.pos)throw std::runtime_error("truncated snapshot");
  Bytes b(r.bytes.begin()+r.pos,r.bytes.begin()+r.pos+size);r.pos+=size;return b;
}
class StateCodec {
public:
 static Bytes encode(const RiskGateway& g){
  if(g.halted_)throw std::runtime_error("cannot snapshot halted gateway");
  Bytes b;std::vector<AccountId> ids;for(const auto& a:g.risk_.accounts_)ids.push_back(a.first);std::sort(ids.begin(),ids.end());
  put(b,ids.size(),4);
  for(auto id:ids){const auto& s=g.risk_.accounts_.at(id).state;
   put(b,id,8);put(b,s.position,8);put(b,s.reserved_buy,8);put(b,s.reserved_sell,8);put(b,s.last_accepted_time,8);
   put(b,s.admissions.size(),4);for(auto time:s.admissions)put(b,time,8);
  }
  ids.clear();for(const auto& o:g.orders_)ids.push_back(o.first);std::sort(ids.begin(),ids.end());put(b,ids.size(),4);
  for(auto id:ids){const auto& o=*g.orders_.at(id);
   put(b,id,8);put(b,o.account,8);put(b,o.buy,1);put(b,o.total,8);put(b,o.remaining,8);
   put(b,o.filled,8);put(b,o.limit,8);put(b,static_cast<uint8_t>(o.state),1);
  }
  put(b,g.book_.bids().size(),4);for(const auto& row:g.book_.bids())put(b,row.second.ptr()->id,8);
  put(b,g.book_.asks().size(),4);for(const auto& row:g.book_.asks())put(b,row.second.ptr()->id,8);
  put(b,g.book_.market_price(),8);put(b,g.events_,8);return b;
 }
 static void restore(RiskGateway& g,const Bytes& b){
  Reader r(b);const auto accounts=r.get(4);
  if(accounts!=g.risk_.accounts_.size())throw std::runtime_error("snapshot account count");
  std::set<AccountId> seen;
  for(size_t i=0;i<accounts;++i){
   auto id=r.get(8);if(!seen.insert(id).second)throw std::runtime_error("duplicate snapshot account");
   auto it=g.risk_.accounts_.find(id);if(it==g.risk_.accounts_.end())throw std::runtime_error("unknown snapshot account");
   auto& s=it->second.state;const auto& limits=it->second.limits;
   s.position=r.signed64();s.reserved_buy=r.get(8);s.reserved_sell=r.get(8);s.last_accepted_time=r.get(8);
   auto n=r.get(4);if(n>limits.max_orders_per_second||n>(b.size()-r.pos)/8)throw std::runtime_error("invalid admission count");
   for(size_t j=0;j<n;++j){auto time=r.get(8);if(time>s.last_accepted_time||(!s.admissions.empty()&&time<s.admissions.back()))throw std::runtime_error("invalid admission time");s.admissions.push_back(time);}
   if((!s.admissions.empty()&&s.admissions.back()!=s.last_accepted_time)||
      static_cast<__int128>(s.position)+s.reserved_buy>limits.max_long_position||
      static_cast<__int128>(s.position)-s.reserved_sell < -static_cast<__int128>(limits.max_short_position))
      throw std::runtime_error("invalid snapshot risk state");
  }
  auto orders=r.get(4);if(orders>(b.size()-r.pos)/50)throw std::runtime_error("invalid order count");
  std::map<AccountId,std::pair<Quantity,Quantity>> reservations;
  for(size_t i=0;i<orders;++i){
   auto id=r.get(8),account=r.get(8),side=r.get(1),total=r.get(8),remaining=r.get(8),filled=r.get(8),price=r.get(8),state=r.get(1);
   auto a=g.risk_.accounts_.find(account);if(a==g.risk_.accounts_.end())throw std::runtime_error("snapshot order account");
   const auto& l=a->second.limits;
   if(side>1||state>2||!total||total>l.max_order_qty||filled>total||remaining>total-filled||
      price<static_cast<uint64_t>(l.min_price)||price>static_cast<uint64_t>(l.max_price)||price>l.max_order_notional/total)
      throw std::runtime_error("invalid snapshot order");
   if((state==0&&(!remaining||remaining!=total-filled))||(state==1&&(remaining||filled!=total))||(state==2&&remaining))
      throw std::runtime_error("invalid order lifecycle");
   std::unique_ptr<OrderRecord> o(new OrderRecord({id,account,bool(side),static_cast<int64_t>(total),static_cast<int64_t>(price)}));
   o->remaining=remaining;o->filled=filled;o->state=static_cast<OrderState>(state);
   if(!g.orders_.emplace(id,std::move(o)).second)throw std::runtime_error("duplicate order ID");
   auto& reserved=side?reservations[account].first:reservations[account].second;
   if(remaining>UINT64_MAX-reserved)throw std::runtime_error("reservation overflow");
   reserved+=remaining;
  }
  for(auto id:seen){const auto& s=g.risk_.accounts_.at(id).state;const auto& sums=reservations[id];
   if(s.reserved_buy!=sums.first||s.reserved_sell!=sums.second)throw std::runtime_error("snapshot reservation mismatch");
  }
  // Reconstruct in captured FIFO order with callbacks detached. Only open
  // quantity is submitted, then the original total and fill history restored.
  g.book_.set_order_listener(nullptr);std::set<OrderId> live;
  for(bool buy:{true,false}){
   auto count=r.get(4);if(count>orders)throw std::runtime_error("invalid book count");
   for(size_t j=0;j<count;++j){
    auto id=r.get(8);auto it=g.orders_.find(id);
    if(it==g.orders_.end()||!live.insert(id).second)throw std::runtime_error("invalid book ID");
    auto& o=*it->second;if(o.buy!=buy||o.state!=OrderState::Open)throw std::runtime_error("invalid live order");
    auto total=o.total;o.total=o.remaining;bool matched=g.book_.add(&o);o.total=total;
    if(matched)throw std::runtime_error("crossed snapshot book");
   }
  }
  for(const auto& o:g.orders_)if(o.second->state==OrderState::Open&&!live.count(o.first))throw std::runtime_error("missing live order");
  auto market=r.get(8);g.events_=r.get(8);r.end();g.book_.set_market_price(market);g.book_.set_order_listener(&g);
  if(encode(g)!=b)throw std::runtime_error("noncanonical snapshot");
 }
};
void Exchange::save_snapshot(const std::string& path){
 if(mode_==Durability::NoWal)throw std::runtime_error("snapshot requires WAL");
 if(halted_)throw std::runtime_error("exchange halted");
 struct stat wal_stat{},target{};
 if(fstat(fd_,&wal_stat))io_error("snapshot WAL stat");
 if(stat(path.c_str(),&target)==0&&wal_stat.st_dev==target.st_dev&&wal_stat.st_ino==target.st_ino)
  throw std::runtime_error("snapshot aliases WAL");
 flush();pending_=sequence_;
 auto wal=read_file(fd_);auto records=scan(wal);
 if(records.partial_tail||records.records.back().sequence!=sequence_)throw std::runtime_error("snapshot WAL boundary mismatch");
 Bytes payload;auto config=encode_config(config_);put(payload,config.size(),4);payload.insert(payload.end(),config.begin(),config.end());
 put(payload,wal.size(),8);put(payload,byte_hash(wal,wal.size()),8);put(payload,logical_floor_,8);put(payload,books_.size(),4);
 for(const auto& book:books_){auto state=StateCodec::encode(*book.second);put(payload,book.first,4);put(payload,state.size(),8);payload.insert(payload.end(),state.begin(),state.end());}
 if(payload.size()>512ULL*1024*1024-24)throw std::runtime_error("snapshot size limit");
 auto data=frame(256,sequence_,payload);const auto temp=path+".tmp";
 int fd=open(temp.c_str(),O_WRONLY|O_CREAT|O_CLOEXEC|O_NOFOLLOW,0600);if(fd<0)io_error("open snapshot temporary");
 try{
  struct stat st{};if(fstat(fd,&st))io_error("snapshot stat");
  if(!S_ISREG(st.st_mode)||(st.st_dev==wal_stat.st_dev&&st.st_ino==wal_stat.st_ino))throw std::runtime_error("invalid snapshot temporary file");
  if(ftruncate(fd,0))io_error("snapshot truncate");
  write_all(fd,data.data(),data.size());fire("snapshot_after_write");sync_file(fd);fire("snapshot_after_sync");
  if(close(fd)){fd=-1;io_error("snapshot close");}fd=-1;
  if(rename(temp.c_str(),path.c_str()))io_error("snapshot rename");
  fire("snapshot_after_rename");sync_parent(path);
 }catch(...){if(fd>=0)close(fd);throw;}
}
void Exchange::restore_snapshot(const std::string& path,const Scan& wal,const Bytes& wal_bytes){
 int fd=open(path.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW);if(fd<0)io_error("open snapshot");
 Bytes bytes;try{bytes=read_file(fd);}catch(...){close(fd);throw;}close(fd);
 if(bytes.size()<24)throw std::runtime_error("truncated snapshot frame");
 Reader frame_reader(bytes);
 if(frame_reader.get(4)!=0x3142574c||frame_reader.get(2)!=1||frame_reader.get(2)!=256)throw std::runtime_error("snapshot version/type");
 auto length=frame_reader.get(4),seq=frame_reader.get(8);
 if(length!=bytes.size()-24||seq>=wal.records.size())throw std::runtime_error("snapshot length/sequence");
 auto payload=take(frame_reader,length);auto checksum=frame_reader.get(4);frame_reader.end();
 if(checksum!=crc32(bytes.data(),bytes.size()-4))throw std::runtime_error("snapshot checksum mismatch");
 Reader r(payload);auto config=take(r,r.get(4));if(config!=encode_config(config_))throw std::runtime_error("snapshot configuration mismatch");
 auto offset=r.get(8),hash=r.get(8);
 if(offset!=wal.records[seq].end_offset||hash!=byte_hash(wal_bytes,offset))throw std::runtime_error("snapshot belongs to different WAL prefix");
 logical_floor_=r.get(8);auto symbols=r.get(4);if(symbols!=books_.size())throw std::runtime_error("snapshot symbol count");
 std::set<uint32_t> seen;
 for(size_t i=0;i<symbols;++i){auto symbol=r.get(4);auto state=take(r,r.get(8));
  if(!books_.count(symbol)||!seen.insert(symbol).second)throw std::runtime_error("snapshot symbol mismatch");
  StateCodec::restore(*books_.at(symbol),state);
 }
 r.end();sequence_=seq;
 // Verify the clock floor against the already validated WAL prefix.
 Timestamp expected=0;for(size_t i=1;i<=seq;++i)expected=std::max(expected,decode_command(wal.records[i].type,wal.records[i].payload).time);
 if(expected!=logical_floor_)throw std::runtime_error("snapshot logical clock mismatch");
}
} }

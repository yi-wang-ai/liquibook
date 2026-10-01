#include "audit.h"
#include <chrono>
#include <sstream>

namespace exchange { namespace audit {
using namespace durable;
static std::string field(const FIX::Message& m,int tag){
  if(tag==35)return m.getHeader().isSetField(tag)?m.getHeader().getField(tag):"";
  return m.isSetField(tag)?m.getField(tag):"";
}
static uint64_t number(const std::string& value){
  if(value.empty()||value.find_first_not_of("0123456789")!=std::string::npos)throw std::runtime_error("invalid audit integer");
  return std::stoull(value);
}
static int64_t ticks(const std::string& value,unsigned decimals){
  auto dot=value.find('.');std::string whole=dot==std::string::npos?value:value.substr(0,dot);
  std::string fraction=dot==std::string::npos?"":value.substr(dot+1);
  if(whole.empty()||whole.find_first_not_of("0123456789")!=std::string::npos||fraction.find_first_not_of("0123456789")!=std::string::npos||fraction.size()>decimals)throw std::runtime_error("invalid audit decimal");
  while(fraction.size()<decimals)fraction.push_back('0');uint64_t scale=1;for(unsigned i=0;i<decimals;++i)scale*=10;
  auto w=std::stoull(whole),f=fraction.empty()?0:std::stoull(fraction);if(w>(uint64_t(INT64_MAX)-f)/scale)throw std::runtime_error("audit decimal overflow");return w*scale+f;
}
static unsigned decimals(const std::string& value){auto dot=value.find('.');return dot==std::string::npos?0:value.size()-dot-1;}
std::vector<Record> derive(const fix::Request& q,Timestamp time,uint64_t ordinal,uint64_t before,uint64_t after,const std::vector<fix::Response>& responses){
  std::vector<Record> out;Record risk;risk.category=Category::Risk;risk.ordinal=ordinal;risk.sequence=after;risk.timestamp=time;
  risk.event_id=std::to_string(ordinal)+":risk:0";risk.client=q.client;risk.cl_ord_id=q.id;risk.symbol=q.symbol;risk.side=q.side;
  risk.decision=after>before?"ACCEPTED":q.duplicate?"REPLAYED":"REJECTED";
  if(!responses.empty())risk.reason=field(responses.front().message,58);out.push_back(risk);
  if(after==before)return out;
  if(q.type=="D"){
    for(const auto& response:responses)if(field(response.message,35)=="8"&&field(response.message,150)=="0"){
      Record order;order.category=Category::Order;order.ordinal=ordinal;order.sequence=after;order.timestamp=time;
      order.event_id=std::to_string(ordinal)+":order:0";order.client=response.client;
      order.order_id=number(field(response.message,37));order.cl_ord_id=field(response.message,11);order.symbol=field(response.message,55);order.side=field(response.message,54);
      order.quantity=number(field(response.message,38));order.price=ticks(field(response.message,44),decimals(field(response.message,44)));out.push_back(order);break;
    }
  }
  size_t event_index=0,trade_index=0;std::vector<Record> fills;
  for(const auto& response:responses){
    if(field(response.message,35)!="8")continue;const auto exec=field(response.message,150);
    if(exec!="0"&&exec!="F"&&exec!="4"&&exec!="5")continue;
    Record event;event.category=Category::OrderEvent;event.ordinal=ordinal;event.event_index=++event_index;event.sequence=after;event.timestamp=time;
    event.event_id=std::to_string(ordinal)+":event:"+std::to_string(event_index);event.client=response.client;
    event.order_id=number(field(response.message,37));event.cl_ord_id=field(response.message,11);event.symbol=field(response.message,55);event.side=field(response.message,54);
    event.event_type=exec;event.quantity=field(response.message,38).empty()?0:number(field(response.message,38));
    event.leaves=number(field(response.message,151));event.cumulative=number(field(response.message,14));
    if(!field(response.message,44).empty())event.price=ticks(field(response.message,44),decimals(field(response.message,44)));
    out.push_back(event);
    if(exec=="F"){Record fill=event;fill.quantity=number(field(response.message,32));fill.price=ticks(field(response.message,31),decimals(field(response.message,31)));fills.push_back(fill);}
  }
  for(size_t i=0;i+1<fills.size();i+=2){
    const auto& a=fills[i];const auto& b=fills[i+1];if(a.quantity!=b.quantity||a.price!=b.price||a.side==b.side)throw std::runtime_error("unpaired audit fill");
    Record trade;trade.category=Category::Trade;trade.ordinal=ordinal;trade.event_index=++trade_index;trade.sequence=after;trade.timestamp=time;
    trade.event_id=std::to_string(ordinal)+":trade:"+std::to_string(trade_index);trade.symbol=a.symbol;trade.quantity=a.quantity;trade.price=a.price;
    trade.buy_order_id=a.side=="1"?a.order_id:b.order_id;trade.sell_order_id=a.side=="2"?a.order_id:b.order_id;out.push_back(trade);
  }
  return out;
}
static void put_string(Bytes& b,const std::string& value){if(value.size()>4096)throw std::runtime_error("audit field too large");put(b,value.size(),4);b.insert(b.end(),value.begin(),value.end());}
static std::string get_string(Reader& r){auto n=r.get(4);if(n>4096||n>r.bytes.size()-r.pos)throw std::runtime_error("invalid audit string");std::string s(r.bytes.begin()+r.pos,r.bytes.begin()+r.pos+n);r.pos+=n;return s;}
Bytes encode(const Record& r){
  Bytes b;put(b,static_cast<uint8_t>(r.category),1);for(const auto* s:{&r.event_id,&r.client,&r.cl_ord_id,&r.symbol,&r.side,&r.event_type,&r.decision,&r.reason})put_string(b,*s);
  for(auto n:{r.ordinal,r.event_index,r.sequence,r.timestamp,r.order_id,r.buy_order_id,r.sell_order_id})put(b,n,8);
  for(auto n:{r.quantity,r.price,r.leaves,r.cumulative})put(b,static_cast<uint64_t>(n),8);return b;
}
Record decode(const Bytes& b){
  Reader rd(b);Record r;auto c=rd.get(1);if(c<1||c>4)throw std::runtime_error("invalid audit category");r.category=static_cast<Category>(c);
  for(auto* s:{&r.event_id,&r.client,&r.cl_ord_id,&r.symbol,&r.side,&r.event_type,&r.decision,&r.reason})*s=get_string(rd);
  uint64_t* values[]={&r.ordinal,&r.event_index,&r.sequence,&r.timestamp,&r.order_id,&r.buy_order_id,&r.sell_order_id};for(auto p:values)*p=rd.get(8);
  int64_t* signed_values[]={&r.quantity,&r.price,&r.leaves,&r.cumulative};for(auto p:signed_values){uint64_t n=rd.get(8);std::memcpy(p,&n,8);}rd.end();return r;
}
void Store::append(uint16_t type,uint64_t seq,const Bytes& payload){auto bytes=frame(type,seq,payload);write_all(fd_,bytes.data(),bytes.size());}
void Store::recover(const Bytes& binding){
  auto bytes=read_file(fd_);size_t pos=0;std::set<uint64_t> acknowledged;
  while(pos<bytes.size()){
    if(bytes.size()-pos<24)break;Reader h(bytes);h.pos=pos;if(h.get(4)!=0x3142574c||h.get(2)!=1)throw std::runtime_error("audit spool schema");auto type=h.get(2),length=h.get(4),seq=h.get(8);
    if(length>65536||bytes.size()-pos<24ULL+length)break;auto end=pos+24+length;Reader checksum(bytes);checksum.pos=end-4;if(checksum.get(4)!=crc32(bytes.data()+pos,20+length))throw std::runtime_error("audit spool checksum");
    Bytes payload(bytes.begin()+pos+20,bytes.begin()+end-4);
    if(pos==0){if(type!=800||seq||payload!=binding)throw std::runtime_error("audit spool binding mismatch");}
    else if(type==801){if(seq!=next_++)throw std::runtime_error("audit spool sequence gap");auto record=decode(payload);if(!known_.insert(record.event_id).second)throw std::runtime_error("duplicate audit event ID");pending_[seq]=std::move(record);}
    else if(type==802){if(length||!pending_.count(seq)||!acknowledged.insert(seq).second)throw std::runtime_error("invalid audit acknowledgment");}
    else throw std::runtime_error("audit spool record type");pos=end;
  }
  if(pos!=bytes.size()){if(ftruncate(fd_,pos))io_error("truncate audit spool");sync_file(fd_);}
  for(auto seq:acknowledged)pending_.erase(seq);
  if(bytes.empty()){append(800,0,binding);sync_file(fd_);sync_parent(path_);}
}
static void require(PGresult* result,ExecStatusType expected,const char* action){if(!result||PQresultStatus(result)!=expected){std::string error=result?PQresultErrorMessage(result):"no result";if(result)PQclear(result);throw std::runtime_error(std::string(action)+": "+error);}PQclear(result);}
static void schema(PGconn* db){
  const char* sql=R"SQL(
CREATE TABLE IF NOT EXISTS orders(event_id text PRIMARY KEY,order_id bigint UNIQUE NOT NULL,cl_ord_id text NOT NULL,client_id text NOT NULL,symbol text NOT NULL,side char(1) NOT NULL,price_ticks bigint NOT NULL,original_qty bigint NOT NULL,status text NOT NULL,created_sequence bigint NOT NULL,created_ordinal bigint NOT NULL,created_time_ns numeric(20,0) NOT NULL);
CREATE TABLE IF NOT EXISTS order_events(event_id text PRIMARY KEY,ordinal bigint NOT NULL,event_index bigint NOT NULL,sequence bigint NOT NULL,order_id bigint NOT NULL,cl_ord_id text NOT NULL,event_type text NOT NULL,quantity bigint NOT NULL,price_ticks bigint NOT NULL,leaves_qty bigint NOT NULL,cumulative_qty bigint NOT NULL,event_time_ns numeric(20,0) NOT NULL,UNIQUE(ordinal,event_index));
CREATE TABLE IF NOT EXISTS trades(event_id text PRIMARY KEY,ordinal bigint NOT NULL,event_index bigint NOT NULL,sequence bigint NOT NULL,buy_order_id bigint NOT NULL,sell_order_id bigint NOT NULL,symbol text NOT NULL,price_ticks bigint NOT NULL,quantity bigint NOT NULL,event_time_ns numeric(20,0) NOT NULL,UNIQUE(ordinal,event_index));
CREATE TABLE IF NOT EXISTS risk_events(event_id text PRIMARY KEY,ordinal bigint UNIQUE NOT NULL,sequence bigint NOT NULL,cl_ord_id text NOT NULL,client_id text NOT NULL,symbol text NOT NULL,side text NOT NULL,decision text NOT NULL,reject_reason text NOT NULL,event_time_ns numeric(20,0) NOT NULL);
)SQL";require(PQexec(db,sql),PGRES_COMMAND_OK,"create audit schema");
}
static std::string n(uint64_t value){return std::to_string(value);}static std::string s(int64_t value){return std::to_string(value);}
static void insert(PGconn* db,const Record& r){
  std::vector<std::string> v;std::string sql;
  if(r.category==Category::Risk){sql="INSERT INTO risk_events VALUES($1,$2,$3,$4,$5,$6,$7,$8,$9,$10) ON CONFLICT(event_id) DO NOTHING";v={r.event_id,n(r.ordinal),n(r.sequence),r.cl_ord_id,r.client,r.symbol,r.side,r.decision,r.reason,n(r.timestamp)};}
  else if(r.category==Category::Order){sql="INSERT INTO orders VALUES($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11,$12) ON CONFLICT(event_id) DO NOTHING";v={r.event_id,n(r.order_id),r.cl_ord_id,r.client,r.symbol,r.side,s(r.price),s(r.quantity),"0",n(r.sequence),n(r.ordinal),n(r.timestamp)};}
  else if(r.category==Category::Trade){sql="INSERT INTO trades VALUES($1,$2,$3,$4,$5,$6,$7,$8,$9,$10) ON CONFLICT(event_id) DO NOTHING";v={r.event_id,n(r.ordinal),n(r.event_index),n(r.sequence),n(r.buy_order_id),n(r.sell_order_id),r.symbol,s(r.price),s(r.quantity),n(r.timestamp)};}
  else {sql=R"SQL(WITH added AS (INSERT INTO order_events VALUES($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11,$12) ON CONFLICT(event_id) DO NOTHING RETURNING 1) UPDATE orders SET cl_ord_id=$6,status=CASE WHEN $7='4' THEN '4' WHEN $10='0' THEN '2' WHEN $11>'0' THEN CASE WHEN $10>'0' THEN '1' ELSE '0' END ELSE status END,original_qty=$8,price_ticks=$9 WHERE order_id=$5 AND EXISTS(SELECT 1 FROM added))SQL";v={r.event_id,n(r.ordinal),n(r.event_index),n(r.sequence),n(r.order_id),r.cl_ord_id,r.event_type,s(r.quantity),s(r.price),s(r.leaves),s(r.cumulative),n(r.timestamp)};}
  std::vector<const char*> values;for(const auto& value:v)values.push_back(value.c_str());require(PQexecParams(db,sql.c_str(),values.size(),nullptr,values.data(),nullptr,nullptr,0),PGRES_COMMAND_OK,"insert audit record");
}
Store::Store(const std::string& directory,const std::string& conninfo,const Bytes& binding):path_(directory+"/audit.spool"),conninfo_(conninfo){
  if(mkdir(directory.c_str(),0700)&&errno!=EEXIST)io_error("mkdir audit");fd_=open(path_.c_str(),O_RDWR|O_CREAT|O_APPEND|O_CLOEXEC|O_NOFOLLOW,0600);if(fd_<0)io_error("open audit spool");
  try{if(flock(fd_,LOCK_EX|LOCK_NB))io_error("lock audit spool");recover(binding);worker_=std::thread([this]{run();});}catch(...){close(fd_);fd_=-1;throw;}
}
Store::~Store(){
  {std::lock_guard<std::mutex> lock(mutex_);stopping_=true;}wake_.notify_all();if(worker_.joinable())worker_.join();if(fd_>=0)close(fd_);
}
void Store::enqueue(std::vector<Record> records) noexcept{
  try{std::lock_guard<std::mutex> lock(mutex_);for(auto& r:records)incoming_.push_back(std::move(r));enqueued_+=records.size();auto depth=incoming_.size()+pending_.size();auto old=max_depth_.load();while(depth>old&&!max_depth_.compare_exchange_weak(old,depth)){}wake_.notify_one();}
  catch(...){dropped_+=records.size();}
}
void Store::run(){
  PGconn* db=nullptr;
  while(true){
    std::deque<Record> arrived;
    std::vector<std::pair<uint64_t,Record>> batch;
    {
      std::unique_lock<std::mutex> lock(mutex_);wake_.wait_for(lock,std::chrono::milliseconds(100),[&]{return stopping_||!incoming_.empty()||!pending_.empty();});
      const bool coalesce=!stopping_&&pending_.empty()&&!incoming_.empty();lock.unlock();
      if(coalesce)std::this_thread::sleep_for(std::chrono::milliseconds(2));lock.lock();
      busy_=true;arrived.swap(incoming_);
    }
    std::vector<std::pair<uint64_t,Record>> staged;
    for(auto& record:arrived)if(known_.insert(record.event_id).second){auto seq=next_++;append(801,seq,encode(record));staged.emplace_back(seq,std::move(record));}
    if(!staged.empty())sync_file(fd_);
    {
      std::lock_guard<std::mutex> lock(mutex_);for(auto& item:staged)pending_.emplace(item.first,std::move(item.second));
      if(stopping_){busy_=false;idle_.notify_all();break;}
      for(auto it=pending_.begin();it!=pending_.end()&&batch.size()<100;++it)batch.push_back(*it);
      if(batch.empty()){busy_=false;idle_.notify_all();continue;}
    }
    if(!db||PQstatus(db)!=CONNECTION_OK){if(db)PQfinish(db);db=PQconnectdb(conninfo_.c_str());reconnects_++;if(PQstatus(db)==CONNECTION_OK){try{schema(db);}catch(...){PQfinish(db);db=nullptr;}}}
    if(!db||PQstatus(db)!=CONNECTION_OK){{std::lock_guard<std::mutex> lock(mutex_);busy_=false;}std::this_thread::sleep_for(std::chrono::milliseconds(100));continue;}
    try{
      auto start=steady_now();require(PQexec(db,"BEGIN"),PGRES_COMMAND_OK,"begin audit batch");for(const auto& item:batch)insert(db,item.second);require(PQexec(db,"COMMIT"),PGRES_COMMAND_OK,"commit audit batch");
      auto micros=(steady_now()-start)/1000;last_commit_us_=micros;auto old=max_commit_us_.load();while(micros>old&&!max_commit_us_.compare_exchange_weak(old,micros)){}
      for(const auto& item:batch)append(802,item.first,{});sync_file(fd_);
      {std::lock_guard<std::mutex> lock(mutex_);for(const auto& item:batch)pending_.erase(item.first);committed_+=batch.size();batches_++;busy_=false;if(incoming_.empty()&&pending_.empty())idle_.notify_all();}
    }catch(...){if(db){PQfinish(db);db=nullptr;}{std::lock_guard<std::mutex> lock(mutex_);busy_=false;}std::this_thread::sleep_for(std::chrono::milliseconds(100));}
  }
  if(db)PQfinish(db);
}
bool Store::wait_idle(uint64_t timeout_ms){std::unique_lock<std::mutex> lock(mutex_);return idle_.wait_for(lock,std::chrono::milliseconds(timeout_ms),[&]{return !busy_&&incoming_.empty()&&pending_.empty();});}
Metrics Store::metrics(){std::lock_guard<std::mutex> lock(mutex_);Metrics m;m.enqueued=enqueued_;m.committed=committed_;m.batches=batches_;m.reconnects=reconnects_;m.max_queue_depth=max_depth_;m.dropped=dropped_;m.last_commit_us=last_commit_us_;m.max_commit_us=max_commit_us_;m.memory_depth=incoming_.size();m.durable_depth=pending_.size();return m;}
} }

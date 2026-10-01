#include "adapter.h"
#include <quickfix/Fields.h>
namespace exchange { namespace fix {
using namespace durable;
static void string(Bytes& b,const std::string& s){if(s.size()>4096)throw std::runtime_error("FIX field too large");put(b,s.size(),4);b.insert(b.end(),s.begin(),s.end());}
static std::string string(Reader& r){auto n=r.get(4);if(n>4096||n>r.bytes.size()-r.pos)throw std::runtime_error("invalid FIX field");std::string s(r.bytes.begin()+r.pos,r.bytes.begin()+r.pos+n);r.pos+=n;return s;}
Bytes encode_request(const Request& q,Timestamp time){
 Bytes b;put(b,time,8);
 for(const auto* s:{&q.client,&q.type,&q.id,&q.original,&q.symbol,&q.side,&q.quantity,&q.order_type,&q.price,&q.tif,&q.account})string(b,*s);
 put(b,q.duplicate,1);return b;
}
std::pair<Request,Timestamp> decode_request(const Bytes& b){
 Reader r(b);auto time=r.get(8);Request q;
 for(auto* s:{&q.client,&q.type,&q.id,&q.original,&q.symbol,&q.side,&q.quantity,&q.order_type,&q.price,&q.tif,&q.account})*s=string(r);
 auto dup=r.get(1);if(dup>1)throw std::runtime_error("invalid PossDupFlag");q.duplicate=dup;r.end();return {q,time};
}
Bytes encode_settings(const Settings& s){
 Bytes b=encode_config(s.risk);put(b,s.symbols.size(),4);
 for(const auto& x:s.symbols){string(b,x.first);put(b,x.second.id,4);put(b,x.second.decimals,4);}
 put(b,s.clients.size(),4);for(const auto& x:s.clients){string(b,x.first);put(b,x.second,8);}return b;
}
static std::string field(const FIX::FieldMap& m,int tag){return m.isSetField(tag)?m.getField(tag):"";}
Request decode(const FIX::Message& m,const std::string& client){
 Request q;q.client=client;q.type=field(m.getHeader(),35);q.id=field(m,11);q.original=field(m,41);
 q.symbol=field(m,55);q.side=field(m,54);q.quantity=field(m,38);q.order_type=field(m,40);
 q.price=field(m,44);q.tif=field(m,59);q.account=field(m,1);q.duplicate=field(m.getHeader(),43)=="Y";
 for(int tag:{11,41,55,54,38,40,44,59,1})if(field(m,tag).size()>4096)throw FIX::IncorrectTagValue(tag);
 return q;
}
static uint64_t scale(unsigned places){if(places>12)throw std::invalid_argument("price precision");uint64_t n=1;while(places--)n*=10;return n;}
static int64_t decimal(const std::string& s,unsigned places){
 if(s.empty()||s.size()>30)throw std::invalid_argument("invalid number");
 uint64_t whole=0,fraction=0;unsigned digits=0;bool dot=false,any=false;
 for(char c:s){
  if(c=='.'&&!dot){dot=true;continue;}
  if(c<'0'||c>'9')throw std::invalid_argument("invalid number");
  any=true;
  if(!dot){if(whole>(uint64_t(INT64_MAX)-(c-'0'))/10)throw std::invalid_argument("numeric overflow");whole=whole*10+c-'0';}
  else if(digits<places){fraction=fraction*10+c-'0';++digits;}
  else if(c!='0')throw std::invalid_argument("off tick or fractional quantity");
 }
 if(!any||s.back()=='.')throw std::invalid_argument("invalid number");
 auto factor=scale(places);while(digits++<places)fraction*=10;
 if(whole>(uint64_t(INT64_MAX)-fraction)/factor)throw std::invalid_argument("numeric overflow");
 return whole*factor+fraction;
}
static std::string formatted(unsigned __int128 n,unsigned places){
 auto factor=scale(places);uint64_t whole=n/factor,rest=n%factor;
 if(!places)return std::to_string(whole);
 std::ostringstream out;out<<whole<<'.'<<std::setfill('0')<<std::setw(places)<<rest;return out.str();
}
static void set(FIX::Message& m,int tag,const std::string& value){m.setField(tag,value);}
static FIX::Message base(const std::string& type){FIX::Message m;m.getHeader().setField(8,"FIX.4.4");m.getHeader().setField(35,type);return m;}
static std::string reason(RejectReason r){
 switch(r){
 case RejectReason::DuplicateOrderId:return "DuplicateOrderId";
 case RejectReason::InvalidQuantity:return "InvalidQuantity";
 case RejectReason::InvalidPrice:return "InvalidPrice";
 case RejectReason::OrderQuantityLimit:return "OrderQuantityLimit";
 case RejectReason::OrderNotionalLimit:return "OrderNotionalLimit";
 case RejectReason::PositionLimit:return "PositionLimit";
 case RejectReason::RateLimit:return "RateLimit";
 case RejectReason::UnknownAccount:return "UnknownAccount";
 case RejectReason::UnknownOrder:return "UnknownOrder";
 case RejectReason::OrderNotOpen:return "OrderNotOpen";
 case RejectReason::InvalidTimestamp:return "InvalidTimestamp";
 default:return "None";
 }
}
Response Adapter::reject(const Request& q,uint64_t ordinal,const std::string& why,const Order* o,OrderId oid){
 if(!o&&q.type!="D"){
  auto it=aliases_.find({q.client,q.original});
  if(it!=aliases_.end()){oid=it->second;o=&orders_.at(oid);}
 }
 auto m=base(q.type=="D"?"8":"9");set(m,37,oid?std::to_string(oid):"NONE");set(m,11,q.id.empty()?"MISSING":q.id);
 set(m,39,o?(o->leaves?(o->cumulative?"1":"0"):(o->cumulative==o->total?"2":"4")):"8");set(m,58,why);
 if(q.type=="D"){
  set(m,17,std::to_string(ordinal)+"-R");set(m,150,"8");set(m,55,q.symbol.empty()?"UNKNOWN":q.symbol);
  set(m,54,q.side=="2"?"2":"1");set(m,151,"0");set(m,14,"0");set(m,6,"0");set(m,103,"99");
 }else{set(m,41,q.original.empty()?"MISSING":q.original);set(m,434,q.type=="F"?"1":"2");set(m,102,why=="UnknownOrder"?"1":"99");}
 return {q.client,m};
}
std::vector<Response> Adapter::process(const Request& q,Timestamp time,uint64_t ordinal){
 events_.clear();auto key=std::make_pair(q.client,q.id);Request normalized=q;normalized.duplicate=false;
 const auto fingerprint=encode_request(normalized,0);
 auto prior=requests_.find(key);
 if(prior!=requests_.end()){
  if(q.duplicate&&prior->second.first==fingerprint)return prior->second.second;
  return {reject(q,ordinal,"DuplicateClOrdID")};
 }
 std::vector<Response> result;
 auto fail=[&](const std::string& why){result={reject(q,ordinal,why)};};
 try{
  auto client=settings_.clients.find(q.client);if(client==settings_.clients.end())throw std::invalid_argument("UnknownClient");
  if(q.account!=std::to_string(client->second))throw std::invalid_argument("UnauthorizedAccount");
  if(q.id.empty()||q.id.size()>64||q.id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-")!=std::string::npos)throw std::invalid_argument("InvalidClOrdID");
  auto symbol=settings_.symbols.find(q.symbol);if(symbol==settings_.symbols.end())throw std::invalid_argument("UnknownSymbol");
  if(q.side!="1"&&q.side!="2")throw std::invalid_argument("InvalidSide");
  if(q.type!="D"&&q.type!="F"&&q.type!="G")throw std::invalid_argument("UnsupportedMessageType");
  if(q.type!="F"&&(q.order_type!="2"||q.tif!="1"))throw std::invalid_argument("OnlyGTCLimitSupported");
  Command c;c.symbol=symbol->second.id;c.account=client->second;c.buy=q.side=="1";c.time=time;
  OrderId oid=ordinal;Order old;bool is_new=q.type=="D";
  if(!is_new){
   auto found=aliases_.find({q.client,q.original});if(found==aliases_.end())throw std::invalid_argument("UnknownOrder");
   oid=found->second;old=orders_.at(oid);
   if(old.current!=q.original||old.symbol!=q.symbol||old.side!=q.side)throw std::invalid_argument("OriginalOrderMismatch");
  }
  c.order=oid;
  if(q.type!="F"){c.quantity=decimal(q.quantity,0);c.price=decimal(q.price,symbol->second.decimals);}
  else {c.type=CommandType::CancelOrder;c.quantity=decimal(q.quantity,0);if(uint64_t(c.quantity)!=old.total)throw std::invalid_argument("CancelQuantityMismatch");}
  if(q.type=="G")c.type=CommandType::ReplaceOrder;
  Order metadata=is_new?Order{}:old;metadata.client=q.client;metadata.current=q.id;metadata.symbol=q.symbol;metadata.side=q.side;metadata.account=q.account;metadata.original=q.original;
  orders_[oid]=metadata;
  auto receipt=execute_(c);
  if(!receipt.result){
   if(is_new)orders_.erase(oid);else orders_[oid]=old;
   result={reject(q,ordinal,reason(receipt.result.reason),is_new?nullptr:&old,is_new?0:oid)};
  }else{
   if(!receipt.acknowledged)throw std::runtime_error("successful FIX command is not durable");
   aliases_[key]=oid;
   size_t index=0;
   for(const auto& e:events_){
    auto& o=orders_.at(e.id);o.total=e.total;o.leaves=e.leaves;o.cumulative=e.cumulative;o.price=e.price;
    if(e.type==OrderEventType::Filled)o.cost+=static_cast<unsigned __int128>(e.last_quantity)*e.last_price;
    auto m=base("8");set(m,37,std::to_string(e.id));set(m,17,std::to_string(ordinal)+"-"+std::to_string(++index));set(m,150,std::string(1,e.type==OrderEventType::Accepted?'0':e.type==OrderEventType::Filled?'F':e.type==OrderEventType::Cancelled?'4':'5'));
    set(m,39,e.type==OrderEventType::Cancelled?"4":(e.leaves?(e.cumulative?"1":"0"):"2"));
    set(m,11,o.current);if(!o.original.empty())set(m,41,o.original);
    set(m,1,o.account);set(m,55,o.symbol);set(m,54,o.side);set(m,38,std::to_string(o.total));set(m,151,std::to_string(o.leaves));set(m,14,std::to_string(o.cumulative));
    unsigned decimals=settings_.symbols.at(o.symbol).decimals;
    set(m,44,formatted(o.price,decimals));set(m,6,o.cumulative?formatted(o.cost*1000000/o.cumulative,decimals+6):"0");
    if(e.type==OrderEventType::Filled){set(m,32,std::to_string(e.last_quantity));set(m,31,formatted(e.last_price,decimals));}
    result.push_back({o.client,m});
   }
  }
 }catch(const std::invalid_argument& e){fail(e.what());}
 if(!q.id.empty())requests_[key]={fingerprint,result};
 return result;
}
} }

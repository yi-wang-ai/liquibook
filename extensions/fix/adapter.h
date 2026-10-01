#pragma once
#include "../durability/segmented.h"
#include <quickfix/Message.h>
#include <map>
namespace exchange { namespace fix {
struct Request {
 std::string client,type,id,original,symbol,side,quantity,order_type,price,tif,account;
 bool duplicate=false;
};
struct Symbol {uint32_t id;unsigned decimals;};
struct Settings {
 durable::Config risk;
 std::map<std::string,Symbol> symbols;
 std::map<std::string,AccountId> clients;
};
struct Response {std::string client;FIX::Message message;};
Request decode(const FIX::Message&,const std::string& client);
durable::Bytes encode_request(const Request&,Timestamp);
std::pair<Request,Timestamp> decode_request(const durable::Bytes&);
durable::Bytes encode_settings(const Settings&);
class Adapter {
 struct Order {
  std::string client,current,symbol,side,account,original;
  Quantity total=0,leaves=0,cumulative=0;
  Money price=0;
  unsigned __int128 cost=0;
 };
 Settings settings_;
 std::map<OrderId,Order> orders_;
 std::map<std::pair<std::string,std::string>,OrderId> aliases_;
 std::map<std::pair<std::string,std::string>,std::pair<durable::Bytes,std::vector<Response>>> requests_;
 std::vector<OrderEvent> events_;
 std::function<durable::Receipt(const durable::Command&)> execute_;
 Response reject(const Request&,uint64_t,const std::string&,const Order* known=nullptr,OrderId oid=0);
public:
 Adapter(const Settings& settings):settings_(settings){}
 void executor(std::function<durable::Receipt(const durable::Command&)> e){execute_=std::move(e);}
 void observe(uint32_t,const OrderEvent& e){events_.push_back(e);}
 std::vector<Response> process(const Request&,Timestamp,uint64_t ordinal);
};
} }

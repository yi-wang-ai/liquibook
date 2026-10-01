#include "service.h"
namespace exchange { namespace fix {
using namespace durable;
void Service::append(uint16_t type,uint64_t seq,const Bytes& payload){
 auto bytes=frame(type,seq,payload);
 auto end=lseek(journal_,0,SEEK_END);if(end<0||uint64_t(end)+bytes.size()>512ULL*1024*1024)throw std::runtime_error("adapter journal size limit");
 write_all(journal_,bytes.data(),bytes.size());sync_file(journal_);
}
std::vector<Service::Item> Service::recover_journal(){
 auto b=read_file(journal_);std::vector<Item> items;
 if(b.empty()){
  if(engine_.sequence())throw std::runtime_error("business WAL exists without adapter journal");
  append(700,0,binding_);return items;
 }
 size_t pos=0;bool config=false;std::set<uint64_t> done;
 while(pos<b.size()){
  if(b.size()-pos<24)break;
  Bytes header(b.begin()+pos,b.begin()+pos+20);Reader r(header);
  if(r.get(4)!=0x3142574c||r.get(2)!=1)throw std::runtime_error("adapter journal schema");
  auto type=r.get(2),length=r.get(4),seq=r.get(8);
  if(length>65536)throw std::runtime_error("adapter journal length");
  if(b.size()-pos<length+24)break;
  Bytes checksum(b.begin()+pos+20+length,b.begin()+pos+24+length);Reader cr(checksum);
  if(cr.get(4)!=crc32(b.data()+pos,20+length))throw std::runtime_error("adapter journal checksum");
  Bytes payload(b.begin()+pos+20,b.begin()+pos+20+length);
  if(!config){if(type!=700||seq||payload!=binding_)throw std::runtime_error("adapter configuration mismatch");config=true;}
  else if(type==701){
   if(seq!=items.size()+1)throw std::runtime_error("adapter ordinal gap");
   auto decoded=decode_request(payload);Item item;item.ordinal=seq;item.request=decoded.first;item.time=decoded.second;items.push_back(std::move(item));
  }else if(type==702){
   if(length||!seq||seq>items.size()||!done.insert(seq).second)throw std::runtime_error("invalid delivery marker");
  }else throw std::runtime_error("adapter journal record type");
  pos+=length+24;
 }
 if(!config)throw std::runtime_error("incomplete adapter configuration");
 if(pos!=b.size()){if(ftruncate(journal_,pos))io_error("truncate adapter journal");sync_file(journal_);}
 for(auto& item:items)item.delivered=done.count(item.ordinal);
 return items;
}
void Service::remember(uint64_t id,std::vector<Response> responses){
 Pending p;p.ordinal=id;p.responses=std::move(responses);p.sent.resize(p.responses.size(),false);pending_.emplace(id,std::move(p));
}
Service::Service(const std::string& dir,const Settings& settings,std::function<bool(const Response&)> send,std::function<Timestamp()> clock,
 Service::Audit audit)
 :settings_(settings),engine_(dir+"/business",settings.risk,100000,Durability::EveryCommand),adapter_(settings),send_(std::move(send)),clock_(std::move(clock)),audit_(std::move(audit)){
 // Caller owns and creates the parent; business WAL holds the exclusive writer lock.
 for(const auto& s:settings_.symbols)if(s.second.decimals>6)throw std::invalid_argument("unsupported tick precision");
 journal_=open((dir+"/adapter.journal").c_str(),O_RDWR|O_CREAT|O_CLOEXEC|O_NOFOLLOW,0600);
 if(journal_<0)io_error("open adapter journal");
 try{
  binding_=engine_.stream_identity();auto encoded=encode_settings(settings_);binding_.insert(binding_.end(),encoded.begin(),encoded.end());
  if(flock(journal_,LOCK_EX|LOCK_NB))io_error("lock adapter journal");
  auto items=recover_journal();sync_parent(dir+"/adapter.journal");
  Exchange shadow("",settings.risk,Durability::NoWal);
  shadow.set_observer([this](uint32_t s,const OrderEvent& e){adapter_.observe(s,e);});
  adapter_.executor([&](const Command& c){
   if(shadow.sequence()==engine_.sequence()&&shadow.canonical_state()!=engine_.canonical_state())throw std::runtime_error("adapter recovery prefix mismatch");
   auto result=shadow.execute(c);
   if(result.result&&shadow.sequence()>engine_.sequence()){
    auto actual=engine_.execute(c);
    if(!actual.acknowledged||actual.applied_sequence!=shadow.sequence())throw std::runtime_error("adapter recovery divergence");
   }
   result.acknowledged=bool(result.result);return result;
  });
  for(const auto& item:items){
   auto before=shadow.sequence();auto reports=adapter_.process(item.request,item.time,item.ordinal);auto after=shadow.sequence();ordinal_=item.ordinal;
   if(audit_)try{audit_(item.request,item.time,item.ordinal,before,after,reports);}catch(...){}
   if(!item.delivered)remember(item.ordinal,std::move(reports));
  }
  if(shadow.canonical_state()!=engine_.canonical_state())throw std::runtime_error("adapter/business recovery mismatch");
  engine_.set_observer([this](uint32_t s,const OrderEvent& e){adapter_.observe(s,e);});
  adapter_.executor([this](const Command& c){auto result=engine_.execute(c);fire("after_business_commit");return result;});
  worker_=std::thread([this]{run();});
 }catch(...){close(journal_);journal_=-1;throw;}
}
Service::~Service(){
 {std::lock_guard<std::mutex> lock(mutex_);stopping_=true;}wake_.notify_all();
 if(worker_.joinable())worker_.join();if(journal_>=0)close(journal_);
}
void Service::run(){
 for(;;){
  std::function<void()> task;
  {std::unique_lock<std::mutex> lock(mutex_);wake_.wait(lock,[&]{return stopping_||!queue_.empty();});
   if(queue_.empty()&&stopping_)return;task=std::move(queue_.front());queue_.pop_front();}
  wake_.notify_all();task();
 }
}
void Service::request(const Request& q,Timestamp time){
 if(failed_)return;
 try{
  if(ordinal_==UINT64_MAX)throw std::runtime_error("adapter ordinal exhausted");
  auto id=ordinal_+1;append(701,id,encode_request(q,time));ordinal_=id;fire("after_adapter_intent");
  auto before=engine_.sequence();auto reports=adapter_.process(q,time,id);auto after=engine_.sequence();
  if(audit_)try{audit_(q,time,id,before,after,reports);}catch(...){}
  remember(id,std::move(reports));deliver();
 }catch(...){failed_=true;}
}
void Service::deliver(){
 if(failed_)return;
 try{
  // Preserve report ordering per client even when another session is offline.
  std::set<std::string> blocked;
  for(auto it=pending_.begin();it!=pending_.end();){
   auto& p=it->second;bool all=true;
   for(size_t i=0;i<p.responses.size();++i)if(!p.sent[i]){
    const auto& response=p.responses[i];
    if(!blocked.count(response.client)&&send_(response))p.sent[i]=true;
    else{blocked.insert(response.client);all=false;}
   }
   if(all){fire("after_report_send");append(702,p.ordinal,{});it=pending_.erase(it);}
   else ++it;
  }
 }catch(...){failed_=true;}
}
void Service::enqueue(const Request& q){
 std::unique_lock<std::mutex> lock(mutex_);
 if(stopping_||failed_||queue_.size()>=4096)throw std::runtime_error("gateway stopping or queue full");
 queue_.push_back([this,q]{try{request(q,clock_?clock_():engine_.now());}catch(...){failed_=true;}});lock.unlock();wake_.notify_one();
}
void Service::retry_delivery(){
 std::lock_guard<std::mutex> lock(mutex_);if(!stopping_){queue_.push_back([this]{deliver();});wake_.notify_one();}
}
std::string Service::state(){
 auto promise=std::make_shared<std::promise<std::string>>();auto future=promise->get_future();
 {std::lock_guard<std::mutex> lock(mutex_);queue_.push_back([this,promise]{
  if(failed_)promise->set_exception(std::make_exception_ptr(std::runtime_error("gateway halted")));
  else promise->set_value(engine_.canonical_state());
 });}wake_.notify_one();return future.get();
}
uint64_t Service::sequence(){
 auto promise=std::make_shared<std::promise<uint64_t>>();auto future=promise->get_future();
 {std::lock_guard<std::mutex> lock(mutex_);queue_.push_back([this,promise]{if(failed_)promise->set_exception(std::make_exception_ptr(std::runtime_error("gateway halted")));else promise->set_value(engine_.sequence());});}
 wake_.notify_one();return future.get();
}
void Service::drain(){sequence();}
void Service::set_hook(std::function<void(const char*)> hook){
 drain();auto promise=std::make_shared<std::promise<void>>();auto future=promise->get_future();
 {std::lock_guard<std::mutex> lock(mutex_);queue_.push_back([this,hook,promise]{hook_=hook;promise->set_value();});}wake_.notify_one();future.get();
}
} }

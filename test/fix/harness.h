#pragma once
#include "../../extensions/fix/application.h"
#include "../../extensions/fix/demo_settings.h"
#include <quickfix/SocketAcceptor.h>
#include <quickfix/SocketInitiator.h>
#include <quickfix/FileStore.h>
#include <quickfix/SessionSettings.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <fstream>
using namespace exchange;
namespace fx=exchange::fix;
inline std::string value(const FIX::Message& m,int tag){return m.isSetField(tag)?m.getField(tag):"";}
inline fx::Settings fix_settings(){return fx::demo_settings();}
inline void erase_owned_tree(const std::string& path){
 DIR* d=opendir(path.c_str());if(!d)return;
 while(auto e=readdir(d)){std::string n=e->d_name;if(n=="."||n=="..")continue;auto p=path+"/"+n;
  struct stat st{};if(lstat(p.c_str(),&st)==0&&S_ISDIR(st.st_mode))erase_owned_tree(p);else unlink(p.c_str());}
 closedir(d);rmdir(path.c_str());
}
struct Client : FIX::Application {
 struct Entry {std::string client;FIX::Message message;};
 std::mutex mutex;std::condition_variable changed;std::vector<Entry> messages,admin;unsigned logons=0;
 void onCreate(const FIX::SessionID&) override {}
 void onLogon(const FIX::SessionID&) override {std::lock_guard<std::mutex> l(mutex);++logons;changed.notify_all();}
 void onLogout(const FIX::SessionID&) override {}
 void toAdmin(FIX::Message&,const FIX::SessionID&) override {}
 void toApp(FIX::Message&,const FIX::SessionID&) throw(FIX::DoNotSend) override {}
 void fromAdmin(const FIX::Message& m,const FIX::SessionID& id) throw(FIX::FieldNotFound,FIX::IncorrectDataFormat,FIX::IncorrectTagValue,FIX::RejectLogon) override {
  std::lock_guard<std::mutex> l(mutex);admin.push_back({id.getSenderCompID().getValue(),m});changed.notify_all();
 }
 void fromApp(const FIX::Message& m,const FIX::SessionID& id) throw(FIX::FieldNotFound,FIX::IncorrectDataFormat,FIX::IncorrectTagValue,FIX::UnsupportedMessageType) override {
  std::lock_guard<std::mutex> l(mutex);messages.push_back({id.getSenderCompID().getValue(),m});changed.notify_all();
 }
 void wait_logons(unsigned count){
  std::unique_lock<std::mutex> l(mutex);if(!changed.wait_for(l,std::chrono::seconds(10),[&]{return logons>=count;}))throw std::runtime_error("FIX logon timeout");
 }
 FIX::Message wait(const std::string& id,const std::string& exec="",const std::string& client="C1",const std::string& cumulative=""){
  std::unique_lock<std::mutex> l(mutex);FIX::Message found;
  auto check=[&]{for(auto it=messages.rbegin();it!=messages.rend();++it)if(it->client==client&&value(it->message,11)==id&&(exec.empty()||value(it->message,150)==exec)&&(cumulative.empty()||value(it->message,14)==cumulative)){found=it->message;return true;}return false;};
  if(!changed.wait_for(l,std::chrono::seconds(10),check))throw std::runtime_error("FIX report timeout: "+id+" exec="+exec);
  return found;
 }
};
struct Network {
 std::string dir;fx::Settings config;Client client;
 std::unique_ptr<fx::Service> service;std::unique_ptr<fx::Application> app;
 std::unique_ptr<FIX::SessionSettings> accept_settings,init_settings;
 std::unique_ptr<FIX::FileStoreFactory> accept_store,init_store;
 std::unique_ptr<FIX::SocketAcceptor> acceptor;
 std::unique_ptr<FIX::SocketInitiator> initiator;
 explicit Network(fx::Settings settings=fix_settings(),bool deterministic=true,fx::Service::Audit audit={}):config(std::move(settings)){
  char name[]="/tmp/liquibook-fix-test-XXXXXX";auto p=mkdtemp(name);if(!p)throw std::runtime_error("mkdtemp");dir=p;
  mkdir((dir+"/accept").c_str(),0700);mkdir((dir+"/client").c_str(),0700);
  service.reset(new fx::Service(dir,config,[](const fx::Response& r){return fx::send(r);},deterministic?std::function<Timestamp()>([]{return 1000000000ULL;}):std::function<Timestamp()>(),std::move(audit)));
  app.reset(new fx::Application(*service));
  int fd=socket(AF_INET,SOCK_STREAM,0);sockaddr_in addr{};addr.sin_family=AF_INET;addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
  if(bind(fd,reinterpret_cast<sockaddr*>(&addr),sizeof(addr)))throw std::runtime_error("bind test port");
  socklen_t size=sizeof(addr);getsockname(fd,reinterpret_cast<sockaddr*>(&addr),&size);auto port=ntohs(addr.sin_port);close(fd);
  std::string common="\nStartTime=00:00:00\nEndTime=00:00:00\nHeartBtInt=30\nReconnectInterval=1\nSocketNodelay=Y\nUseDataDictionary=Y\nDataDictionary=/opt/quickfix/FIX44.xml\nResetOnLogon=N\nResetOnLogout=N\nResetOnDisconnect=N\nPersistMessages=Y\nLogoutTimeout=1\n";
  std::string a="[DEFAULT]\nConnectionType=acceptor\nSocketAcceptHost=127.0.0.1\nSocketAcceptPort="+std::to_string(port)+"\nFileStorePath="+dir+"/accept"+common;
  std::string i="[DEFAULT]\nConnectionType=initiator\nSocketConnectHost=127.0.0.1\nSocketConnectPort="+std::to_string(port)+"\nFileStorePath="+dir+"/client"+common;
  for(const auto& c:config.clients){
   a+="[SESSION]\nBeginString=FIX.4.4\nSenderCompID=EXCHANGE\nTargetCompID="+c.first+"\n";
   i+="[SESSION]\nBeginString=FIX.4.4\nSenderCompID="+c.first+"\nTargetCompID=EXCHANGE\n";
  }
  std::istringstream as(a),is(i);accept_settings.reset(new FIX::SessionSettings(as));init_settings.reset(new FIX::SessionSettings(is));
  accept_store.reset(new FIX::FileStoreFactory(*accept_settings));init_store.reset(new FIX::FileStoreFactory(*init_settings));
  acceptor.reset(new FIX::SocketAcceptor(*app,*accept_store,*accept_settings));initiator.reset(new FIX::SocketInitiator(client,*init_store,*init_settings));
  acceptor->start();initiator->start();client.wait_logons(config.clients.size());
 }
 ~Network(){
  if(initiator)initiator->stop(true);if(acceptor)acceptor->stop(true);
  if(service){try{service->drain();}catch(...){}}
  initiator.reset();acceptor.reset();app.reset();service.reset();erase_owned_tree(dir);
 }
 FIX::Session* session(const std::string& c="C1"){return FIX::Session::lookupSession(FIX::SessionID("FIX.4.4",c,"EXCHANGE"));}
 void send(FIX::Message m,const std::string& c="C1"){if(!FIX::Session::sendToTarget(m,FIX::SessionID("FIX.4.4",c,"EXCHANGE")))throw std::runtime_error("client send failed");}
 void reconnect(){
  unsigned before;{std::lock_guard<std::mutex> l(client.mutex);before=client.logons;}
  initiator->stop(true);initiator.reset();
  initiator.reset(new FIX::SocketInitiator(client,*init_store,*init_settings));initiator->start();client.wait_logons(before+config.clients.size());
 }
 FIX::Message order(std::string id,std::string side="1",std::string qty="10",std::string price="100.00",std::string account="1"){
  FIX::Message m;m.getHeader().setField(8,"FIX.4.4");m.getHeader().setField(35,"D");
  for(auto x:std::vector<std::pair<int,std::string>>{{11,id},{1,account},{55,"ESZ6"},{54,side},{38,qty},{40,"2"},{44,price},{59,"1"}})m.setField(x.first,x.second);
  m.setField(FIX::TransactTime());return m;
 }
 FIX::Message cancel(std::string id,std::string original){
  auto m=order(id);m.getHeader().setField(35,"F");m.setField(41,original);m.removeField(40);m.removeField(44);m.removeField(59);return m;
 }
 FIX::Message replace(std::string id,std::string original,std::string qty="20",std::string price="101.00"){
  auto m=order(id,"1",qty,price);m.getHeader().setField(35,"G");m.setField(41,original);return m;
 }
 FIX::Message submit(FIX::Message m,const std::string& c="C1",const std::string& exec=""){
  auto id=value(m,11);send(m,c);auto r=client.wait(id,exec,c);service->drain();return r;
 }
};

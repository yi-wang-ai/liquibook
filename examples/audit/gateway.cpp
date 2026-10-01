#include "../../extensions/audit/audit.h"
#include "../../extensions/fix/application.h"
#include "../../extensions/fix/demo_settings.h"
#include <quickfix/SocketAcceptor.h>
#include <quickfix/FileStore.h>
#include <csignal>
#include <iostream>

namespace { volatile std::sig_atomic_t stopped=0; void signal_handler(int){stopped=1;} }

int main(int argc,char** argv){
  if(argc!=4){std::cerr<<"Usage: audit_gateway SETTINGS.cfg STATE_DIRECTORY POSTGRES_CONNINFO\n";return 2;}
  try{
    const std::string root=argv[2],engine=root+"/gateway",audit_path=root+"/audit";
    if(mkdir(root.c_str(),0700)&&errno!=EEXIST)exchange::durable::io_error("mkdir state");
    if(mkdir(engine.c_str(),0700)&&errno!=EEXIST)exchange::durable::io_error("mkdir gateway");
    auto settings=exchange::fix::demo_settings();
    exchange::audit::Store audit(audit_path,argv[3],exchange::fix::encode_settings(settings));
    exchange::fix::Service service(engine,settings,[](const exchange::fix::Response& r){return exchange::fix::send(r);},{},
      [&audit](const exchange::fix::Request& q,exchange::Timestamp time,uint64_t ordinal,uint64_t before,uint64_t after,const std::vector<exchange::fix::Response>& responses){
        audit.enqueue(exchange::audit::derive(q,time,ordinal,before,after,responses));
      });
    exchange::fix::Application app(service);FIX::SessionSettings session_settings(argv[1]);FIX::FileStoreFactory store(session_settings);
    FIX::SocketAcceptor acceptor(app,store,session_settings);std::signal(SIGINT,signal_handler);std::signal(SIGTERM,signal_handler);
    acceptor.start();std::cout<<"FIX 4.4 gateway with asynchronous PostgreSQL audit running\n";
    while(!stopped){std::this_thread::sleep_for(std::chrono::milliseconds(200));service.drain();}
    acceptor.stop(true);service.drain();
    if(!audit.wait_idle(30000))std::cerr<<"Audit backlog retained in durable spool\n";
    return 0;
  }catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}
}

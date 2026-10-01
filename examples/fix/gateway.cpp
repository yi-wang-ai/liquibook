#include "../../extensions/fix/application.h"
#include "../../extensions/fix/demo_settings.h"
#include <quickfix/SocketAcceptor.h>
#include <quickfix/FileStore.h>
#include <csignal>
#include <iostream>
namespace {volatile std::sig_atomic_t stop=0;void signal_handler(int){stop=1;}}
int main(int argc,char** argv){
 if(argc!=3){std::cerr<<"Usage: fix_gateway SETTINGS.cfg STATE_DIRECTORY\n";return 2;}
 try{
  if(mkdir(argv[2],0700)&&errno!=EEXIST)exchange::durable::io_error("mkdir gateway");
  exchange::fix::Service service(argv[2],exchange::fix::demo_settings(),[](const exchange::fix::Response& r){return exchange::fix::send(r);});
  exchange::fix::Application app(service);FIX::SessionSettings settings(argv[1]);FIX::FileStoreFactory store(settings);
  FIX::SocketAcceptor acceptor(app,store,settings);std::signal(SIGINT,signal_handler);std::signal(SIGTERM,signal_handler);
  acceptor.start();std::cout<<"FIX 4.4 gateway running\n";
  while(!stop){std::this_thread::sleep_for(std::chrono::milliseconds(200));service.drain();}
  acceptor.stop(true);service.drain();return 0;
 }catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}
}

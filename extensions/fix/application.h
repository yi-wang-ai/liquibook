#pragma once
#include "service.h"
#include <quickfix/Application.h>
#include <quickfix/Session.h>
namespace exchange { namespace fix {
class Application : public FIX::Application {
 Service& service_;
public:
 explicit Application(Service& service):service_(service){}
 void onCreate(const FIX::SessionID&) override {}
 void onLogon(const FIX::SessionID&) override {service_.retry_delivery();}
 void onLogout(const FIX::SessionID&) override {}
 void toAdmin(FIX::Message&,const FIX::SessionID&) override {}
 void toApp(FIX::Message&,const FIX::SessionID&) throw(FIX::DoNotSend) override {}
 void fromAdmin(const FIX::Message&,const FIX::SessionID&) throw(FIX::FieldNotFound,FIX::IncorrectDataFormat,FIX::IncorrectTagValue,FIX::RejectLogon) override {}
 void fromApp(const FIX::Message& m,const FIX::SessionID& session)
 throw(FIX::FieldNotFound,FIX::IncorrectDataFormat,FIX::IncorrectTagValue,FIX::UnsupportedMessageType) override {
  auto q=decode(m,session.getTargetCompID().getValue());
  if(q.type!="D"&&q.type!="F"&&q.type!="G")throw FIX::UnsupportedMessageType();
  // Preserve QuickFIX's dynamic exception contract; never acknowledge enqueue failure.
  try{service_.enqueue(q);}catch(...){
   FIX::Message reject;reject.getHeader().setField(8,"FIX.4.4");reject.getHeader().setField(35,"j");
   reject.setField(372,q.type);reject.setField(380,"4");reject.setField(58,"Gateway unavailable or queue full");
   if(!q.id.empty())reject.setField(379,q.id);
   if(m.getHeader().isSetField(34))reject.setField(45,m.getHeader().getField(34));
   try{FIX::Session::sendToTarget(reject,session);}catch(...){}
  }
 }
};
inline bool send(const Response& response,const std::string& server="EXCHANGE"){
 FIX::SessionID id("FIX.4.4",server,response.client);
 auto session=FIX::Session::lookupSession(id);
 if(!session||!session->isLoggedOn())return false;
 auto message=response.message;return FIX::Session::sendToTarget(message,id);
}
} }

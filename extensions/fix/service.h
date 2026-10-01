#pragma once
#include "adapter.h"
#include <condition_variable>
#include <atomic>
#include <deque>
#include <future>
#include <mutex>
#include <thread>
namespace exchange { namespace fix {
class Service {
public:
 using Audit=std::function<void(const Request&,Timestamp,uint64_t,uint64_t,uint64_t,const std::vector<Response>&)>;
private:
 struct Pending {uint64_t ordinal;std::vector<Response> responses;std::vector<bool> sent;};
 struct Item {Request request;Timestamp time;uint64_t ordinal;bool delivered=false;};
 Settings settings_;
 durable::SegmentedExchange engine_;
 Adapter adapter_;
 int journal_=-1;
 durable::Bytes binding_;
 uint64_t ordinal_=0;
 std::map<uint64_t,Pending> pending_;
 std::function<bool(const Response&)> send_;
 std::function<Timestamp()> clock_;
 Audit audit_;
 std::function<void(const char*)> hook_;
 std::mutex mutex_;
 std::condition_variable wake_;
 std::deque<std::function<void()>> queue_;
 bool stopping_=false;
 std::atomic<bool> failed_{false};
 std::thread worker_;
 void append(uint16_t,uint64_t,const durable::Bytes&);
 std::vector<Item> recover_journal();
 void run();
 void deliver();
 void remember(uint64_t,std::vector<Response>);
 void request(const Request&,Timestamp);
 void fire(const char* stage){if(hook_)hook_(stage);}
public:
 Service(const std::string& directory,const Settings&,std::function<bool(const Response&)> send,
         std::function<Timestamp()> clock={},
         Audit audit={});
 ~Service();
 void enqueue(const Request&); // bounded nonblocking enqueue; worker owns every engine mutation
 void retry_delivery();
 std::string state();
 uint64_t sequence();
 void drain();
 void set_hook(std::function<void(const char*)> hook);
};
} }

#include "segmented.h"
namespace exchange { namespace durable {
std::vector<uint64_t> SegmentedExchange::inventory(const char* prefix,const char* suffix)const{
 DIR* d=opendir(dir_.c_str());if(!d)io_error("opendir");
 std::vector<uint64_t> result;const std::string pre=prefix,suf=suffix;
 while(auto e=readdir(d)){std::string n=e->d_name;
  if(n.size()!=pre.size()+20+suf.size()||n.compare(0,pre.size(),pre)||n.compare(n.size()-suf.size(),suf.size(),suf))continue;
  auto digits=n.substr(pre.size(),20);if(digits.find_first_not_of("0123456789")!=std::string::npos)continue;
  try{result.push_back(std::stoull(digits));}catch(...){closedir(d);throw;}
 }closedir(d);std::sort(result.begin(),result.end());return result;
}
Bytes SegmentedExchange::read(const std::string& path,uint64_t offset,uint64_t size){
 int fd=open(path.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW);if(fd<0)io_error("open recovery file");
 try{
  struct stat st{};if(fstat(fd,&st))io_error("stat recovery file");
  if(!S_ISREG(st.st_mode)||st.st_size<0||uint64_t(st.st_size)>512ULL*1024*1024||offset>uint64_t(st.st_size))
   throw std::runtime_error("invalid recovery file size");
  size=std::min(size,uint64_t(st.st_size)-offset);Bytes b(size);size_t done=0;
  while(done<size){auto n=pread(fd,b.data()+done,size-done,offset+done);
   if(n<0&&errno==EINTR)continue;if(n<=0)io_error("pread");
   done+=n;if(recording_recovery_)stats_.bytes_read+=n;}
  close(fd);return b;
 }catch(...){close(fd);throw;}
}
Bytes SegmentedExchange::unpack(const Bytes& b,uint16_t type,uint64_t* seq)const{
 if(b.size()<24)throw std::runtime_error("short frame");
 Reader r(b);
 if(r.get(4)!=0x3142574c||r.get(2)!=1||r.get(2)!=type)throw std::runtime_error("frame schema");
 auto length=r.get(4),sequence=r.get(8);
 if(length!=b.size()-24)throw std::runtime_error("frame length");
 Bytes payload(b.begin()+20,b.end()-4);r.pos=b.size()-4;
 if(r.get(4)!=crc32(b.data(),b.size()-4))throw std::runtime_error("frame checksum");
 if(seq)*seq=sequence;return payload;
}
void SegmentedExchange::publish(const std::string& path,const Bytes& bytes,bool hooks){
 const auto temp=path+".tmp";
 // Only owned temporary names are reused, never followed or truncated in place.
 if(unlink(temp.c_str())&&errno!=ENOENT)io_error("remove stale temporary");
 int fd=open(temp.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);if(fd<0)io_error("create temporary");
 try{
  write_all(fd,bytes.data(),bytes.size());if(hooks)fire("snapshot_after_write");
  sync_file(fd);if(hooks)fire("snapshot_after_sync");
  if(close(fd)){fd=-1;io_error("close temporary");}fd=-1;
  if(rename(temp.c_str(),path.c_str()))io_error("publish");
  if(hooks)fire("snapshot_after_rename");sync_parent(path);
  if(hooks)fire("snapshot_after_directory_sync");
 }catch(...){if(fd>=0)close(fd);throw;}
}
Bytes SegmentedExchange::segment_header(uint64_t id)const{
 if(!id||id-1>UINT64_MAX/capacity_)throw std::runtime_error("segment overflow");
 Bytes b=identity_;put(b,id,8);put(b,capacity_,8);return frame(512,(id-1)*capacity_,b);
}
void SegmentedExchange::check_header(uint64_t id){
 if(read(wal(id),0,header_size)!=segment_header(id))throw std::runtime_error("segment identity/header mismatch");
}
void SegmentedExchange::create_segment(uint64_t id){
 if(access(wal(id).c_str(),F_OK)==0)throw std::runtime_error("segment already exists");
 publish(wal(id),segment_header(id));
}
void SegmentedExchange::open_current(){
 if(fd_>=0)close(fd_);
 fd_=open(wal(segment_).c_str(),O_RDWR|O_APPEND|O_CLOEXEC|O_NOFOLLOW);if(fd_<0)io_error("open active segment");
}
SegmentedExchange::SegmentedExchange(const std::string& dir,const Config& config,uint64_t capacity,Durability mode,uint32_t group,bool full)
 :config_(config),dir_(dir),mode_(mode),capacity_(capacity),group_(group){
 if(!capacity||capacity>1000000||!group||mode==Durability::NoWal)throw std::invalid_argument("invalid segmented options");
 engine_.reset(new Exchange("",config_,Durability::NoWal));
 if(mkdir(dir.c_str(),0700)&&errno!=EEXIST)io_error("mkdir WAL");
 lock_=open((dir+"/LOCK").c_str(),O_RDWR|O_CREAT|O_CLOEXEC|O_NOFOLLOW,0600);if(lock_<0)io_error("open lock");
 try{
  if(flock(lock_,LOCK_EX|LOCK_NB))io_error("lock directory");
  const auto start=steady_now();const auto manifest=dir+"/MANIFEST";
  if(access(manifest.c_str(),F_OK)!=0){
   if(!inventory("wal-",".log").empty()||!inventory("snapshot-",".bin").empty())throw std::runtime_error("missing manifest");
   identity_.resize(16);int random=open("/dev/urandom",O_RDONLY|O_CLOEXEC);if(random<0)io_error("random");
   size_t done=0;while(done<16){auto n=::read(random,identity_.data()+done,16-done);if(n<0&&errno==EINTR)continue;if(n<=0){close(random);io_error("random read");}done+=n;}close(random);
   Bytes b=identity_;put(b,capacity_,8);auto c=encode_config(config_);b.insert(b.end(),c.begin(),c.end());
   create_segment(1);publish(manifest,frame(514,0,b));sync_parent(dir_);
  }
  auto b=unpack(read(manifest),514);Reader r(b);
  if(b.size()<24)throw std::runtime_error("short manifest");
  identity_=Bytes(b.begin(),b.begin()+16);r.pos=16;
  if(r.get(8)!=capacity_||Bytes(b.begin()+24,b.end())!=encode_config(config_))throw std::runtime_error("manifest config mismatch");
  if(inventory("wal-",".log").empty()){
   throw std::runtime_error("missing all segments");
  }
  recover(full);stats_.total_ms=(steady_now()-start)/1e6;recording_recovery_=false;
 }catch(...){if(fd_>=0){close(fd_);fd_=-1;}close(lock_);lock_=-1;throw;}
}
std::unique_ptr<Exchange> SegmentedExchange::load_snapshot(uint64_t seq,uint64_t& id,uint64_t& offset){
 uint64_t actual=0;auto b=unpack(read(snapshot(seq)),513,&actual);
 if(actual!=seq||b.size()<32||!std::equal(identity_.begin(),identity_.end(),b.begin()))throw std::runtime_error("snapshot identity");
 Reader r(b);r.pos=16;id=r.get(8);offset=r.get(8);
 if(id!=seq/capacity_+1||offset!=header_size+(seq%capacity_)*record_size)throw std::runtime_error("snapshot anchor");
 check_header(id);
 struct stat st{};if(stat(wal(id).c_str(),&st)||uint64_t(st.st_size)<offset)throw std::runtime_error("snapshot anchor beyond WAL");
 std::unique_ptr<Exchange> x(new Exchange("",config_,Durability::NoWal));
 x->load_checkpoint_state(Bytes(b.begin()+32,b.end()),seq);return x;
}
void SegmentedExchange::recover(bool full){
 uint64_t id=1,offset=header_size;auto start=steady_now();
 if(!full){auto snaps=inventory("snapshot-",".bin");
  for(auto it=snaps.rbegin();it!=snaps.rend();++it){
   try{uint64_t candidate_id,candidate_offset;auto candidate=load_snapshot(*it,candidate_id,candidate_offset);
    engine_=std::move(candidate);id=candidate_id;offset=candidate_offset;stats_.snapshot_sequence=*it;break;
   }catch(const std::runtime_error&){/* Try the previous durable generation; never ignore tail corruption. */}
  }
 }
 stats_.snapshot_ms=(steady_now()-start)/1e6;start=steady_now();
 auto segments=inventory("wal-",".log");
 auto it=std::lower_bound(segments.begin(),segments.end(),id);
 if(it==segments.end()||*it!=id)throw std::runtime_error("required segment missing");
 for(;it!=segments.end();++it){
  if(*it!=id)throw std::runtime_error("segment gap");
  check_header(id);auto b=read(wal(id),offset);size_t pos=0;
  bool last=it+1==segments.end();
  if(offset+b.size()>header_size+capacity_*record_size)throw std::runtime_error("segment exceeds capacity");
  while(pos<b.size()){
   if(b.size()-pos<record_size){
    if(!last)throw std::runtime_error("truncated sealed segment");
    int fd=open(wal(id).c_str(),O_WRONLY|O_CLOEXEC|O_NOFOLLOW);if(fd<0)io_error("open truncate");
    if(ftruncate(fd,offset+pos)){close(fd);io_error("truncate tail");}
    try{sync_file(fd);}catch(...){close(fd);throw;}close(fd);break;
   }
   Bytes bytes(b.begin()+pos,b.begin()+pos+record_size);Reader header(bytes);header.pos=6;auto type=header.get(2);
   uint64_t seq;auto payload=unpack(bytes,type,&seq);
   if(type<1||type>3||seq!=engine_->sequence()+1)throw std::runtime_error("tail sequence/type");
   auto c=decode_command(type,payload);++stats_.records_decoded;
   if(!engine_->execute(c).result)throw std::runtime_error("replay rejected");
   pos+=record_size;
  }
  if(!last&&offset+pos!=header_size+capacity_*record_size)throw std::runtime_error("incomplete sealed segment");
  segment_=id;++id;offset=header_size;
 }
 open_current();sync_file(fd_);durable_=engine_->sequence();
 engine_->clock_origin_=steady_now();engine_->clock_base_=engine_->logical_floor_;
 stats_.tail_ms=(steady_now()-start)/1e6;
}
Receipt SegmentedExchange::execute(const Command& c){
 if(halted_)throw std::runtime_error("segmented exchange halted");
 auto result=engine_->validate(c);if(!result)return {result,sequence(),durable_,false};
 if(sequence()==UINT64_MAX)throw std::overflow_error("sequence exhausted");
 try{
  uint64_t required=sequence()/capacity_+1;
  if(required!=segment_){
   flush();create_segment(required);segment_=required;open_current();fire("after_segment_rotation");
  }
  auto bytes=frame(static_cast<uint16_t>(c.type),sequence()+1,encode_command(c));
  fire("after_risk");
  if(hook_){write_all(fd_,bytes.data(),bytes.size()/2);fire("during_wal_write");write_all(fd_,bytes.data()+bytes.size()/2,bytes.size()-bytes.size()/2);}
  else write_all(fd_,bytes.data(),bytes.size());
  fire("after_wal_write");
  if(mode_==Durability::EveryCommand||(mode_==Durability::Group&&(sequence()+1)%group_==0)){
   sync_file(fd_);durable_=sequence()+1;fire("after_sync");
  }
  auto applied=engine_->execute(c);if(!applied.result)throw std::runtime_error("application rejected");
  fire("after_match");return {applied.result,sequence(),durable_,durable_>=sequence()};
 }catch(...){halted_=true;throw;}
}
uint64_t SegmentedExchange::flush(){
 if(halted_)throw std::runtime_error("segmented exchange halted");
 try{sync_file(fd_);durable_=sequence();return durable_;}catch(...){halted_=true;throw;}
}
void SegmentedExchange::save_snapshot(bool retire){
 if(halted_)throw std::runtime_error("segmented exchange halted");
 try{
  flush();
  const auto id=sequence()/capacity_+1,offset=header_size+(sequence()%capacity_)*record_size;
  if(id!=segment_){create_segment(id);segment_=id;open_current();}
  Bytes payload=identity_;put(payload,id,8);put(payload,offset,8);
  auto state=engine_->checkpoint_state();payload.insert(payload.end(),state.begin(),state.end());
  if(payload.size()>512ULL*1024*1024-24)throw std::runtime_error("snapshot too large");
  publish(snapshot(sequence()),frame(513,sequence(),payload),true);
  uint64_t verified_id,verified_offset;
  if(load_snapshot(sequence(),verified_id,verified_offset)->canonical_state()!=canonical_state())throw std::runtime_error("snapshot verification failed");
  fire("snapshot_verified");
  if(!retire)return;
  auto snaps=inventory("snapshot-",".bin");std::vector<uint64_t> valid;
  for(auto it=snaps.rbegin();it!=snaps.rend()&&valid.size()<2;++it){
   try{load_snapshot(*it,verified_id,verified_offset);valid.push_back(*it);}catch(const std::runtime_error&){}
  }
  // With only one valid snapshot keep all WAL for full-replay fallback.
  if(valid.size()<2)return;
  const uint64_t oldest=valid.back(),keep_segment=oldest/capacity_+1;
  for(auto seq:snaps)if(seq<oldest){if(unlink(snapshot(seq).c_str()))io_error("retire snapshot");}
  sync_parent(snapshot(sequence()));fire("before_segment_deletion");
  for(auto seg:inventory("wal-",".log"))if(seg<keep_segment){
   if(unlink(wal(seg).c_str()))io_error("retire segment");fire("during_segment_deletion");
  }
  sync_parent(wal(segment_));fire("after_segment_deletion");
 }catch(...){halted_=true;throw;}
}
} }

#include "fzvs_session.hpp"
#include "fzvs_protocol.h"
#include <algorithm>
#include <arpa/inet.h>
#include <chrono>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <functional>
#include <ifaddrs.h>
#include <memory>
#include <net/if.h>
#include <netdb.h>
#include <set>
#include <system_error>
#include <sys/socket.h>
#include <unistd.h>

namespace fzvs {
Session session;
Startup startup;
static uint64_t clockMs() { return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
static uint64_t randomNonce() { uint64_t n=0; int fd=open("/dev/urandom",O_RDONLY); if(fd>=0) { if(read(fd,&n,8)!=8)n=0; close(fd); } return n; }
static std::vector<in_addr> interfaces() {
  std::set<uint32_t> unique; ifaddrs *list=nullptr;
  if(getifaddrs(&list)==0) {
    for(auto p=list;p;p=p->ifa_next) if(p->ifa_addr && p->ifa_addr->sa_family==AF_INET && (p->ifa_flags&IFF_UP) && !(p->ifa_flags&IFF_POINTOPOINT))
      unique.insert(((sockaddr_in*)p->ifa_addr)->sin_addr.s_addr);
    freeifaddrs(list);
  }
  unique.insert(htonl(INADDR_LOOPBACK)); std::vector<in_addr> out;
  for(auto n:unique) out.push_back(in_addr{n}); return out;
}
static int udp() { int fd=socket(AF_INET,SOCK_DGRAM,0); if(fd>=0 && fcntl(fd,F_SETFL,O_NONBLOCK)<0) {close(fd);return -1;} return fd; }
static void closeSocket(int& fd) { if(fd>=0)close(fd);fd=-1; }
struct Discovery {
  int listener=-1,query=-1,responder=-1; uint64_t lastQuery=0,nonce=0;
  std::vector<in_addr> networks=interfaces();
  DiscoveryStatus status;
  std::vector<DiscoveryFailure> hostFailures,browseFailures;
  uint64_t browseStarted=0,lastListen=0;
  std::function<std::vector<in_addr>()> enumerate=interfaces;
  std::function<ssize_t(int,const void*,size_t,int,const sockaddr*,socklen_t)> sendDatagram=::sendto;
  void failure(std::vector<DiscoveryFailure>& errors,const char* operation,std::string interface,int code=errno) {
    if(errors.size()<16)errors.push_back({operation,std::move(interface),strerror(code),code});
  }
  void retry() {
    closeSocket(listener);closeSocket(responder);closeSocket(query);
    networks=enumerate();lastQuery=lastListen=browseStarted=0;status={};hostFailures.clear();browseFailures.clear();
  }
  void publish(bool browsing,const std::vector<Room>& rooms) {
    status.failures=hostFailures;status.failures.insert(status.failures.end(),browseFailures.begin(),browseFailures.end());
    if(browsing) {
      if(!rooms.empty())status.phase=DiscoveryPhase::Rooms;
      else if(query<0 && !browseFailures.empty())status.phase=DiscoveryPhase::Unavailable;
      else status.phase=clockMs()-browseStarted<2000?DiscoveryPhase::Searching:DiscoveryPhase::Empty;
    } else status.phase=hostFailures.empty()?DiscoveryPhase::Idle:DiscoveryPhase::Unavailable;
  }
  ~Discovery(){closeSocket(listener);closeSocket(query);closeSocket(responder);}
  bool listen(std::string& error,const std::string& bindAddress) {
    if(listener>=0)return true;
    networks=enumerate();hostFailures.clear();lastListen=clockMs();
    listener=udp(); int yes=1;
    if(listener<0) {failure(hostFailures,"socket","discovery listener");error="LAN discovery socket unavailable; direct address still works";return false;}
    setsockopt(listener,SOL_SOCKET,SO_REUSEADDR,&yes,sizeof(yes));
#ifdef SO_REUSEPORT
    setsockopt(listener,SOL_SOCKET,SO_REUSEPORT,&yes,sizeof(yes));
#endif
    sockaddr_in a{};a.sin_family=AF_INET;a.sin_port=htons(12001);a.sin_addr.s_addr=INADDR_ANY;
    if(bind(listener,(sockaddr*)&a,sizeof(a))) {failure(hostFailures,"bind","0.0.0.0:12001");error="LAN discovery port unavailable; direct address still works";closeSocket(listener);return false;}
    bool joined=false;
    for(auto ip:networks) {ip_mreq m{};inet_pton(AF_INET,"239.255.70.90",&m.imr_multiaddr);m.imr_interface=ip;if(setsockopt(listener,IPPROTO_IP,IP_ADD_MEMBERSHIP,&m,sizeof(m))==0)joined=true;else failure(hostFailures,"IP_ADD_MEMBERSHIP",inet_ntoa(ip));}
    if(!joined){error="LAN multicast unavailable; direct address still works";closeSocket(listener);return false;}
    // Replies must originate from the interface where the game socket is bound.
    responder=udp();sockaddr_in source{};source.sin_family=AF_INET;
    inet_pton(AF_INET,bindAddress.c_str(),&source.sin_addr);
    if(responder<0 || bind(responder,(sockaddr*)&source,sizeof(source))) {
      failure(hostFailures,"bind reply socket",bindAddress);error="LAN reply socket unavailable; direct address still works";closeSocket(responder);closeSocket(listener);return false;
    }
    return true;
  }
  void advertise(const fz_server_status& status,const HostOptions& host) {
    if(listener<0)return;
    for(int i=0;i<32;i++) {
      uint8_t bytes[257];sockaddr_in peer{};socklen_t len=sizeof(peer);
      auto n=recvfrom(listener,bytes,sizeof(bytes),0,(sockaddr*)&peer,&len);if(n<0)break;
      if(n!=16 || memcmp(bytes,"FZVD",4) || bytes[4]!=1 || bytes[5]!=1 || !status.owner_joined || status.closed)continue;
      // A room bound to loopback must not invite guests on another machine.
      if(host.bind.rfind("127.",0)==0 && (ntohl(peer.sin_addr.s_addr)>>24)!=127)continue;
      uint8_t reply[96]{};memcpy(reply,"FZVD",4);reply[4]=1;reply[5]=2;reply[6]=FZ_VERSION;
      memcpy(reply+8,bytes+8,8);fz_put64(reply+16,status.session);fz_put16(reply+24,(uint16_t)status.port);
      reply[26]=(uint8_t)status.phase;reply[27]=(uint8_t)status.occupied;reply[28]=(uint8_t)status.players;
      reply[29]=(uint8_t)status.track;reply[30]=(uint8_t)status.league;reply[31]=!host.key.empty();
      for(size_t j=0;j<std::min<size_t>(63,host.name.size());j++)reply[32+j]=(host.name[j]>=32 && host.name[j]<127)?host.name[j]:'?';
      if(sendDatagram(responder,reply,sizeof(reply),0,(sockaddr*)&peer,len)<0)failure(hostFailures,"send reply",inet_ntoa(peer.sin_addr));
    }
  }
  void scan(std::vector<Room>& rooms,std::string& warning) {
    uint64_t now=clockMs();
    if(!browseStarted)browseStarted=now;
    if(query<0 && now-lastQuery<2000)return;
    if(query<0) {
      networks=enumerate();browseFailures.clear();lastQuery=now;
      query=udp();nonce=randomNonce();
      if(query<0){failure(browseFailures,"socket","discovery browser");return;}
      sockaddr_in local{};local.sin_family=AF_INET;
      if(bind(query,(sockaddr*)&local,sizeof(local))) {failure(browseFailures,"bind","browser ephemeral port");closeSocket(query);return;}
      if(!nonce){failure(browseFailures,"random nonce","browser",EIO);closeSocket(query);return;}
      unsigned char ttl=1,loop=1;
      if(setsockopt(query,IPPROTO_IP,IP_MULTICAST_TTL,&ttl,sizeof(ttl)) || setsockopt(query,IPPROTO_IP,IP_MULTICAST_LOOP,&loop,sizeof(loop))) {
        failure(browseFailures,"multicast options","browser");closeSocket(query);return;
      }
      lastQuery=0;
    }
    if(now-lastQuery>=2000) {
      networks=enumerate();browseFailures.clear();
      uint8_t q[16]{};memcpy(q,"FZVD",4);q[4]=1;q[5]=1;q[6]=FZ_VERSION;fz_put64(q+8,nonce);
      sockaddr_in dst{};dst.sin_family=AF_INET;dst.sin_port=htons(12001);inet_pton(AF_INET,"239.255.70.90",&dst.sin_addr);
      bool sent=false;
      for(auto ip:networks) {
        if(setsockopt(query,IPPROTO_IP,IP_MULTICAST_IF,&ip,sizeof(ip))) {failure(browseFailures,"IP_MULTICAST_IF",inet_ntoa(ip));continue;}
        if(sendDatagram(query,q,sizeof(q),0,(sockaddr*)&dst,sizeof(dst))==(ssize_t)sizeof(q)){sent=true;status.queries++;}
        else failure(browseFailures,"send query",inet_ntoa(ip));
      }
      if(!sent && browseFailures.empty())failure(browseFailures,"enumerate interfaces","IPv4",ENETUNREACH);
      if(!sent) {warning="Nearby games unavailable. Enter a host address or retry.";closeSocket(query);}
      else warning.clear();
      lastQuery=now;
    }
    for(int i=0;i<64;i++) {
      uint8_t b[257];sockaddr_in src{};socklen_t len=sizeof(src);auto n=recvfrom(query,b,sizeof(b),0,(sockaddr*)&src,&len);if(n<0){if(query>=0 && errno!=EAGAIN && errno!=EWOULDBLOCK){failure(browseFailures,"receive reply","browser");closeSocket(query);}break;}
      if(n!=96 || memcmp(b,"FZVD",4) || b[4]!=1 || b[5]!=2 || fz_u64(b+8)!=nonce || !fz_u64(b+16)
         || !fz_u16(b+24) || fz_u16(b+24)==12001 || b[26]>FZ_RESULTS || b[27]>4 || b[28]<2 || b[28]>4 || b[29]>4 || b[30]>2 || b[31]>1)continue;
      status.replies++;
      Room r;r.session=fz_u64(b+16);r.seen=now;r.version=b[6];r.phase=b[26];r.occupied=b[27];r.players=b[28];r.track=b[29];r.league=b[30];r.password=b[31];
      for(int j=32;j<96 && b[j];j++)r.name+=(b[j]>=32 && b[j]<127)?(char)b[j]:'?';
      char address[INET_ADDRSTRLEN];inet_ntop(AF_INET,&src.sin_addr,address,sizeof(address));r.address=std::string(address)+":"+std::to_string(fz_u16(b+24));
      auto old=std::find_if(rooms.begin(),rooms.end(),[&](auto& x){return x.session==r.session;});
      if(old!=rooms.end())*old=r;else if(rooms.size()<64)rooms.push_back(r);
    }
    std::erase_if(rooms,[&](auto& r){return now-r.seen>6000;});
  }
};
// getaddrinfo has no portable cancellation API. A bounded resolver task owns
// only this shared result (never Session, sockets, emulator or UI). Cancellation
// discards its result immediately; it cannot block server shutdown or app exit.
struct Resolution { std::mutex mutex; bool done=false;std::string address,error; };
static std::atomic<unsigned> resolvers{0};
static std::shared_ptr<Resolution> resolve(std::string address) {
  auto out=std::make_shared<Resolution>();
  auto first=address.find_first_not_of(" \t\r\n");
  if(first==std::string::npos){out->done=true;out->error="Enter a host address";return out;}
  address=address.substr(first,address.find_last_not_of(" \t\r\n")-first+1);
  if(address.find(':')==std::string::npos)address+=":12000";
  auto colon=address.rfind(':');
  if(colon==std::string::npos || colon==0 || colon+1==address.size()) {out->done=true;out->error="Enter a hostname or IPv4 address followed by :port";return out;}
  auto host=address.substr(0,colon),port=address.substr(colon+1);
  if(port.size()>5 || port.find_first_not_of("0123456789")!=std::string::npos || std::stoul(port)==0 || std::stoul(port)>65535 || std::stoul(port)==12001) {out->done=true;out->error="Invalid game port (12001 is reserved for discovery)";return out;}
  if(resolvers.fetch_add(1)>=4) {resolvers--;out->done=true;out->error="Previous DNS requests are still finishing; retry shortly";return out;}
  try { std::thread([out,host,port] {
    addrinfo hints{},*list=nullptr;hints.ai_family=AF_INET;hints.ai_socktype=SOCK_DGRAM;hints.ai_flags=AI_NUMERICSERV;
    int result=getaddrinfo(host.c_str(),port.c_str(),&hints,&list);
    std::lock_guard<std::mutex> lock(out->mutex);
    if(result)out->error=std::string("Cannot resolve server: ")+gai_strerror(result);
    else {char ip[INET_ADDRSTRLEN];inet_ntop(AF_INET,&((sockaddr_in*)list->ai_addr)->sin_addr,ip,sizeof(ip));out->address=std::string(ip)+":"+port;freeaddrinfo(list);}
    out->done=true;resolvers--;
  }).detach(); } catch(const std::system_error& error) {resolvers--;out->done=true;out->error=std::string("Cannot start DNS resolver: ")+error.what();}
  return out;
}
Session::~Session(){shutdown();}
void Session::start(){std::lock_guard<std::mutex> lock(mutex);if(!worker.joinable()){quitting=false;try {worker=std::thread(&Session::run,this);} catch(const std::system_error& error) {published.state="error";published.message=std::string("Cannot start session worker: ")+error.what();}}}
void Session::shutdown(){quitting=true;wake.notify_all();if(worker.joinable())worker.join();}
bool Session::enqueue(Command c){start();std::lock_guard<std::mutex> lock(mutex);if(!worker.joinable())return false;if(c.type==0)commands.clear();else if(commands.size()>=8)return false;c.generation=++generation;published.state=c.type==0?"stopping":"starting";published.message.clear();published.generation=c.generation;commands.push_back(std::move(c));connection.reset();wake.notify_all();return true;}
bool Session::host(HostOptions h){return enqueue({1,std::move(h),{},{},0});}
bool Session::join(std::string address,std::string key){return enqueue({2,{},std::move(address),std::move(key),0});}
void Session::stop(){enqueue({0,{},{},{},0});}
void Session::browse(bool enabled){start();browsing=enabled;wake.notify_all();}
void Session::retryDiscovery(){start();discoveryRetry=true;wake.notify_all();}
void Session::connected(){std::lock_guard<std::mutex> lock(mutex);if(published.state=="connecting")published.state="connected";}
SessionView Session::view(){std::lock_guard<std::mutex> lock(mutex);return published;}
std::optional<Connection> Session::takeConnection(){std::lock_guard<std::mutex> lock(mutex);auto out=connection;connection.reset();if(out && out->generation!=generation)return {};return out;}
void Session::run() {
  fz_server *server=nullptr;HostOptions options;SessionView state;Discovery discovery;
  {std::lock_guard<std::mutex> lock(mutex);state.events=published.events;}
  std::shared_ptr<Resolution> resolution;std::string joinKey;uint64_t operation=0,started=0,lastPublish=0;bool ready=false,ownsReadyFile=false;
  FILE *logFile=nullptr;std::string lastFailure,lastDiscoveryReport;
  auto drain=[&] {
    fz_server_event event;
    while(server && fz_server_next_event(server,&event)) {
      if(state.events.size()==512)state.events.pop_front();state.events.push_back(event);
      const char *level[]={"WARN","INFO","DEBUG","TRACE"};
      // Keep normal stdout usable; the host file retains all trace events.
      if(event.level<=2) {fprintf(stdout,"%llu HOST %s race=%u player=%d %s\n",(unsigned long long)event.time_us,level[event.level],event.race,event.player+1,event.message);fflush(stdout);}
      if(logFile)fprintf(logFile,"%llu %s race=%u player=%d %s\n",(unsigned long long)event.time_us,level[event.level],event.race,event.player+1,event.message);
    }
  };
  auto stopServer=[&] {
    if(server){fz_server_close(server);drain();fz_server_destroy(server);server=nullptr;}
    if(logFile){fclose(logFile);logFile=nullptr;}
    if(ownsReadyFile)unlink(options.readyFile.c_str());ownsReadyFile=false;
    closeSocket(discovery.listener);closeSocket(discovery.responder);resolution.reset();state.hosting=false;state.server={};ready=false;
  };
  while(!quitting) {
    std::deque<Command> pending;
    {std::lock_guard<std::mutex> lock(mutex);pending.swap(commands);}
    if(discoveryRetry.exchange(false)) {discovery.retry();state.discoveryWarning.clear();state.rooms.clear();}
    for(auto& c:pending) {
      stopServer();operation=c.generation;state.generation=operation;state.message.clear();state.state="idle";
      if(c.type==1) {
        options=std::move(c.host);state.state="starting";
        uint64_t nonce=randomNonce();char error[256]{};
        fz_server_config config{options.bind.c_str(),options.key.c_str(),options.port,options.players,options.track,options.league,3,-1,nonce};
        if(!nonce){state.state="error";state.message="Cannot create host identity";continue;}
        if(options.name.empty())options.name="F-Zero VS";
        if(options.name.size()>63){state.state="error";state.message="Room name must be at most 63 bytes";continue;}
        server=fz_server_create(&config,error,sizeof(error));
        if(!server){state.state="error";state.message=error;continue;}
        if(!options.logPath.empty())logFile=fopen(options.logPath.c_str(),"a");
        if(!options.logPath.empty() && !logFile){state.message="Cannot open host log file";stopServer();state.state="error";continue;}
        if(logFile)setvbuf(logFile,nullptr,_IOLBF,0);
        state.hosting=true;started=clockMs();state.addresses.clear();
        for(auto ip:discovery.networks) if(ntohl(ip.s_addr)!=INADDR_LOOPBACK && (options.bind=="0.0.0.0" || options.bind==inet_ntoa(ip))) state.addresses+=std::string(inet_ntoa(ip))+":"+std::to_string(options.port)+"\n";
        if(options.bind=="0.0.0.0")state.addresses+="127.0.0.1:"+std::to_string(options.port);
        else if(state.addresses.empty())state.addresses=options.bind+":"+std::to_string(options.port);
        std::string address=(options.bind=="0.0.0.0" ? "127.0.0.1" : options.bind)+":"+std::to_string(options.port);
        {std::lock_guard<std::mutex> lock(mutex);if(operation==generation)connection=Connection{address,options.key,nonce,operation};}
      } else if(c.type==2) {state.state="resolving";joinKey=c.key;resolution=resolve(c.address);}
    }
    if(resolution) {
      bool done=false;std::string address,error;
      {std::lock_guard<std::mutex> lock(resolution->mutex);done=resolution->done;address=resolution->address;error=resolution->error;}
      if(done){resolution.reset();if(!error.empty()){state.state="error";state.message=error;}else {state.state="connecting";std::lock_guard<std::mutex> lock(mutex);if(operation==generation)connection=Connection{address,joinKey,0,operation};}}
    }
    if(server) {
      int result=fz_server_poll(server,4);drain();fz_server_get_status(server,&state.server);
      if(result<=0){state.message=result<0?"Server socket failed":"Host left; room ended";stopServer();state.state=result<0?"error":"idle";}
      else if(!ready && state.server.owner_joined) {
        bool failed=false;
        if(!options.readyFile.empty()) {FILE *f=fopen(options.readyFile.c_str(),"w");failed=!f;if(f){ownsReadyFile=true;failed=fputs("ready\n",f)<0;if(fclose(f))failed=true;}}
        if(failed){state.message="Cannot write host readiness file";stopServer();state.state="error";}
        else {ready=true;state.state="hosting";discovery.listen(state.discoveryWarning,options.bind);}
      } else if(!ready && clockMs()-started>10000) {state.message="Local host failed to join reserved P1 slot";stopServer();state.state="error";}
      if(server && ready) {
        if(discovery.listener<0 && clockMs()-discovery.lastListen>=2000)discovery.listen(state.discoveryWarning,options.bind);
        discovery.advertise(state.server,options);
      }
    }
    if(state.state=="error") {
      auto key=std::to_string(operation)+state.message;
      if(key!=lastFailure) {
        lastFailure=key;fz_server_event error{};error.time_us=clockMs()*1000;error.level=0;error.player=-1;
        snprintf(error.message,sizeof(error.message),"%s",state.message.c_str());
        if(state.events.size()==512)state.events.pop_front();state.events.push_back(error);
        fprintf(stderr,"FZVS SESSION ERROR %s\n",error.message);fflush(stderr);
      }
    }
    if(browsing)discovery.scan(state.rooms,state.discoveryWarning);
    else {closeSocket(discovery.query);discovery.browseStarted=0;state.rooms.clear();}
    std::erase_if(state.rooms,[&](auto& r){return clockMs()-r.seen>6000;});
    discovery.publish(browsing,state.rooms);state.discovery=discovery.status;
    if(state.discovery.phase!=DiscoveryPhase::Unavailable)state.discoveryWarning.clear();
    std::string report;
    for(auto& f:state.discovery.failures)report+=f.operation+" ["+f.interface+"]: "+f.message+" ("+std::to_string(f.code)+")\n";
    if(report!=lastDiscoveryReport) {
      fz_server_event event{};event.time_us=clockMs()*1000;event.player=-1;event.level=report.empty()?1:0;
      snprintf(event.message,sizeof(event.message),"Discovery: %s",report.empty()?"recovered":report.c_str());
      if(state.events.size()==512)state.events.pop_front();state.events.push_back(event);
      fprintf(stderr,"FZVS %s\n",event.message);fflush(stderr);
      if(logFile)fprintf(logFile,"%llu %s\n",(unsigned long long)event.time_us,event.message);
      lastDiscoveryReport=report;
    }
    if(clockMs()-lastPublish>=50) {
      std::lock_guard<std::mutex> lock(mutex);
      if(published.state=="connected" && state.state=="connecting" && published.generation==operation)state.state="connected";
      if(operation==generation)published=state;lastPublish=clockMs();
    }
    if(!server){std::unique_lock<std::mutex> lock(mutex);wake.wait_for(lock,std::chrono::milliseconds(10),[&]{return quitting || !commands.empty();});}
  }
  stopServer();state.state="idle";std::lock_guard<std::mutex> lock(mutex);published=state;connection.reset();commands.clear();
}
}

#include "fzvs_session.hpp"
#include "fzvs_protocol.h"
#include <arpa/inet.h>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <ifaddrs.h>
#include <net/if.h>
#include <sys/socket.h>
#include <unistd.h>
using namespace fzvs;
static void waitFor(std::function<bool()> predicate,int milliseconds=3000) {
  auto end=std::chrono::steady_clock::now()+std::chrono::milliseconds(milliseconds);
  while(!predicate()) {assert(std::chrono::steady_clock::now()<end);usleep(5000);}
}
static unsigned freePort() {
  int fd=socket(AF_INET,SOCK_DGRAM,0);assert(fd>=0);sockaddr_in a{};a.sin_family=AF_INET;a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
  assert(bind(fd,(sockaddr*)&a,sizeof(a))==0);socklen_t len=sizeof(a);assert(getsockname(fd,(sockaddr*)&a,&len)==0);unsigned port=ntohs(a.sin_port);close(fd);return port==12001?freePort():port;
}
// Distinguish a product discovery failure from a host that drops all multicast.
static bool multicastAvailable() {
  int receiver=socket(AF_INET,SOCK_DGRAM,0),sender=socket(AF_INET,SOCK_DGRAM,0);assert(receiver>=0 && sender>=0);
  sockaddr_in address{};address.sin_family=AF_INET;assert(bind(receiver,(sockaddr*)&address,sizeof(address))==0);
  assert(bind(sender,(sockaddr*)&address,sizeof(address))==0);socklen_t length=sizeof(address);assert(getsockname(receiver,(sockaddr*)&address,&length)==0);
  inet_pton(AF_INET,"239.255.70.90",&address.sin_addr);unsigned char loop=1;setsockopt(sender,IPPROTO_IP,IP_MULTICAST_LOOP,&loop,sizeof(loop));
  ifaddrs* list=nullptr;assert(getifaddrs(&list)==0);
  for(auto p=list;p;p=p->ifa_next)if(p->ifa_addr && p->ifa_addr->sa_family==AF_INET && (p->ifa_flags&IFF_UP)) {
    in_addr ip=((sockaddr_in*)p->ifa_addr)->sin_addr;ip_mreq membership{address.sin_addr,ip};
    if(setsockopt(receiver,IPPROTO_IP,IP_ADD_MEMBERSHIP,&membership,sizeof(membership))==0 && setsockopt(sender,IPPROTO_IP,IP_MULTICAST_IF,&ip,sizeof(ip))==0)
      sendto(sender,"probe",5,0,(sockaddr*)&address,sizeof(address));
  }
  freeifaddrs(list);timeval timeout{0,500000};setsockopt(receiver,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));char bytes[16];bool ok=recv(receiver,bytes,sizeof(bytes),0)==5;
  close(receiver);close(sender);return ok;
}
static void checkDiscoveryReply(const SessionView& room) {
  int fd=socket(AF_INET,SOCK_DGRAM,0);assert(fd>=0);sockaddr_in target{};target.sin_family=AF_INET;target.sin_addr.s_addr=htonl(INADDR_LOOPBACK);target.sin_port=htons(12001);
  uint8_t query[16]{};memcpy(query,"FZVD",4);query[4]=1;query[5]=1;query[6]=FZ_VERSION;fz_put64(query+8,987654321);
  assert(sendto(fd,query,sizeof(query),0,(sockaddr*)&target,sizeof(target))==sizeof(query));
  // Reusable discovery ports can route a unicast query to another room. Rotate
  // the query endpoint until this room answers, without stopping other hosts.
  auto sent=std::chrono::steady_clock::now();
  waitFor([&]{
    if(std::chrono::steady_clock::now()-sent>std::chrono::milliseconds(100)) {
      close(fd);fd=socket(AF_INET,SOCK_DGRAM,0);assert(fd>=0);
      assert(sendto(fd,query,sizeof(query),0,(sockaddr*)&target,sizeof(target))==sizeof(query));
      sent=std::chrono::steady_clock::now();
    }
    uint8_t reply[257];sockaddr_in source{};socklen_t length=sizeof(source);auto n=recvfrom(fd,reply,sizeof(reply),MSG_DONTWAIT,(sockaddr*)&source,&length);
    if(n<0)return false;
    assert(n==96 && !memcmp(reply,"FZVD",4) && reply[4]==1 && reply[5]==2 && reply[6]==FZ_VERSION);
    assert(fz_u64(reply+8)==987654321);
    if(fz_u64(reply+16)!=room.server.session)return false;
    assert(fz_u16(reply+24)==room.server.port);
    assert(reply[27]==1 && reply[28]==2 && reply[31]==1 && !strcmp((char*)reply+32,"Core test"));
    assert(source.sin_addr.s_addr==htonl(INADDR_LOOPBACK));return true;});close(fd);
}
struct Peer {
  int fd;uint64_t nonce,session=0,token=0;uint32_t race=0,seq=0;
  Peer(unsigned port,uint64_t n):nonce(n) {fd=socket(AF_INET,SOCK_DGRAM,0);assert(fd>=0);sockaddr_in a{};a.sin_family=AF_INET;a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);a.sin_port=htons(port);assert(connect(fd,(sockaddr*)&a,sizeof(a))==0);}
  ~Peer(){close(fd);}
  void send(fz_packet p){p.session=session;p.token=token;p.race=race;uint8_t bytes[FZ_MAX_PACKET];size_t n=fz_encode(bytes,sizeof(bytes),&p);assert(n);assert(::send(fd,bytes,n,0)==(ssize_t)n);}
  void hello(const char* key="") {fz_packet p{};p.type=FZ_HELLO;p.length=9+strlen(key);fz_put64(p.payload,nonce);p.payload[8]=strlen(key);memcpy(p.payload+9,key,strlen(key));send(p);}
  fz_packet receive(unsigned type,int timeout=3000) {
    fz_packet p{};waitFor([&]{uint8_t bytes[257];auto n=recv(fd,bytes,sizeof(bytes),MSG_DONTWAIT);if(n<=0 || !fz_decode(&p,bytes,n))return false;if(p.type==FZ_PING){p.type=FZ_PONG;send(p);}return p.type==type;},timeout);return p;
  }
  void joined(int id){auto p=receive(FZ_WELCOME);assert(p.payload[0]==id && fz_u64(p.payload+1)==nonce);session=p.session;token=p.token;race=p.race;}
  void rejected(int reason){auto p=receive(FZ_JOIN_REJECT);assert(fz_u64(p.payload)==nonce && p.payload[8]==reason);}
  void command(uint8_t cmd,uint8_t value=0){fz_packet p{};p.type=FZ_COMMAND;p.length=cmd==FZ_SELECT?2:1;p.payload[0]=cmd;p.payload[1]=value;p.seq=++seq;send(p);}
  void heartbeat(){fz_packet p{};p.type=FZ_PING;p.length=8;send(p);}
};
static Connection connection(Session& s){std::optional<Connection> c;waitFor([&]{c=s.takeConnection();return bool(c);});return *c;}
int main() {
  Session host;HostOptions options;options.bind="127.0.0.1";options.port=freePort();options.name="Core test";options.key="secret";
  options.readyFile="/tmp/fzvs-session-test-"+std::to_string(getpid())+".ready";
  assert(host.host(options));auto c=connection(host);
  assert(c.ownerNonce && access(options.readyFile.c_str(),F_OK)!=0);
  Peer early(options.port,123);early.hello("secret");early.rejected(FZ_REJECT_STARTING);
  Peer owner(options.port,c.ownerNonce);owner.hello("secret");owner.joined(0);
  waitFor([&]{return host.view().server.owner_joined && access(options.readyFile.c_str(),F_OK)==0;});
  checkDiscoveryReply(host.view());
  Peer wrong(options.port,124);wrong.hello("wrong");wrong.rejected(FZ_REJECT_PASSWORD);
  Peer guest(options.port,125);guest.hello("secret");guest.joined(1);
  Peer full(options.port,126);full.hello("secret");full.rejected(FZ_REJECT_FULL);
  // Unsupported protocol reports a nonce-correlated rejection without changing gameplay layouts.
  uint8_t raw[FZ_HEADER+9]{};memcpy(raw,"FZVS",4);raw[4]=99;raw[5]=FZ_HELLO;fz_put16(raw+6,9);fz_put64(raw+FZ_HEADER,126);
  assert(::send(full.fd,raw,sizeof(raw),0)==sizeof(raw));full.rejected(FZ_REJECT_VERSION);
  Session occupied;assert(occupied.host(options));waitFor([&]{return occupied.view().state=="error";});assert(occupied.view().message.find("listen")!=std::string::npos);occupied.shutdown();assert(access(options.readyFile.c_str(),F_OK)==0);
  owner.command(FZ_SELECT,0);guest.command(FZ_SELECT,1);waitFor([&]{return host.view().server.phase==FZ_LOADING;});
  full.hello("secret");full.rejected(FZ_REJECT_RACING);
  owner.command(FZ_LEAVE);auto closed=guest.receive(FZ_ROOM_CLOSED);assert(closed.token==guest.token && closed.session==guest.session);
  waitFor([&]{return !host.view().hosting && access(options.readyFile.c_str(),F_OK)!=0;});
  // Repeated start/stop frees the port and cancels unpublished owner connections.
  for(int i=0;i<12;i++){assert(host.host(options));connection(host);host.stop();waitFor([&]{return host.view().state=="idle";});assert(!host.takeConnection());}
  assert(host.join("invalid..hostname:12000",""));host.stop();waitFor([&]{return host.view().state=="idle";});assert(!host.takeConnection());
  assert(host.join("localhost:12001",""));waitFor([&]{return host.view().state=="error";});
  assert(host.join("invalid..hostname:12000",""));waitFor([&]{return host.view().state=="error";});assert(host.view().message.find("resolve")!=std::string::npos);
  // Independent hosted rooms share discovery port; browse replies identify both sessions.
  Session other,browser;options.key="";options.bind="0.0.0.0";assert(host.host(options));c=connection(host);Peer p1(options.port,c.ownerNonce);p1.hello();p1.joined(0);
  HostOptions second=options;second.port=freePort();second.readyFile="";second.name="Second room";assert(other.host(second));c=connection(other);Peer p2(second.port,c.ownerNonce);p2.hello();p2.joined(0);
  bool multicastWorks=multicastAvailable();
  browser.browse(true);
  bool found=false;auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
  while(std::chrono::steady_clock::now()<deadline) {
    p1.heartbeat();p2.heartbeat();
    if(browser.view().rooms.size()>=2){found=true;break;}usleep(20000);
  }
  if(found) {
    auto rooms=browser.view().rooms;assert(rooms[0].session!=rooms[1].session);
    for(auto& r:rooms)assert(r.occupied==1 && r.players==2 && !r.password);
  } else {
    assert(!multicastWorks && !std::getenv("FZVS_REQUIRE_MULTICAST"));
    fprintf(stderr,"SKIP multicast reception: no two-room replies on this machine; verify on an unrestricted LAN.\n");
  }
  host.shutdown();other.shutdown();waitFor([&]{return browser.view().rooms.empty();},8000);browser.shutdown();
  assert(access(options.readyFile.c_str(),F_OK)!=0);
  puts("Host reservation, rejections, lifecycle, cancellation, port reuse and concurrent rooms passed.");
}

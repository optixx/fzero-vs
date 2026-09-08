// Exercise the worker-owned discovery implementation with controlled interface
// enumeration and sends, without introducing test hooks into the public session API.
#include "../client/fzvs_session.cpp"
#include <cassert>
using namespace fzvs;
int main() {
  Discovery d;std::vector<Room> rooms;std::string warning;unsigned refreshes=0;
  d.enumerate=[&]{refreshes++;return std::vector<in_addr>{{htonl(INADDR_LOOPBACK)}};};
  d.sendDatagram=[](int,const void*,size_t,int,const sockaddr*,socklen_t)->ssize_t {errno=EHOSTUNREACH;return -1;};
  d.scan(rooms,warning);d.publish(true,rooms);
  assert(d.query<0 && d.status.phase==DiscoveryPhase::Unavailable && !warning.empty());
  assert(d.status.failures.size()==1 && d.status.failures[0].operation=="send query" && d.status.failures[0].interface=="127.0.0.1" && d.status.failures[0].code==EHOSTUNREACH);
  d.sendDatagram=[](int,const void*,size_t length,int,const sockaddr*,socklen_t)->ssize_t {return length;};
  d.retry();d.scan(rooms,warning);d.publish(true,rooms);
  assert(d.query>=0 && warning.empty() && d.status.failures.empty() && refreshes>=4);
  assert(d.status.phase==DiscoveryPhase::Searching && d.status.queries==1 && d.status.replies==0);
  d.browseStarted=clockMs()-3000;d.publish(true,rooms);assert(d.status.phase==DiscoveryPhase::Empty);
  // Interface disappearance is diagnosed; another retry re-enumerates instead
  // of retaining the stale address for the lifetime of the application.
  d.enumerate=[] {return std::vector<in_addr>{{inet_addr("192.0.2.123")}};};
  d.retry();d.scan(rooms,warning);d.publish(true,rooms);assert(d.status.phase==DiscoveryPhase::Unavailable);
  assert(d.status.failures[0].operation=="IP_MULTICAST_IF");
  d.enumerate=[] {return std::vector<in_addr>{{htonl(INADDR_LOOPBACK)}};};d.retry();d.scan(rooms,warning);d.publish(true,rooms);assert(d.status.failures.empty());
  // Inject a valid unicast reply; successful sends alone must never show Rooms.
  sockaddr_in address{};socklen_t length=sizeof(address);assert(getsockname(d.query,(sockaddr*)&address,&length)==0);address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
  int fd=udp();uint8_t reply[96]{};memcpy(reply,"FZVD",4);reply[4]=1;reply[5]=2;reply[6]=FZ_VERSION;fz_put64(reply+8,d.nonce);fz_put64(reply+16,123);fz_put16(reply+24,12000);reply[28]=2;memcpy(reply+32,"Test room",10);
  assert(sendto(fd,reply,sizeof(reply),0,(sockaddr*)&address,sizeof(address))==sizeof(reply));usleep(10000);d.scan(rooms,warning);d.publish(true,rooms);
  assert(rooms.size()==1 && rooms[0].address=="127.0.0.1:12000" && d.status.phase==DiscoveryPhase::Rooms && d.status.replies==1);
  rooms[0].seen=clockMs()-7000;d.scan(rooms,warning);d.publish(true,rooms);assert(rooms.empty());
  d.retry();assert(d.query<0 && d.listener<0 && d.responder<0);close(fd);
  auto resolved=resolve(" localhost ");for(int i=0;i<3000;i++){{std::lock_guard<std::mutex> lock(resolved->mutex);if(resolved->done)break;}usleep(1000);}
  {std::lock_guard<std::mutex> lock(resolved->mutex);assert(resolved->done && resolved->address=="127.0.0.1:12000");}
  puts("Discovery: send failure, interface replacement, retry, warning recovery, reply validation, expiry and default port passed.");
}

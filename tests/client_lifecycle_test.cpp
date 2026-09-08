#include "fzvs_client.hpp"
#include <arpa/inet.h>
#include <cassert>
#include <chrono>
#include <cstring>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>
using namespace fzvs;
int main() {
  int server=socket(AF_INET,SOCK_DGRAM,0);assert(server>=0);
  sockaddr_in local{};local.sin_family=AF_INET;local.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
  assert(bind(server,(sockaddr*)&local,sizeof(local))==0);socklen_t size=sizeof(local);assert(getsockname(server,(sockaddr*)&local,&size)==0);
  timeval timeout{1,0};setsockopt(server,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
  Client client;client.config.enabled=true;client.config.address="127.0.0.1:"+std::to_string(ntohs(local.sin_port));
  std::vector<uint8_t> rom(512*1024,0x42),ram(128*1024);int resets=0;
  Memory memory;
  memory.ram=[&](uint32_t a){return ram.at(a);};memory.rom=[&](uint32_t a){return rom.at(a);};
  memory.writeRam=[&](uint32_t a,uint8_t v){ram.at(a)=v;};memory.writeRom=[&](uint32_t a,uint8_t v){rom.at(a)=v;};memory.reset=[&]{resets++;};
  sockaddr_in remote{};uint64_t nonce=0;
  auto attach=[&]{uint8_t stale[257];while(recv(server,stale,sizeof(stale),MSG_DONTWAIT)>0){}assert(client.attach(memory));client.tick();uint8_t bytes[257];socklen_t length=sizeof(remote);auto n=recvfrom(server,bytes,sizeof(bytes),0,(sockaddr*)&remote,&length);fz_packet p{};assert(n>0 && fz_decode(&p,bytes,n) && p.type==FZ_HELLO);nonce=fz_u64(p.payload);};
  auto send=[&](fz_packet p){uint8_t bytes[FZ_MAX_PACKET];size_t n=fz_encode(bytes,sizeof(bytes),&p);assert(sendto(server,bytes,n,0,(sockaddr*)&remote,sizeof(remote))==(ssize_t)n);usleep(2000);client.tick();};
  auto patched=[&]{client.frame();assert(rom[0x18176]==0xea);};
  auto restored=[&]{for(auto b:rom)assert(b==0x42);};
  // A stale/foreign join rejection cannot cancel a new attempt.
  attach();fz_packet reject{};reject.type=FZ_JOIN_REJECT;reject.length=9;fz_put64(reject.payload,nonce+1);reject.payload[8]=FZ_REJECT_PASSWORD;send(reject);assert(client.view().game!="disconnected");
  fz_put64(reject.payload,nonce);send(reject);assert(client.view().message=="Incorrect room password");restored();
  auto welcome=[&]{fz_packet p{};p.type=FZ_WELCOME;p.length=9;p.payload[0]=1;fz_put64(p.payload+1,nonce);p.session=123;p.token=456;p.race=1;send(p);};
  attach();welcome();patched();fz_packet closed{};closed.type=FZ_ROOM_CLOSED;closed.length=1;closed.session=123;closed.token=999;send(closed);assert(rom[0x18176]==0xea);
  closed.token=456;send(closed);assert(client.view().message=="Host closed the room");restored();
  // Lost shutdown notice: silence still restores patches after the heartbeat timeout.
  attach();welcome();patched();auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(7);
  while(client.view().game!="disconnected") {assert(std::chrono::steady_clock::now()<deadline);client.tick();usleep(10000);}
  assert(client.view().message.find("Server connection lost")!=std::string::npos);restored();
  client.detach();assert(client.view().player==-1);assert(resets>=6);close(server);
  puts("Nonce rejection, authenticated close, lost shutdown timeout and ROM restoration passed.");
}

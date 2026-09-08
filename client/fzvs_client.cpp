#include "fzvs_client.hpp"
#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <sstream>
#include <stdexcept>
#include <sys/socket.h>
#include <unistd.h>
#include <algorithm>

namespace fzvs {
Client client;
static uint64_t now() { return (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
static const uint8_t slots[4][4]={{0,3,2,1},{1,3,2,0},{2,3,1,0},{3,2,1,0}};
static const uint8_t selector[4]={3,0,1,2};
static const uint8_t palettes[4][4]={{14,12,10,8},{8,12,10,14},{10,12,8,14},{12,10,8,14}};
static const char *colors[4]={"Pink","Blue","Green","Yellow"};
View Client::view() { std::lock_guard<std::mutex> lock(mutex); return published; }
void Client::request(uint8_t command,std::vector<uint8_t> data) {
  std::lock_guard<std::mutex> lock(mutex);
  if(requests.size()<8) { data.insert(data.begin(),command); requests.push_back(std::move(data)); }
}
void Client::log(const std::string& text) {
  message=text;
  fprintf(stdout,"%llu FZVS race=%u player=%d %s\n",(unsigned long long)now(),race,id+1,text.c_str()); fflush(stdout);
  events.push_back(text); if(events.size()>12) events.pop_front();
}
void Client::failure(const std::string& text) { log(text); change(Disconnected); publish(); }
bool Client::attach(Memory m) {
  detach(); memory=std::move(m);
  auto colon=config.address.rfind(':');
  if(colon==std::string::npos || config.key.size()>FZ_KEY_MAX) { failure("Invalid server address or room key length"); return false; }
  addrinfo hints{},*addresses=nullptr; hints.ai_family=AF_INET; hints.ai_socktype=SOCK_DGRAM;
  if(getaddrinfo(config.address.substr(0,colon).c_str(),config.address.substr(colon+1).c_str(),&hints,&addresses)) {
    failure("Cannot resolve server address"); return false;
  }
  socket=::socket(AF_INET,SOCK_DGRAM,0);
  bool ok=socket>=0 && connect(socket,addresses->ai_addr,addresses->ai_addrlen)==0 && fcntl(socket,F_SETFL,O_NONBLOCK)==0;
  freeaddrinfo(addresses);
  if(!ok) { if(socket>=0) close(socket); socket=-1; failure("Cannot open UDP connection"); return false; }
  int random=open("/dev/urandom",O_RDONLY);
  if(random<0 || read(random,&nonce,sizeof(nonce))!=(ssize_t)sizeof(nonce)) { if(random>=0) close(random); failure("Cannot create session nonce"); close(socket); socket=-1; return false; }
  close(random);
  session=token=0; id=-1; race=commandSeq=stateSeq=snapshotSeq=0; phase=FZ_LOBBY;
  lastRecv=rateTime=now(); lastHello=lastPing=lastPublish=0; frames=0; bestRtt=1e12;
  rx=tx=bytes=stale=retries=gaps=snapshots=prevRx=prevTx=prevBytes=prevFrames=0;
  rtt=jitter=offset=0; peers={}; pending.clear(); resetGame(); log("Connecting to "+config.address); publish(); return true;
}
void Client::detach() {
  if(socket>=0) {
    if(id>=0 && pending.empty()) { fz_packet packet{}; packet.type=FZ_COMMAND; packet.seq=++commandSeq; packet.length=1; packet.payload[0]=FZ_LEAVE; send(packet); }
    close(socket); socket=-1;
  }
  restore(); memory={}; id=-1; pending.clear();
}
void Client::send(fz_packet packet) {
  packet.session=session; packet.token=token; packet.race=race;
  uint8_t data[FZ_MAX_PACKET]; auto n=fz_encode(data,sizeof(data),&packet);
  if(socket>=0 && ::send(socket,data,n,0)==(ssize_t)n) { tx++; bytes+=n; }
}
void Client::queue(const std::vector<uint8_t>& command) {
  if(id<0 || pending.size()>=8 || command.size()>sizeof(fz_packet{}.payload)) return;
  Pending p; p.packet.type=FZ_COMMAND; p.packet.seq=++commandSeq; p.packet.length=(uint16_t)command.size();
  std::copy(command.begin(),command.end(),p.packet.payload); pending.push_back(p);
}
void Client::receive(const fz_packet& p) {
  auto t=now();
  if(p.type==FZ_WELCOME && p.length==9 && fz_u64(p.payload+1)==nonce && p.payload[0]<4 && p.session && p.token) {
    if(id>=0) return;
    id=p.payload[0]; session=p.session; token=p.token; race=p.race; commandSeq=p.ack; lastRecv=t;
    log("Joined as P"+std::to_string(id+1)+" ("+colors[id]+")"); return;
  }
  if(id<0 || p.session!=session || p.token!=token) return;
  if(p.type==FZ_PING && p.length==8) {
    fz_packet reply{}; reply.type=FZ_PONG; reply.length=8; memcpy(reply.payload,p.payload,8); send(reply); lastRecv=t; return;
  }
  if(p.type==FZ_PONG && p.length==16) {
    auto sent=fz_u64(p.payload); if(sent>t || t-sent>5000000) return;
    double sample=(double)(t-sent)/1000; jitter=0.9*jitter+0.1*std::abs(sample-rtt); rtt=rtt ? rtt*0.8+sample*0.2 : sample;
    if(sample<bestRtt) { bestRtt=sample; offset=(double)fz_u64(p.payload+8)-((double)sent+(double)t)/2; }
    lastRecv=t; return;
  }
  if(p.type==FZ_ERROR && p.length==2) {
    log("Server rejected command "+std::to_string(p.payload[0])+" (state changed or action unavailable)");
  }
  if(p.type!=FZ_SNAPSHOT || p.length!=FZ_SNAPSHOT_SIZE || p.payload[0]>FZ_RESULTS || p.payload[1]<2 || p.payload[1]>4
     || (p.payload[2]>3 && p.payload[2]!=255) || p.payload[3]>4 || p.payload[4]>2) return;
  for(int i=0;i<4;i++) if(p.payload[24+i*16]>FZ_DISCONNECTED || p.payload[25+i*16]>3) return;
  if(!fz_newer(p.seq,snapshotSeq) || (p.race!=race && !fz_newer(p.race,race))) { stale++; return; }
  if(snapshotSeq) { uint32_t delta=p.seq-snapshotSeq; if(delta<10000) gaps+=delta-1; }
  snapshotSeq=p.seq; snapshots++; lastRecv=t;
  while(!pending.empty() && (p.ack==pending.front().packet.seq || fz_newer(p.ack,pending.front().packet.seq))) pending.pop_front();
  if(p.race!=race) {
    race=p.race; commandSeq=p.ack; stateSeq=0; pending.clear(); resetGame();
    log("New race lobby");
  }
  phase=p.payload[0]; expected=p.payload[1]; host=p.payload[2]; track=p.payload[3]; league=p.payload[4]; armedMask=p.payload[5];
  startTime=fz_u64(p.payload+16);
  for(int i=0;i<4;i++) {
    const auto *q=p.payload+24+i*16; peers[i]={q[0],q[1],q[6],fz_u16(q+2),fz_u16(q+4),fz_u32(q+8),fz_u32(q+12)};
  }
  if(phase==FZ_COUNTDOWN && startTime!=armedTime) {
    std::vector<uint8_t> arm(9); arm[0]=FZ_ARM; fz_put64(arm.data()+1,startTime); queue(arm); armedTime=startTime;
  }
}
void Client::tick() {
  if(socket<0 || game==Disconnected) return;
  uint64_t t=now();
  for(int budget=0;budget<128;budget++) {
    uint8_t bytesIn[FZ_MAX_PACKET+1]; auto n=recv(socket,bytesIn,sizeof(bytesIn),0);
    if(n<0) break;
    fz_packet p; if(fz_decode(&p,bytesIn,(size_t)n)) { rx++; bytes+=(uint64_t)n; receive(p); }
  }
  t=now(); // receive() advances lastRecv; avoid unsigned subtraction against the earlier timestamp
  if(id<0) {
    if(t-lastHello>250000) {
      fz_packet p{}; p.type=FZ_HELLO; p.length=(uint16_t)(9+config.key.size()); fz_put64(p.payload,nonce); p.payload[8]=(uint8_t)config.key.size();
      memcpy(p.payload+9,config.key.data(),config.key.size()); send(p); lastHello=t;
    }
  } else {
    std::deque<std::vector<uint8_t>> actions;
    { std::lock_guard<std::mutex> lock(mutex); actions.swap(requests); }
    for(auto& action:actions) queue(action);
    if(!pending.empty() && t-pending.front().sent>=250000) {
      auto& p=pending.front(); if(p.sent) retries++; send(p.packet); p.sent=t;
    }
    if(t-lastPing>=500000) { fz_packet p{}; p.type=FZ_PING; p.length=8; fz_put64(p.payload,t); send(p); lastPing=t; }
  }
  if(t-lastRecv>(id<0 ? 10000000u : 5000000u)) {
    failure(id<0 ? "Join timed out: check server, room key, or room availability" : "Server connection lost; leave and reconnect for the next lobby");
    restore();
    if(memory.reset) memory.reset();
  }
  if(t-lastPublish>=200000) publish();
}
void Client::change(Game next) {
  static const char *names[]={"init","car-select","waiting","prepare","location-load","racing","finished","results","disconnected"};
  if(game!=next) { log(std::string("game=")+names[game]+"->"+names[next]); game=next; }
}
void Client::patch(uint32_t address,std::initializer_list<uint8_t> values) {
  for(auto value:values) { if(!originals.count(address)) originals[address]=memory.rom(address); memory.writeRom(address++,value); }
}
void Client::restore() { if(memory.writeRom) for(auto [address,value]:originals) memory.writeRom(address,value); originals.clear(); }
void Client::resetGame() {
  restore(); if(memory.reset) memory.reset();
  game=Init; selectedSent=loadedSent=finishedSent=false; armedTime=startTime=0; car=0;
}
uint16_t Client::word(uint32_t a) { return (uint16_t)(memory.ram(a)|(uint16_t)memory.ram(a+1)<<8); }
void Client::putWord(uint32_t a,uint16_t v) { memory.writeRam(a,(uint8_t)v); memory.writeRam(a+1,(uint8_t)(v>>8)); }
std::vector<uint8_t> Client::position(uint8_t command) {
  std::vector<uint8_t> data(6); data[0]=command; fz_put16(data.data()+1,word(0xb70)); fz_put16(data.data()+3,word(0xb90)); data[5]=memory.ram(0xbd1); return data;
}
void Client::prepareRace() {
  patch(0x5486,{0x4c,0xa7,0xd4}); patch(0x532b,{0xea,0xea,0xea});
  patch(0x52ef,{0xa9,selector[id]}); patch(0x572b,{0xa9,selector[id]});
  for(int slot=0;slot<4;slot++) {
    if(slot) memory.writeRam(0x1131+slot*2,peers[slots[id][slot]].car);
    memory.writeRam(0xc41+slot*2,palettes[id][slot]);
  }
  for(auto address:{0x5dfc,0x5dda,0xdb6,0x3db2,0x3dd5,0x3def,0x3dfe,0x3dae,0x3dcb,0x3de5}) patch(address,{0xea,0xea,0xea});
  patch(0xd3f,{0}); patch(0x48ff,{0x80}); patch(0x4d84,{0x80});
  patch(0x1851f,{0xa5,0x55,0xf0,0x74});
}
void Client::opponents() {
  for(int slot=1;slot<4;slot++) {
    auto& peer=peers[slots[id][slot]];
    bool visible=peer.status==FZ_PLAYER_LOADED || peer.status==FZ_PLAYER_RACING;
    putWord(0xb70+slot*2,visible ? peer.x : 0); putWord(0xb90+slot*2,visible ? peer.y : 0);
    memory.writeRam(0xbd1+slot*2,peer.orientation);
  }
  putWord(0xb78,0); putWord(0xb98,0);
}
void Client::frame() {
  if(!memory.ram || game==Disconnected) return;
  frames++;
  if(config.baseline) { if(memory.ram(0x54)==2) game=Race; else game=CarSelect; return; }
  switch(game) {
  case Init:
    patch(0x18176,{0xea}); patch(0x18143,{0x80}); change(CarSelect); break;
  case CarSelect:
    if(memory.ram(0x55)==3) { const uint8_t menu[4]={0,2,1,3}; car=menu[memory.ram(0x5a)&3]; }
    if(memory.ram(0x55)==5) { patch(0x1851f,{0x5c,0x1f,0x85,0x03}); change(Waiting); } break;
  case Waiting:
    if(id>=0 && !selectedSent && phase==FZ_LOBBY) { queue({FZ_SELECT,car}); selectedSent=true; }
    if(id>=0 && phase==FZ_LOADING) { prepareRace(); change(Prepare); } break;
  case Prepare:
    // Confirm league and class through the game's own handlers. The legacy
    // INC $55 shortcut at $03:87E6 skips class-confirmation initialization,
    // leaving the track's Mode 7 graphics corrupted on accurate emulation.
    patch(0x18795,{0x80}); patch(0x187fe,{0x80});
    memory.writeRam(0x53,track); memory.writeRam(0x5a,league); change(Location); break;
  case Location:
    patch(0xab1,{0xea,0xea,0xea,0xea,0xea,0xea});
    if(memory.ram(0x54)==2 && memory.ram(0x55)==0 && memory.ram(0x56)==2) {
      if(!loadedSent && phase==FZ_LOADING) { queue(position(FZ_LOADED)); loadedSent=true; }
      memory.writeRam(0x53,0);
      uint8_t mask=0; for(int i=0;i<4;i++) if(peers[i].status!=FZ_EMPTY && peers[i].status!=FZ_DISCONNECTED) mask|=(uint8_t)(1<<i);
      if(phase==FZ_RACING || (phase==FZ_COUNTDOWN && mask && (armedMask&mask)==mask && bestRtt<1e12 && (double)now()+offset>=(double)startTime)) {
        opponents(); patch(0xab1,{0xe6,0x55,0xe6,0x55,0x64,0x56});
        log("start_skew_us="+std::to_string((long long)((double)now()+offset-(double)startTime))); change(Race);
      }
    } break;
  case Race: {
    if(memory.ram(0x54)==3) { if(!finishedSent) { queue({FZ_FINISH}); finishedSent=true; } change(Finished); break; }
    auto data=position(0); fz_packet p{}; p.type=FZ_STATE; p.seq=++stateSeq; p.length=5; memcpy(p.payload,data.data()+1,5); send(p);
    opponents(); break;
  }
  case Finished:
    opponents(); if(phase==FZ_RESULTS) { resultsTime=now(); change(Results); } break;
  case Results:
    if(config.autoNext && id==host && resultsTime && now()-resultsTime>3000000) { queue({FZ_NEXT}); resultsTime=0; } break;
  case Disconnected: break;
  }
}
void Client::publish() {
  View v; v.connected=id>=0 && game!=Disconnected; v.player=id; v.host=host; v.expected=expected; v.phase=phase;
  v.race=race; v.track=track; v.league=league; v.rtt=rtt; v.jitter=jitter; v.stale=stale; v.retries=retries;
  v.loss=snapshots+gaps ? 100.0*gaps/(snapshots+gaps) : 0; v.peers=peers; v.message=message; v.patches=(unsigned)originals.size();
  static const char *names[]={"init","car-select","waiting","prepare","location-load","racing","finished","results","disconnected"};
  v.game=names[game];
  if(memory.ram) { v.ram54=memory.ram(0x54); v.ram55=memory.ram(0x55); v.ram56=memory.ram(0x56); v.x=word(0xb70); v.y=word(0xb90); }
  auto t=now(); double elapsed=(double)(t-rateTime)/1000000;
  if(elapsed>0) { v.rxRate=(rx-prevRx)/elapsed; v.txRate=(tx-prevTx)/elapsed; v.kbps=(bytes-prevBytes)/elapsed/1024; v.fps=(frames-prevFrames)/elapsed; }
  prevRx=rx; prevTx=tx; prevBytes=bytes; prevFrames=frames; rateTime=t; lastPublish=t;
  if(t-lastLog>5000000) {
    lastLog=t;
    fprintf(stdout,"%llu FZVS race=%u player=%d stats game=%s ram=%02x/%02x/%02x fps=%.1f rtt_ms=%.1f jitter_ms=%.1f loss=%.1f rx_pps=%.1f tx_pps=%.1f x=%u y=%u\n",
      (unsigned long long)t,race,id+1,v.game.c_str(),v.ram54,v.ram55,v.ram56,v.fps,v.rtt,v.jitter,v.loss,v.rxRate,v.txRate,v.x,v.y); fflush(stdout);
  }
  for(auto& event:events) v.events+=event+"\n";
  std::lock_guard<std::mutex> lock(mutex); published=std::move(v);
}
bool Client::scriptedButton(const std::string& name) {
  if(config.autoCar<0) return false;
  if(game==Race) return name=="B";
  bool pulse=frames%45<3;
  if(game==CarSelect && memory.ram && memory.ram(0x54)==1 && memory.ram(0x55)==1) {
    const int menu[4]={0,2,1,3};
    if(memory.ram(0x5a)!=menu[config.autoCar]) return name=="Down" && pulse;
  }
  return name=="Start" && pulse;
}
// A tiny frontend-only bitmap font. Overlay pixels never touch emulated VRAM.
void Client::overlay(uint32_t *pixels,unsigned width,unsigned height,unsigned pitch) {
  if(!config.overlay || !width || height<10) return;
  static const uint8_t font[][5]={
    {62,81,73,69,62},{0,66,127,64,0},{66,97,81,73,70},{33,65,69,75,49},{24,20,18,127,16},
    {39,69,69,69,57},{60,74,73,73,48},{1,113,9,5,3},{54,73,73,73,54},{6,73,73,41,30},
    {126,17,17,17,126},{127,73,73,73,54},{62,65,65,65,34},{127,65,65,34,28},{127,73,73,73,65},
    {127,9,9,9,1},{62,65,73,73,122},{127,8,8,8,127},{0,65,127,65,0},{32,64,65,63,1},
    {127,8,20,34,65},{127,64,64,64,64},{127,2,12,2,127},{127,4,8,16,127},{62,65,65,65,62},
    {127,9,9,9,6},{62,65,81,33,94},{127,9,25,41,70},{70,73,73,73,49},{1,1,127,1,1},
    {63,64,64,64,63},{31,32,64,32,31},{63,64,56,64,63},{99,20,8,20,99},{7,8,112,8,7},{97,81,73,69,67}
  };
  static const uint8_t percent[5]={0x23,0x13,0x08,0x64,0x62};
  auto v=view(); char line[160];
  snprintf(line,sizeof(line),"RTT %.0fMS JIT %.0fMS LOSS %.1f%%",v.rtt,v.jitter,v.loss);
  for(unsigned y=0;y<10;y++) for(unsigned x=0;x<width;x++) pixels[y*pitch+x]=(pixels[y*pitch+x]&0xff000000)|((pixels[y*pitch+x]&0x00fefefe)>>2);
  for(unsigned n=0;line[n] && n*6+7<width;n++) {
    char c=line[n];
    const uint8_t *glyph=c>='0'&&c<='9' ? font[c-'0'] : c>='A'&&c<='Z' ? font[10+c-'A'] : c=='%' ? percent : nullptr;
    for(unsigned x=0;x<5;x++) for(unsigned y=0;y<7;y++) {
      bool on=glyph ? (glyph[x]&(1u<<y))!=0 : c=='-' ? y==3 : c=='.' ? x==2&&y==6 : false;
      if(on) pixels[(2+y)*pitch+3+n*6+x]=0xffffffff;
    }
  }
}
}

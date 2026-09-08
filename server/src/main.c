#include "fzvs_protocol.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

typedef struct {
  struct sockaddr_in address;
  uint64_t token, nonce, seen, updated, armed;
  uint32_t command_seq, state_seq;
  uint16_t x,y;
  uint8_t status,car,orientation;
  uint64_t packets,bytes;
  uint64_t ping;
  double rtt,jitter;
} Player;
typedef struct {
  int socket, log_level, trace_player;
  FILE *json;
  uint64_t session,start,phase_since,rx,tx,rejected,stale;
  uint32_t race,snapshot_seq;
  uint8_t phase,expected,host,track,league;
  char key[FZ_KEY_MAX+1];
  Player players[4];
} Server;
static volatile sig_atomic_t stopping;
static uint64_t now_us(void) {
  struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
  return (uint64_t)t.tv_sec*1000000+(uint64_t)t.tv_nsec/1000;
}
static uint64_t random_id(void) {
  uint64_t id=0;
  int fd=open("/dev/urandom",O_RDONLY);
  if(fd<0 || read(fd,&id,sizeof(id))!=(ssize_t)sizeof(id)) { perror("random source"); exit(1); }
  close(fd); return id ? id : 1;
}
static void interrupt_server(int sig) { (void)sig; stopping=1; }
static void log_event(Server *s,int level,int player,const char *fmt,...) __attribute__((format(printf,4,5)));
static void log_event(Server *s,int level,int player,const char *fmt,...) {
  if(level>s->log_level || (level==3 && s->trace_player>=0 && s->trace_player!=player)) return;
  static const char *const levels[]={"WARN","INFO","DEBUG","TRACE"};
  char msg[512]; va_list ap; va_start(ap,fmt); vsnprintf(msg,sizeof(msg),fmt,ap); va_end(ap);
  uint64_t t=now_us();
  fprintf(stdout,"%" PRIu64 " %s session=%016" PRIx64 " race=%u player=%d %s\n",t,levels[level],s->session,s->race,player+1,msg);
  if(s->json) {
    fprintf(s->json,"{\"monotonic_us\":%" PRIu64 ",\"level\":\"%s\",\"session\":\"%016" PRIx64 "\",\"race\":%u,\"player\":%d,\"message\":\"",t,levels[level],s->session,s->race,player+1);
    for(const unsigned char *p=(const unsigned char*)msg;*p;p++) {
      if(*p=='"'||*p=='\\') fputc('\\',s->json);
      if(*p>=32) fputc(*p,s->json); else fprintf(s->json,"\\u%04x",*p);
    }
    fputs("\"}\n",s->json);
  }
}
static int occupied(const Player *p) { return p->status!=FZ_EMPTY && p->status!=FZ_DISCONNECTED; }
static unsigned count(Server *s) { unsigned n=0; for(int i=0;i<4;i++) n+=(unsigned)occupied(&s->players[i]); return n; }
static int same_address(const struct sockaddr_in *a,const struct sockaddr_in *b) {
  return a->sin_port==b->sin_port && a->sin_addr.s_addr==b->sin_addr.s_addr;
}
static void transmit(Server *s,const struct sockaddr_in *address,fz_packet *packet) {
  uint8_t data[FZ_MAX_PACKET]; size_t size=fz_encode(data,sizeof(data),packet);
  if(sendto(s->socket,data,size,0,(const struct sockaddr*)address,sizeof(*address))==(ssize_t)size) s->tx++;
}
static void send_player(Server *s,int i,fz_packet *packet) {
  Player *p=&s->players[i]; packet->session=s->session; packet->race=s->race;
  packet->token=p->token; packet->ack=p->command_seq; transmit(s,&p->address,packet);
}
static void phase(Server *s,uint8_t value) {
  if(s->phase==value) return;
  log_event(s,1,-1,"state=%s->%s",fz_phase_name(s->phase),fz_phase_name(value));
  s->phase=value; s->phase_since=now_us();
}
static void snapshot(Server *s) {
  fz_packet packet={0}; packet.type=FZ_SNAPSHOT; packet.length=FZ_SNAPSHOT_SIZE; packet.seq=++s->snapshot_seq;
  uint8_t *b=packet.payload; b[0]=s->phase; b[1]=s->expected; b[2]=s->host; b[3]=s->track; b[4]=s->league;
  uint64_t now=now_us(); fz_put64(b+8,now); fz_put64(b+16,s->start);
  for(int i=0;i<4;i++) {
    Player *p=&s->players[i]; uint8_t *q=b+24+i*16;
    if(occupied(p) && p->armed==s->start && s->start) b[5]|=(uint8_t)(1u<<i);
    q[0]=p->status; q[1]=p->car; fz_put16(q+2,p->x); fz_put16(q+4,p->y); q[6]=p->orientation;
    fz_put32(q+8,p->state_seq);
    uint64_t age=p->updated ? (now-p->updated)/1000 : 0;
    fz_put32(q+12,(uint32_t)(age>UINT32_MAX ? UINT32_MAX : age));
  }
  for(int i=0;i<4;i++) if(occupied(&s->players[i])) send_player(s,i,&packet);
}
static void begin_lobby(Server *s) {
  s->race++; s->start=0; phase(s,FZ_LOBBY);
  for(int i=0;i<4;i++) {
    Player *p=&s->players[i];
    if(!occupied(p)) { memset(p,0,sizeof(*p)); continue; }
    p->status=FZ_JOINED; p->x=p->y=0; p->armed=0; p->updated=0; p->state_seq=0;
  }
  log_event(s,1,-1,"new_lobby participants=%u",count(s));
}
static void drop(Server *s,int i,const char *reason) {
  Player *p=&s->players[i]; p->status=FZ_DISCONNECTED; p->x=p->y=0;
  log_event(s,0,i,"disconnected reason=%s",reason);
  if(s->host==i) { s->host=255; for(int j=0;j<4;j++) if(occupied(&s->players[j])) { s->host=(uint8_t)j; break; } }
  if(s->phase==FZ_LOBBY) memset(p,0,sizeof(*p));
  if(!count(s)) { begin_lobby(s); s->host=255; }
}
static int all_at_least(Server *s,uint8_t status) {
  if(!count(s)) return 0;
  for(int i=0;i<4;i++) if(occupied(&s->players[i]) && s->players[i].status<status) return 0;
  return 1;
}
static void advance(Server *s,uint64_t now) {
  for(int i=0;i<4;i++) if(occupied(&s->players[i]) && now-s->players[i].seen>=5000000) drop(s,i,"heartbeat_timeout");
  if(s->phase==FZ_LOBBY && count(s)==s->expected && all_at_least(s,FZ_SELECTED)) phase(s,FZ_LOADING);
  if(s->phase==FZ_LOADING && all_at_least(s,FZ_PLAYER_LOADED)) {
    s->start=now+2000000; phase(s,FZ_COUNTDOWN);
    for(int i=0;i<4;i++) s->players[i].armed=0;
  }
  if(s->phase==FZ_COUNTDOWN) {
    int all=1;
    for(int i=0;i<4;i++) if(occupied(&s->players[i]) && s->players[i].armed!=s->start) all=0;
    if(!all && now+500000>=s->start) {
      s->start=now+2000000;
      log_event(s,2,-1,"start_rescheduled waiting_for_ack=1");
    } else if(all && now>=s->start) {
      phase(s,FZ_RACING);
      for(int i=0;i<4;i++) if(occupied(&s->players[i])) s->players[i].status=FZ_PLAYER_RACING;
    }
  }
  if(s->phase==FZ_RACING && all_at_least(s,FZ_FINISHED)) phase(s,FZ_RESULTS);
}
static void reject(Server *s) { s->rejected++; }
static void welcome(Server *s,int i) {
  fz_packet reply={0}; reply.type=FZ_WELCOME; reply.length=9; reply.payload[0]=(uint8_t)i;
  fz_put64(reply.payload+1,s->players[i].nonce); send_player(s,i,&reply);
}
static void receive_packet(Server *s,const uint8_t *data,size_t size,const struct sockaddr_in *address) {
  fz_packet packet;
  if(!fz_decode(&packet,data,size)) { reject(s); return; }
  s->rx++;
  if(packet.type==FZ_HELLO) {
    if(packet.length<9 || packet.payload[8]>FZ_KEY_MAX || packet.length!=9+packet.payload[8]
       || packet.payload[8]!=strlen(s->key) || memcmp(packet.payload+9,s->key,packet.payload[8])) { reject(s); return; }
    uint64_t nonce=fz_u64(packet.payload);
    for(int i=0;i<4;i++) if(occupied(&s->players[i]) && same_address(address,&s->players[i].address)) {
      if(nonce==s->players[i].nonce) welcome(s,i);
      return;
    }
    if(s->phase!=FZ_LOBBY || count(s)>=s->expected) return;
    for(int i=0;i<4;i++) if(!occupied(&s->players[i])) {
      Player *p=&s->players[i]; memset(p,0,sizeof(*p)); p->address=*address;
      p->token=random_id(); p->nonce=nonce; p->seen=now_us(); p->status=FZ_JOINED;
      if(s->host==255) s->host=(uint8_t)i;
      log_event(s,1,i,"joined endpoint=%s:%u",inet_ntoa(address->sin_addr),ntohs(address->sin_port));
      welcome(s,i); snapshot(s); return;
    }
    return;
  }
  int index=-1;
  for(int i=0;i<4;i++) if(occupied(&s->players[i]) && packet.session==s->session && packet.token==s->players[i].token
       && same_address(address,&s->players[i].address)) { index=i; break; }
  if(index<0) { reject(s); return; }
  Player *p=&s->players[index];
  if(packet.type==FZ_PONG && packet.length==8 && p->ping && fz_u64(packet.payload)==p->ping) {
    uint64_t t=now_us(); double sample=(double)(t-p->ping)/1000;
    double delta=sample-p->rtt; if(delta<0) delta=-delta;
    p->jitter=p->jitter*0.9+delta*0.1; p->rtt=p->rtt ? p->rtt*0.8+sample*0.2 : sample;
    p->ping=0; p->seen=t; return;
  }
  if(packet.type==FZ_PING && packet.length==8) {
    p->seen=now_us(); fz_packet reply={0}; reply.type=FZ_PONG; reply.length=16;
    memcpy(reply.payload,packet.payload,8); fz_put64(reply.payload+8,p->seen); send_player(s,index,&reply); return;
  }
  if(packet.race!=s->race) { s->stale++; return; }
  if(packet.type==FZ_STATE) {
    if(packet.length!=5 || s->phase!=FZ_RACING || p->status!=FZ_PLAYER_RACING) { reject(s); return; }
    if(!fz_newer(packet.seq,p->state_seq)) { s->stale++; return; }
    p->state_seq=packet.seq; p->x=fz_u16(packet.payload); p->y=fz_u16(packet.payload+2); p->orientation=packet.payload[4];
    p->updated=p->seen=now_us(); p->packets++; p->bytes+=size;
    log_event(s,3,index,"state seq=%u x=%u y=%u orientation=%u",p->state_seq,p->x,p->y,p->orientation); return;
  }
  if(packet.type!=FZ_COMMAND || packet.length<1) { reject(s); return; }
  if(!fz_newer(packet.seq,p->command_seq)) { s->stale++; return; }
  if(packet.seq!=p->command_seq+1) { reject(s); return; }
  int valid=0; uint8_t cmd=packet.payload[0];
  switch(cmd) {
  case FZ_SELECT:
    valid=packet.length==2 && packet.payload[1]<4 && s->phase==FZ_LOBBY;
    if(valid) { p->car=packet.payload[1]; p->status=FZ_SELECTED; } break;
  case FZ_LOADED:
    valid=packet.length==6 && s->phase==FZ_LOADING;
    if(valid) { p->x=fz_u16(packet.payload+1); p->y=fz_u16(packet.payload+3); p->orientation=packet.payload[5]; p->status=FZ_PLAYER_LOADED; p->updated=now_us(); } break;
  case FZ_ARM:
    valid=packet.length==9 && s->phase==FZ_COUNTDOWN && fz_u64(packet.payload+1)==s->start;
    if(valid) p->armed=s->start; break;
  case FZ_FINISH:
    valid=packet.length==1 && s->phase==FZ_RACING && p->status==FZ_PLAYER_RACING;
    if(valid) { p->status=FZ_FINISHED; p->x=p->y=0; } break;
  case FZ_NEXT:
    valid=packet.length==1 && index==s->host && (s->phase==FZ_RESULTS || s->phase==FZ_LOBBY);
    break;
  case FZ_CONFIG:
    valid=packet.length==4 && index==s->host && s->phase==FZ_LOBBY && packet.payload[1]>=2 && packet.payload[1]<=4
      && packet.payload[1]>=count(s) && packet.payload[2]<5 && packet.payload[3]<3;
    if(valid) { s->expected=packet.payload[1]; s->track=packet.payload[2]; s->league=packet.payload[3]; } break;
  case FZ_LEAVE: valid=packet.length==1; break;
  default: break;
  }
  /* A well-sequenced command gets acknowledged even if its phase expired, so a
     rescheduled ARM or late UI action cannot block the reliable command stream. */
  p->command_seq=packet.seq; p->seen=now_us();
  if(!valid) {
    reject(s); log_event(s,2,index,"command_rejected type=%u phase=%s seq=%u",cmd,fz_phase_name(s->phase),packet.seq);
    fz_packet reply={0}; reply.type=FZ_ERROR; reply.length=2; reply.payload[0]=cmd; reply.payload[1]=1; send_player(s,index,&reply);
  } else {
    log_event(s,1,index,"command=%u status=%s seq=%u",cmd,fz_status_name(p->status),packet.seq);
    if(cmd==FZ_SELECT) log_event(s,1,index,"selected car=%s",fz_car_name(p->car));
    if(cmd==FZ_CONFIG) log_event(s,1,index,"configured players=%u league=%u track=%u",s->expected,s->league,s->track);
    if(cmd==FZ_NEXT) begin_lobby(s);
    if(cmd==FZ_LEAVE) drop(s,index,"left_room");
  }
  advance(s,now_us()); snapshot(s);
}
static int number(const char *text,int low,int high) {
  char *end; errno=0; long n=strtol(text,&end,10);
  if(errno || !*text || *end || n<low || n>high) { fprintf(stderr,"Invalid number: %s\n",text); exit(2); }
  return (int)n;
}
int main(int argc,char **argv) {
  Server s={0}; s.expected=2; s.host=255; s.race=1; s.log_level=1; s.trace_player=-1;
  const char *bind_address="127.0.0.1",*ready=NULL,*json_path=NULL; int port=12000;
  for(int i=1;i<argc;i++) {
    if(!strcmp(argv[i],"--help")) {
      puts("fzvs-server --bind IPv4 --port N --players 2..4 --track 0..4 --league 0..2\n"
           "  --room-key KEY --ready-file PATH --log-level info|debug|trace --json-log PATH --trace-player 1..4"); return 0;
    }
    if(i+1>=argc) { fprintf(stderr,"Missing value: %s\n",argv[i]); return 2; }
    const char *flag=argv[i],*value=argv[++i];
    if(!strcmp(flag,"--bind")) bind_address=value;
    else if(!strcmp(flag,"--port")) port=number(value,1,65535);
    else if(!strcmp(flag,"--players")) s.expected=(uint8_t)number(value,2,4);
    else if(!strcmp(flag,"--track")) s.track=(uint8_t)number(value,0,4);
    else if(!strcmp(flag,"--league")) s.league=(uint8_t)number(value,0,2);
    else if(!strcmp(flag,"--room-key")) { if(strlen(value)>FZ_KEY_MAX) return 2; strcpy(s.key,value); }
    else if(!strcmp(flag,"--ready-file")) ready=value;
    else if(!strcmp(flag,"--json-log")) json_path=value;
    else if(!strcmp(flag,"--trace-player")) s.trace_player=number(value,1,4)-1;
    else if(!strcmp(flag,"--log-level")) {
      if(!strcmp(value,"info")) s.log_level=1; else if(!strcmp(value,"debug")) s.log_level=2;
      else if(!strcmp(value,"trace")) s.log_level=3; else return 2;
    } else { fprintf(stderr,"Unknown option: %s\n",flag); return 2; }
  }
  setvbuf(stdout,NULL,_IOLBF,0);
  if(json_path) { s.json=fopen(json_path,"a"); if(!s.json) { perror("json-log"); return 1; } setvbuf(s.json,NULL,_IOLBF,0); }
  s.session=random_id(); s.phase_since=now_us();
  s.socket=socket(AF_INET,SOCK_DGRAM,0); if(s.socket<0) { perror("socket"); return 1; }
  struct sockaddr_in address={0}; address.sin_family=AF_INET; address.sin_port=htons((uint16_t)port);
  if(inet_pton(AF_INET,bind_address,&address.sin_addr)!=1) { fputs("--bind requires an IPv4 address\n",stderr); return 2; }
  if(bind(s.socket,(struct sockaddr*)&address,sizeof(address)) || fcntl(s.socket,F_SETFL,O_NONBLOCK)) { perror("bind/nonblocking"); return 1; }
  signal(SIGINT,interrupt_server); signal(SIGTERM,interrupt_server);
  if(ready) {
    FILE *file=fopen(ready,"w"); if(!file) { perror("ready-file"); return 1; }
    int failed=fprintf(file,"ready\n")<0; if(fclose(file)) failed=1;
    if(failed) { perror("ready-file write"); return 1; }
  }
  log_event(&s,1,-1,"listening address=%s port=%d players=%u",bind_address,port,s.expected);
  uint64_t tick=now_us(),summary=tick;
  while(!stopping) {
    struct pollfd fd={s.socket,POLLIN,0};
    uint64_t before=now_us();
    int timeout=(int)((tick>before ? tick-before : 0)/1000);
    if(timeout>16) timeout=16;
    if(poll(&fd,1,timeout)<0 && errno!=EINTR) { perror("poll"); break; }
    for(int budget=0;budget<128 && !stopping;budget++) {
      uint8_t bytes[FZ_MAX_PACKET+1]; struct sockaddr_in remote; socklen_t length=sizeof(remote);
      ssize_t n=recvfrom(s.socket,bytes,sizeof(bytes),0,(struct sockaddr*)&remote,&length);
      if(n<0) { if(errno!=EAGAIN && errno!=EWOULDBLOCK && errno!=EINTR) perror("recvfrom"); break; }
      receive_packet(&s,bytes,(size_t)n,&remote);
    }
    uint64_t now=now_us();
    if(now>=tick) { advance(&s,now); snapshot(&s); tick=now+16667; }
    if(now-summary>=5000000) {
      log_event(&s,2,-1,"network rx=%" PRIu64 " tx=%" PRIu64 " rejected=%" PRIu64 " stale=%" PRIu64,s.rx,s.tx,s.rejected,s.stale);
      for(int i=0;i<4;i++) if(occupied(&s.players[i])) {
        log_event(&s,2,i,"network rtt_ms=%.1f jitter_ms=%.1f rx_pps=%.1f bytes_per_sec=%.1f state_age_ms=%" PRIu64,
          s.players[i].rtt,s.players[i].jitter,
          (double)s.players[i].packets*1000000/(double)(now-summary),(double)s.players[i].bytes*1000000/(double)(now-summary),
          s.players[i].updated ? (now-s.players[i].updated)/1000 : 0);
        s.players[i].packets=s.players[i].bytes=0;
        fz_packet ping={0}; ping.type=FZ_PING; ping.length=8; s.players[i].ping=now;
        fz_put64(ping.payload,now); send_player(&s,i,&ping);
      }
      summary=now;
    }
  }
  log_event(&s,1,-1,"shutdown"); close(s.socket); if(ready) unlink(ready); if(s.json) fclose(s.json); return 0;
}

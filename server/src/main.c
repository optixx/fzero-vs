#include "fzvs_server.h"
#include "fzvs_protocol.h"
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <unistd.h>
static volatile sig_atomic_t stopping;
static void interrupt_server(int sig) { (void)sig; stopping=1; }
static void drain(fz_server *s,FILE *json) {
  fz_server_event e; const char *levels[]={"WARN","INFO","DEBUG","TRACE"};
  while(fz_server_next_event(s,&e)) {
    printf("%" PRIu64 " %s session=%016" PRIx64 " race=%u player=%d %s\n",e.time_us,levels[e.level],e.session,e.race,e.player+1,e.message);
    if(json) {
      fprintf(json,"{\"monotonic_us\":%" PRIu64 ",\"level\":\"%s\",\"session\":\"%016" PRIx64 "\",\"race\":%u,\"player\":%d,\"message\":\"",e.time_us,levels[e.level],e.session,e.race,e.player+1);
      for(const unsigned char *p=(const unsigned char*)e.message;*p;p++) {
        if(*p=='"'||*p=='\\') fputc('\\',json);
        if(*p>=32) fputc(*p,json); else fprintf(json,"\\u%04x",*p);
      }
      fputs("\"}\n",json);
    }
  }
}
static int number(const char *text,int low,int high) {
  char *end; errno=0; long n=strtol(text,&end,10);
  if(errno || !*text || *end || n<low || n>high) { fprintf(stderr,"Invalid number: %s\n",text); exit(2); }
  return (int)n;
}
int main(int argc,char **argv) {
  fz_server_config c={"127.0.0.1","",12000,2,0,0,1,-1,0};
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
    else if(!strcmp(flag,"--players")) c.players=(uint8_t)number(value,2,4);
    else if(!strcmp(flag,"--track")) c.track=(uint8_t)number(value,0,4);
    else if(!strcmp(flag,"--league")) c.league=(uint8_t)number(value,0,2);
    else if(!strcmp(flag,"--room-key")) { if(strlen(value)>FZ_KEY_MAX) return 2; c.room_key=value; }
    else if(!strcmp(flag,"--ready-file")) ready=value;
    else if(!strcmp(flag,"--json-log")) json_path=value;
    else if(!strcmp(flag,"--trace-player")) c.trace_player=number(value,1,4)-1;
    else if(!strcmp(flag,"--log-level")) {
      if(!strcmp(value,"info")) c.log_level=1; else if(!strcmp(value,"debug")) c.log_level=2;
      else if(!strcmp(value,"trace")) c.log_level=3; else return 2;
    } else { fprintf(stderr,"Unknown option: %s\n",flag); return 2; }
  }
  setvbuf(stdout,NULL,_IOLBF,0);
  FILE *json=NULL;
  if(json_path) { json=fopen(json_path,"a"); if(!json) { perror("json-log"); return 1; } setvbuf(json,NULL,_IOLBF,0); }
  c.bind_address=bind_address; c.port=(unsigned)port;
  char error[256]; fz_server *s=fz_server_create(&c,error,sizeof(error));
  if(!s) { fprintf(stderr,"%s\n",error); if(json) fclose(json); return 1; }
  signal(SIGINT,interrupt_server); signal(SIGTERM,interrupt_server);
  if(ready) {
    FILE *file=fopen(ready,"w"); int failed=!file;
    if(file) { failed=fprintf(file,"ready\n")<0; if(fclose(file)) failed=1; }
    if(failed) { perror("ready-file"); fz_server_destroy(s); if(json) fclose(json); return 1; }
  }
  int result=1;
  while(!stopping && result>0) { result=fz_server_poll(s,16); drain(s,json); }
  fz_server_close(s); drain(s,json); fz_server_destroy(s);
  if(ready) unlink(ready); if(json) fclose(json); return result<0 ? 1 : 0;
}

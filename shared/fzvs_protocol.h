#ifndef FZVS_PROTOCOL_H
#define FZVS_PROTOCOL_H
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define FZ_VERSION 1
#define FZ_HEADER 36
#define FZ_MAX_PACKET 256
#define FZ_PLAYERS 4
#define FZ_SNAPSHOT_SIZE 88
#define FZ_KEY_MAX 64
enum { FZ_HELLO=1, FZ_WELCOME, FZ_COMMAND, FZ_STATE, FZ_SNAPSHOT, FZ_PING, FZ_PONG, FZ_ERROR, FZ_JOIN_REJECT, FZ_ROOM_CLOSED };
enum { FZ_REJECT_PASSWORD=1, FZ_REJECT_FULL, FZ_REJECT_RACING, FZ_REJECT_STARTING, FZ_REJECT_VERSION };
enum { FZ_SELECT=1, FZ_LOADED, FZ_ARM, FZ_FINISH, FZ_NEXT, FZ_CONFIG, FZ_LEAVE };
enum { FZ_LOBBY, FZ_LOADING, FZ_COUNTDOWN, FZ_RACING, FZ_RESULTS };
enum { FZ_EMPTY, FZ_JOINED, FZ_SELECTED, FZ_PLAYER_LOADED, FZ_PLAYER_RACING, FZ_FINISHED, FZ_DISCONNECTED };
typedef struct {
  uint8_t type;
  uint16_t length;
  uint64_t session, token;
  uint32_t race, seq, ack;
  uint8_t payload[FZ_MAX_PACKET-FZ_HEADER];
} fz_packet;
static inline uint16_t fz_u16(const uint8_t *p) { return (uint16_t)(p[0] | (uint16_t)p[1]<<8); }
static inline uint32_t fz_u32(const uint8_t *p) { return (uint32_t)fz_u16(p) | (uint32_t)fz_u16(p+2)<<16; }
static inline uint64_t fz_u64(const uint8_t *p) { return (uint64_t)fz_u32(p) | (uint64_t)fz_u32(p+4)<<32; }
static inline void fz_put16(uint8_t *p,uint16_t v) { p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); }
static inline void fz_put32(uint8_t *p,uint32_t v) { fz_put16(p,(uint16_t)v); fz_put16(p+2,(uint16_t)(v>>16)); }
static inline void fz_put64(uint8_t *p,uint64_t v) { fz_put32(p,(uint32_t)v); fz_put32(p+4,(uint32_t)(v>>32)); }
static inline int fz_newer(uint32_t a,uint32_t b) { return a!=b && (uint32_t)(a-b)<UINT32_C(0x80000000); }
static inline size_t fz_encode(uint8_t *out,size_t capacity,const fz_packet *p) {
  size_t size=FZ_HEADER+p->length;
  if(p->length>sizeof(p->payload) || capacity<size || p->type<FZ_HELLO || p->type>FZ_ROOM_CLOSED) return 0;
  memcpy(out,"FZVS",4); out[4]=FZ_VERSION; out[5]=p->type; fz_put16(out+6,p->length);
  fz_put64(out+8,p->session); fz_put32(out+16,p->race); fz_put32(out+20,p->seq);
  fz_put32(out+24,p->ack); fz_put64(out+28,p->token); memcpy(out+FZ_HEADER,p->payload,p->length);
  return size;
}
static inline int fz_decode(fz_packet *p,const uint8_t *in,size_t size) {
  if(size<FZ_HEADER || size>FZ_MAX_PACKET || memcmp(in,"FZVS",4) || in[4]!=FZ_VERSION
     || in[5]<FZ_HELLO || in[5]>FZ_ROOM_CLOSED || fz_u16(in+6)!=size-FZ_HEADER) return 0;
  memset(p,0,sizeof(*p)); p->type=in[5]; p->length=fz_u16(in+6);
  p->session=fz_u64(in+8); p->race=fz_u32(in+16); p->seq=fz_u32(in+20);
  p->ack=fz_u32(in+24); p->token=fz_u64(in+28); memcpy(p->payload,in+FZ_HEADER,p->length);
  return 1;
}
static inline const char *fz_phase_name(unsigned n) {
  static const char *const names[]={"lobby","loading","countdown","racing","results"};
  return n<5 ? names[n] : "invalid";
}
static inline const char *fz_status_name(unsigned n) {
  static const char *const names[]={"empty","joined","selected","loaded","racing","finished","disconnected"};
  return n<7 ? names[n] : "invalid";
}
static inline const char *fz_car_name(unsigned n) {
  static const char *const names[]={"Blue Falcon","Wild Goose","Golden Fox","Fire Stingray"};
  return n<4 ? names[n] : "unknown";
}
/* Zero is a valid machine ID, but JOINED/EMPTY slots have no selection yet.
   Use status rather than the initialized car byte when presenting the roster. */
static inline const char *fz_selected_car_name(unsigned status,unsigned car) {
  return status>=FZ_SELECTED && status<=FZ_FINISHED ? fz_car_name(car) : "Machine not selected";
}
static inline const char *fz_player_color(unsigned player) {
  static const char *const colors[]={"Pink","Blue","Green","Yellow"};
  return player<4 ? colors[player] : "Unknown";
}
#endif

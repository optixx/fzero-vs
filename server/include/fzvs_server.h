#ifndef FZVS_SERVER_H
#define FZVS_SERVER_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* All calls for an instance belong to one thread. No global signals, streams,
   callbacks or process exits; callers choose their own lifecycle and log sinks. */
typedef struct fz_server fz_server;
typedef struct {
  const char *bind_address, *room_key;
  unsigned port, players, track, league;
  int log_level, trace_player;
  uint64_t owner_nonce; /* 0: dedicated server; otherwise reserve P1 for this nonce */
} fz_server_config;
typedef struct {
  uint64_t time_us,session;
  uint32_t race;
  int level,player;
  char message[512];
} fz_server_event;
typedef struct {
  uint8_t status,car;
  double rtt_ms,jitter_ms;
  uint64_t movement_packets,movement_bytes,age_ms;
} fz_server_peer;
typedef struct {
  uint64_t session,rx,tx,rejected,stale,dropped_events;
  uint32_t race;
  unsigned port,phase,players,occupied,host,track,league;
  int owner_joined,closed;
  fz_server_peer peers[4];
} fz_server_status;
fz_server *fz_server_create(const fz_server_config *,char *error,size_t error_size);
/* Bounded to 128 packets and at most 16 ms of waiting. 1=running, 0=closed,
   -1=socket failure. On failure an error event is available to drain. */
int fz_server_poll(fz_server *,unsigned wait_ms);
void fz_server_get_status(fz_server *,fz_server_status *);
int fz_server_next_event(fz_server *,fz_server_event *);
/* Notify peers while their authenticated sessions still exist; idempotent. */
void fz_server_close(fz_server *);
void fz_server_destroy(fz_server *);
#ifdef __cplusplus
}
#endif
#endif

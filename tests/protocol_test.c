#include "fzvs_protocol.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
  fz_packet p={0},q; uint8_t data[FZ_MAX_PACKET];
  p.type=FZ_STATE; p.length=5; p.session=UINT64_C(0x123456789abcdef0);
  p.token=UINT64_MAX; p.seq=UINT32_MAX; p.race=7;
  fz_put16(p.payload,0x1234); fz_put16(p.payload+2,0x5678); p.payload[4]=9;
  size_t n=fz_encode(data,sizeof(data),&p);
  assert(n==41 && data[36]==0x34 && data[37]==0x12 && data[8]==0xf0);
  assert(fz_decode(&q,data,n) && q.session==p.session && q.token==p.token);
  assert(fz_newer(0,UINT32_MAX) && !fz_newer(UINT32_MAX,0));
  for(size_t i=0;i<n;i++) assert(!fz_decode(&q,data,i));
  uint32_t random=1;
  for(unsigned i=0;i<100000;i++) {
    for(unsigned j=0;j<sizeof(data);j++) { random=random*1664525+1013904223; data[j]=(uint8_t)(random>>24); }
    (void)fz_decode(&q,data,i%257);
  }
  puts("Protocol roundtrip, endian, truncation, sequence wrap and 100000 malformed inputs passed.");
}

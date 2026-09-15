#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>
#define private public
#include "fzvs_client.hpp"
#undef private
#include <cassert>

using namespace fzvs;

int main() {
  Client client;
  client.jitter=0;
  assert(client.interpolationDelay()==50000);
  client.jitter=5;
  assert(client.interpolationDelay()==60000);
  client.jitter=100;
  assert(client.interpolationDelay()==100000);
  client.jitter=0;

  Client::Snapshot first{},second{};
  first.time=1000000; second.time=1100000;
  first.peers[3]={FZ_PLAYER_RACING,0,250,100,200,1,0};
  second.peers[3]={FZ_PLAYER_RACING,0,10,300,400,2,0};
  client.snapshotBuffer={first,second};

  // A 50 ms delay turns this into the midpoint between the two samples.
  auto midpoint=client.renderedPeers(1100000)[3];
  assert(midpoint.x==200 && midpoint.y==300 && midpoint.orientation==2);
  // No prediction: a gap holds the latest snapshot exactly.
  auto held=client.renderedPeers(1300000)[3];
  assert(held.x==300 && held.y==400 && held.orientation==10);
  // A single sample is immediately usable while the buffer fills.
  client.snapshotBuffer={first};
  auto startup=client.renderedPeers(2000000)[3];
  assert(startup.x==100 && startup.y==200 && startup.orientation==250);

  std::vector<uint8_t> ram(128*1024);
  client.memory.writeRam=[&](uint32_t address,uint8_t value) { ram.at(address)=value; };
  client.id=0;
  client.peers[3]={FZ_DISCONNECTED,0,0,0,0,0,0};
  client.snapshotBuffer={first};
  client.opponents();
  assert(fz_u16(ram.data()+0xb72)==0 && fz_u16(ram.data()+0xb92)==0 && ram[0xbd3]==0);

  client.peers[3]=first.peers[3];
  for(unsigned i=0;i<17;i++) { client.peers[3].x=(uint16_t)i; client.bufferSnapshot(2000000+i); }
  assert(client.snapshotBuffer.size()==16 && client.snapshotBuffer.front().peers[3].x==1);
  client.resetGame();
  assert(client.snapshotBuffer.empty());

  puts("Snapshot interpolation, buffering, gap hold and visibility behavior passed.");
}

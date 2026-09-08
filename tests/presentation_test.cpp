#include "fzvs_presentation.hpp"
#include <cassert>
#include <cstdio>
using namespace fzvs;
int main() {
  View v;auto p=presentRoom(v);assert(p.players.empty() && !p.canConfigure && p.guidance=="Connect to a room to see network statistics");
  assert(connectionProgress("resolving",false)=="Looking up the host…");
  assert(connectionProgress("starting",true).find("P1")!=std::string::npos);
  v.connected=true;v.player=v.host=0;v.expected=4;v.peers[0].status=FZ_JOINED;
  p=presentRoom(v);assert(p.players.size()==4 && p.occupied==1 && p.canConfigure && !p.canNext);
  assert(p.players[1].find("Open slot")!=std::string::npos && p.guidance=="Waiting for players");
  for(auto& peer:v.peers)peer.status=FZ_JOINED;
  assert(presentRoom(v).guidance=="Select your machine in the game");
  v.peers[0].status=FZ_SELECTED;assert(presentRoom(v).guidance=="Waiting for machine selections");
  assert(!validRoomChange(v,3,0,0));assert(validRoomChange(v,4,4,2));assert(!validRoomChange(v,4,0,0));
  v.player=1;assert(!presentRoom(v).canConfigure && !validRoomChange(v,4,4,2));
  v.phase=FZ_LOADING;assert(presentRoom(v).guidance=="Loading track");
  v.phase=FZ_COUNTDOWN;assert(presentRoom(v).guidance=="Race starting");
  v.phase=FZ_RACING;assert(presentRoom(v).guidance=="Racing" && !presentRoom(v).canNext);
  v.phase=FZ_RESULTS;assert(presentRoom(v).guidance=="Race complete" && !presentRoom(v).canNext);
  v.player=0;assert(presentRoom(v).canNext);
  v.connected=false;v.game="disconnected";assert(presentRoom(v).players.empty());
  v.connected=true;v.phase=FZ_LOBBY;v.expected=2;assert(presentRoom(v).players.size()==2);
  assert(defaultRoomName("MacBookPro.localdomain")=="MacBookPro's game");assert(defaultRoomName(std::string(100,'x')).size()<=63);
  puts("Presentation: setup, connecting, host/guest lobby, race phases, vacancies and configuration validation passed.");
}

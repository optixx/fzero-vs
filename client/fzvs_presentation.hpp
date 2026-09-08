#pragma once
#include "fzvs_client.hpp"
#include <algorithm>

namespace fzvs {
struct RoomPresentation {
  bool connected=false,canConfigure=false,canNext=false;
  unsigned occupied=0;
  std::string guidance;
  std::vector<std::string> players;
};
inline RoomPresentation presentRoom(const View& v) {
  RoomPresentation p;p.connected=v.connected;
  if(!v.connected){p.guidance="Connect to a room to see network statistics";return p;}
  for(int i=0;i<std::clamp(v.expected,2,4);i++) {
    const auto& peer=v.peers[i];bool occupied=peer.status!=FZ_EMPTY && peer.status!=FZ_DISCONNECTED;
    if(occupied)p.occupied++;
    std::string text="P"+std::to_string(i+1)+"  ·  Color: "+fz_player_color(i);
    if(i==v.player)text+="  ·  You";
    if(i==v.host)text+="  ·  Host";
    text+="  —  ";text+=occupied?std::string("Machine: ")+fz_selected_car_name(peer.status,peer.car):"Open slot";
    p.players.push_back(text);
  }
  p.canConfigure=v.player==v.host && v.phase==FZ_LOBBY;
  p.canNext=v.player==v.host && v.phase==FZ_RESULTS;
  if(v.phase==FZ_RESULTS)p.guidance="Race complete";
  else if(v.phase==FZ_RACING)p.guidance="Racing";
  else if(v.phase==FZ_COUNTDOWN)p.guidance="Race starting";
  else if(v.phase==FZ_LOADING)p.guidance="Loading track";
  else if(p.occupied<(unsigned)v.expected)p.guidance="Waiting for players";
  else if(v.player>=0 && v.player<4 && v.peers[v.player].status<FZ_SELECTED)p.guidance="Select your machine in the game";
  else p.guidance="Waiting for machine selections";
  return p;
}
inline std::string connectionProgress(const std::string& state,bool hosting) {
  if(state=="resolving")return "Looking up the host…";
  if(state=="stopping")return "Leaving room…";
  return hosting?"Creating room and connecting as P1…":"Connecting to room…";
}
inline bool validRoomChange(const View& v,unsigned players,unsigned track,unsigned league) {
  auto p=presentRoom(v);
  return p.canConfigure && players>=2 && players<=4 && players>=p.occupied && track<5 && league<3
    && (players!=(unsigned)v.expected || track!=(unsigned)v.track || league!=(unsigned)v.league);
}
inline std::string defaultRoomName(std::string computer) {
  auto dot=computer.find('.');if(dot!=std::string::npos)computer.resize(dot);
  if(computer.empty())computer="F-Zero VS";
  return computer.substr(0,56)+"'s game";
}
}

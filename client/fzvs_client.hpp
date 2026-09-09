#pragma once
#include "fzvs_protocol.h"
#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace fzvs {
struct Config {
  bool enabled=false,mute=false,overlay=true,capture=false,autoNext=false,baseline=false;
  std::string address="127.0.0.1:12000",key,profile,label="F-Zero VS",input="keyboard";
  uint64_t ownerNonce=0; // supplied only by the in-process host controller
  int x=40,y=60,width=640,height=480,autoCar=-1;
};
struct Memory {
  std::function<uint8_t(uint32_t)> ram,rom;
  std::function<void(uint32_t,uint8_t)> writeRam,writeRom;
  std::function<void()> reset;
};
struct Peer { uint8_t status=0,car=0,orientation=0; uint16_t x=0,y=0; uint32_t seq=0,age=0; };
struct View {
  bool connected=false;
  int player=-1,host=-1,expected=2,phase=0,track=0,league=0;
  uint32_t race=0;
  double rtt=0,jitter=0,loss=0,rxRate=0,txRate=0,kbps=0,fps=0;
  uint64_t stale=0,retries=0;
  uint8_t ram54=0,ram55=0,ram56=0;
  uint16_t x=0,y=0;
  unsigned patches=0;
  std::string game="unloaded",message="Load the supported F-Zero US ROM",events;
  std::array<Peer,4> peers;
};
class Client {
public:
  Config config;
  bool attach(Memory memory); // caller validates the ROM SHA-256 first
  void detach();
  void tick();
  void frame();
  View view();
  void request(uint8_t command,std::vector<uint8_t> data={});
  bool scriptedButton(const std::string& name);
  void overlay(uint32_t *pixels,unsigned width,unsigned height,unsigned pitch);
  bool active() const { return config.enabled; }
  void failure(const std::string& text);
private:
  enum Game { Init,CarSelect,Waiting,Prepare,Location,Race,Finished,Results,Disconnected };
  struct Pending { fz_packet packet{}; uint64_t sent=0; };
  struct Snapshot { uint64_t time=0; std::array<Peer,4> peers{}; };
  Memory memory;
  int socket=-1,id=-1;
  uint64_t session=0,token=0,nonce=0,lastRecv=0,lastHello=0,lastPing=0,startTime=0,armedTime=0,lastPublish=0,lastLog=0,resultsTime=0;
  uint32_t race=0,commandSeq=0,stateSeq=0,snapshotSeq=0;
  uint8_t phase=0,host=255,expected=2,track=0,league=0,armedMask=0,car=0;
  uint64_t rx=0,tx=0,bytes=0,stale=0,retries=0,gaps=0,snapshots=0,frames=0;
  uint64_t prevRx=0,prevTx=0,prevBytes=0,prevFrames=0,rateTime=0;
  double rtt=0,jitter=0,offset=0,bestRtt=1e12;
  std::array<Peer,4> peers;
  std::deque<Snapshot> snapshotBuffer;
  Game game=Init;
  bool loadedSent=false,selectedSent=false,finishedSent=false;
  std::map<uint32_t,uint8_t> originals;
  std::deque<Pending> pending;
  std::mutex mutex;
  View published;
  std::deque<std::vector<uint8_t>> requests;
  std::deque<std::string> events;
  std::string message;
  void send(fz_packet packet);
  void queue(const std::vector<uint8_t>& command);
  void receive(const fz_packet& packet);
  void publish();
  void log(const std::string& text);
  void change(Game next);
  void resetGame();
  void patch(uint32_t offset,std::initializer_list<uint8_t> bytes);
  void restore();
  void prepareRace();
  void opponents();
  uint64_t interpolationDelay() const;
  std::array<Peer,4> renderedPeers(uint64_t serverTime) const;
  void bufferSnapshot(uint64_t serverTime);
  void clearSnapshotBuffer();
  uint16_t word(uint32_t address);
  void putWord(uint32_t address,uint16_t value);
  std::vector<uint8_t> position(uint8_t command);
};
extern Client client;
}

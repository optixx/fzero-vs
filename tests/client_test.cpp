#include "fzvs_client.hpp"
#include <cassert>
#include <chrono>
#include <thread>
#include <cstdio>
#include <cstdlib>

// Exercise the real client against the real server with observable memory.
// This does not emulate the game: real ROM execution is covered by the desktop harness.
struct Machine {
  fzvs::Client client;
  std::vector<uint8_t> ram=std::vector<uint8_t>(128*1024), rom=std::vector<uint8_t>(512*1024,0x42);
  unsigned resets=0;
  void attach(const char* port) {
    client.config.enabled=true; client.config.address=std::string("127.0.0.1:")+port;
    client.config.key="memory-test";
    assert(client.attach({[&](uint32_t a){return ram.at(a);},[&](uint32_t a){return rom.at(a);},
      [&](uint32_t a,uint8_t v){ram.at(a)=v;},[&](uint32_t a,uint8_t v){rom.at(a)=v;},
      [&]{resets++; std::fill(ram.begin(),ram.end(),0);}}));
  }
};
int main(int argc,char** argv) {
  assert(argc==5); std::array<Machine,4> m;
  for(int i=0;i<4;i++) m[i].attach(argv[i+1]);
  auto pump=[&] { for(auto& x:m) {x.client.tick();x.client.frame();} std::this_thread::sleep_for(std::chrono::milliseconds(3)); };
  auto wait=[&](auto predicate) {
    auto end=std::chrono::steady_clock::now()+std::chrono::seconds(12);
    while(!predicate()) { assert(std::chrono::steady_clock::now()<end); pump(); }
  };
  wait([&]{for(auto& x:m) if(!x.client.view().connected)return false;return true;});
  std::array<Machine*,4> ids{};
  for(auto& x:m) {auto id=x.client.view().player;assert(id>=0&&id<4&&!ids[id]);ids[id]=&x;}
  constexpr uint8_t slots[4][4]={{0,3,2,1},{1,3,2,0},{2,3,1,0},{3,2,1,0}};
  constexpr uint8_t palette[4][4]={{14,12,10,8},{8,12,10,14},{10,12,8,14},{12,10,8,14}};
  for(int round=0;round<2;round++) {
    ids[0]->client.request(FZ_CONFIG,{4,(uint8_t)(round?4:0),(uint8_t)(round?2:0)});
    wait([&]{return ids[0]->client.view().track==(round?4:0);});
    for(int id=0;id<4;id++) {auto& x=*ids[id];x.ram[0x55]=3;x.ram[0x5a]=std::array<uint8_t,4>{0,2,1,3}[id];}
    pump();
    for(auto& x:m) x.ram[0x55]=5;
    wait([&]{for(auto& x:m) if(x.client.view().game!="location-load")return false;return true;});
    for(int id=0;id<4;id++) {
      auto& x=*ids[id];
      assert(x.rom[0x18176]==0xea&&x.rom[0x18143]==0x80);
      assert(x.rom[0x5486]==0x4c&&x.rom[0x5487]==0xa7&&x.rom[0x5488]==0xd4);
      for(auto a:{0x532b,0x5dfc,0x5dda,0xdb6,0x3db2,0x3dd5,0x3def,0x3dfe,0x3dae,0x3dcb,0x3de5})
        for(int n=0;n<3;n++)assert(x.rom[a+n]==0xea);
      assert((x.rom[0x52ef]==0xa9&&x.rom[0x52f0]==std::array<int,4>{3,0,1,2}[id]));
      assert((x.rom[0x572b]==0xa9&&x.rom[0x572c]==std::array<int,4>{3,0,1,2}[id]));
      assert(x.rom[0xd3f]==0&&x.rom[0x48ff]==0x80&&x.rom[0x4d84]==0x80);
      assert(x.rom[0x18795]==0x80&&x.rom[0x187fe]==0x80&&x.rom[0x187e6]==0x42);
      assert(x.ram[0x53]==(round?4:0)&&x.ram[0x5a]==(round?2:0));
      for(int s=0;s<4;s++) {assert(x.ram[0xc41+s*2]==palette[id][s]);if(s)assert(x.ram[0x1131+s*2]==slots[id][s]);}
      for(int n=0;n<6;n++)assert(x.rom[0xab1+n]==0xea);
      x.ram[0x54]=2;x.ram[0x55]=0;x.ram[0x56]=2;
      fz_put16(x.ram.data()+0xb70,(uint16_t)(1000+id));fz_put16(x.ram.data()+0xb90,(uint16_t)(2000+id));x.ram[0xbd1]=(uint8_t)(30+id);
    }
    wait([&]{for(auto& x:m)if(x.client.view().game!="racing")return false;return true;});
    for(int id=0;id<4;id++) {
      auto& x=*ids[id];assert(x.rom[0xab1]==0xe6&&x.rom[0xab6]==0x56);
      for(int s=1;s<4;s++) {auto peer=slots[id][s];assert(fz_u16(x.ram.data()+0xb70+s*2)==1000+peer);assert(fz_u16(x.ram.data()+0xb90+s*2)==2000+peer);assert(x.ram[0xbd1+s*2]==30+peer);}
      assert(fz_u16(x.ram.data()+0xb78)==0&&fz_u16(x.ram.data()+0xb98)==0);
      x.ram[0x54]=3;
    }
    wait([&]{for(auto& x:m)if(x.client.view().game!="results")return false;return true;});
    auto generation=ids[0]->client.view().race;
    ids[0]->client.request(FZ_NEXT);
    wait([&]{for(auto& x:m)if(x.client.view().race!=generation+1)return false;return true;});
    for(auto& x:m) {assert(x.resets==(unsigned)(round+2));assert(x.rom[0x5dda]==0x42);assert(x.rom[0xab1]==0x42);}
  }
  for(auto& x:m) {x.client.detach();for(auto b:x.rom)assert(b==0x42);}
  puts("Client memory, four slot mappings, barriers, results and ROM restoration passed");
}

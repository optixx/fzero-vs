#pragma once
#include "fzvs_server.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace fzvs {
struct HostOptions {
  std::string bind="0.0.0.0",key,name,logPath,readyFile;
  unsigned port=12000,players=2,track=0,league=0;
};
struct Startup { bool requested=false,hosting=false; HostOptions options; };
extern Startup startup;
struct Room {
  uint64_t session=0,seen=0;
  std::string name,address;
  unsigned players=0,occupied=0,phase=0,track=0,league=0,version=0;
  bool password=false;
};
struct Connection {
  std::string address,key;
  uint64_t ownerNonce=0,generation=0;
};
enum class DiscoveryPhase { Idle, Searching, Empty, Rooms, Unavailable };
struct DiscoveryFailure { std::string operation,interface,message; int code=0; };
struct DiscoveryStatus {
  DiscoveryPhase phase=DiscoveryPhase::Idle;
  std::vector<DiscoveryFailure> failures;
  uint64_t queries=0,replies=0;
};
struct SessionView {
  std::string state="idle",message,discoveryWarning,addresses;
  DiscoveryStatus discovery;
  bool hosting=false;
  uint64_t generation=0;
  fz_server_status server{};
  std::vector<Room> rooms;
  std::deque<fz_server_event> events;
};
// The server and discovery sockets are owned exclusively by worker. GUI callers
// consume value snapshots; the worker never calls the emulator or UI.
class Session {
public:
  Session()=default;
  ~Session();
  void start();
  void shutdown();
  bool host(HostOptions);
  bool join(std::string address,std::string key);
  void stop();
  void browse(bool enabled);
  void connected();
  void retryDiscovery();
  SessionView view();
  std::optional<Connection> takeConnection();
private:
  struct Command { int type; HostOptions host; std::string address,key; uint64_t generation; };
  std::mutex mutex;
  std::condition_variable wake;
  std::deque<Command> commands;
  std::optional<Connection> connection;
  SessionView published;
  std::thread worker;
  std::atomic<bool> quitting{false},browsing{false},discoveryRetry{false};
  uint64_t generation=0;
  bool enqueue(Command);
  void run();
};
extern Session session;
}

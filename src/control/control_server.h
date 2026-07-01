// Local HTTP control + status API for headless operation.
//
// A screenless device has no touch UI, so sibling modules on the same device
// (the voice-command module, the phone-command module) drive the assistant
// through this API. Those modules own the actual voice / phone protocols;
// this server only exposes jusiai-assistant's own functions:
//
//   POST /start             begin the AI call
//   POST /stop              end the AI call
//   POST /mic     {muted}   set microphone mute on/off
//   POST /camera  {enabled} set camera on/off
//   GET  /status            current state as JSON
//   GET  /events            Server-Sent Events stream of state changes
//   GET  /healthz           liveness probe
//
// Bound to 127.0.0.1 by default — the sibling modules are local.
#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <thread>

#include "app_config.h"

namespace httplib {
class Server;
}

namespace jusiai {

class AssistantController;

class ControlServer {
 public:
  ControlServer(AssistantController* controller, const AppConfig& config);
  ~ControlServer();

  ControlServer(const ControlServer&) = delete;
  ControlServer& operator=(const ControlServer&) = delete;

  // Bind the port and start serving on background threads. Returns false if
  // the port could not be bound.
  bool start();
  // Stop serving, close any open event streams and join all threads.
  void stop();

 private:
  // Poll the controller and push an SSE event whenever the state changes.
  void watch_loop();

  AssistantController* controller_;
  std::string bind_addr_;
  int port_;

  std::unique_ptr<httplib::Server> server_;
  std::thread server_thread_;
  std::thread watch_thread_;
  std::atomic<bool> running_{false};

  // SSE fan-out — defined in the .cpp.
  struct EventHub;
  std::unique_ptr<EventHub> hub_;
};

}  // namespace jusiai

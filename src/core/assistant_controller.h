// AssistantController — drives the one-tap "AI assistant" closed loop:
//
//   create room (device API)  ->  connect to LiveKit  ->  publish mic + camera
//   ->  dispatch the AI agent  ->  converse  ->  stop agent + disconnect
//
// All blocking work (HTTP, LiveKit connect/disconnect) runs on a private
// worker thread so the UI thread never stalls. UI code interacts through the
// request_*() commands and polls snapshot() each frame.
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "api/device_api_client.h"
#include "app_config.h"
#include "media/audio_io.h"
#include "media/camera_io.h"
#include "media/frame_buffer.h"
#include "rtc/livekit_session.h"

namespace jusiai {

// High-level lifecycle state of a conversation.
enum class AssistantState {
  Idle,          // ready, no conversation
  CreatingRoom,  // calling the device API to create a room
  Connecting,    // joining the LiveKit room, publishing tracks
  WaitingAgent,  // agent dispatched, waiting for it to join
  InCall,        // agent online, conversation in progress
  Stopping,      // tearing the conversation down
  Error,         // last operation failed
};

// Immutable view of the controller state for the UI to render.
struct UiSnapshot {
  AssistantState state = AssistantState::Idle;
  std::string status;          // primary status line
  std::string detail;          // secondary line (hint or error)
  bool mic_muted = false;
  bool cam_muted = false;
  bool agent_speaking = false;
  bool camera_available = false;
};

class AssistantController {
 public:
  AssistantController(const AppConfig& config);
  ~AssistantController();

  AssistantController(const AssistantController&) = delete;
  AssistantController& operator=(const AssistantController&) = delete;

  // Start the preview camera and the worker thread. Call once.
  bool init();
  // Stop the conversation (if any), worker thread and media engines.
  void shutdown();

  // UI / control commands — non-blocking, handled on the worker thread.
  void request_start();
  void request_stop();
  void request_toggle_mute();
  void request_toggle_camera();
  // Explicit (idempotent) variants for the remote control API, which knows the
  // desired state rather than just wanting a flip.
  void request_set_mic_muted(bool muted);
  void request_set_camera_muted(bool muted);

  // Thread-safe snapshot for rendering.
  UiSnapshot snapshot() const;

  // Latest camera frame for the preview widget.
  FrameBuffer* preview() { return &preview_; }

 private:
  enum class EventType {
    CmdStart,
    CmdStop,
    CmdToggleMute,
    CmdToggleCamera,
    CmdSetMute,
    CmdSetCamera,
    CmdQuit,
    EvAgentOnline,
    EvAgentOffline,
    EvDisconnected,
  };
  struct Event {
    EventType type;
    std::string text;
    bool flag = false;  // payload for CmdSetMute / CmdSetCamera
  };

  void worker_loop();
  void post(EventType type, std::string text = {}, bool flag = false);
  void handle(const Event& ev);

  // Closed-loop steps (worker thread only).
  void do_start();
  void do_stop();
  void teardown(AssistantState final_state, const std::string& status,
                const std::string& detail);

  void set_state(AssistantState state, const std::string& status,
                 const std::string& detail = {});
  AssistantState current_state() const;

  AppConfig config_;
  DeviceApiClient api_;

  FrameBuffer preview_;
  AudioEngine audio_;
  CameraEngine camera_;

  std::unique_ptr<LiveKitSession> session_;  // worker thread only
  std::string room_id_;                      // worker thread only

  // Worker thread + event queue.
  std::thread worker_;
  std::mutex queue_mutex_;
  std::condition_variable queue_cv_;
  std::deque<Event> queue_;
  bool worker_running_ = false;

  // WaitingAgent timeout deadline (worker thread only).
  std::chrono::steady_clock::time_point agent_deadline_{};

  // State exposed to the UI.
  mutable std::mutex state_mutex_;
  AssistantState state_ = AssistantState::Idle;
  std::string status_text_ = "Ready";
  std::string detail_text_;

  std::atomic<bool> mic_muted_{false};
  std::atomic<bool> cam_muted_{false};
  std::atomic<bool> agent_speaking_{false};
};

}  // namespace jusiai

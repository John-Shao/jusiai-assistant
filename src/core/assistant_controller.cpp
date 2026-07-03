#include "core/assistant_controller.h"

#include "log.h"

namespace jusiai {
namespace {

constexpr auto kAgentJoinTimeout = std::chrono::seconds(45);

// Human-readable status / hint strings. Machine-readable state is in
// AssistantState; these are Simplified Chinese labels the control API
// forwards to the phone/remote client for display.
constexpr const char* kStatusReady        = "就绪";
constexpr const char* kHintWaiting        = "等待呼叫指令";
constexpr const char* kStatusCreatingRoom = "正在创建房间...";
constexpr const char* kStatusConnecting   = "正在连接...";
constexpr const char* kStatusWakingAgent  = "正在唤起 AI 助手...";
constexpr const char* kStatusListening    = "聆听中";
constexpr const char* kStatusEndingCall   = "正在结束通话...";
constexpr const char* kStatusCallEnded    = "通话已结束";
constexpr const char* kHintAgentLeft      = "AI 助手已离开房间";
constexpr const char* kErrAgentNoResponse = "AI 助手未响应";
constexpr const char* kHintTryAgain       = "请重试";
constexpr const char* kErrDisconnected    = "连接已断开";
constexpr const char* kHintConnectionLost = "网络连接已丢失";
constexpr const char* kErrCreateRoom      = "无法创建房间";
constexpr const char* kErrDeviceAuth      = "设备认证失败";
constexpr const char* kErrQuota           = "使用配额已用尽";
constexpr const char* kErrConnect         = "无法连接";
constexpr const char* kHintMediaServer    = "无法连接到媒体服务器";
constexpr const char* kErrPublishAudio    = "无法发布音频";

// Map a start-chat-bot failure HTTP status to a user-facing state label.
// 401/403 => device key / device_id binding problem (config, not transient);
// 429 => usage quota exhausted (transient, resets with the quota window);
// anything else => generic "cannot create room". The backend's own message is
// carried separately as the Error detail.
const char* start_error_label(int http_status) {
  if (http_status == 401 || http_status == 403) return kErrDeviceAuth;
  if (http_status == 429) return kErrQuota;
  return kErrCreateRoom;
}

}  // namespace

AssistantController::AssistantController(const AppConfig& config)
    : config_(config),
      api_(config.base_url, config.device_api_key, config.tls_verify),
      audio_(config.audio_sample_rate, config.audio_channels,
             config.audio_mic_gain, config.audio_aec,
             config.audio_aec_delay_ms),
      camera_(config.camera_width, config.camera_height, config.camera_fps,
              config.camera_rotation, config.camera_device) {}

AssistantController::~AssistantController() { shutdown(); }

bool AssistantController::init() {
  if (worker_running_) return true;

  if (!camera_.start()) {
    LOG_WARN("controller: camera engine failed to start");
  }

  worker_running_ = true;
  worker_ = std::thread(&AssistantController::worker_loop, this);
  set_state(AssistantState::Idle, kStatusReady, kHintWaiting);
  LOG_INFO("controller: ready (%s)", config_.summary().c_str());
  return true;
}

void AssistantController::shutdown() {
  if (worker_running_) {
    post(EventType::CmdQuit);
    if (worker_.joinable()) worker_.join();
    worker_running_ = false;
  }
  camera_.stop();
  audio_.stop_mic();
  audio_.stop_speaker();
}

// --- UI commands ---------------------------------------------------------

void AssistantController::request_start() { post(EventType::CmdStart); }
void AssistantController::request_stop() { post(EventType::CmdStop); }
void AssistantController::request_toggle_mute() {
  post(EventType::CmdToggleMute);
}
void AssistantController::request_toggle_camera() {
  post(EventType::CmdToggleCamera);
}
void AssistantController::request_set_mic_muted(bool muted) {
  post(EventType::CmdSetMute, {}, muted);
}
void AssistantController::request_set_camera_muted(bool muted) {
  post(EventType::CmdSetCamera, {}, muted);
}

bool AssistantController::set_agent_config(const AppConfig& config) {
  {
    std::lock_guard<std::mutex> lock(config_mutex_);
    config_.provider = config.provider;
    config_.voice = config.voice;
    config_.prompt_id = config.prompt_id;
  }

  LOG_INFO("controller: agent config updated profile=%s voice=%s prompt_id=%s",
           config.provider.empty() ? "default" : config.provider.c_str(),
           config.voice.empty() ? "default" : config.voice.c_str(),
           config.prompt_id.empty() ? "<none>" : config.prompt_id.c_str());

  const AssistantState state = current_state();
  if (state == AssistantState::CreatingRoom ||
      state == AssistantState::Connecting ||
      state == AssistantState::WaitingAgent ||
      state == AssistantState::InCall) {
    post(EventType::CmdRestart);
    return true;
  }
  return false;
}

AppConfig AssistantController::config_snapshot() const {
  std::lock_guard<std::mutex> lock(config_mutex_);
  return config_;
}

UiSnapshot AssistantController::snapshot() const {
  UiSnapshot s;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    s.state = state_;
    s.status = status_text_;
    s.detail = detail_text_;
  }
  s.mic_muted = mic_muted_.load();
  s.cam_muted = cam_muted_.load();
  s.agent_speaking = agent_speaking_.load();
  s.camera_available = camera_.has_camera();
  return s;
}

// --- Worker thread -------------------------------------------------------

void AssistantController::post(EventType type, std::string text, bool flag,
                               std::uint64_t generation) {
  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    queue_.push_back(Event{type, std::move(text), flag, generation});
  }
  queue_cv_.notify_one();
}

void AssistantController::worker_loop() {
  for (;;) {
    Event ev;
    {
      std::unique_lock<std::mutex> lock(queue_mutex_);
      auto has_event = [this] { return !queue_.empty(); };

      if (current_state() == AssistantState::WaitingAgent) {
        if (!queue_cv_.wait_until(lock, agent_deadline_, has_event)) {
          // Timed out with no event — the agent never showed up.
          lock.unlock();
          if (current_state() == AssistantState::WaitingAgent) {
            teardown(AssistantState::Error, kErrAgentNoResponse,
                     kHintTryAgain);
          }
          continue;
        }
      } else {
        queue_cv_.wait(lock, has_event);
      }
      ev = std::move(queue_.front());
      queue_.pop_front();
    }

    if (ev.type == EventType::CmdQuit) {
      if (current_state() != AssistantState::Idle) {
        teardown(AssistantState::Idle, kStatusReady,
                 kHintWaiting);
      }
      break;
    }
    handle(ev);
  }
}

void AssistantController::handle(const Event& ev) {
  const AssistantState state = current_state();
  if ((ev.type == EventType::EvAgentOnline ||
       ev.type == EventType::EvAgentOffline ||
       ev.type == EventType::EvDisconnected) &&
      ev.generation != session_generation_) {
    LOG_DEBUG("controller: ignoring stale session event type=%d generation=%llu",
              static_cast<int>(ev.type),
              static_cast<unsigned long long>(ev.generation));
    return;
  }

  switch (ev.type) {
    case EventType::CmdStart:
      if (state == AssistantState::Idle || state == AssistantState::Error) {
        do_start();
      }
      break;

    case EventType::CmdStop:
      if (state != AssistantState::Idle && state != AssistantState::Stopping) {
        do_stop();
      }
      break;

    case EventType::CmdToggleMute: {
      // Only the worker thread mutates mic_muted_, so a plain flip is safe.
      const bool new_muted = !mic_muted_.load();
      mic_muted_.store(new_muted);
      if (session_) session_->set_mic_muted(new_muted);
      LOG_INFO("controller: microphone %s", new_muted ? "muted" : "live");
      break;
    }

    case EventType::CmdToggleCamera: {
      const bool new_muted = !cam_muted_.load();
      cam_muted_.store(new_muted);
      if (session_) session_->set_camera_muted(new_muted);
      LOG_INFO("controller: camera %s", new_muted ? "off" : "on");
      break;
    }

    case EventType::CmdSetMute:
      mic_muted_.store(ev.flag);
      if (session_) session_->set_mic_muted(ev.flag);
      LOG_INFO("controller: microphone %s", ev.flag ? "muted" : "live");
      break;

    case EventType::CmdSetCamera:
      cam_muted_.store(ev.flag);
      if (session_) session_->set_camera_muted(ev.flag);
      LOG_INFO("controller: camera %s", ev.flag ? "off" : "on");
      break;

    case EventType::CmdRestart:
      if (state == AssistantState::CreatingRoom ||
          state == AssistantState::Connecting ||
          state == AssistantState::WaitingAgent ||
          state == AssistantState::InCall) {
        LOG_INFO("controller: restarting conversation for updated agent config");
        teardown(AssistantState::Idle, kStatusReady, kHintWaiting);
        do_start();
      }
      break;

    case EventType::EvAgentOnline:
      if (state == AssistantState::WaitingAgent) {
        set_state(AssistantState::InCall, kStatusListening);
      }
      break;

    case EventType::EvAgentOffline:
      if (state == AssistantState::InCall) {
        teardown(AssistantState::Idle, kStatusCallEnded,
                 kHintAgentLeft);
      }
      break;

    case EventType::EvDisconnected:
      if (state == AssistantState::Connecting ||
          state == AssistantState::WaitingAgent ||
          state == AssistantState::InCall) {
        teardown(AssistantState::Error, kErrDisconnected,
                 ev.text.empty() ? std::string(kHintConnectionLost)
                                  : ev.text);
      }
      break;

    case EventType::CmdQuit:
      break;  // handled in worker_loop
  }
}

// --- Closed-loop steps ---------------------------------------------------

void AssistantController::do_start() {
  // 1. Set up the 1v1 AI session in one call: the device API get-or-creates the
  //    room, returns LiveKit credentials, and dispatches the AI agent.
  set_state(AssistantState::CreatingRoom, kStatusCreatingRoom, {});
  RoomCredentials creds;
  LOG_INFO("controller: calling device-api start_chat_bot ...");
  const AppConfig cfg = config_snapshot();
  ApiOutcome o = api_.start_chat_bot(cfg.device_id, cfg.provider, cfg.voice,
                                     cfg.prompt_id, creds);
  last_start_http_status_.store(o.http_status);
  LOG_INFO("controller: start_chat_bot -> ok=%d http=%d %s", o.ok, o.http_status,
           o.error.c_str());
  if (!o.ok) {
    teardown(AssistantState::Error, start_error_label(o.http_status), o.error);
    return;
  }
  room_id_ = creds.room_id;
  LOG_INFO("controller: room %s, livekit url=%s", creds.room_id.c_str(),
           creds.livekit_url.c_str());

  // 2. Join the LiveKit room.
  set_state(AssistantState::Connecting, kStatusConnecting);
  session_ = std::make_unique<LiveKitSession>();
  const std::uint64_t generation = ++session_generation_;

  LiveKitSession::Callbacks cb;
  cb.on_disconnected = [this, generation](const std::string& reason) {
    post(EventType::EvDisconnected, reason, false, generation);
  };
  cb.on_agent_online = [this, generation](const std::string&) {
    post(EventType::EvAgentOnline, {}, false, generation);
  };
  cb.on_agent_offline = [this, generation] {
    post(EventType::EvAgentOffline, {}, false, generation);
  };
  cb.on_agent_audio = [this](const std::int16_t* s, int spc, int sr, int ch) {
    audio_.play_agent_audio(s, spc, sr, ch);
  };
  cb.on_agent_speaking = [this](bool speaking) {
    agent_speaking_.store(speaking);
  };
  session_->set_callbacks(std::move(cb));

  if (!session_->connect(creds.livekit_url, creds.livekit_token)) {
    teardown(AssistantState::Error, kErrConnect,
             kHintMediaServer);
    return;
  }

  // 3. Publish the local microphone (and camera).
  if (!session_->publish_audio(audio_.audio_source())) {
    teardown(AssistantState::Error, kErrPublishAudio, {});
    return;
  }
  audio_.start_mic();
  mic_muted_.store(false);
  cam_muted_.store(false);

  if (cfg.publish_video) {
    if (session_->publish_video(camera_.video_source())) {
      camera_.set_publishing(true);
    } else {
      LOG_WARN("controller: continuing without a camera track");
    }
  }

  // 4. The AI agent was already dispatched by start_chat_bot (step 1); it joins
  //    the room and waits for this device participant. Wait for it to come
  //    online, up to the deadline the worker loop enforces.
  set_state(AssistantState::WaitingAgent, kStatusWakingAgent);
  agent_deadline_ = std::chrono::steady_clock::now() + kAgentJoinTimeout;
  LOG_INFO("controller: waiting for the AI agent to join");
}

void AssistantController::do_stop() {
  teardown(AssistantState::Idle, kStatusReady, kHintWaiting);
}

void AssistantController::teardown(AssistantState final_state,
                                   const std::string& status,
                                   const std::string& detail) {
  set_state(AssistantState::Stopping, kStatusEndingCall, {});

  if (!room_id_.empty()) {
    const AppConfig cfg = config_snapshot();
    api_.stop_chat_bot(cfg.device_id);  // best effort: remove agent + end room
  }
  audio_.stop_mic();
  camera_.set_publishing(false);
  if (session_) {
    session_->disconnect();
    session_.reset();
  }
  audio_.stop_speaker();

  room_id_.clear();
  agent_speaking_.store(false);
  mic_muted_.store(false);
  cam_muted_.store(false);

  set_state(final_state, status, detail);
}

// --- State ---------------------------------------------------------------

void AssistantController::set_state(AssistantState state,
                                    const std::string& status,
                                    const std::string& detail) {
  LOG_INFO("controller: state[%d] %s%s%s", static_cast<int>(state),
           status.c_str(), detail.empty() ? "" : " | ", detail.c_str());
  std::lock_guard<std::mutex> lock(state_mutex_);
  state_ = state;
  status_text_ = status;
  detail_text_ = detail;
}

AssistantState AssistantController::current_state() const {
  std::lock_guard<std::mutex> lock(state_mutex_);
  return state_;
}

}  // namespace jusiai

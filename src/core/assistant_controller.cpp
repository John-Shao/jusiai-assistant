#include "core/assistant_controller.h"

#include "i18n.h"
#include "log.h"

namespace jusiai {
namespace {

constexpr auto kAgentJoinTimeout = std::chrono::seconds(45);

}  // namespace

AssistantController::AssistantController(const AppConfig& config)
    : config_(config),
      api_(config.base_url, config.device_api_key, config.tls_verify),
      audio_(config.audio_sample_rate, config.audio_channels,
             config.audio_mic_gain, config.audio_aec,
             config.audio_aec_delay_ms),
      camera_(config.camera_width, config.camera_height, config.camera_fps,
              config.camera_rotation, config.camera_device, &preview_) {}

AssistantController::~AssistantController() { shutdown(); }

bool AssistantController::init() {
  if (worker_running_) return true;

  if (!camera_.start()) {
    LOG_WARN("controller: camera engine failed to start; preview disabled");
  }

  worker_running_ = true;
  worker_ = std::thread(&AssistantController::worker_loop, this);
  set_state(AssistantState::Idle, tr(Msg::StatusReady), tr(Msg::HintTapToBegin));
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

void AssistantController::post(EventType type, std::string text, bool flag) {
  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    queue_.push_back(Event{type, std::move(text), flag});
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
            teardown(AssistantState::Error, tr(Msg::ErrAgentNoResponse),
                     tr(Msg::HintTryAgain));
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
        teardown(AssistantState::Idle, tr(Msg::StatusReady),
                 tr(Msg::HintTapToBegin));
      }
      break;
    }
    handle(ev);
  }
}

void AssistantController::handle(const Event& ev) {
  const AssistantState state = current_state();

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

    case EventType::EvAgentOnline:
      if (state == AssistantState::WaitingAgent) {
        set_state(AssistantState::InCall, tr(Msg::StatusListening));
      }
      break;

    case EventType::EvAgentOffline:
      if (state == AssistantState::InCall) {
        teardown(AssistantState::Idle, tr(Msg::StatusCallEnded),
                 tr(Msg::HintAgentLeft));
      }
      break;

    case EventType::EvDisconnected:
      if (state == AssistantState::Connecting ||
          state == AssistantState::WaitingAgent ||
          state == AssistantState::InCall) {
        teardown(AssistantState::Error, tr(Msg::ErrDisconnected),
                 ev.text.empty() ? std::string(tr(Msg::HintConnectionLost))
                                  : ev.text);
      }
      break;

    case EventType::CmdQuit:
      break;  // handled in worker_loop
  }
}

// --- Closed-loop steps ---------------------------------------------------

void AssistantController::do_start() {
  // 1. Create an anonymous 1v1 AI room via the device API.
  set_state(AssistantState::CreatingRoom, tr(Msg::StatusCreatingRoom), {});
  RoomCredentials creds;
  LOG_INFO("controller: calling device-api create_room ...");
  ApiOutcome o = api_.create_room(config_.device_id, config_.room_name, creds);
  LOG_INFO("controller: create_room -> ok=%d http=%d %s", o.ok, o.http_status,
           o.error.c_str());
  if (!o.ok) {
    teardown(AssistantState::Error, tr(Msg::ErrCreateRoom), o.error);
    return;
  }
  room_id_ = creds.room_id;
  LOG_INFO("controller: room %s, livekit url=%s", creds.room_id.c_str(),
           creds.livekit_url.c_str());

  // 2. Join the LiveKit room.
  set_state(AssistantState::Connecting, tr(Msg::StatusConnecting));
  session_ = std::make_unique<LiveKitSession>();

  LiveKitSession::Callbacks cb;
  cb.on_disconnected = [this](const std::string& reason) {
    post(EventType::EvDisconnected, reason);
  };
  cb.on_agent_online = [this](const std::string&) {
    post(EventType::EvAgentOnline);
  };
  cb.on_agent_offline = [this] { post(EventType::EvAgentOffline); };
  cb.on_agent_audio = [this](const std::int16_t* s, int spc, int sr, int ch) {
    audio_.play_agent_audio(s, spc, sr, ch);
  };
  cb.on_agent_speaking = [this](bool speaking) {
    agent_speaking_.store(speaking);
  };
  session_->set_callbacks(std::move(cb));

  if (!session_->connect(creds.livekit_url, creds.livekit_token)) {
    teardown(AssistantState::Error, tr(Msg::ErrConnect),
             tr(Msg::HintMediaServer));
    return;
  }

  // 3. Publish the local microphone (and camera).
  if (!session_->publish_audio(audio_.audio_source())) {
    teardown(AssistantState::Error, tr(Msg::ErrPublishAudio), {});
    return;
  }
  audio_.start_mic();
  mic_muted_.store(false);
  cam_muted_.store(false);

  if (config_.publish_video) {
    if (session_->publish_video(camera_.video_source())) {
      camera_.set_publishing(true);
    } else {
      LOG_WARN("controller: continuing without a camera track");
    }
  }

  // 4. Dispatch the AI agent into the room.
  set_state(AssistantState::WaitingAgent, tr(Msg::StatusWakingAgent));
  ApiOutcome a = api_.start_ai_agent(room_id_, config_.device_id,
                                     config_.provider, config_.voice,
                                     config_.prompt_label);
  if (!a.ok) {
    teardown(AssistantState::Error, tr(Msg::ErrStartAgent), a.error);
    return;
  }

  // The worker loop now waits for EvAgentOnline up to this deadline.
  agent_deadline_ = std::chrono::steady_clock::now() + kAgentJoinTimeout;
  LOG_INFO("controller: waiting for the AI agent to join");
}

void AssistantController::do_stop() {
  teardown(AssistantState::Idle, tr(Msg::StatusReady), tr(Msg::HintTapToBegin));
}

void AssistantController::teardown(AssistantState final_state,
                                   const std::string& status,
                                   const std::string& detail) {
  set_state(AssistantState::Stopping, tr(Msg::StatusEndingCall), {});

  if (!room_id_.empty()) {
    api_.stop_ai_agent(room_id_);  // best effort
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

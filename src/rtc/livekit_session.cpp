#include "rtc/livekit_session.h"

#include <exception>

#include "log.h"

namespace jusiai {
namespace {

constexpr const char kAudioTrackName[] = "jusi-mic";
constexpr const char kVideoTrackName[] = "jusi-cam";

}  // namespace

LiveKitSession::LiveKitSession() : room_(std::make_unique<livekit::Room>()) {
  room_->setDelegate(this);
}

LiveKitSession::~LiveKitSession() { disconnect(); }

void LiveKitSession::set_callbacks(Callbacks cb) { cb_ = std::move(cb); }

bool LiveKitSession::is_agent_identity(const std::string& identity) {
  return identity.rfind("ai-agent", 0) == 0;
}

bool LiveKitSession::is_agent_online() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return !agent_identity_.empty();
}

bool LiveKitSession::connect(const std::string& url, const std::string& token) {
  livekit::RoomOptions options;
  options.auto_subscribe = true;   // required to receive the agent's audio
  options.dynacast = false;

  LOG_INFO("livekit: connecting to %s", url.c_str());
  bool ok = false;
  try {
    ok = room_->Connect(url, token, options);
  } catch (const std::exception& e) {
    LOG_ERROR("livekit: Connect threw: %s", e.what());
    return false;
  }
  if (!ok) {
    LOG_ERROR("livekit: connection failed");
    return false;
  }

  connected_.store(true);
  LOG_INFO("livekit: connected to room '%s'", room_->room_info().name.c_str());

  // Defensive: the agent normally joins after us, but handle the rare case
  // where it is already present.
  for (const auto& p : room_->remoteParticipants()) {
    if (p && is_agent_identity(p->identity())) {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        agent_identity_ = p->identity();
      }
      if (cb_.on_agent_online) cb_.on_agent_online(p->identity());
    }
  }

  if (cb_.on_connected) cb_.on_connected();
  return true;
}

bool LiveKitSession::publish_audio(
    const std::shared_ptr<livekit::AudioSource>& source) {
  if (!source || !connected_.load()) return false;
  try {
    audio_source_ = source;
    audio_track_ =
        livekit::LocalAudioTrack::createLocalAudioTrack(kAudioTrackName, source);

    livekit::TrackPublishOptions opts;
    opts.source = livekit::TrackSource::SOURCE_MICROPHONE;
    opts.dtx = false;        // keep a continuous stream for the agent's VAD
    opts.simulcast = false;
    room_->localParticipant()->publishTrack(audio_track_, opts);
    LOG_INFO("livekit: microphone track published");
    return true;
  } catch (const std::exception& e) {
    LOG_ERROR("livekit: publish_audio failed: %s", e.what());
    audio_track_.reset();
    return false;
  }
}

bool LiveKitSession::publish_video(
    const std::shared_ptr<livekit::VideoSource>& source) {
  if (!source || !connected_.load()) return false;
  try {
    video_source_ = source;
    video_track_ =
        livekit::LocalVideoTrack::createLocalVideoTrack(kVideoTrackName, source);

    livekit::TrackPublishOptions opts;
    opts.source = livekit::TrackSource::SOURCE_CAMERA;
    opts.simulcast = false;
    room_->localParticipant()->publishTrack(video_track_, opts);
    LOG_INFO("livekit: camera track published (%dx%d)", source->width(),
             source->height());
    return true;
  } catch (const std::exception& e) {
    LOG_ERROR("livekit: publish_video failed: %s", e.what());
    video_track_.reset();
    return false;
  }
}

void LiveKitSession::set_mic_muted(bool muted) {
  if (!audio_track_) return;
  if (muted) {
    audio_track_->mute();
  } else {
    audio_track_->unmute();
  }
  LOG_INFO("livekit: microphone %s", muted ? "muted" : "unmuted");
}

void LiveKitSession::set_camera_muted(bool muted) {
  if (!video_track_) return;
  if (muted) {
    video_track_->mute();
  } else {
    video_track_->unmute();
  }
  LOG_INFO("livekit: camera %s", muted ? "muted" : "unmuted");
}

void LiveKitSession::disconnect() {
  stop_all_audio_readers();

  const bool was_connected = connected_.exchange(false);
  if (room_) {
    if (was_connected) {
      try {
        if (auto* lp = room_->localParticipant()) {
          if (audio_track_ && audio_track_->publication()) {
            lp->unpublishTrack(audio_track_->publication()->sid());
          }
          if (video_track_ && video_track_->publication()) {
            lp->unpublishTrack(video_track_->publication()->sid());
          }
        }
      } catch (const std::exception& e) {
        LOG_WARN("livekit: unpublish during disconnect failed: %s", e.what());
      }
    }
    // The SDK exposes no Room::Disconnect(): detach the delegate and destroy
    // the Room — that drops the FFI handle and leaves the LiveKit session.
    room_->setDelegate(nullptr);
    room_.reset();
    if (was_connected) LOG_INFO("livekit: disconnected");
  }

  audio_track_.reset();
  video_track_.reset();
  audio_source_.reset();
  video_source_.reset();
  {
    std::lock_guard<std::mutex> lock(mutex_);
    agent_identity_.clear();
  }
}

// --- Agent audio reader --------------------------------------------------

void LiveKitSession::start_audio_reader(
    const std::string& sid, const std::shared_ptr<livekit::Track>& track) {
  livekit::AudioStream::Options opts;
  opts.capacity = 50;  // ring buffer: drop oldest if playback stalls

  auto stream = livekit::AudioStream::fromTrack(track, opts);
  if (!stream) {
    LOG_WARN("livekit: failed to open agent audio stream for %s", sid.c_str());
    return;
  }

  AudioReader reader;
  reader.stream = stream;
  reader.thread = std::thread([this, stream]() {
    livekit::AudioFrameEvent ev;
    while (stream->read(ev)) {
      const livekit::AudioFrame& f = ev.frame;
      if (cb_.on_agent_audio && !f.data().empty()) {
        cb_.on_agent_audio(f.data().data(), f.samples_per_channel(),
                           f.sample_rate(), f.num_channels());
      }
    }
  });

  std::lock_guard<std::mutex> lock(mutex_);
  audio_readers_[sid] = std::move(reader);
  LOG_INFO("livekit: agent audio reader started (%s)", sid.c_str());
}

void LiveKitSession::stop_audio_reader(const std::string& sid) {
  AudioReader reader;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = audio_readers_.find(sid);
    if (it == audio_readers_.end()) return;
    reader = std::move(it->second);
    audio_readers_.erase(it);
  }
  if (reader.stream) reader.stream->close();
  if (reader.thread.joinable()) reader.thread.join();
  LOG_INFO("livekit: agent audio reader stopped (%s)", sid.c_str());
}

void LiveKitSession::stop_all_audio_readers() {
  std::map<std::string, AudioReader> readers;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    readers = std::move(audio_readers_);
    audio_readers_.clear();
  }
  for (auto& [sid, reader] : readers) {
    if (reader.stream) reader.stream->close();
    if (reader.thread.joinable()) reader.thread.join();
  }
}

// --- RoomDelegate --------------------------------------------------------

void LiveKitSession::onParticipantConnected(
    livekit::Room&, const livekit::ParticipantConnectedEvent& ev) {
  if (!ev.participant) return;
  const std::string id = ev.participant->identity();
  LOG_INFO("livekit: participant joined: %s", id.c_str());
  if (is_agent_identity(id)) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      agent_identity_ = id;
    }
    if (cb_.on_agent_online) cb_.on_agent_online(id);
  }
}

void LiveKitSession::onParticipantDisconnected(
    livekit::Room&, const livekit::ParticipantDisconnectedEvent& ev) {
  if (!ev.participant) return;
  const std::string id = ev.participant->identity();
  LOG_INFO("livekit: participant left: %s", id.c_str());

  bool was_agent = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (id == agent_identity_) {
      agent_identity_.clear();
      was_agent = true;
    }
  }
  if (was_agent && cb_.on_agent_offline) cb_.on_agent_offline();
}

void LiveKitSession::onTrackSubscribed(
    livekit::Room&, const livekit::TrackSubscribedEvent& ev) {
  if (!ev.track || !ev.publication) return;
  if (ev.track->kind() != livekit::TrackKind::KIND_AUDIO) return;

  const std::string sid = ev.publication->sid();
  const char* who = ev.participant ? ev.participant->identity().c_str() : "?";
  LOG_INFO("livekit: subscribed to audio track %s from %s", sid.c_str(), who);
  start_audio_reader(sid, ev.track);
}

void LiveKitSession::onTrackUnsubscribed(
    livekit::Room&, const livekit::TrackUnsubscribedEvent& ev) {
  if (!ev.publication) return;
  stop_audio_reader(ev.publication->sid());
}

void LiveKitSession::onActiveSpeakersChanged(
    livekit::Room&, const livekit::ActiveSpeakersChangedEvent& ev) {
  std::string agent;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    agent = agent_identity_;
  }
  if (agent.empty()) return;

  bool agent_speaking = false;
  for (const auto* p : ev.speakers) {
    if (p && p->identity() == agent) {
      agent_speaking = true;
      break;
    }
  }
  if (cb_.on_agent_speaking) cb_.on_agent_speaking(agent_speaking);
}

void LiveKitSession::onConnectionStateChanged(
    livekit::Room&, const livekit::ConnectionStateChangedEvent& ev) {
  LOG_DEBUG("livekit: connection state -> %d", static_cast<int>(ev.state));
}

void LiveKitSession::onDisconnected(livekit::Room&,
                                    const livekit::DisconnectedEvent& ev) {
  connected_.store(false);
  LOG_INFO("livekit: room disconnected (reason %d)",
           static_cast<int>(ev.reason));
  if (cb_.on_disconnected) {
    cb_.on_disconnected("Connection closed (reason " +
                        std::to_string(static_cast<int>(ev.reason)) + ")");
  }
}

void LiveKitSession::onReconnecting(livekit::Room&,
                                    const livekit::ReconnectingEvent&) {
  LOG_WARN("livekit: reconnecting...");
  if (cb_.on_reconnecting) cb_.on_reconnecting();
}

void LiveKitSession::onReconnected(livekit::Room&,
                                   const livekit::ReconnectedEvent&) {
  LOG_INFO("livekit: reconnected");
  if (cb_.on_reconnected) cb_.on_reconnected();
}

}  // namespace jusiai

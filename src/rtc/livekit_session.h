// Thin wrapper over livekit::Room that drives one AI-assistant conversation:
// connect, publish the local microphone + camera, subscribe to the AI agent's
// audio, and surface lifecycle events as plain callbacks.
//
// All on_* callbacks may run on internal LiveKit threads; handlers must be
// thread-safe and must not block.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "livekit/livekit.h"

namespace jusiai {

class LiveKitSession : private livekit::RoomDelegate {
 public:
  struct Callbacks {
    std::function<void()> on_connected;
    std::function<void(const std::string& reason)> on_disconnected;
    std::function<void()> on_reconnecting;
    std::function<void()> on_reconnected;
    // The AI agent (identity prefixed "ai-agent") joined / left the room.
    std::function<void(const std::string& identity)> on_agent_online;
    std::function<void()> on_agent_offline;
    // Decoded interleaved int16 PCM from the agent's audio track.
    std::function<void(const std::int16_t* samples, int samples_per_channel,
                       int sample_rate, int channels)>
        on_agent_audio;
    // The agent started / stopped being an active speaker.
    std::function<void(bool speaking)> on_agent_speaking;
  };

  LiveKitSession();
  ~LiveKitSession() override;

  LiveKitSession(const LiveKitSession&) = delete;
  LiveKitSession& operator=(const LiveKitSession&) = delete;

  void set_callbacks(Callbacks cb);

  // Connect to the LiveKit room. Blocks until the connection resolves.
  bool connect(const std::string& url, const std::string& token);

  // Publish the local microphone / camera. The media engines own the sources
  // and push frames into them directly; the session owns the resulting tracks.
  bool publish_audio(const std::shared_ptr<livekit::AudioSource>& source);
  bool publish_video(const std::shared_ptr<livekit::VideoSource>& source);

  // Stop transmitting microphone audio without unpublishing the track.
  void set_mic_muted(bool muted);

  // Stop transmitting camera video without unpublishing the track.
  void set_camera_muted(bool muted);

  // Leave the room and release every track / reader thread.
  void disconnect();

  bool is_connected() const { return connected_.load(); }
  bool is_agent_online() const;

 private:
  // livekit::RoomDelegate overrides.
  void onParticipantConnected(livekit::Room&,
                              const livekit::ParticipantConnectedEvent&) override;
  void onParticipantDisconnected(
      livekit::Room&, const livekit::ParticipantDisconnectedEvent&) override;
  void onTrackSubscribed(livekit::Room&,
                         const livekit::TrackSubscribedEvent&) override;
  void onTrackUnsubscribed(livekit::Room&,
                           const livekit::TrackUnsubscribedEvent&) override;
  void onActiveSpeakersChanged(
      livekit::Room&, const livekit::ActiveSpeakersChangedEvent&) override;
  void onConnectionStateChanged(
      livekit::Room&, const livekit::ConnectionStateChangedEvent&) override;
  void onDisconnected(livekit::Room&,
                      const livekit::DisconnectedEvent&) override;
  void onReconnecting(livekit::Room&,
                      const livekit::ReconnectingEvent&) override;
  void onReconnected(livekit::Room&,
                     const livekit::ReconnectedEvent&) override;

  // Background reader that pumps an agent audio track into on_agent_audio.
  struct AudioReader {
    std::shared_ptr<livekit::AudioStream> stream;
    std::thread thread;
  };

  void start_audio_reader(const std::string& sid,
                          const std::shared_ptr<livekit::Track>& track);
  void stop_audio_reader(const std::string& sid);
  void stop_all_audio_readers();
  static bool is_agent_identity(const std::string& identity);

  Callbacks cb_;

  std::unique_ptr<livekit::Room> room_;
  std::shared_ptr<livekit::AudioSource> audio_source_;
  std::shared_ptr<livekit::VideoSource> video_source_;
  std::shared_ptr<livekit::LocalAudioTrack> audio_track_;
  std::shared_ptr<livekit::LocalVideoTrack> video_track_;

  std::atomic<bool> connected_{false};

  mutable std::mutex mutex_;
  std::string agent_identity_;
  std::map<std::string, AudioReader> audio_readers_;  // track sid -> reader
};

}  // namespace jusiai

// Audio engine: captures the microphone into a livekit::AudioSource and plays
// the AI agent's decoded PCM back through the speaker.
//
// Microphone capture runs on its own thread. play_agent_audio() is called from
// a LiveKit reader thread; the speaker is opened lazily on the first frame so
// its sample rate matches whatever the agent produces.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>

#include "livekit/audio_source.h"

class SDLMicSource;
class DDLSpeakerSink;

namespace jusiai {

class AudioEngine {
 public:
  AudioEngine(int sample_rate, int channels);
  ~AudioEngine();

  AudioEngine(const AudioEngine&) = delete;
  AudioEngine& operator=(const AudioEngine&) = delete;

  // The microphone source, ready to be wrapped in a LiveKit track. Valid for
  // the lifetime of the engine.
  std::shared_ptr<livekit::AudioSource> audio_source() const {
    return audio_source_;
  }

  // Begin / end microphone capture.
  bool start_mic();
  void stop_mic();

  // Feed one decoded PCM frame from the agent to the speaker. Thread-safe.
  void play_agent_audio(const std::int16_t* samples, int samples_per_channel,
                        int sample_rate, int channels);
  void stop_speaker();

 private:
  void mic_loop();
  void on_mic_frame(const std::int16_t* samples, int samples_per_channel,
                    int sample_rate, int channels);

  const int sample_rate_;
  const int channels_;

  std::shared_ptr<livekit::AudioSource> audio_source_;

  std::unique_ptr<SDLMicSource> mic_;
  std::thread mic_thread_;
  std::atomic<bool> mic_running_{false};

  std::mutex speaker_mutex_;
  std::unique_ptr<DDLSpeakerSink> speaker_;
  int speaker_rate_ = 0;
  int speaker_channels_ = 0;
};

}  // namespace jusiai

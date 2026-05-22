// Audio engine (RV1126B): captures the microphone into a livekit::AudioSource
// and plays the AI agent's decoded PCM back through the speaker — both via
// ALSA (ES8389 codec, card 0).
//
// Microphone capture runs on its own thread. play_agent_audio() is called from
// a LiveKit reader thread and only enqueues; a writer thread drains into ALSA
// so the SDK callback is never blocked by the codec's hardware pacing.
//
// Acoustic echo cancellation: the speaker and microphone are the one ES8389
// codec, so the mic captures the agent's own playback. A WebRTC APM removes it
// — the playback writer thread feeds it the reference signal (what is being
// rendered) and the capture thread feeds it the near-end mic signal.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>

#include "livekit/audio_processing_module.h"
#include "livekit/audio_source.h"

namespace jusiai {

class AlsaPlayback;  // defined in audio_io.cpp
class AlsaCapture;   // defined in audio_io.cpp

class AudioEngine {
 public:
  // `aec` enables WebRTC echo cancellation; `aec_delay_ms` is the initial
  // speaker->mic delay hint given to the canceller.
  AudioEngine(int sample_rate, int channels, float mic_gain, bool aec,
              int aec_delay_ms);
  ~AudioEngine();

  AudioEngine(const AudioEngine&) = delete;
  AudioEngine& operator=(const AudioEngine&) = delete;

  // The microphone source, ready to be wrapped in a LiveKit track.
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

  const int sample_rate_;
  const int channels_;
  const float mic_gain_;

  std::shared_ptr<livekit::AudioSource> audio_source_;

  // Echo canceller, shared between the capture thread (processStream) and the
  // playback writer thread (processReverseStream). Null when AEC is disabled
  // or the APM could not be created.
  std::shared_ptr<livekit::AudioProcessingModule> apm_;

  std::unique_ptr<AlsaCapture> capture_;
  std::thread mic_thread_;
  std::atomic<bool> mic_running_{false};

  std::unique_ptr<AlsaPlayback> playback_;
};

}  // namespace jusiai

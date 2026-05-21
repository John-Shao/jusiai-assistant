#include "media/audio_io.h"

#include <chrono>
#include <exception>
#include <vector>

#include "livekit/audio_frame.h"
#include "log.h"
#include "sdl_media.h"

namespace jusiai {

AudioEngine::AudioEngine(int sample_rate, int channels)
    : sample_rate_(sample_rate), channels_(channels) {
  // queue_size_ms = 0 -> real-time capture mode: frames are forwarded
  // synchronously, which is what a hardware-paced microphone wants.
  audio_source_ =
      std::make_shared<livekit::AudioSource>(sample_rate_, channels_, 0);
}

AudioEngine::~AudioEngine() {
  stop_mic();
  stop_speaker();
}

bool AudioEngine::start_mic() {
  if (mic_running_.load()) return true;

  const int frame_samples = sample_rate_ / 100;  // 10 ms frames
  mic_ = std::make_unique<SDLMicSource>(
      sample_rate_, channels_, frame_samples,
      [this](const std::int16_t* s, int spc, int rate, int ch) {
        on_mic_frame(s, spc, rate, ch);
      });

  if (!mic_->init()) {
    LOG_ERROR("audio: failed to open microphone");
    mic_.reset();
    return false;
  }

  mic_running_.store(true);
  mic_thread_ = std::thread(&AudioEngine::mic_loop, this);
  LOG_INFO("audio: microphone capture started (%d Hz, %d ch)", sample_rate_,
           channels_);
  return true;
}

void AudioEngine::stop_mic() {
  if (mic_running_.exchange(false)) {
    if (mic_thread_.joinable()) mic_thread_.join();
  }
  mic_.reset();
}

void AudioEngine::mic_loop() {
  while (mic_running_.load()) {
    mic_->pump();
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
}

void AudioEngine::on_mic_frame(const std::int16_t* samples,
                               int samples_per_channel, int sample_rate,
                               int channels) {
  if (!samples || samples_per_channel <= 0) return;

  const std::size_t total =
      static_cast<std::size_t>(samples_per_channel) * channels;
  std::vector<std::int16_t> data(samples, samples + total);

  try {
    livekit::AudioFrame frame(std::move(data), sample_rate, channels,
                              samples_per_channel);
    audio_source_->captureFrame(frame, 100);
  } catch (const std::exception& e) {
    // A transient capture hiccup must not kill the loop.
    LOG_DEBUG("audio: captureFrame dropped a frame: %s", e.what());
  }
}

void AudioEngine::play_agent_audio(const std::int16_t* samples,
                                   int samples_per_channel, int sample_rate,
                                   int channels) {
  if (!samples || samples_per_channel <= 0) return;

  std::lock_guard<std::mutex> lock(speaker_mutex_);

  // (Re)open the speaker if absent or if the agent's format changed.
  if (!speaker_ || speaker_rate_ != sample_rate ||
      speaker_channels_ != channels) {
    speaker_ = std::make_unique<DDLSpeakerSink>(sample_rate, channels);
    if (!speaker_->init()) {
      LOG_ERROR("audio: failed to open speaker");
      speaker_.reset();
      return;
    }
    speaker_rate_ = sample_rate;
    speaker_channels_ = channels;
    LOG_INFO("audio: speaker opened (%d Hz, %d ch)", sample_rate, channels);
  }

  speaker_->enqueue(samples, samples_per_channel);
}

void AudioEngine::stop_speaker() {
  std::lock_guard<std::mutex> lock(speaker_mutex_);
  speaker_.reset();
  speaker_rate_ = 0;
  speaker_channels_ = 0;
}

}  // namespace jusiai

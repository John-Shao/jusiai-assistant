#include "media/audio_io.h"

#include <alsa/asoundlib.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <exception>
#include <mutex>
#include <thread>
#include <vector>

#include "livekit/audio_frame.h"
#include "log.h"
#include "media/alsa_setup.h"

namespace jusiai {
namespace {

// The board has no PulseAudio; talk to the ES8389 codec directly. plughw adds
// the conversion layer so the agent's arbitrary sample rate still plays.
const char* device_or(const char* env, const char* fallback) {
  const char* v = std::getenv(env);
  return (v && *v) ? v : fallback;
}

// The ES8389 + Rockchip SAI capture path only streams in 2-channel mode — a
// mono open returns -EIO on the first read. We always capture stereo and
// down-mix to the mono frame the SDK / AI agent expects.
constexpr int kCaptureChannels = 2;
// Power-of-two ALSA period. The SAI DMA rejects the odd geometry that
// snd_pcm_set_params() derives from a latency hint (also -EIO); an explicit
// 1024-frame period is what the stock firmware (Rockit) uses and works.
constexpr snd_pcm_uframes_t kCapturePeriod = 1024;
constexpr unsigned int kCapturePeriods = 4;

}  // namespace

// ===========================================================================
// AlsaCapture — blocking microphone capture from the ES8389 codec.
//
// Opens hw:0,0 in stereo with an explicit hw_params geometry (the only
// configuration the SAI capture DMA accepts) and hands the caller mono frames
// down-mixed from the hardware's two channels.
// ===========================================================================
class AlsaCapture {
 public:
  ~AlsaCapture() { close(); }

  bool open(int sample_rate) {
    const char* dev = device_or("ALSA_CAPTURE_DEVICE", "hw:0,0");
    int err = snd_pcm_open(&pcm_, dev, SND_PCM_STREAM_CAPTURE, 0);
    if (err < 0) {
      LOG_ERROR("alsa: capture open(%s) failed: %s", dev, snd_strerror(err));
      pcm_ = nullptr;
      return false;
    }

    snd_pcm_hw_params_t* hp;
    snd_pcm_hw_params_alloca(&hp);
    snd_pcm_hw_params_any(pcm_, hp);

    unsigned int rate = static_cast<unsigned int>(sample_rate);
    snd_pcm_uframes_t period = kCapturePeriod;
    unsigned int periods = kCapturePeriods;
    if ((err = snd_pcm_hw_params_set_access(
             pcm_, hp, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0 ||
        (err = snd_pcm_hw_params_set_format(pcm_, hp,
                                            SND_PCM_FORMAT_S16_LE)) < 0 ||
        (err = snd_pcm_hw_params_set_channels(pcm_, hp, kCaptureChannels)) < 0 ||
        (err = snd_pcm_hw_params_set_rate_near(pcm_, hp, &rate, nullptr)) < 0 ||
        (err = snd_pcm_hw_params_set_period_size_near(pcm_, hp, &period,
                                                      nullptr)) < 0 ||
        (err = snd_pcm_hw_params_set_periods_near(pcm_, hp, &periods,
                                                  nullptr)) < 0 ||
        (err = snd_pcm_hw_params(pcm_, hp)) < 0) {
      LOG_ERROR("alsa: capture hw_params failed: %s", snd_strerror(err));
      close();
      return false;
    }
    snd_pcm_prepare(pcm_);
    LOG_INFO("alsa: microphone capture opened (%s, %u Hz, stereo, period %lu)",
             dev, rate, static_cast<unsigned long>(period));
    return true;
  }

  // Fill `out` with exactly `frames` mono samples (blocking), down-mixed from
  // hardware stereo. Returns `frames` on success, -1 on a fatal error.
  //
  // snd_pcm_readi() may return fewer frames than asked (period boundaries,
  // recovered xruns); the SDK's direct capture rejects any frame that is not
  // exactly 10 ms, so we loop until the whole request is satisfied.
  int read_mono(std::int16_t* out, int frames) {
    if (!pcm_) return -1;
    if (static_cast<int>(stereo_.size()) < frames * kCaptureChannels)
      stereo_.resize(static_cast<std::size_t>(frames) * kCaptureChannels);

    int filled = 0;
    int recoveries = 0;
    while (filled < frames) {
      snd_pcm_sframes_t r = snd_pcm_readi(
          pcm_, stereo_.data() + static_cast<std::size_t>(filled) * 2,
          frames - filled);
      if (r < 0) {
        if (err_logged_ < 6) {
          LOG_WARN("alsa: capture readi error: %s", snd_strerror((int)r));
          ++err_logged_;
        }
        if (snd_pcm_recover(pcm_, static_cast<int>(r), /*silent=*/1) < 0)
          return -1;
        if (++recoveries > 8) return -1;  // caller sleeps and retries
        continue;
      }
      filled += static_cast<int>(r);
    }
    // Down-mix L/R to mono (average — halves any single-channel clipping).
    for (int i = 0; i < frames; ++i) {
      int l = stereo_[2 * i];
      int rr = stereo_[2 * i + 1];
      out[i] = static_cast<std::int16_t>((l + rr) / 2);
    }
    if (!first_ok_) {
      LOG_INFO("alsa: capture producing 10ms frames");
      first_ok_ = true;
    }
    return frames;
  }

  void close() {
    if (pcm_) {
      snd_pcm_close(pcm_);
      pcm_ = nullptr;
    }
  }

 private:
  snd_pcm_t* pcm_ = nullptr;
  std::vector<std::int16_t> stereo_;  // interleaved L/R capture scratch
  int err_logged_ = 0;
  bool first_ok_ = false;
};

// ===========================================================================
// AlsaPlayback — speaker playback over the *same* hw:0,0 codec the microphone
// captures from. Because the ES8389/SAI is a single full-duplex device, the
// playback PCM MUST be opened with hw_params identical to the running capture
// stream (S16_LE / 48 kHz / stereo / 1024-frame period) — a mismatched open
// (e.g. plughw mono) reconfigures the shared SAI and stalls capture dead
// (proven by tools/fdtest.c). The agent's audio is therefore resampled and
// up-mixed to that fixed hardware format in software.
//
// The SDK callback only enqueues; a writer thread drains the ring into
// snd_pcm_writei at the hardware pace.
// ===========================================================================
class AlsaPlayback {
 public:
  // `apm`, when set, is fed the reference signal for echo cancellation.
  explicit AlsaPlayback(std::shared_ptr<livekit::AudioProcessingModule> apm)
      : apm_(std::move(apm)) {}
  ~AlsaPlayback() { stop(); }

  bool ensure_open(int hw_rate) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (pcm_) return true;

    const char* dev = device_or("ALSA_PCM_DEVICE", "hw:0,0");
    int err = snd_pcm_open(&pcm_, dev, SND_PCM_STREAM_PLAYBACK, 0);
    if (err < 0) {
      LOG_ERROR("alsa: playback open(%s) failed: %s", dev, snd_strerror(err));
      pcm_ = nullptr;
      return false;
    }

    snd_pcm_hw_params_t* hp;
    snd_pcm_hw_params_alloca(&hp);
    snd_pcm_hw_params_any(pcm_, hp);

    unsigned int rate = static_cast<unsigned int>(hw_rate);
    snd_pcm_uframes_t period = kCapturePeriod;
    unsigned int periods = kCapturePeriods;
    if ((err = snd_pcm_hw_params_set_access(
             pcm_, hp, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0 ||
        (err = snd_pcm_hw_params_set_format(pcm_, hp,
                                            SND_PCM_FORMAT_S16_LE)) < 0 ||
        (err = snd_pcm_hw_params_set_channels(pcm_, hp, kCaptureChannels)) < 0 ||
        (err = snd_pcm_hw_params_set_rate_near(pcm_, hp, &rate, nullptr)) < 0 ||
        (err = snd_pcm_hw_params_set_period_size_near(pcm_, hp, &period,
                                                      nullptr)) < 0 ||
        (err = snd_pcm_hw_params_set_periods_near(pcm_, hp, &periods,
                                                  nullptr)) < 0 ||
        (err = snd_pcm_hw_params(pcm_, hp)) < 0) {
      LOG_ERROR("alsa: playback hw_params failed: %s", snd_strerror(err));
      snd_pcm_close(pcm_);
      pcm_ = nullptr;
      return false;
    }
    snd_pcm_prepare(pcm_);
    hw_rate_ = static_cast<int>(rate);
    stop_.store(false);
    writer_ = std::thread(&AlsaPlayback::writer_loop, this);
    LOG_INFO("alsa: speaker playback opened (%s, %d Hz, stereo, period %lu)",
             dev, hw_rate_, static_cast<unsigned long>(period));
    return true;
  }

  // Convert one agent audio frame (interleaved, `ch` channels, `sr` Hz) to the
  // fixed hardware format — mono down-mix, linear resample to hw_rate, stereo
  // up-mix — and push the interleaved stereo samples onto the ring.
  void submit(const std::int16_t* samples, int spc, int sr, int ch) {
    if (!samples || spc <= 0 || hw_rate_ <= 0) return;

    // 1) down-mix to mono.
    std::vector<std::int16_t> mono(static_cast<std::size_t>(spc));
    for (int i = 0; i < spc; ++i) {
      if (ch <= 1) {
        mono[i] = samples[i];
      } else {
        int a = samples[i * ch];
        int b = samples[i * ch + 1];
        mono[i] = static_cast<std::int16_t>((a + b) / 2);
      }
    }

    // 2) resample to hw_rate (linear; identity when the agent already
    //    matches — the doubao provider streams 48 kHz, so it usually does).
    const std::int16_t* src = mono.data();
    int n = spc;
    std::vector<std::int16_t> res;
    if (sr > 0 && sr != hw_rate_) {
      long out_n = static_cast<long>(spc) * hw_rate_ / sr;
      res.resize(static_cast<std::size_t>(out_n));
      for (long i = 0; i < out_n; ++i) {
        double pos = static_cast<double>(i) * sr / hw_rate_;
        long idx = static_cast<long>(pos);
        double frac = pos - idx;
        int s0 = mono[idx < spc ? idx : spc - 1];
        int s1 = (idx + 1 < spc) ? mono[idx + 1] : s0;
        res[i] = static_cast<std::int16_t>(s0 + (s1 - s0) * frac);
      }
      src = res.data();
      n = static_cast<int>(out_n);
    }

    // 3) duplicate to stereo and enqueue; bound the ring to ~1 s.
    std::lock_guard<std::mutex> lock(ring_mutex_);
    for (int i = 0; i < n; ++i) {
      ring_.push_back(src[i]);
      ring_.push_back(src[i]);
    }
    const std::size_t cap = static_cast<std::size_t>(hw_rate_) * 2 * 2;
    while (ring_.size() > cap) ring_.pop_front();
  }

  void stop() {
    stop_.store(true);
    if (writer_.joinable()) writer_.join();
    std::lock_guard<std::mutex> lock(mutex_);
    if (pcm_) {
      snd_pcm_close(pcm_);
      pcm_ = nullptr;
    }
  }

 private:
  void writer_loop() {
    const int frames = hw_rate_ / 100;  // 10 ms
    const std::size_t want = static_cast<std::size_t>(frames) * kCaptureChannels;
    // Jitter cushion: hold playback until ~50 ms is queued. Without it the
    // ring drains faster than the network-jittered agent stream refills it,
    // and every momentary empty splices a slice of silence into the middle
    // of a word — the audible "爆破音" crackle.
    const std::size_t prebuffer = want * 5;
    std::vector<std::int16_t> chunk(want);
    bool draining = false;
    while (!stop_.load()) {
      std::size_t got = 0;
      {
        std::lock_guard<std::mutex> lock(ring_mutex_);
        if (!draining && ring_.size() >= prebuffer) draining = true;
        if (draining) {
          got = std::min(want, ring_.size());
          for (std::size_t i = 0; i < got; ++i) {
            chunk[i] = ring_.front();
            ring_.pop_front();
          }
        }
      }
      if (!draining) {
        // Still filling the cushion — don't touch the PCM yet.
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        continue;
      }
      // Pad with silence so the PCM never underruns (avoids pops).
      std::fill(chunk.begin() + got, chunk.end(), 0);

      // Hand the echo canceller the reference signal — exactly this 10 ms
      // frame about to be rendered, down-mixed to mono (the stereo ring holds
      // L==R). Done before writei so the reference leads the captured echo.
      if (apm_) {
        std::vector<std::int16_t> ref(static_cast<std::size_t>(frames));
        for (int i = 0; i < frames; ++i) ref[i] = chunk[2 * i];
        try {
          livekit::AudioFrame rf(std::move(ref), hw_rate_, 1, frames);
          apm_->processReverseStream(rf);
        } catch (const std::exception& e) {
          if (!apm_warned_) {
            LOG_WARN("audio: APM reverse-stream failed: %s", e.what());
            apm_warned_ = true;
          }
        }
      }

      snd_pcm_sframes_t w = snd_pcm_writei(pcm_, chunk.data(), frames);
      if (w < 0) snd_pcm_recover(pcm_, static_cast<int>(w), /*silent=*/1);
    }
  }

  std::mutex mutex_;
  snd_pcm_t* pcm_ = nullptr;
  int hw_rate_ = 0;  // fixed hardware rate (matches the capture stream)

  std::mutex ring_mutex_;
  std::deque<std::int16_t> ring_;  // interleaved stereo

  std::shared_ptr<livekit::AudioProcessingModule> apm_;  // echo-cancel reference
  bool apm_warned_ = false;        // writer thread only — one-shot error log

  std::atomic<bool> stop_{false};
  std::thread writer_;
};

// ===========================================================================
// AudioEngine
// ===========================================================================
AudioEngine::AudioEngine(int sample_rate, int channels, float mic_gain,
                         bool aec, int aec_delay_ms)
    : sample_rate_(sample_rate),
      channels_(channels),
      mic_gain_(mic_gain > 0.0f ? mic_gain : 1.0f) {
  audio_source_ =
      std::make_shared<livekit::AudioSource>(sample_rate_, channels_, 0);

  if (!aec) {
    LOG_INFO("audio: echo cancellation disabled by config");
    return;
  }
  // The speaker and microphone are the one ES8389 codec, so the mic captures
  // the agent's own playback. The WebRTC APM cancels it; the playback writer
  // thread feeds it the reference and the capture thread the near-end signal.
  try {
    livekit::AudioProcessingModule::Options opt;
    opt.echo_cancellation = true;   // AEC3 — stops the agent talking to itself
    opt.noise_suppression = true;   // far-field mic also picks up room noise
    opt.high_pass_filter = true;    // drop DC offset / low-frequency rumble
    opt.auto_gain_control = false;  // ES8389 PGA already sets the level
    apm_ = std::make_shared<livekit::AudioProcessingModule>(opt);
    // Echo processing needs a delay estimate; the fixed full-duplex ALSA
    // geometry keeps it roughly constant, so set the hint once.
    apm_->setStreamDelayMs(aec_delay_ms > 0 ? aec_delay_ms : 0);
    LOG_INFO("audio: echo cancellation enabled (delay hint %d ms)",
             aec_delay_ms);
  } catch (const std::exception& e) {
    LOG_ERROR("audio: could not create the echo canceller: %s — "
              "continuing without AEC",
              e.what());
    apm_.reset();
  }
}

AudioEngine::~AudioEngine() {
  stop_mic();
  stop_speaker();
}

bool AudioEngine::start_mic() {
  if (mic_running_.load()) return true;

  // Configure the ES8389 codec capture path (mic PGA, ADC volume, ALC off).
  // Without this the on-board mic is silent / far too quiet for the agent.
  if (app_alsa_mic_setup() != 0) {
    LOG_WARN("audio: ES8389 mic setup incomplete — capture may be weak");
  }

  capture_ = std::make_unique<AlsaCapture>();
  if (!capture_->open(sample_rate_)) {
    capture_.reset();
    return false;
  }
  mic_running_.store(true);
  mic_thread_ = std::thread(&AudioEngine::mic_loop, this);
  LOG_INFO("audio: microphone capture started (gain %.1fx)",
           static_cast<double>(mic_gain_));
  return true;
}

void AudioEngine::stop_mic() {
  if (mic_running_.exchange(false)) {
    if (mic_thread_.joinable()) mic_thread_.join();
  }
  capture_.reset();
}

void AudioEngine::mic_loop() {
  const int frame = sample_rate_ / 100;  // 10 ms
  std::vector<std::int16_t> buf(static_cast<std::size_t>(frame) * channels_);

  int level_peak_in = 0;   // diagnostics: peak mic amplitude before AEC
  int level_peak_out = 0;  // diagnostics: peak mic amplitude after AEC
  int level_frames = 0;

  while (mic_running_.load()) {
    int got = capture_->read_mono(buf.data(), frame);
    if (got < 0) {
      // Fatal read error: readi returns immediately, so sleep before the
      // retry — otherwise this thread spins a CPU core and starves playback.
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      continue;
    }
    if (got == 0) continue;  // recovered xrun

    const std::size_t total = static_cast<std::size_t>(got) * channels_;
    std::vector<std::int16_t> data(buf.begin(), buf.begin() + total);
    // Software gain — the ES8389 mic level is low; clip on overflow.
    if (mic_gain_ != 1.0f) {
      for (auto& s : data) {
        int v = static_cast<int>(s * mic_gain_);
        s = static_cast<std::int16_t>(v < -32768 ? -32768
                                                 : (v > 32767 ? 32767 : v));
      }
    }
    // Pre-AEC peak — the raw mic level (what would be sent without AEC).
    for (std::int16_t s : data) {
      int a = s < 0 ? -static_cast<int>(s) : s;
      if (a > level_peak_in) level_peak_in = a;
    }

    livekit::AudioFrame frame_obj(std::move(data), sample_rate_, channels_,
                                  got);
    // Echo cancellation — removes the agent's own playback from the mic
    // signal, in-place, before it is sent upstream.
    if (apm_) {
      try {
        apm_->processStream(frame_obj);
      } catch (const std::exception& e) {
        static bool warned = false;
        if (!warned) {
          LOG_WARN("audio: APM stream processing failed: %s", e.what());
          warned = true;
        }
      }
    }

    // Post-AEC peak — what is actually sent upstream. While the agent speaks
    // and no one else does, the gap below the pre-AEC peak is the cancelled
    // echo (in == out when AEC is disabled).
    for (std::int16_t s : frame_obj.data()) {
      int a = s < 0 ? -static_cast<int>(s) : s;
      if (a > level_peak_out) level_peak_out = a;
    }
    if (++level_frames >= 200) {  // ~2 s of 10 ms frames
      LOG_INFO("audio: mic peak in=%d out=%d /32767 (gain %.1fx)",
               level_peak_in, level_peak_out, static_cast<double>(mic_gain_));
      level_peak_in = 0;
      level_peak_out = 0;
      level_frames = 0;
    }

    try {
      audio_source_->captureFrame(frame_obj, 100);
    } catch (const std::exception& e) {
      LOG_DEBUG("audio: captureFrame dropped a frame: %s", e.what());
    }
  }
}

void AudioEngine::play_agent_audio(const std::int16_t* samples,
                                   int samples_per_channel, int sample_rate,
                                   int channels) {
  if (!samples || samples_per_channel <= 0) return;
  if (!playback_) playback_ = std::make_unique<AlsaPlayback>(apm_);
  // Open the speaker PCM at the capture hardware rate so its hw_params match
  // the running microphone stream — mandatory on this full-duplex codec.
  if (!playback_->ensure_open(sample_rate_)) return;
  playback_->submit(samples, samples_per_channel, sample_rate, channels);
}

void AudioEngine::stop_speaker() { playback_.reset(); }

}  // namespace jusiai

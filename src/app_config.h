// Runtime configuration: backend endpoint, AI agent options and media
// parameters. Resolved from (in increasing priority) built-in defaults, a
// config file, JUSIAI_* environment variables and command-line flags.
#pragma once

#include <string>

#include "log.h"

namespace jusiai {

struct AppConfig {
  // --- Backend (JuSi Meet device API) ---
  std::string base_url = "https://meet.jusiai.com";
  std::string device_api_key = "jusi-device-2025";
  // Verify the backend's TLS certificate. Off by default: the RV1126B
  // Buildroot rootfs ships no CA bundle (auth is the pre-shared device key).
  bool tls_verify = false;
  std::string device_id;                       // auto-generated if empty

  // --- AI agent ---
  std::string provider = "doubao";               // doubao | doubao_s2s | qwen
  std::string voice;                            // empty -> provider default
  std::string prompt_id;                        // preferred v2 prompt selector
  std::string prompt_label = "通用 AI 助手";
  std::string prompt_content;                   // optional custom prompt text

  // --- Media ---
  // camera_width/height are the OUTPUT (post-rotation) resolution. The
  // RV1126B sensor is mounted rotated, so frames are rotated camera_rotation
  // degrees clockwise before use.
  int camera_width = 480;
  int camera_height = 640;
  int camera_fps = 30;
  int camera_rotation = 90;                       // 0 | 90 | 180 | 270
  std::string camera_device = "/dev/video-camera0";
  int audio_sample_rate = 48000;
  int audio_channels = 1;
  // Extra software gain on the mic. The ES8389 codec setup (alsa_setup.c)
  // already brings the level up, so 1.0 (none) is the default.
  float audio_mic_gain = 1.0f;
  // Acoustic echo cancellation (WebRTC APM). The speaker and microphone share
  // the one ES8389 codec, so without AEC the agent hears its own voice played
  // back and talks to itself. On by default.
  bool audio_aec = true;
  // Estimated speaker->mic round-trip delay (ms) fed to the echo canceller as
  // a starting hint. Tune on-board if residual echo leaks through.
  int audio_aec_delay_ms = 90;
  bool publish_video = true;

  // --- Runtime ---
  // Begin the AI call immediately on launch, without an explicit `/start`.
  bool autostart = false;

  // Local HTTP control + status API — the only way to drive the device.
  // Bind to 0.0.0.0 for phone/remote direct control; keep 127.0.0.1 when a
  // sibling voice/phone module on the same device is the client.
  std::string control_bind = "127.0.0.1";
  int control_port = 8765;

  LogLevel log_level = LogLevel::Info;

  // Human-readable one-line summary for startup logging (key redacted).
  std::string summary() const;
};

// Parse command line / environment / config file into an AppConfig.
// On --help or a parse error, prints to stderr; `should_exit` is set and
// `exit_code` carries the process exit status the caller should use.
AppConfig load_config(int argc, char** argv, bool& should_exit, int& exit_code);

// Persist the runtime-selectable AI agent settings changed through the local
// control API. These overrides are loaded after the config file and before
// environment variables / command-line flags.
bool save_runtime_agent_config(const AppConfig& cfg, std::string* error);

// Return a stable device id. When `configured` is empty, derive one from
// the SoC eFuse serial (via /proc/cpuinfo "Serial") as `JUSI-<serial>` —
// the on-die value survives rootfs / storage / U-Boot resets. If no such
// serial exists (desktop / x86 dev), fall back to a random id persisted
// under ~/.config/jusiai-assistant/device_id.
std::string resolve_device_id(const std::string& configured);

}  // namespace jusiai

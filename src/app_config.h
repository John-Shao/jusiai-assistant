// Runtime configuration: backend endpoint, AI agent options, media and UI
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
  std::string device_id;                       // auto-generated if empty
  std::string room_name = "Linux AI 助手";

  // --- AI agent ---
  std::string provider = "doubao";               // doubao | doubao_s2s | qwen
  std::string voice;                            // empty -> provider default
  std::string prompt_label = "通用 AI 助手";

  // --- Media ---
  int camera_width = 640;
  int camera_height = 480;
  int camera_fps = 30;
  int audio_sample_rate = 48000;
  int audio_channels = 1;
  bool publish_video = true;

  // --- UI ---
  int window_width = 1024;
  int window_height = 600;
  bool fullscreen = false;
  bool autostart = false;  // begin the AI call immediately on launch

  LogLevel log_level = LogLevel::Info;

  // Human-readable one-line summary for startup logging (key redacted).
  std::string summary() const;
};

// Parse command line / environment / config file into an AppConfig.
// On --help or a parse error, prints to stderr; `should_exit` is set and
// `exit_code` carries the process exit status the caller should use.
AppConfig load_config(int argc, char** argv, bool& should_exit, int& exit_code);

// Return a stable device id, generating and persisting one under
// ~/.config/jusiai-assistant/device_id when `configured` is empty.
std::string resolve_device_id(const std::string& configured);

}  // namespace jusiai

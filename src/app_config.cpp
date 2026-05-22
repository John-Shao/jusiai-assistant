#include "app_config.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>

namespace fs = std::filesystem;

namespace jusiai {
namespace {

std::string trim(const std::string& s) {
  std::size_t b = s.find_first_not_of(" \t\r\n");
  if (b == std::string::npos) return {};
  std::size_t e = s.find_last_not_of(" \t\r\n");
  return s.substr(b, e - b + 1);
}

bool parse_bool(const std::string& v, bool fallback) {
  std::string s = v;
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  if (s == "1" || s == "true" || s == "yes" || s == "on") return true;
  if (s == "0" || s == "false" || s == "no" || s == "off") return false;
  return fallback;
}

int parse_int(const std::string& v, int fallback) {
  try {
    return std::stoi(trim(v));
  } catch (...) {
    return fallback;
  }
}

float parse_float(const std::string& v, float fallback) {
  try {
    return std::stof(trim(v));
  } catch (...) {
    return fallback;
  }
}

LogLevel parse_log_level(const std::string& v, LogLevel fallback) {
  std::string s = trim(v);
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  if (s == "debug") return LogLevel::Debug;
  if (s == "info") return LogLevel::Info;
  if (s == "warn" || s == "warning") return LogLevel::Warn;
  if (s == "error") return LogLevel::Error;
  return fallback;
}

std::string home_dir() {
  const char* h = std::getenv("HOME");
  return h ? std::string(h) : std::string(".");
}

fs::path config_root() { return fs::path(home_dir()) / ".config" / "jusiai-assistant"; }

// Apply a single key/value pair to the config.
void apply_kv(AppConfig& cfg, const std::string& key, const std::string& value) {
  const std::string v = trim(value);
  if (key == "base_url") {
    if (!v.empty()) cfg.base_url = v;
  } else if (key == "device_api_key") {
    if (!v.empty()) cfg.device_api_key = v;
  } else if (key == "tls_verify") {
    cfg.tls_verify = parse_bool(v, cfg.tls_verify);
  } else if (key == "device_id") {
    cfg.device_id = v;
  } else if (key == "room_name") {
    if (!v.empty()) cfg.room_name = v;
  } else if (key == "provider") {
    if (!v.empty()) cfg.provider = v;
  } else if (key == "voice") {
    cfg.voice = v;
  } else if (key == "prompt_label") {
    cfg.prompt_label = v;
  } else if (key == "camera_width") {
    cfg.camera_width = parse_int(v, cfg.camera_width);
  } else if (key == "camera_height") {
    cfg.camera_height = parse_int(v, cfg.camera_height);
  } else if (key == "camera_fps") {
    cfg.camera_fps = parse_int(v, cfg.camera_fps);
  } else if (key == "camera_rotation") {
    cfg.camera_rotation = parse_int(v, cfg.camera_rotation);
  } else if (key == "camera_device") {
    if (!v.empty()) cfg.camera_device = v;
  } else if (key == "audio_sample_rate") {
    cfg.audio_sample_rate = parse_int(v, cfg.audio_sample_rate);
  } else if (key == "audio_channels") {
    cfg.audio_channels = parse_int(v, cfg.audio_channels);
  } else if (key == "audio_mic_gain") {
    cfg.audio_mic_gain = parse_float(v, cfg.audio_mic_gain);
  } else if (key == "audio_aec") {
    cfg.audio_aec = parse_bool(v, cfg.audio_aec);
  } else if (key == "audio_aec_delay_ms") {
    cfg.audio_aec_delay_ms = parse_int(v, cfg.audio_aec_delay_ms);
  } else if (key == "publish_video") {
    cfg.publish_video = parse_bool(v, cfg.publish_video);
  } else if (key == "window_width") {
    cfg.window_width = parse_int(v, cfg.window_width);
  } else if (key == "window_height") {
    cfg.window_height = parse_int(v, cfg.window_height);
  } else if (key == "fullscreen") {
    cfg.fullscreen = parse_bool(v, cfg.fullscreen);
  } else if (key == "autostart") {
    cfg.autostart = parse_bool(v, cfg.autostart);
  } else if (key == "language") {
    if (!v.empty()) cfg.language = v;
  } else if (key == "headless") {
    cfg.headless = parse_bool(v, cfg.headless);
  } else if (key == "control_bind") {
    if (!v.empty()) cfg.control_bind = v;
  } else if (key == "control_port") {
    cfg.control_port = parse_int(v, cfg.control_port);
  } else if (key == "log_level") {
    cfg.log_level = parse_log_level(v, cfg.log_level);
  } else {
    LOG_WARN("config: ignoring unknown key '%s'", key.c_str());
  }
}

bool load_config_file(AppConfig& cfg, const fs::path& path) {
  std::ifstream in(path);
  if (!in.is_open()) return false;
  LOG_INFO("config: loading %s", path.string().c_str());

  std::string line;
  while (std::getline(in, line)) {
    std::string t = trim(line);
    if (t.empty() || t[0] == '#') continue;
    std::size_t eq = t.find('=');
    if (eq == std::string::npos) continue;
    apply_kv(cfg, trim(t.substr(0, eq)), t.substr(eq + 1));
  }
  return true;
}

void apply_env(AppConfig& cfg) {
  struct EnvKey {
    const char* env;
    const char* key;
  };
  static const EnvKey kEnv[] = {
      {"JUSIAI_BASE_URL", "base_url"},
      {"JUSIAI_DEVICE_API_KEY", "device_api_key"},
      {"JUSIAI_DEVICE_ID", "device_id"},
      {"JUSIAI_ROOM_NAME", "room_name"},
      {"JUSIAI_PROVIDER", "provider"},
      {"JUSIAI_VOICE", "voice"},
      {"JUSIAI_PROMPT_LABEL", "prompt_label"},
      {"JUSIAI_AUTOSTART", "autostart"},
      {"JUSIAI_LANGUAGE", "language"},
      {"JUSIAI_HEADLESS", "headless"},
      {"JUSIAI_CONTROL_BIND", "control_bind"},
      {"JUSIAI_CONTROL_PORT", "control_port"},
      {"JUSIAI_LOG_LEVEL", "log_level"},
  };
  for (const auto& e : kEnv) {
    if (const char* val = std::getenv(e.env)) apply_kv(cfg, e.key, val);
  }
}

void print_usage(const char* prog) {
  std::fprintf(stderr,
      "JuSi AI Assistant " JUSIAI_VERSION " — Linux audio/video AI assistant\n\n"
      "Usage: %s [options]\n\n"
      "Options:\n"
      "  --config <path>        Configuration file to load\n"
      "  --base-url <url>       Backend base URL (default https://meet.jusiai.com)\n"
      "  --device-api-key <k>   Device API pre-shared key\n"
      "  --device-id <id>       Device identifier (default: auto-generated)\n"
      "  --provider <p>         AI provider: doubao | doubao_s2s | qwen\n"
      "  --voice <id>           AI output voice id\n"
      "  --prompt-label <name>  AI assistant persona label\n"
      "  --no-video             Do not publish the local camera track\n"
      "  --no-aec               Disable acoustic echo cancellation\n"
      "  --aec-delay <ms>       Echo-canceller speaker->mic delay hint (ms)\n"
      "  --fullscreen           Start the window in fullscreen\n"
      "  --autostart            Begin the AI call immediately on launch\n"
      "  --language <lang>      Interface language: zh | en (default zh)\n"
      "  --headless             Run with no display UI (for screenless devices)\n"
      "  --control-port <port>  Local HTTP control API port (0 disables it)\n"
      "  --control-bind <addr>  Control API bind address (default 127.0.0.1)\n"
      "  --log-level <lvl>      debug | info | warn | error (default info)\n"
      "  -h, --help             Show this help and exit\n",
      prog);
}

}  // namespace

std::string AppConfig::summary() const {
  std::ostringstream os;
  os << "base_url=" << base_url << " provider=" << provider
     << " device_id=" << (device_id.empty() ? "<auto>" : device_id)
     << " video=" << (publish_video ? "on" : "off")
     << " aec=" << (audio_aec ? "on" : "off")
     << " window=" << window_width << "x" << window_height
     << " headless=" << (headless ? "on" : "off");
  if (control_port > 0) {
    os << " control=" << control_bind << ":" << control_port;
  }
  return os.str();
}

std::string resolve_device_id(const std::string& configured) {
  if (!configured.empty()) return configured;

  fs::path id_path = config_root() / "device_id";
  std::error_code ec;

  // Reuse a previously generated id.
  if (fs::exists(id_path, ec)) {
    std::ifstream in(id_path);
    std::string id;
    std::getline(in, id);
    id = trim(id);
    if (!id.empty()) return id;
  }

  // Generate JUSI-LINUX-XXXXXXXXXXXX (12 hex digits of randomness).
  std::random_device rd;
  std::uniform_int_distribution<int> dist(0, 15);
  std::string id = "JUSI-LINUX-";
  for (int i = 0; i < 12; ++i) id += "0123456789abcdef"[dist(rd)];

  fs::create_directories(config_root(), ec);
  std::ofstream out(id_path);
  if (out.is_open()) {
    out << id << "\n";
    LOG_INFO("config: generated device id %s", id.c_str());
  } else {
    LOG_WARN("config: could not persist device id to %s", id_path.string().c_str());
  }
  return id;
}

AppConfig load_config(int argc, char** argv, bool& should_exit, int& exit_code) {
  should_exit = false;
  exit_code = 0;
  AppConfig cfg;

  // Pass 1: find an explicit --config before loading any file.
  std::string config_path;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
      config_path = argv[i + 1];
    } else if (std::strcmp(argv[i], "-h") == 0 ||
               std::strcmp(argv[i], "--help") == 0) {
      print_usage(argv[0]);
      should_exit = true;
      return cfg;
    }
  }

  // Layer: config file.
  if (!config_path.empty()) {
    if (!load_config_file(cfg, config_path)) {
      std::fprintf(stderr, "error: cannot open config file: %s\n",
                   config_path.c_str());
      should_exit = true;
      exit_code = 1;
      return cfg;
    }
  } else {
    for (const fs::path& p : {fs::path("jusiai-assistant.conf"),
                              config_root() / "jusiai-assistant.conf"}) {
      if (load_config_file(cfg, p)) break;
    }
  }

  // Layer: environment.
  apply_env(cfg);

  // Layer: command-line flags (highest priority).
  auto next = [&](int& i) -> const char* {
    if (i + 1 >= argc) {
      std::fprintf(stderr, "error: %s requires an argument\n", argv[i]);
      should_exit = true;
      exit_code = 1;
      return nullptr;
    }
    return argv[++i];
  };

  for (int i = 1; i < argc && !should_exit; ++i) {
    std::string a = argv[i];
    if (a == "--config") {
      ++i;  // already handled
    } else if (a == "--base-url") {
      if (const char* v = next(i)) cfg.base_url = v;
    } else if (a == "--device-api-key") {
      if (const char* v = next(i)) cfg.device_api_key = v;
    } else if (a == "--device-id") {
      if (const char* v = next(i)) cfg.device_id = v;
    } else if (a == "--provider") {
      if (const char* v = next(i)) cfg.provider = v;
    } else if (a == "--voice") {
      if (const char* v = next(i)) cfg.voice = v;
    } else if (a == "--prompt-label") {
      if (const char* v = next(i)) cfg.prompt_label = v;
    } else if (a == "--no-video") {
      cfg.publish_video = false;
    } else if (a == "--no-aec") {
      cfg.audio_aec = false;
    } else if (a == "--aec-delay") {
      if (const char* v = next(i))
        cfg.audio_aec_delay_ms = parse_int(v, cfg.audio_aec_delay_ms);
    } else if (a == "--fullscreen") {
      cfg.fullscreen = true;
    } else if (a == "--autostart") {
      cfg.autostart = true;
    } else if (a == "--language") {
      if (const char* v = next(i)) cfg.language = v;
    } else if (a == "--headless") {
      cfg.headless = true;
    } else if (a == "--control-port") {
      if (const char* v = next(i)) cfg.control_port = parse_int(v, cfg.control_port);
    } else if (a == "--control-bind") {
      if (const char* v = next(i)) cfg.control_bind = v;
    } else if (a == "--log-level") {
      if (const char* v = next(i)) cfg.log_level = parse_log_level(v, cfg.log_level);
    } else {
      std::fprintf(stderr, "error: unknown option: %s\n", a.c_str());
      print_usage(argv[0]);
      should_exit = true;
      exit_code = 1;
    }
  }

  if (should_exit) return cfg;

  // Normalise: strip a trailing '/' from base_url so endpoint joins are clean.
  while (!cfg.base_url.empty() && cfg.base_url.back() == '/') {
    cfg.base_url.pop_back();
  }
  cfg.device_id = resolve_device_id(cfg.device_id);

  // A headless device has no touch UI, so it must be reachable through the
  // control API — give it a default port if the operator did not set one.
  if (cfg.headless && cfg.control_port == 0) {
    cfg.control_port = 8765;
    LOG_INFO("config: headless with no control_port set — defaulting to %d",
             cfg.control_port);
  }
  return cfg;
}

}  // namespace jusiai

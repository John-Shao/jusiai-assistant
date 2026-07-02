#include "app_config.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>

#include <nlohmann/json.hpp>

namespace fs = std::filesystem;
using json = nlohmann::json;

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

fs::path runtime_agent_config_path() {
  return config_root() / "agent_config.json";
}

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
  } else if (key == "provider" || key == "profile_code") {
    if (!v.empty()) cfg.provider = v;
  } else if (key == "voice") {
    cfg.voice = v;
  } else if (key == "prompt_id") {
    cfg.prompt_id = v;
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
  } else if (key == "autostart") {
    cfg.autostart = parse_bool(v, cfg.autostart);
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

}  // anonymous namespace

bool has_runtime_agent_config() {
  std::error_code ec;
  return fs::exists(runtime_agent_config_path(), ec);
}

bool quarantine_runtime_agent_config() {
  const fs::path src = runtime_agent_config_path();
  std::error_code ec;
  if (!fs::exists(src, ec)) return false;
  const auto ts = static_cast<long long>(std::time(nullptr));
  const fs::path dst = src.string() + ".rejected." + std::to_string(ts);
  fs::rename(src, dst, ec);
  if (ec) {
    LOG_WARN("config: could not quarantine %s: %s", src.string().c_str(),
             ec.message().c_str());
    return false;
  }
  LOG_INFO("config: quarantined runtime agent config to %s",
           dst.string().c_str());
  return true;
}

namespace {

void load_runtime_agent_config(AppConfig& cfg) {
  const fs::path path = runtime_agent_config_path();
  std::ifstream in(path);
  if (!in.is_open()) return;

  try {
    json j = json::parse(in);
    if (!j.is_object()) return;
    LOG_INFO("config: loading runtime agent config %s", path.string().c_str());

    if (j.contains("profile_code") && j["profile_code"].is_string()) {
      cfg.provider = j["profile_code"].get<std::string>();
    }
    if (j.contains("provider") && j["provider"].is_string()) {
      cfg.provider = j["provider"].get<std::string>();
    }
    if (j.contains("voice") && j["voice"].is_string()) {
      cfg.voice = j["voice"].get<std::string>();
    }
    if (j.contains("prompt_id") && j["prompt_id"].is_string()) {
      cfg.prompt_id = j["prompt_id"].get<std::string>();
    }
  } catch (const std::exception& e) {
    LOG_WARN("config: ignoring bad runtime agent config %s: %s",
             path.string().c_str(), e.what());
  }
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
      {"JUSIAI_PROVIDER", "provider"},
      {"JUSIAI_PROFILE_CODE", "profile_code"},
      {"JUSIAI_VOICE", "voice"},
      {"JUSIAI_PROMPT_ID", "prompt_id"},
      {"JUSIAI_AUTOSTART", "autostart"},
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
      "JuSi AI Assistant " JUSIAI_VERSION " — headless audio/video AI assistant\n\n"
      "Usage: %s [options]\n\n"
      "Options:\n"
      "  --config <path>        Configuration file to load\n"
      "  --base-url <url>       Backend base URL (default https://meet.jusiai.com)\n"
      "  --device-api-key <k>   Device API pre-shared key\n"
      "  --device-id <id>       Device identifier (default: auto-generated)\n"
      "  --provider <p>         AI provider: doubao | doubao_s2s | qwen\n"
      "  --profile-code <p>     AI profile code (alias for --provider)\n"
      "  --voice <voice>        AI output voice\n"
      "  --prompt-id <id>       Prompt id from ai-agent-config-v2\n"
      "  --no-video             Do not publish the local camera track\n"
      "  --no-aec               Disable acoustic echo cancellation\n"
      "  --aec-delay <ms>       Echo-canceller speaker->mic delay hint (ms)\n"
      "  --autostart            Begin the AI call immediately on launch\n"
      "  --control-port <port>  Local HTTP control API port (default 8765)\n"
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
     << " aec=" << (audio_aec ? "on" : "off");
  if (control_port > 0) {
    os << " control=" << control_bind << ":" << control_port;
  } else {
    os << " control=disabled";
  }
  return os.str();
}

bool save_runtime_agent_config(const AppConfig& cfg, std::string* error) {
  const fs::path path = runtime_agent_config_path();
  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);
  if (ec) {
    if (error) *error = "cannot create config directory: " + ec.message();
    return false;
  }

  json j = {
      {"profile_code", cfg.provider},
      {"voice", cfg.voice},
      {"prompt_id", cfg.prompt_id},
  };

  // Atomic replace: write the new JSON to a sibling `.tmp`, then rename() it
  // onto the target. On POSIX, rename over an existing file is atomic, so a
  // mid-write interrupt (SIGKILL, power loss) can never leave the target
  // half-written and unparseable — which the load path would silently discard,
  // losing the operator's persisted override.
  //
  // Note: this guarantees "either the old or the new file is visible", not
  // "the new file survives a power loss right after we return". Full
  // durability would additionally fsync(fd) and fsync(directory) before the
  // rename; we don't, because the plain iostream write path stays simpler and
  // the operator can always re-POST if the very last write is lost.
  const fs::path tmp = path.string() + ".tmp";
  {
    std::ofstream out(tmp, std::ios::trunc);
    if (!out.is_open()) {
      if (error) *error = "cannot open " + tmp.string() + " for writing";
      return false;
    }
    out << j.dump(2) << "\n";
    out.close();
    if (!out) {
      if (error) *error = "failed to write " + tmp.string();
      fs::remove(tmp, ec);
      return false;
    }
  }
  fs::rename(tmp, path, ec);
  if (ec) {
    if (error) *error = "cannot rename " + tmp.string() + " -> " +
                        path.string() + ": " + ec.message();
    std::error_code rm_ec;
    fs::remove(tmp, rm_ec);
    return false;
  }
  LOG_INFO("config: saved runtime agent config %s", path.string().c_str());
  return true;
}

std::string resolve_device_id(const std::string& configured) {
  if (!configured.empty()) return configured;

  // Prefer the SoC eFuse serial exposed via /proc/cpuinfo. It lives in
  // on-die OTP so it survives rootfs / storage / U-Boot resets — only a
  // board-level SoC replacement changes it. 64-bit Rockchip chip IDs are
  // globally unique per chip, so `JUSI-<serial>` is already unique without
  // any product-line prefix.
  {
    std::ifstream cpuinfo("/proc/cpuinfo");
    std::string line;
    while (std::getline(cpuinfo, line)) {
      if (line.compare(0, 6, "Serial") != 0) continue;
      auto colon = line.find(':');
      if (colon == std::string::npos) continue;
      std::string serial = trim(line.substr(colon + 1));
      if (!serial.empty()) {
        std::string id = "JUSI-" + serial;
        LOG_INFO("config: device_id from SoC serial: %s", id.c_str());
        return id;
      }
    }
  }

  // Fallback (desktop / x86 dev with no SoC serial): generate a random id
  // once and persist it. Note this ID does NOT survive a rootfs wipe.
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

AppConfig load_config(int argc, char** argv, bool& should_exit, int& exit_code,
                      bool skip_runtime_layer) {
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

  // Layer: runtime agent config persisted by the local control API.
  // (Skipped by main.cpp's self-heal path when a bad file has just been
  // quarantined.)
  if (!skip_runtime_layer) {
    load_runtime_agent_config(cfg);
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
    } else if (a == "--provider" || a == "--profile-code") {
      if (const char* v = next(i)) cfg.provider = v;
    } else if (a == "--voice") {
      if (const char* v = next(i)) cfg.voice = v;
    } else if (a == "--prompt-id") {
      if (const char* v = next(i)) cfg.prompt_id = v;
    } else if (a == "--no-video") {
      cfg.publish_video = false;
    } else if (a == "--no-aec") {
      cfg.audio_aec = false;
    } else if (a == "--aec-delay") {
      if (const char* v = next(i))
        cfg.audio_aec_delay_ms = parse_int(v, cfg.audio_aec_delay_ms);
    } else if (a == "--autostart") {
      cfg.autostart = true;
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
  return cfg;
}

}  // namespace jusiai

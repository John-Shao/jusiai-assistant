#include "control/control_server.h"

#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>

#include <nlohmann/json.hpp>

#include "core/assistant_controller.h"
#include "httplib.h"
#include "log.h"

using json = nlohmann::json;

namespace jusiai {
namespace {

// Stable, language-independent name for a lifecycle state. Callers should key
// off this rather than the human-readable (translated) status text.
const char* state_name(AssistantState s) {
  switch (s) {
    case AssistantState::Idle:         return "idle";
    case AssistantState::CreatingRoom: return "creating_room";
    case AssistantState::Connecting:   return "connecting";
    case AssistantState::WaitingAgent: return "waiting_agent";
    case AssistantState::InCall:       return "in_call";
    case AssistantState::Stopping:     return "stopping";
    case AssistantState::Error:        return "error";
  }
  return "unknown";
}

std::string snapshot_json(const UiSnapshot& s) {
  json j = {
      {"state", state_name(s.state)},
      {"status", s.status},
      {"detail", s.detail},
      {"mic_muted", s.mic_muted},
      {"camera_muted", s.cam_muted},
      {"agent_speaking", s.agent_speaking},
      {"camera_available", s.camera_available},
  };
  return j.dump();
}

json agent_config_json(const AppConfig& cfg) {
  return {
      {"profile_code", cfg.provider},
      {"provider", cfg.provider},
      {"voice", cfg.voice},
      {"prompt_id", cfg.prompt_id},
  };
}

bool snapshots_differ(const UiSnapshot& a, const UiSnapshot& b) {
  return a.state != b.state || a.status != b.status || a.detail != b.detail ||
         a.mic_muted != b.mic_muted || a.cam_muted != b.cam_muted ||
         a.agent_speaking != b.agent_speaking ||
         a.camera_available != b.camera_available;
}

// Read a boolean argument from either a query parameter or a JSON body field.
// Returns -1 when absent / unparseable, 0 or 1 otherwise.
int read_bool_arg(const httplib::Request& req, const char* name) {
  auto parse = [](std::string v) -> int {
    for (auto& c : v) c = static_cast<char>(std::tolower((unsigned char)c));
    if (v == "1" || v == "true" || v == "yes" || v == "on") return 1;
    if (v == "0" || v == "false" || v == "no" || v == "off") return 0;
    return -1;
  };
  if (req.has_param(name)) return parse(req.get_param_value(name));
  if (!req.body.empty()) {
    try {
      json b = json::parse(req.body);
      if (b.contains(name)) {
        if (b[name].is_boolean()) return b[name].get<bool>() ? 1 : 0;
        if (b[name].is_string()) return parse(b[name].get<std::string>());
        if (b[name].is_number()) return b[name].get<int>() != 0 ? 1 : 0;
      }
    } catch (...) {
      // not JSON — treated as absent
    }
  }
  return -1;
}

bool assign_string_value(const json& v, std::string* out,
                         const std::string& name, std::string* error) {
  if (v.is_null()) {
    out->clear();
    return true;
  }
  if (v.is_string()) {
    *out = v.get<std::string>();
    return true;
  }
  *error = "'" + name + "' must be a string or null";
  return false;
}

bool assign_object_string(const json& obj, const char* field,
                          std::string* out, const std::string& name,
                          std::string* error, bool* changed) {
  if (!obj.contains(field)) return true;
  if (!assign_string_value(obj[field], out, name + "." + field, error)) {
    return false;
  }
  *changed = true;
  return true;
}

bool parse_agent_config_update(const httplib::Request& req, AppConfig* cfg,
                               std::string* error) {
  if (req.body.empty()) {
    *error = "missing JSON body";
    return false;
  }

  json body;
  try {
    body = json::parse(req.body);
  } catch (const std::exception& e) {
    *error = std::string("invalid JSON: ") + e.what();
    return false;
  }
  if (!body.is_object()) {
    *error = "JSON body must be an object";
    return false;
  }

  bool changed = false;
  if (body.contains("profile_code")) {
    if (!assign_string_value(body["profile_code"], &cfg->provider,
                             "profile_code", error)) return false;
    changed = true;
  }
  if (body.contains("provider")) {
    if (!assign_string_value(body["provider"], &cfg->provider,
                             "provider", error)) return false;
    changed = true;
  }
  if (body.contains("profile")) {
    const json& p = body["profile"];
    if (p.is_object()) {
      if (!assign_object_string(p, "code", &cfg->provider, "profile", error,
                                &changed)) {
        return false;
      }
    } else {
      if (!assign_string_value(p, &cfg->provider, "profile", error)) {
        return false;
      }
      changed = true;
    }
  }

  if (body.contains("voice")) {
    const json& v = body["voice"];
    if (v.is_object()) {
      if (!assign_object_string(v, "value", &cfg->voice, "voice", error,
                                &changed)) {
        return false;
      }
    } else {
      if (!assign_string_value(v, &cfg->voice, "voice", error)) return false;
      changed = true;
    }
  }

  if (body.contains("prompt")) {
    const json& p = body["prompt"];
    if (!p.is_object()) {
      *error = "'prompt' must be an object";
      return false;
    }
    if (!assign_object_string(p, "id", &cfg->prompt_id, "prompt", error,
                              &changed)) {
      return false;
    }
  }

  if (body.contains("prompt_id")) {
    if (!assign_string_value(body["prompt_id"], &cfg->prompt_id, "prompt_id",
                             error)) return false;
    changed = true;
  }

  if (!changed) {
    *error = "no supported fields; expected profile_code/provider/profile, "
             "voice, prompt_id or prompt";
    return false;
  }
  return true;
}

}  // namespace

// SSE fan-out: holds the latest status payload and a monotonic sequence so
// every connected client streams each change exactly once.
struct ControlServer::EventHub {
  std::mutex m;
  std::condition_variable cv;
  std::uint64_t seq = 0;
  std::string latest;
  bool closed = false;

  void publish(const std::string& payload) {
    {
      std::lock_guard<std::mutex> lk(m);
      ++seq;
      latest = payload;
    }
    cv.notify_all();
  }

  // Block until a payload newer than *cursor is available; on return *cursor
  // is advanced and *out holds it. Returns false once the hub is closing.
  bool wait_next(std::uint64_t* cursor, std::string* out) {
    std::unique_lock<std::mutex> lk(m);
    cv.wait(lk, [&] { return closed || seq > *cursor; });
    if (closed) return false;
    *cursor = seq;
    *out = latest;
    return true;
  }

  void shutdown() {
    {
      std::lock_guard<std::mutex> lk(m);
      closed = true;
    }
    cv.notify_all();
  }
};

ControlServer::ControlServer(AssistantController* controller,
                             const AppConfig& config)
    : controller_(controller),
      bind_addr_(config.control_bind),
      port_(config.control_port),
      hub_(std::make_unique<EventHub>()) {}

ControlServer::~ControlServer() { stop(); }

bool ControlServer::start() {
  if (running_.load()) return true;

  server_ = std::make_unique<httplib::Server>();

  auto ok_json = [](httplib::Response& res) {
    res.set_content("{\"ok\":true}", "application/json");
  };
  auto bad_request = [](httplib::Response& res, const std::string& msg) {
    res.status = 400;
    res.set_content(json({{"ok", false}, {"error", msg}}).dump(),
                    "application/json");
  };

  server_->Post("/start", [this, ok_json](const httplib::Request&,
                                          httplib::Response& res) {
    LOG_INFO("control: POST /start");
    controller_->request_start();
    ok_json(res);
  });

  server_->Post("/stop", [this, ok_json](const httplib::Request&,
                                         httplib::Response& res) {
    LOG_INFO("control: POST /stop");
    controller_->request_stop();
    ok_json(res);
  });

  server_->Post("/mic", [this, ok_json, bad_request](
                            const httplib::Request& req,
                            httplib::Response& res) {
    const int muted = read_bool_arg(req, "muted");
    if (muted < 0) {
      bad_request(res, "missing or invalid 'muted' (expected true/false)");
      return;
    }
    LOG_INFO("control: POST /mic muted=%d", muted);
    controller_->request_set_mic_muted(muted != 0);
    ok_json(res);
  });

  server_->Post("/camera", [this, ok_json, bad_request](
                               const httplib::Request& req,
                               httplib::Response& res) {
    const int enabled = read_bool_arg(req, "enabled");
    if (enabled < 0) {
      bad_request(res, "missing or invalid 'enabled' (expected true/false)");
      return;
    }
    LOG_INFO("control: POST /camera enabled=%d", enabled);
    controller_->request_set_camera_muted(enabled == 0);
    ok_json(res);
  });

  server_->Get("/config", [this](const httplib::Request&,
                                 httplib::Response& res) {
    const std::string body =
        agent_config_json(controller_->config_snapshot()).dump();
    LOG_INFO("control: GET /config -> %s", body.c_str());
    res.set_content(body, "application/json");
  });

  server_->Post("/config", [this, bad_request](const httplib::Request& req,
                                               httplib::Response& res) {
    LOG_INFO("control: POST /config body: %s", req.body.c_str());
    AppConfig cfg = controller_->config_snapshot();
    std::string error;
    if (!parse_agent_config_update(req, &cfg, &error)) {
      LOG_WARN("control: POST /config rejected: %s", error.c_str());
      bad_request(res, error);
      return;
    }

    // Persist THEN apply. Safe today because set_agent_config() cannot fail
    // (it only mutates in-memory state under a mutex). If you ever add
    // validation to set_agent_config, run that check BEFORE this save so a
    // rejected update never leaks onto disk.
    if (!save_runtime_agent_config(cfg, &error)) {
      res.status = 500;
      res.set_content(json({{"ok", false}, {"error", error}}).dump(),
                      "application/json");
      return;
    }

    const bool restart_queued = controller_->set_agent_config(cfg);
    LOG_INFO("control: POST /config applied -> %s (restart_queued=%d)",
             agent_config_json(cfg).dump().c_str(), restart_queued);
    res.set_content(json({{"ok", true},
                          {"restart_queued", restart_queued},
                          {"config", agent_config_json(cfg)}})
                        .dump(),
                    "application/json");
  });

  server_->Get("/status", [this](const httplib::Request&,
                                 httplib::Response& res) {
    res.set_content(snapshot_json(controller_->snapshot()), "application/json");
  });

  server_->Get("/healthz", [](const httplib::Request&,
                              httplib::Response& res) {
    res.set_content("{\"ok\":true}", "application/json");
  });

  // Server-Sent Events: each connection streams every status change. A fresh
  // client starts at cursor 0 and so receives the current status at once.
  server_->Get("/events", [this](const httplib::Request&,
                                 httplib::Response& res) {
    res.set_header("Cache-Control", "no-cache");
    auto cursor = std::make_shared<std::uint64_t>(0);
    res.set_chunked_content_provider(
        "text/event-stream",
        [this, cursor](std::size_t, httplib::DataSink& sink) -> bool {
          std::string payload;
          if (!hub_->wait_next(cursor.get(), &payload)) return false;
          const std::string frame = "event: status\ndata: " + payload + "\n\n";
          return sink.write(frame.data(), frame.size());
        });
  });

  if (!server_->bind_to_port(bind_addr_, port_)) {
    LOG_ERROR("control: cannot bind %s:%d", bind_addr_.c_str(), port_);
    server_.reset();
    return false;
  }

  running_.store(true);
  // Seed the hub so a client connecting before the first state change still
  // gets the current status immediately.
  hub_->publish(snapshot_json(controller_->snapshot()));

  server_thread_ = std::thread([this] { server_->listen_after_bind(); });
  watch_thread_ = std::thread(&ControlServer::watch_loop, this);

  LOG_INFO("control: HTTP control API listening on %s:%d", bind_addr_.c_str(),
           port_);
  return true;
}

void ControlServer::stop() {
  if (!running_.exchange(false)) return;

  hub_->shutdown();             // unblock every open /events stream
  if (server_) server_->stop();  // closes connections, ends listen_after_bind

  if (watch_thread_.joinable()) watch_thread_.join();
  if (server_thread_.joinable()) server_thread_.join();
  server_.reset();
  LOG_INFO("control: HTTP control API stopped");
}

void ControlServer::watch_loop() {
  UiSnapshot last;
  bool have_last = false;
  auto last_beat = std::chrono::steady_clock::now();

  while (running_.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    if (!running_.load()) break;

    UiSnapshot s = controller_->snapshot();
    const auto now = std::chrono::steady_clock::now();
    // Re-publish at least every 10 s as an SSE keep-alive, which also lets a
    // failed write surface a client that has gone away.
    const bool beat = now - last_beat >= std::chrono::seconds(10);
    if (!have_last || beat || snapshots_differ(s, last)) {
      hub_->publish(snapshot_json(s));
      last = s;
      have_last = true;
      last_beat = now;
    }
  }
}

}  // namespace jusiai

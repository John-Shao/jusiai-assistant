#include "api/device_api_client.h"

#include <nlohmann/json.hpp>

#include "httplib.h"
#include "log.h"

using json = nlohmann::json;

namespace jusiai {
namespace {

constexpr int kConnectTimeoutSec = 10;
constexpr int kReadTimeoutSec = 20;

// Pull a human-readable message out of an error response body.
std::string extract_error_message(const std::string& body, int status) {
  if (!body.empty()) {
    try {
      json j = json::parse(body);
      for (const char* key : {"error", "detail", "message"}) {
        if (j.contains(key) && j[key].is_string()) {
          return j[key].get<std::string>();
        }
      }
      if (j.contains("errors")) return j["errors"].dump();
    } catch (...) {
      // not JSON — fall through
    }
  }
  return "HTTP " + std::to_string(status);
}

std::string describe_transport_error(httplib::Error err) {
  switch (err) {
    case httplib::Error::Connection:        return "Cannot reach the server";
    case httplib::Error::ConnectionTimeout: return "Connection timed out";
    case httplib::Error::Read:              return "Failed to read the response";
    case httplib::Error::Write:             return "Failed to send the request";
    case httplib::Error::SSLConnection:
    case httplib::Error::SSLLoadingCerts:
    case httplib::Error::SSLServerVerification:
                                            return "HTTPS connection failed";
    default:                                return "Network request failed";
  }
}

}  // namespace

DeviceApiClient::DeviceApiClient(std::string base_url,
                                 std::string device_api_key, bool verify_tls)
    : base_url_(std::move(base_url)),
      auth_header_("Bearer " + device_api_key),
      verify_tls_(verify_tls) {}

ApiOutcome DeviceApiClient::post_json(const std::string& path,
                                      const std::string& body,
                                      std::string& response_body) {
  response_body.clear();

  httplib::Client cli(base_url_);
  cli.set_connection_timeout(kConnectTimeoutSec);
  cli.set_read_timeout(kReadTimeoutSec);
  cli.set_write_timeout(kReadTimeoutSec);
  cli.set_follow_location(true);
  cli.enable_server_certificate_verification(verify_tls_);

  httplib::Headers headers = {{"Authorization", auth_header_}};
  LOG_DEBUG("device-api: POST %s%s", base_url_.c_str(), path.c_str());

  httplib::Result res = cli.Post(path, headers, body, "application/json");
  if (!res) {
    return ApiOutcome::failure(0, describe_transport_error(res.error()));
  }

  response_body = res->body;
  if (res->status >= 200 && res->status < 300) {
    return ApiOutcome::success(res->status);
  }

  LOG_WARN("device-api: %s -> HTTP %d: %s", path.c_str(), res->status,
           res->body.c_str());
  return ApiOutcome::failure(res->status,
                             extract_error_message(res->body, res->status));
}

ApiOutcome DeviceApiClient::create_room(const std::string& device_id,
                                        const std::string& room_name,
                                        RoomCredentials& out) {
  out = RoomCredentials{};

  json req = {{"device_id", device_id}};
  if (!room_name.empty()) req["name"] = room_name;

  std::string body;
  ApiOutcome outcome = post_json("/device-api/v1.0/rooms/", req.dump(), body);
  if (!outcome.ok) return outcome;

  try {
    json j = json::parse(body);
    out.room_id = j.value("id", "");
    out.slug = j.value("slug", "");
    if (j.contains("livekit") && j["livekit"].is_object()) {
      const json& lk = j["livekit"];
      out.livekit_url = lk.value("url", "");
      out.livekit_token = lk.value("token", "");
    }
  } catch (const std::exception& e) {
    return ApiOutcome::failure(outcome.http_status,
                               std::string("Bad room response: ") + e.what());
  }

  if (!out.valid()) {
    return ApiOutcome::failure(outcome.http_status,
                               "Incomplete room info from the server");
  }
  LOG_INFO("device-api: room created id=%s slug=%s", out.room_id.c_str(),
           out.slug.c_str());
  return outcome;
}

ApiOutcome DeviceApiClient::start_ai_agent(const std::string& room_id,
                                           const std::string& device_id,
                                           const std::string& provider,
                                           const std::string& voice,
                                           const std::string& prompt_label) {
  json config = json::object();
  if (!voice.empty()) config["voice"] = voice;
  if (!prompt_label.empty()) config["prompt_label"] = prompt_label;

  json req = {{"device_id", device_id}};
  if (!provider.empty()) req["provider"] = provider;
  if (!config.empty()) req["config"] = config;

  std::string body;
  ApiOutcome outcome = post_json(
      "/device-api/v1.0/rooms/" + room_id + "/start-ai-agent/", req.dump(),
      body);
  if (outcome.ok) {
    LOG_INFO("device-api: AI agent dispatched (provider=%s)", provider.c_str());
  }
  return outcome;
}

ApiOutcome DeviceApiClient::stop_ai_agent(const std::string& room_id) {
  std::string body;
  ApiOutcome outcome = post_json(
      "/device-api/v1.0/rooms/" + room_id + "/stop-ai-agent/", "{}", body);
  if (outcome.ok) LOG_INFO("device-api: AI agent stopped");
  return outcome;
}

}  // namespace jusiai

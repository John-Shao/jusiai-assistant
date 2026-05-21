// HTTP client for the JuSi Meet device API (device-api/v1.0).
//
// These endpoints authenticate with the pre-shared DEVICE_API_KEY carried in
// an Authorization: Bearer header — never a user token. The client drives the
// same closed loop the Android "AI 助手" uses:
//
//   create_room()    POST /device-api/v1.0/rooms/
//   start_ai_agent() POST /device-api/v1.0/rooms/{id}/start-ai-agent/
//   stop_ai_agent()  POST /device-api/v1.0/rooms/{id}/stop-ai-agent/
#pragma once

#include <string>

#include "api/api_types.h"

namespace jusiai {

class DeviceApiClient {
 public:
  // `base_url` is e.g. "https://meet.jusiai.com" (no trailing slash).
  // `verify_tls` enables server certificate verification (needs a CA store).
  DeviceApiClient(std::string base_url, std::string device_api_key,
                  bool verify_tls);

  // Create an anonymous 1v1 AI room. On success fills `out` with the LiveKit
  // connection info needed to join.
  ApiOutcome create_room(const std::string& device_id,
                         const std::string& room_name,
                         RoomCredentials& out);

  // Dispatch the AI agent into `room_id`. `voice` / `prompt_label` may be empty
  // to use the provider defaults.
  ApiOutcome start_ai_agent(const std::string& room_id,
                            const std::string& device_id,
                            const std::string& provider,
                            const std::string& voice,
                            const std::string& prompt_label);

  // Remove the AI agent from `room_id`.
  ApiOutcome stop_ai_agent(const std::string& room_id);

 private:
  // Perform a JSON POST; returns the outcome and, on HTTP 2xx, the body.
  ApiOutcome post_json(const std::string& path, const std::string& body,
                       std::string& response_body);

  std::string base_url_;
  std::string auth_header_;  // "Bearer <device_api_key>"
  bool verify_tls_;
};

}  // namespace jusiai

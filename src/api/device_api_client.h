// HTTP client for the JuSi Meet device API (device-api/v1.0).
//
// These endpoints authenticate with the pre-shared DEVICE_API_KEY carried in
// an Authorization: Bearer header — never a user token. The client drives the
// same closed loop the Android "AI 助手" uses:
//
//   start_chat_bot()  POST /device-api/v1.0/rooms/start-chat-bot/  (room + agent, one call)
//   stop_chat_bot()   POST /device-api/v1.0/rooms/stop-chat-bot/   (remove agent + end room)
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

  // Set up a 1v1 AI session in a single call: the backend get-or-creates the
  // device's anonymous room, mints a LiveKit join token, and dispatches the AI
  // agent. On success fills `out` with the LiveKit connection info needed to
  // join. `profile_code` may be empty to use the backend default; `voice`
  // selects the output voice; `prompt_id` selects a predefined prompt from
  // ai-agent-config-v2 (empty => provider default). start-chat-bot only accepts
  // prompt_id — custom prompt_label/prompt_content are not supported here.
  ApiOutcome start_chat_bot(const std::string& device_id,
                            const std::string& profile_code,
                            const std::string& voice,
                            const std::string& prompt_id,
                            RoomCredentials& out);

  // End the device's active 1v1 AI session: removes the AI agent and ends the
  // room. Located by `device_id` (no room id needed); idempotent.
  ApiOutcome stop_chat_bot(const std::string& device_id);

 private:
  // Perform a JSON POST; returns the outcome and, on HTTP 2xx, the body.
  ApiOutcome post_json(const std::string& path, const std::string& body,
                       std::string& response_body);

  std::string base_url_;
  std::string auth_header_;  // "Bearer <device_api_key>"
  bool verify_tls_;
};

}  // namespace jusiai

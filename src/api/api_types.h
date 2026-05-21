// Plain data types exchanged with the JuSi Meet device API.
#pragma once

#include <string>

namespace jusiai {

// LiveKit connection info returned by POST /device-api/v1.0/rooms/.
struct RoomCredentials {
  std::string room_id;        // room UUID — the {id} for start/stop-ai-agent
  std::string slug;           // 6-digit meeting code
  std::string livekit_url;    // LiveKit server WebSocket URL
  std::string livekit_token;  // join token, identity = device-<device_id>

  bool valid() const {
    return !room_id.empty() && !livekit_url.empty() && !livekit_token.empty();
  }
};

// Outcome of a device API call.
struct ApiOutcome {
  bool ok = false;
  int http_status = 0;     // 0 when the request never reached the server
  std::string error;       // user-facing message, set when ok == false

  static ApiOutcome success(int status) { return {true, status, {}}; }
  static ApiOutcome failure(int status, std::string msg) {
    return {false, status, std::move(msg)};
  }
};

}  // namespace jusiai

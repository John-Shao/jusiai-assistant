// JuSi AI Assistant — headless audio/video AI assistant client.
//
// Runs on screenless devices (Rockchip RV1126B IPC boards and similar). All
// interaction happens through the local HTTP control API (see control/); the
// closed loop itself (device-API connect → LiveKit → publish mic + camera →
// converse → hang up) runs on the controller's worker thread.
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>

#include "app_config.h"
#include "control/control_server.h"
#include "core/assistant_controller.h"
#include "livekit/livekit.h"
#include "log.h"

namespace {

// The RV1126B Buildroot rootfs ships no system CA store, so point the TLS
// libraries (OpenSSL for the device API, rustls inside the LiveKit SDK) at the
// ca-certificates.crt staged next to the executable. Honour an existing
// SSL_CERT_FILE if the operator set one.
void configure_tls_bundle() {
  if (std::getenv("SSL_CERT_FILE")) return;

  char buf[1024];
  ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (n <= 0) return;
  buf[n] = '\0';

  std::string path(buf);
  const auto slash = path.find_last_of('/');
  if (slash == std::string::npos) return;
  const std::string ca = path.substr(0, slash) + "/ca-certificates.crt";
  if (access(ca.c_str(), R_OK) != 0) return;

  setenv("SSL_CERT_FILE", ca.c_str(), 1);
  LOG_INFO("tls: using CA bundle %s", ca.c_str());
}

// Signal-driven quit — the closed loop runs on the controller's worker
// thread; main() just blocks here until asked to exit.
std::atomic<bool> g_quit{false};
void on_signal(int) { g_quit.store(true); }

void install_signal_handlers() {
  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);
}

void wait_for_signal() {
  LOG_INFO("running — send SIGINT/SIGTERM to quit");
  while (!g_quit.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }
  LOG_INFO("stopping");
}

enum class FirstStartOutcome {
  Success,        // reached InCall
  ServerRejected, // reached Error and start_chat_bot was HTTP 400 (bad agent config)
  Other,          // Error with any other status, or timeout / quit
};

// Watch the controller's snapshot until autostart's very first start attempt
// lands on InCall or Error (bounded ~60 s and by g_quit so SIGTERM is prompt).
FirstStartOutcome wait_first_start(const jusiai::AssistantController& c) {
  using clock = std::chrono::steady_clock;
  const auto deadline = clock::now() + std::chrono::seconds(60);
  while (clock::now() < deadline && !g_quit.load()) {
    const jusiai::UiSnapshot s = c.snapshot();
    if (s.state == jusiai::AssistantState::InCall) {
      return FirstStartOutcome::Success;
    }
    if (s.state == jusiai::AssistantState::Error) {
      // Only HTTP 400 means the persisted agent config itself was rejected
      // (serializer validation). 401/403 (device key / binding) and 429
      // (quota) are not agent-config problems, so quarantining and retrying
      // with defaults would not help — surface those normally instead.
      const int http = c.last_start_http_status();
      return (http == 400) ? FirstStartOutcome::ServerRejected
                           : FirstStartOutcome::Other;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
  }
  return FirstStartOutcome::Other;
}

}  // namespace

int main(int argc, char** argv) {
  using namespace jusiai;

  // --- Configuration ------------------------------------------------------
  bool should_exit = false;
  int exit_code = 0;
  AppConfig config = load_config(argc, argv, should_exit, exit_code);
  if (should_exit) return exit_code;
  set_log_level(config.log_level);

  LOG_INFO("JuSi AI Assistant %s starting", JUSIAI_VERSION);
  LOG_INFO("config: %s", config.summary().c_str());

  configure_tls_bundle();

  // --- LiveKit SDK --------------------------------------------------------
  // Must run before any AudioSource / VideoSource / Room is created.
  livekit::initialize();

  int result = 0;
  {
    AssistantController controller(config);
    if (!controller.init()) {
      LOG_ERROR("failed to initialise the assistant controller");
      result = 1;
    } else {
      // Local HTTP control + status API — the normal way to drive the device.
      std::unique_ptr<ControlServer> control;
      if (config.control_port > 0) {
        control = std::make_unique<ControlServer>(&controller, config);
        if (!control->start()) {
          LOG_ERROR("control API failed to start — the device would be "
                    "uncontrollable");
          result = 1;
        }
      } else {
        LOG_INFO("control API disabled by control_port=%d",
                 config.control_port);
        if (!config.autostart) {
          LOG_WARN("control API is disabled and autostart is off; no local "
                   "start command will be available");
        }
      }

      if (result == 0) {
        install_signal_handlers();  // catch SIGINT/SIGTERM during the guardrail
        if (config.autostart) {
          const bool had_runtime = has_runtime_agent_config();
          LOG_INFO("autostart: beginning the AI call");
          controller.request_start();

          // Self-heal guardrail: a bad value in the persisted runtime config
          // (a voice / prompt_id / profile_code that the backend has removed
          // or the operator typo'd via POST /config) would 400-lock the
          // device on every boot. If the first autostart attempt is rejected
          // by the backend with an HTTP 4xx, quarantine the file and retry
          // once with the config-file / env / CLI defaults. One shot only —
          // further failures are surfaced normally so the operator sees them.
          if (had_runtime &&
              wait_first_start(controller) ==
                  FirstStartOutcome::ServerRejected) {
            LOG_WARN(
                "autostart: backend rejected the persisted runtime agent "
                "config — quarantining and retrying once with defaults");
            if (quarantine_runtime_agent_config()) {
              bool _se = false;
              int _ec = 0;
              AppConfig defaults =
                  load_config(argc, argv, _se, _ec,
                              /*skip_runtime_layer=*/true);
              controller.set_agent_config(defaults);
              controller.request_start();
            }
          }
        }
        wait_for_signal();
      }
      if (control) control->stop();
    }
    controller.shutdown();
  }

  livekit::shutdown();
  LOG_INFO("JuSi AI Assistant stopped");
  return result;
}

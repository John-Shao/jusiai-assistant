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

void wait_for_signal() {
  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);
  LOG_INFO("running — send SIGINT/SIGTERM to quit");
  while (!g_quit.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }
  LOG_INFO("stopping");
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
        if (config.autostart) {
          LOG_INFO("autostart: beginning the AI call");
          controller.request_start();
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

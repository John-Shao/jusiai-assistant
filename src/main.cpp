// JuSi AI Assistant — RV1126B audio/video AI assistant client.
//
// Reproduces the one-tap "AI 助手" flow of the JuSi Meet Android app on the
// Rockchip RV1126B board, built on the LiveKit C++ SDK with an LVGL GUI
// rendered to the Linux framebuffer (V4L2 camera, ALSA audio, no SDL/GPU).
#include <unistd.h>

#include <cstdlib>
#include <string>

#include "app_config.h"
#include "core/assistant_controller.h"
#include "i18n.h"
#include "livekit/livekit.h"
#include "log.h"
#include "ui/ui_app.h"

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

}  // namespace

int main(int argc, char** argv) {
  using namespace jusiai;

  // --- Configuration ------------------------------------------------------
  bool should_exit = false;
  int exit_code = 0;
  AppConfig config = load_config(argc, argv, should_exit, exit_code);
  if (should_exit) return exit_code;
  set_log_level(config.log_level);
  // Select the UI language before anything produces user-facing text.
  set_language(lang_from_string(config.language));

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
      UiApp ui(&controller, config);
      if (!ui.init()) {
        LOG_ERROR("failed to initialise the UI");
        result = 1;
      } else {
        if (config.autostart) {
          LOG_INFO("autostart: beginning the AI call");
          controller.request_start();
        }
        ui.run();  // blocks until SIGINT/SIGTERM
      }
    }
    controller.shutdown();
  }

  livekit::shutdown();
  LOG_INFO("JuSi AI Assistant stopped");
  return result;
}

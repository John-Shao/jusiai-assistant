// JuSi AI Assistant — Linux audio/video AI assistant client.
//
// Reproduces the one-tap "AI 助手" flow of the JuSi Meet Android app on Linux,
// built on the LiveKit C++ SDK with an LVGL (SDL3) GUI.
#include <SDL3/SDL.h>

#include "app_config.h"
#include "core/assistant_controller.h"
#include "livekit/livekit.h"
#include "log.h"
#include "ui/ui_app.h"

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

  // --- SDL ----------------------------------------------------------------
  if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) {
    LOG_ERROR("SDL_Init failed: %s", SDL_GetError());
    return 1;
  }
  if (!SDL_InitSubSystem(SDL_INIT_CAMERA)) {
    LOG_WARN("SDL camera subsystem unavailable: %s", SDL_GetError());
  }

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
        ui.run();  // blocks until the window is closed
      }
    }
    controller.shutdown();
  }

  livekit::shutdown();
  SDL_Quit();
  LOG_INFO("JuSi AI Assistant stopped");
  return result;
}

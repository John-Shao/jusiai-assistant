// SDL3 backend for LVGL v8.4: hosts an SDL window/renderer, registers an LVGL
// display driver (framebuffer -> SDL texture) and a pointer input driver fed
// by SDL mouse and touch events.
//
// All methods must be called from the same (UI) thread; LVGL is not
// thread-safe.
#pragma once

#include <cstdint>
#include <string>

#include "lvgl.h"

struct SDL_Window;
struct SDL_Renderer;
struct SDL_Texture;

namespace jusiai {

class LvSdlDisplay {
 public:
  LvSdlDisplay() = default;
  ~LvSdlDisplay();

  LvSdlDisplay(const LvSdlDisplay&) = delete;
  LvSdlDisplay& operator=(const LvSdlDisplay&) = delete;

  // Create the window and wire up LVGL. SDL_INIT_VIDEO must already be done.
  bool init(const std::string& title, int width, int height, bool fullscreen);

  // Drain SDL events into the input state / quit flag.
  void poll_events();
  // Advance the LVGL tick, run the LVGL task handler and present the frame.
  void render();

  bool quit_requested() const { return quit_; }

  // Actual framebuffer size (may differ from requested when fullscreen).
  int width() const { return width_; }
  int height() const { return height_; }

 private:
  static void flush_cb(lv_disp_drv_t* drv, const lv_area_t* area,
                       lv_color_t* color_p);
  static void input_read_cb(lv_indev_drv_t* drv, lv_indev_data_t* data);

  SDL_Window* window_ = nullptr;
  SDL_Renderer* renderer_ = nullptr;
  SDL_Texture* texture_ = nullptr;

  int width_ = 0;
  int height_ = 0;
  bool quit_ = false;

  // LVGL driver state — must outlive registration.
  lv_disp_draw_buf_t draw_buf_{};
  lv_disp_drv_t disp_drv_{};
  lv_indev_drv_t indev_drv_{};
  lv_color_t* lv_buf_ = nullptr;

  std::uint32_t last_tick_ = 0;

  // Latest pointer state (mouse / touch), read by input_read_cb.
  int ptr_x_ = 0;
  int ptr_y_ = 0;
  bool ptr_pressed_ = false;
};

}  // namespace jusiai

// LVGL backend for RV1126B: renders to the Linux framebuffer (/dev/fb0, the
// rockchipdrmfb device) and feeds a pointer input driver from an evdev
// touchscreen.
//
// All methods must be called from the same (UI) thread; LVGL is not
// thread-safe. Replaces the SDL3 backend used by the desktop build.
#pragma once

#include <atomic>
#include <cstdint>
#include <string>

#include "lvgl.h"

namespace jusiai {

class LvDisplay {
 public:
  LvDisplay() = default;
  ~LvDisplay();

  LvDisplay(const LvDisplay&) = delete;
  LvDisplay& operator=(const LvDisplay&) = delete;

  // Open the framebuffer + touchscreen and wire up LVGL. `title` is accepted
  // for API compatibility with the desktop backend but ignored. `width` /
  // `height` are hints — the real size comes from the panel.
  bool init(const std::string& title, int width, int height, bool fullscreen);

  // Drain evdev touch events into the pointer state; refresh the quit flag.
  void poll_events();
  // Advance the LVGL tick and run the LVGL task handler (flushes to the fb).
  void render();

  bool quit_requested() const;

  // Actual panel size.
  int width() const { return width_; }
  int height() const { return height_; }

 private:
  static void flush_cb(lv_disp_drv_t* drv, const lv_area_t* area,
                       lv_color_t* color_p);
  static void input_read_cb(lv_indev_drv_t* drv, lv_indev_data_t* data);

  bool open_framebuffer();
  bool open_touch();
  void drain_touch();

  // Framebuffer.
  int fb_fd_ = -1;
  std::uint8_t* fb_mem_ = nullptr;
  std::size_t fb_size_ = 0;
  int fb_stride_ = 0;   // bytes per row
  int fb_bpp_ = 32;     // bits per pixel
  int width_ = 0;
  int height_ = 0;

  // Touchscreen (evdev).
  int touch_fd_ = -1;
  int touch_min_x_ = 0, touch_max_x_ = 0;
  int touch_min_y_ = 0, touch_max_y_ = 0;
  int ptr_x_ = 0, ptr_y_ = 0;
  bool ptr_pressed_ = false;
  // Raw accumulators for the in-progress evdev packet.
  int raw_x_ = 0, raw_y_ = 0;

  // LVGL driver state — must outlive registration.
  lv_disp_draw_buf_t draw_buf_{};
  lv_disp_drv_t disp_drv_{};
  lv_indev_drv_t indev_drv_{};
  lv_color_t* lv_buf_ = nullptr;

  std::uint32_t last_tick_ = 0;
};

}  // namespace jusiai

#include "ui/lv_sdl_disp.h"

#include <SDL3/SDL.h>

#include <cstdlib>

#include "log.h"

namespace jusiai {

LvSdlDisplay::~LvSdlDisplay() {
  if (texture_) SDL_DestroyTexture(texture_);
  if (renderer_) SDL_DestroyRenderer(renderer_);
  if (window_) SDL_DestroyWindow(window_);
  std::free(lv_buf_);
}

bool LvSdlDisplay::init(const std::string& title, int width, int height,
                        bool fullscreen) {
  SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE;
  if (fullscreen) flags |= SDL_WINDOW_FULLSCREEN;

  window_ = SDL_CreateWindow(title.c_str(), width, height, flags);
  if (!window_) {
    LOG_ERROR("ui: SDL_CreateWindow failed: %s", SDL_GetError());
    return false;
  }

  renderer_ = SDL_CreateRenderer(window_, nullptr);
  if (!renderer_) {
    LOG_ERROR("ui: SDL_CreateRenderer failed: %s", SDL_GetError());
    return false;
  }

  // Use the real surface size — fullscreen may not honour the request.
  SDL_GetWindowSize(window_, &width_, &height_);
  if (width_ <= 0 || height_ <= 0) {
    width_ = width;
    height_ = height;
  }

  texture_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_ARGB8888,
                               SDL_TEXTUREACCESS_STREAMING, width_, height_);
  if (!texture_) {
    LOG_ERROR("ui: SDL_CreateTexture failed: %s", SDL_GetError());
    return false;
  }

  lv_init();

  // Partial-render buffer (~1/8 of the screen).
  const std::size_t buf_px = static_cast<std::size_t>(width_) * height_ / 8;
  lv_buf_ = static_cast<lv_color_t*>(std::malloc(buf_px * sizeof(lv_color_t)));
  if (!lv_buf_) {
    LOG_ERROR("ui: failed to allocate the LVGL draw buffer");
    return false;
  }
  lv_disp_draw_buf_init(&draw_buf_, lv_buf_, nullptr, buf_px);

  lv_disp_drv_init(&disp_drv_);
  disp_drv_.hor_res = static_cast<lv_coord_t>(width_);
  disp_drv_.ver_res = static_cast<lv_coord_t>(height_);
  disp_drv_.flush_cb = &LvSdlDisplay::flush_cb;
  disp_drv_.draw_buf = &draw_buf_;
  disp_drv_.user_data = this;
  lv_disp_drv_register(&disp_drv_);

  lv_indev_drv_init(&indev_drv_);
  indev_drv_.type = LV_INDEV_TYPE_POINTER;
  indev_drv_.read_cb = &LvSdlDisplay::input_read_cb;
  indev_drv_.user_data = this;
  lv_indev_drv_register(&indev_drv_);

  last_tick_ = static_cast<std::uint32_t>(SDL_GetTicks());
  LOG_INFO("ui: window ready (%dx%d%s)", width_, height_,
           fullscreen ? ", fullscreen" : "");
  return true;
}

void LvSdlDisplay::poll_events() {
  SDL_Event e;
  while (SDL_PollEvent(&e)) {
    switch (e.type) {
      case SDL_EVENT_QUIT:
        quit_ = true;
        break;
      case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        quit_ = true;
        break;
      case SDL_EVENT_MOUSE_MOTION:
        ptr_x_ = static_cast<int>(e.motion.x);
        ptr_y_ = static_cast<int>(e.motion.y);
        break;
      case SDL_EVENT_MOUSE_BUTTON_DOWN:
        if (e.button.button == SDL_BUTTON_LEFT) {
          ptr_x_ = static_cast<int>(e.button.x);
          ptr_y_ = static_cast<int>(e.button.y);
          ptr_pressed_ = true;
        }
        break;
      case SDL_EVENT_MOUSE_BUTTON_UP:
        if (e.button.button == SDL_BUTTON_LEFT) ptr_pressed_ = false;
        break;
      case SDL_EVENT_FINGER_DOWN:
      case SDL_EVENT_FINGER_MOTION:
        ptr_x_ = static_cast<int>(e.tfinger.x * width_);
        ptr_y_ = static_cast<int>(e.tfinger.y * height_);
        ptr_pressed_ = true;
        break;
      case SDL_EVENT_FINGER_UP:
        ptr_pressed_ = false;
        break;
      default:
        break;
    }
  }
}

void LvSdlDisplay::render() {
  const std::uint32_t now = static_cast<std::uint32_t>(SDL_GetTicks());
  lv_tick_inc(now - last_tick_);
  last_tick_ = now;

  lv_timer_handler();

  SDL_RenderClear(renderer_);
  SDL_RenderTexture(renderer_, texture_, nullptr, nullptr);
  SDL_RenderPresent(renderer_);
}

void LvSdlDisplay::flush_cb(lv_disp_drv_t* drv, const lv_area_t* area,
                            lv_color_t* color_p) {
  auto* self = static_cast<LvSdlDisplay*>(drv->user_data);
  const int w = lv_area_get_width(area);
  const int h = lv_area_get_height(area);

  SDL_Rect rect{area->x1, area->y1, w, h};
  SDL_UpdateTexture(self->texture_, &rect, color_p,
                    w * static_cast<int>(sizeof(lv_color_t)));

  lv_disp_flush_ready(drv);
}

void LvSdlDisplay::input_read_cb(lv_indev_drv_t* drv, lv_indev_data_t* data) {
  auto* self = static_cast<LvSdlDisplay*>(drv->user_data);
  data->point.x = static_cast<lv_coord_t>(self->ptr_x_);
  data->point.y = static_cast<lv_coord_t>(self->ptr_y_);
  data->state =
      self->ptr_pressed_ ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

}  // namespace jusiai

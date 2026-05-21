#include "ui/lv_display.h"

#include <dirent.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "log.h"

namespace jusiai {
namespace {

// Set by SIGINT / SIGTERM — the only way to quit on a headless board.
std::atomic<bool> g_quit_flag{false};
void on_signal(int) { g_quit_flag.store(true); }

std::uint32_t now_ms() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<std::uint32_t>(ts.tv_sec * 1000ULL + ts.tv_nsec / 1000000ULL);
}

bool test_bit(const unsigned long* arr, int bit) {
  return (arr[bit / (8 * sizeof(long))] >> (bit % (8 * sizeof(long)))) & 1UL;
}

}  // namespace

LvDisplay::~LvDisplay() {
  if (touch_fd_ >= 0) ::close(touch_fd_);
  if (fb_mem_ && fb_mem_ != MAP_FAILED) munmap(fb_mem_, fb_size_);
  if (fb_fd_ >= 0) ::close(fb_fd_);
  std::free(lv_buf_);
}

// --- Framebuffer ---------------------------------------------------------

bool LvDisplay::open_framebuffer() {
  const char* env = std::getenv("JUSIAI_FB");
  const char* dev = (env && *env) ? env : "/dev/fb0";

  fb_fd_ = ::open(dev, O_RDWR | O_CLOEXEC);
  if (fb_fd_ < 0) {
    LOG_ERROR("fb: open(%s) failed: %s", dev, std::strerror(errno));
    return false;
  }

  fb_var_screeninfo vinfo{};
  fb_fix_screeninfo finfo{};
  if (ioctl(fb_fd_, FBIOGET_VSCREENINFO, &vinfo) < 0 ||
      ioctl(fb_fd_, FBIOGET_FSCREENINFO, &finfo) < 0) {
    LOG_ERROR("fb: FBIOGET_*SCREENINFO failed: %s", std::strerror(errno));
    return false;
  }

  width_ = static_cast<int>(vinfo.xres);
  height_ = static_cast<int>(vinfo.yres);
  fb_bpp_ = static_cast<int>(vinfo.bits_per_pixel);
  fb_stride_ = static_cast<int>(finfo.line_length);
  fb_size_ = finfo.smem_len ? finfo.smem_len
                            : static_cast<std::size_t>(fb_stride_) * height_;

  if (fb_bpp_ != 32 && fb_bpp_ != 16) {
    LOG_ERROR("fb: unsupported %d bpp (need 16 or 32)", fb_bpp_);
    return false;
  }

  fb_mem_ = static_cast<std::uint8_t*>(
      mmap(nullptr, fb_size_, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd_, 0));
  if (fb_mem_ == MAP_FAILED) {
    LOG_ERROR("fb: mmap failed: %s", std::strerror(errno));
    fb_mem_ = nullptr;
    return false;
  }
  std::memset(fb_mem_, 0, fb_size_);
  LOG_INFO("fb: %s %dx%d %dbpp stride=%d", dev, width_, height_, fb_bpp_,
           fb_stride_);
  return true;
}

// --- Touchscreen (evdev) -------------------------------------------------

bool LvDisplay::open_touch() {
  const char* env = std::getenv("JUSIAI_TOUCH");
  std::string chosen = (env && *env) ? env : "";

  if (chosen.empty()) {
    // Scan /dev/input/event* for the first device exposing absolute axes.
    for (int i = 0; i < 32; ++i) {
      std::string path = "/dev/input/event" + std::to_string(i);
      int fd = ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
      if (fd < 0) continue;
      unsigned long absbits[(ABS_MAX / (8 * sizeof(long))) + 1] = {0};
      bool is_touch = false;
      if (ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(absbits)), absbits) >= 0) {
        is_touch = test_bit(absbits, ABS_MT_POSITION_X) ||
                   test_bit(absbits, ABS_X);
      }
      ::close(fd);
      if (is_touch) {
        chosen = path;
        break;
      }
    }
  }
  if (chosen.empty()) {
    LOG_WARN("touch: no touchscreen found — pointer input disabled");
    return false;
  }

  touch_fd_ = ::open(chosen.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
  if (touch_fd_ < 0) {
    LOG_WARN("touch: open(%s) failed: %s", chosen.c_str(), std::strerror(errno));
    return false;
  }

  input_absinfo ai{};
  if (ioctl(touch_fd_, EVIOCGABS(ABS_MT_POSITION_X), &ai) >= 0 &&
      ai.maximum > ai.minimum) {
    touch_min_x_ = ai.minimum;
    touch_max_x_ = ai.maximum;
  } else if (ioctl(touch_fd_, EVIOCGABS(ABS_X), &ai) >= 0 &&
             ai.maximum > ai.minimum) {
    touch_min_x_ = ai.minimum;
    touch_max_x_ = ai.maximum;
  }
  if (ioctl(touch_fd_, EVIOCGABS(ABS_MT_POSITION_Y), &ai) >= 0 &&
      ai.maximum > ai.minimum) {
    touch_min_y_ = ai.minimum;
    touch_max_y_ = ai.maximum;
  } else if (ioctl(touch_fd_, EVIOCGABS(ABS_Y), &ai) >= 0 &&
             ai.maximum > ai.minimum) {
    touch_min_y_ = ai.minimum;
    touch_max_y_ = ai.maximum;
  }
  LOG_INFO("touch: %s x[%d,%d] y[%d,%d]", chosen.c_str(), touch_min_x_,
           touch_max_x_, touch_min_y_, touch_max_y_);
  return true;
}

void LvDisplay::drain_touch() {
  if (touch_fd_ < 0) return;
  input_event ev;
  while (::read(touch_fd_, &ev, sizeof(ev)) == static_cast<ssize_t>(sizeof(ev))) {
    if (ev.type == EV_ABS) {
      switch (ev.code) {
        case ABS_X:
        case ABS_MT_POSITION_X: raw_x_ = ev.value; break;
        case ABS_Y:
        case ABS_MT_POSITION_Y: raw_y_ = ev.value; break;
        case ABS_MT_TRACKING_ID: ptr_pressed_ = (ev.value != -1); break;
        default: break;
      }
    } else if (ev.type == EV_KEY && ev.code == BTN_TOUCH) {
      ptr_pressed_ = (ev.value != 0);
    } else if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
      // Commit: scale raw axes to screen coordinates.
      int x = raw_x_, y = raw_y_;
      if (touch_max_x_ > touch_min_x_) {
        x = (raw_x_ - touch_min_x_) * (width_ - 1) /
            (touch_max_x_ - touch_min_x_);
      }
      if (touch_max_y_ > touch_min_y_) {
        y = (raw_y_ - touch_min_y_) * (height_ - 1) /
            (touch_max_y_ - touch_min_y_);
      }
      ptr_x_ = x < 0 ? 0 : (x >= width_ ? width_ - 1 : x);
      ptr_y_ = y < 0 ? 0 : (y >= height_ ? height_ - 1 : y);
    }
  }
}

// --- Lifecycle -----------------------------------------------------------

bool LvDisplay::init(const std::string& /*title*/, int /*width*/,
                     int /*height*/, bool /*fullscreen*/) {
  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);

  if (!open_framebuffer()) return false;
  open_touch();  // optional — app still usable without touch

  lv_init();

  const std::size_t buf_px = static_cast<std::size_t>(width_) * height_ / 10;
  lv_buf_ = static_cast<lv_color_t*>(std::malloc(buf_px * sizeof(lv_color_t)));
  if (!lv_buf_) {
    LOG_ERROR("ui: failed to allocate the LVGL draw buffer");
    return false;
  }
  lv_disp_draw_buf_init(&draw_buf_, lv_buf_, nullptr, buf_px);

  lv_disp_drv_init(&disp_drv_);
  disp_drv_.hor_res = static_cast<lv_coord_t>(width_);
  disp_drv_.ver_res = static_cast<lv_coord_t>(height_);
  disp_drv_.flush_cb = &LvDisplay::flush_cb;
  disp_drv_.draw_buf = &draw_buf_;
  disp_drv_.user_data = this;
  lv_disp_drv_register(&disp_drv_);

  lv_indev_drv_init(&indev_drv_);
  indev_drv_.type = LV_INDEV_TYPE_POINTER;
  indev_drv_.read_cb = &LvDisplay::input_read_cb;
  indev_drv_.user_data = this;
  lv_indev_drv_register(&indev_drv_);

  last_tick_ = now_ms();
  LOG_INFO("ui: framebuffer display ready (%dx%d)", width_, height_);
  return true;
}

void LvDisplay::poll_events() { drain_touch(); }

void LvDisplay::render() {
  const std::uint32_t t = now_ms();
  lv_tick_inc(t - last_tick_);
  last_tick_ = t;
  lv_timer_handler();
}

bool LvDisplay::quit_requested() const { return g_quit_flag.load(); }

// --- LVGL callbacks ------------------------------------------------------

void LvDisplay::flush_cb(lv_disp_drv_t* drv, const lv_area_t* area,
                         lv_color_t* color_p) {
  auto* self = static_cast<LvDisplay*>(drv->user_data);
  const int x1 = area->x1, y1 = area->y1;
  const int aw = lv_area_get_width(area);
  const int ah = lv_area_get_height(area);

  if (self->fb_bpp_ == 32) {
    for (int row = 0; row < ah; ++row) {
      std::uint8_t* dst =
          self->fb_mem_ + static_cast<std::size_t>(y1 + row) * self->fb_stride_ +
          static_cast<std::size_t>(x1) * 4;
      std::memcpy(dst, color_p + static_cast<std::size_t>(row) * aw,
                  static_cast<std::size_t>(aw) * 4);
    }
  } else {  // 16 bpp RGB565
    for (int row = 0; row < ah; ++row) {
      auto* dst = reinterpret_cast<std::uint16_t*>(
          self->fb_mem_ + static_cast<std::size_t>(y1 + row) * self->fb_stride_ +
          static_cast<std::size_t>(x1) * 2);
      const lv_color_t* src = color_p + static_cast<std::size_t>(row) * aw;
      for (int col = 0; col < aw; ++col) {
        const lv_color_t c = src[col];
        dst[col] = static_cast<std::uint16_t>(
            ((c.ch.red >> 3) << 11) | ((c.ch.green >> 2) << 5) |
            (c.ch.blue >> 3));
      }
    }
  }
  lv_disp_flush_ready(drv);
}

void LvDisplay::input_read_cb(lv_indev_drv_t* drv, lv_indev_data_t* data) {
  auto* self = static_cast<LvDisplay*>(drv->user_data);
  data->point.x = static_cast<lv_coord_t>(self->ptr_x_);
  data->point.y = static_cast<lv_coord_t>(self->ptr_y_);
  data->state =
      self->ptr_pressed_ ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

}  // namespace jusiai

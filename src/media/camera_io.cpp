#include "media/camera_io.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <exception>

#include "livekit/video_frame.h"
#include "log.h"
#include "media/frame_buffer.h"
#include "sdl_media.h"

namespace jusiai {
namespace {

constexpr SDL_PixelFormat kRgba = SDL_PIXELFORMAT_RGBA32;

// Convert an arbitrary camera surface format to tightly packed RGBA.
bool convert_to_rgba(const std::uint8_t* pixels, int pitch, int width,
                     int height, SDL_PixelFormat format,
                     std::vector<std::uint8_t>& out) {
  const std::size_t packed = static_cast<std::size_t>(width) * height * 4;

  if (format == kRgba && pitch == width * 4) {
    out.assign(pixels, pixels + packed);
    return true;
  }

  SDL_Surface* src = SDL_CreateSurfaceFrom(
      width, height, format, const_cast<std::uint8_t*>(pixels), pitch);
  if (!src) {
    LOG_WARN("camera: SDL_CreateSurfaceFrom failed: %s", SDL_GetError());
    return false;
  }
  SDL_Surface* dst = SDL_ConvertSurface(src, kRgba);
  SDL_DestroySurface(src);
  if (!dst) {
    LOG_WARN("camera: SDL_ConvertSurface failed: %s", SDL_GetError());
    return false;
  }

  out.resize(packed);
  for (int y = 0; y < height; ++y) {
    std::memcpy(out.data() + static_cast<std::size_t>(y) * width * 4,
                static_cast<std::uint8_t*>(dst->pixels) + y * dst->pitch,
                static_cast<std::size_t>(width) * 4);
  }
  SDL_DestroySurface(dst);
  return true;
}

// Nearest-neighbour scale of a packed RGBA image.
void scale_rgba(const std::vector<std::uint8_t>& src, int sw, int sh,
                std::vector<std::uint8_t>& dst, int dw, int dh) {
  dst.resize(static_cast<std::size_t>(dw) * dh * 4);
  for (int y = 0; y < dh; ++y) {
    const int sy = std::min(sh - 1, y * sh / dh);
    const std::uint8_t* srow = src.data() + static_cast<std::size_t>(sy) * sw * 4;
    std::uint8_t* drow = dst.data() + static_cast<std::size_t>(y) * dw * 4;
    for (int x = 0; x < dw; ++x) {
      const int sx = std::min(sw - 1, x * sw / dw);
      std::memcpy(drow + x * 4, srow + sx * 4, 4);
    }
  }
}

// Animated placeholder shown when no camera is available.
void make_synthetic_frame(int width, int height, std::uint64_t frame,
                          std::vector<std::uint8_t>& out) {
  out.resize(static_cast<std::size_t>(width) * height * 4);
  const int bar =
      static_cast<int>((frame * 5) % static_cast<std::uint64_t>(height));
  for (int y = 0; y < height; ++y) {
    const int dist = std::abs(y - bar);
    const int glow = dist < 24 ? (24 - dist) * 4 : 0;
    for (int x = 0; x < width; ++x) {
      std::uint8_t* p =
          out.data() + (static_cast<std::size_t>(y) * width + x) * 4;
      const int shade = 24 + (x * 36) / width;
      p[0] = static_cast<std::uint8_t>(std::min(255, 16 + glow));
      p[1] = static_cast<std::uint8_t>(std::min(255, shade + glow));
      p[2] = static_cast<std::uint8_t>(std::min(255, shade + 24 + glow));
      p[3] = 255;
    }
  }
}

}  // namespace

CameraEngine::CameraEngine(int width, int height, int fps, FrameBuffer* preview)
    : width_(width > 0 ? width : 640),
      height_(height > 0 ? height : 480),
      fps_(fps > 0 ? fps : 30),
      preview_(preview) {}

CameraEngine::~CameraEngine() { stop(); }

bool CameraEngine::start() {
  if (running_.load()) return true;

  try {
    video_source_ = std::make_shared<livekit::VideoSource>(width_, height_);
  } catch (const std::exception& e) {
    LOG_ERROR("camera: VideoSource creation failed: %s", e.what());
    return false;
  }

  running_.store(true);
  capture_thread_ = std::thread(&CameraEngine::capture_loop, this);
  return true;
}

void CameraEngine::stop() {
  if (running_.exchange(false)) {
    if (capture_thread_.joinable()) capture_thread_.join();
  }
  camera_.reset();
  video_source_.reset();
}

void CameraEngine::capture_loop() {
  std::vector<std::uint8_t> scaled;  // reused frame scratch buffer

  camera_ = std::make_unique<SDLCamSource>(
      width_, height_, fps_, kRgba,
      [&](const std::uint8_t* px, int pitch, int w, int h, SDL_PixelFormat fmt,
          Uint64 ts_ns) {
        std::vector<std::uint8_t> rgba;
        if (!convert_to_rgba(px, pitch, w, h, fmt, rgba)) return;
        if (w != width_ || h != height_) {
          scale_rgba(rgba, w, h, scaled, width_, height_);
          deliver(width_, height_, scaled,
                  static_cast<std::int64_t>(ts_ns / 1000));
        } else {
          deliver(width_, height_, rgba,
                  static_cast<std::int64_t>(ts_ns / 1000));
        }
      });

  if (camera_->init()) {
    has_camera_.store(true);
    LOG_INFO("camera: opened (%dx%d @%dfps)", width_, height_, fps_);
  } else {
    has_camera_.store(false);
    camera_.reset();
    LOG_WARN("camera: no device available — using synthetic frames");
  }

  const auto frame_interval = std::chrono::milliseconds(1000 / fps_);
  std::uint64_t synth_frame = 0;
  auto next_synth = std::chrono::steady_clock::now();
  std::vector<std::uint8_t> synth;

  while (running_.load()) {
    if (camera_) {
      camera_->pump();
      std::this_thread::sleep_for(std::chrono::milliseconds(4));
    } else {
      make_synthetic_frame(width_, height_, synth_frame, synth);
      deliver(width_, height_, synth,
              static_cast<std::int64_t>(synth_frame) * 1000000 / fps_);
      ++synth_frame;
      next_synth += frame_interval;
      std::this_thread::sleep_until(next_synth);
    }
  }
}

void CameraEngine::deliver(int width, int height,
                           const std::vector<std::uint8_t>& rgba,
                           std::int64_t timestamp_us) {
  if (rgba.size() != static_cast<std::size_t>(width) * height * 4) return;

  if (preview_) {
    preview_->publish(width, height, rgba.data(), rgba.size());
  }

  if (!publishing_.load() || !video_source_) return;

  try {
    livekit::VideoFrame frame(width, height, livekit::VideoBufferType::RGBA,
                              rgba);
    video_source_->captureFrame(frame, timestamp_us);
  } catch (const std::exception& e) {
    LOG_DEBUG("camera: captureFrame dropped a frame: %s", e.what());
  }
}

}  // namespace jusiai

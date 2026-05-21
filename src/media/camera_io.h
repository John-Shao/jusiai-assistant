// Camera engine: captures webcam frames, mirrors them to the UI preview and
// (while publishing is enabled) into a livekit::VideoSource.
//
// Capture runs on its own thread. Every frame is normalised to a fixed
// resolution so the VideoSource can be created eagerly and reused across
// conversations. If no camera is present the engine falls back to a synthetic
// animated frame so the preview and the published track still work.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#include "livekit/video_source.h"

class SDLCamSource;

namespace jusiai {

class FrameBuffer;

class CameraEngine {
 public:
  CameraEngine(int width, int height, int fps, FrameBuffer* preview);
  ~CameraEngine();

  CameraEngine(const CameraEngine&) = delete;
  CameraEngine& operator=(const CameraEngine&) = delete;

  // Open the camera and start streaming preview frames. The VideoSource is
  // created here and stays valid until the engine is destroyed.
  bool start();
  void stop();

  // VideoSource for the local camera track. Valid after a successful start().
  std::shared_ptr<livekit::VideoSource> video_source() const {
    return video_source_;
  }

  // Toggle whether captured frames are pushed into the VideoSource.
  void set_publishing(bool on) { publishing_.store(on); }

  bool has_camera() const { return has_camera_.load(); }

  int frame_width() const { return width_; }
  int frame_height() const { return height_; }

 private:
  void capture_loop();
  void deliver(int width, int height, const std::vector<std::uint8_t>& rgba,
               std::int64_t timestamp_us);

  const int width_;   // fixed output resolution every frame is scaled to
  const int height_;
  const int fps_;
  FrameBuffer* preview_;  // not owned

  std::shared_ptr<livekit::VideoSource> video_source_;
  std::unique_ptr<SDLCamSource> camera_;
  std::thread capture_thread_;
  std::atomic<bool> running_{false};
  std::atomic<bool> has_camera_{false};
  std::atomic<bool> publishing_{false};
};

}  // namespace jusiai

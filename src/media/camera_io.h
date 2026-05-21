// Camera engine (RV1126B): captures webcam frames via V4L2, mirrors them to
// the UI preview and (while publishing is enabled) into a livekit::VideoSource.
//
// Capture runs on its own thread. The RV1126B camera sensor is mounted with a
// physical rotation, so every frame is rotated by `rotation` degrees and
// normalised to a fixed output resolution. If the camera cannot be opened the
// engine falls back to a synthetic animated frame.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "livekit/video_source.h"

namespace jusiai {

class FrameBuffer;
class V4l2Capture;  // defined in camera_io.cpp

class CameraEngine {
 public:
  // `width`/`height` are the OUTPUT (post-rotation) resolution. `rotation` is
  // 0/90/180/270 degrees clockwise applied to the captured sensor frame.
  CameraEngine(int width, int height, int fps, int rotation,
               std::string device, FrameBuffer* preview);
  ~CameraEngine();

  CameraEngine(const CameraEngine&) = delete;
  CameraEngine& operator=(const CameraEngine&) = delete;

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
  void deliver(const std::vector<std::uint8_t>& rgba, std::int64_t timestamp_us);

  const int width_;      // output width (post-rotation)
  const int height_;     // output height (post-rotation)
  const int fps_;
  const int rotation_;   // 0 / 90 / 180 / 270
  const std::string device_;
  FrameBuffer* preview_;  // not owned

  std::shared_ptr<livekit::VideoSource> video_source_;
  std::unique_ptr<V4l2Capture> camera_;
  std::thread capture_thread_;
  std::atomic<bool> running_{false};
  std::atomic<bool> has_camera_{false};
  std::atomic<bool> publishing_{false};
};

}  // namespace jusiai

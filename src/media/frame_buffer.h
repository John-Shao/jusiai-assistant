// Thread-safe single-slot buffer for the latest camera frame.
//
// The camera thread publishes RGBA frames; the UI thread polls for a newer
// one each render tick. A monotonically increasing version lets the consumer
// skip a copy when nothing changed.
#pragma once

#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

namespace jusiai {

class FrameBuffer {
 public:
  // Store a copy of an RGBA (8-8-8-8) frame. Called from the camera thread.
  void publish(int width, int height, const std::uint8_t* rgba,
               std::size_t size) {
    std::lock_guard<std::mutex> lock(mutex_);
    data_.assign(rgba, rgba + size);
    width_ = width;
    height_ = height;
    ++version_;
  }

  // Copy out the latest frame if it is newer than `seen_version`. On success
  // updates `seen_version` and returns true. Called from the UI thread.
  bool fetch(std::uint64_t& seen_version, std::vector<std::uint8_t>& out,
             int& width, int& height) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (version_ == seen_version || data_.empty()) return false;
    out = data_;
    width = width_;
    height = height_;
    seen_version = version_;
    return true;
  }

  bool has_frame() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return version_ != 0;
  }

 private:
  mutable std::mutex mutex_;
  std::vector<std::uint8_t> data_;
  int width_ = 0;
  int height_ = 0;
  std::uint64_t version_ = 0;
};

}  // namespace jusiai

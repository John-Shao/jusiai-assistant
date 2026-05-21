#include "media/camera_io.h"

#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <exception>

#include "livekit/video_frame.h"
#include "log.h"
#include "media/frame_buffer.h"

namespace jusiai {

// ===========================================================================
// V4l2Capture — multi-planar NV12 capture for /dev/video-camera0 (rkisp on
// RV1126B). Adapted from the SDK board_loopback example.
// ===========================================================================
class V4l2Capture {
 public:
  ~V4l2Capture() { close(); }

  bool open(const std::string& device, int width, int height, int fps) {
    fd_ = ::open(device.c_str(), O_RDWR | O_CLOEXEC);
    if (fd_ < 0) {
      LOG_WARN("v4l2: open(%s) failed: %s", device.c_str(), std::strerror(errno));
      return false;
    }

    v4l2_capability cap{};
    if (ioctl(fd_, VIDIOC_QUERYCAP, &cap) < 0) {
      LOG_WARN("v4l2: QUERYCAP failed: %s", std::strerror(errno));
      return false;
    }
    if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE_MPLANE)) {
      LOG_WARN("v4l2: device is not multi-planar capture");
      return false;
    }

    v4l2_format fmt{};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    fmt.fmt.pix_mp.width = static_cast<__u32>(width);
    fmt.fmt.pix_mp.height = static_cast<__u32>(height);
    fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
    fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
    fmt.fmt.pix_mp.num_planes = 1;
    if (ioctl(fd_, VIDIOC_S_FMT, &fmt) < 0) {
      LOG_WARN("v4l2: S_FMT failed: %s", std::strerror(errno));
      return false;
    }
    width_ = static_cast<int>(fmt.fmt.pix_mp.width);
    height_ = static_cast<int>(fmt.fmt.pix_mp.height);

    v4l2_streamparm parm{};
    parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    parm.parm.capture.timeperframe.numerator = 1;
    parm.parm.capture.timeperframe.denominator = static_cast<__u32>(fps);
    ioctl(fd_, VIDIOC_S_PARM, &parm);  // best effort

    LOG_INFO("v4l2: negotiated %dx%d NV12", width_, height_);
    return true;
  }

  bool start_stream(int buf_count = 4) {
    v4l2_requestbuffers reqbuf{};
    reqbuf.count = static_cast<__u32>(buf_count);
    reqbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    reqbuf.memory = V4L2_MEMORY_MMAP;
    if (ioctl(fd_, VIDIOC_REQBUFS, &reqbuf) < 0) {
      LOG_WARN("v4l2: REQBUFS failed: %s", std::strerror(errno));
      return false;
    }

    buffers_.resize(reqbuf.count);
    for (std::uint32_t i = 0; i < reqbuf.count; ++i) {
      v4l2_buffer buf{};
      v4l2_plane planes[1]{};
      buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
      buf.memory = V4L2_MEMORY_MMAP;
      buf.index = i;
      buf.length = 1;
      buf.m.planes = planes;
      if (ioctl(fd_, VIDIOC_QUERYBUF, &buf) < 0) {
        LOG_WARN("v4l2: QUERYBUF %u failed: %s", i, std::strerror(errno));
        return false;
      }
      buffers_[i].length = planes[0].length;
      buffers_[i].start = mmap(nullptr, planes[0].length, PROT_READ | PROT_WRITE,
                               MAP_SHARED, fd_, planes[0].m.mem_offset);
      if (buffers_[i].start == MAP_FAILED) {
        LOG_WARN("v4l2: mmap %u failed: %s", i, std::strerror(errno));
        return false;
      }
      v4l2_buffer qbuf{};
      v4l2_plane qplanes[1]{};
      qbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
      qbuf.memory = V4L2_MEMORY_MMAP;
      qbuf.index = i;
      qbuf.length = 1;
      qbuf.m.planes = qplanes;
      if (ioctl(fd_, VIDIOC_QBUF, &qbuf) < 0) {
        LOG_WARN("v4l2: initial QBUF %u failed: %s", i, std::strerror(errno));
        return false;
      }
    }

    int t = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (ioctl(fd_, VIDIOC_STREAMON, &t) < 0) {
      LOG_WARN("v4l2: STREAMON failed: %s", std::strerror(errno));
      return false;
    }
    streaming_ = true;
    LOG_INFO("v4l2: streaming, %zu buffers", buffers_.size());
    return true;
  }

  // Copy the next NV12 frame into `out`. Returns false on timeout/error.
  bool dequeue(std::vector<std::uint8_t>& out, int timeout_ms = 1000) {
    pollfd pfd{fd_, POLLIN, 0};
    if (poll(&pfd, 1, timeout_ms) <= 0) return false;

    v4l2_buffer buf{};
    v4l2_plane planes[1]{};
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.length = 1;
    buf.m.planes = planes;
    if (ioctl(fd_, VIDIOC_DQBUF, &buf) < 0) return false;

    const std::size_t used =
        std::min<std::size_t>(planes[0].bytesused, buffers_[buf.index].length);
    out.assign(static_cast<std::uint8_t*>(buffers_[buf.index].start),
               static_cast<std::uint8_t*>(buffers_[buf.index].start) + used);

    v4l2_buffer qbuf{};
    v4l2_plane qplanes[1]{};
    qbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    qbuf.memory = V4L2_MEMORY_MMAP;
    qbuf.index = buf.index;
    qbuf.length = 1;
    qbuf.m.planes = qplanes;
    ioctl(fd_, VIDIOC_QBUF, &qbuf);
    return true;
  }

  void close() {
    if (fd_ >= 0) {
      if (streaming_) {
        int t = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        ioctl(fd_, VIDIOC_STREAMOFF, &t);
      }
      for (auto& b : buffers_) {
        if (b.start && b.start != MAP_FAILED) munmap(b.start, b.length);
      }
      buffers_.clear();
      ::close(fd_);
      fd_ = -1;
    }
  }

  int width() const { return width_; }
  int height() const { return height_; }

 private:
  struct Buf {
    void* start = nullptr;
    std::size_t length = 0;
  };
  int fd_ = -1;
  int width_ = 0;
  int height_ = 0;
  bool streaming_ = false;
  std::vector<Buf> buffers_;
};

namespace {

// Clamp helper for the YUV->RGB matrix.
inline std::uint8_t clamp_u8(int v) {
  return static_cast<std::uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
}

// Convert an NV12 frame to RGBA, applying clockwise `rotation` and scaling to
// the fixed output size in a single pass (output pixel -> source pixel).
void nv12_to_rgba(const std::uint8_t* nv12, int cap_w, int cap_h, int rotation,
                  int out_w, int out_h, std::vector<std::uint8_t>& out) {
  out.resize(static_cast<std::size_t>(out_w) * out_h * 4);
  if (cap_w <= 0 || cap_h <= 0) return;

  const std::uint8_t* y_plane = nv12;
  const std::uint8_t* uv_plane = nv12 + static_cast<std::size_t>(cap_w) * cap_h;

  // Dimensions of the rotated source space (before scaling to out_w/out_h).
  const bool swap = (rotation == 90 || rotation == 270);
  const int rs_w = swap ? cap_h : cap_w;
  const int rs_h = swap ? cap_w : cap_h;

  for (int oy = 0; oy < out_h; ++oy) {
    for (int ox = 0; ox < out_w; ++ox) {
      // Scale output -> rotated-source space.
      int rx = ox * rs_w / out_w;
      int ry = oy * rs_h / out_h;
      if (rx >= rs_w) rx = rs_w - 1;
      if (ry >= rs_h) ry = rs_h - 1;

      // Un-rotate to capture-space pixel.
      int sx, sy;
      switch (rotation) {
        case 90:  sx = ry;              sy = cap_h - 1 - rx; break;
        case 180: sx = cap_w - 1 - rx;  sy = cap_h - 1 - ry; break;
        case 270: sx = cap_w - 1 - ry;  sy = rx;             break;
        default:  sx = rx;              sy = ry;             break;
      }
      if (sx < 0) sx = 0; else if (sx >= cap_w) sx = cap_w - 1;
      if (sy < 0) sy = 0; else if (sy >= cap_h) sy = cap_h - 1;

      const int yy = y_plane[sy * cap_w + sx];
      const std::size_t uv = static_cast<std::size_t>(sy / 2) * cap_w +
                             (sx & ~1);
      const int uu = uv_plane[uv] - 128;
      const int vv = uv_plane[uv + 1] - 128;

      const int c = yy;
      std::uint8_t* p = out.data() + (static_cast<std::size_t>(oy) * out_w + ox) * 4;
      p[0] = clamp_u8(c + ((91881 * vv) >> 16));                       // R
      p[1] = clamp_u8(c - ((22554 * uu + 46802 * vv) >> 16));          // G
      p[2] = clamp_u8(c + ((116130 * uu) >> 16));                      // B
      p[3] = 255;                                                     // A
    }
  }
}

// Animated placeholder shown when no camera is available.
void make_synthetic_frame(int w, int h, std::uint64_t frame,
                          std::vector<std::uint8_t>& out) {
  out.resize(static_cast<std::size_t>(w) * h * 4);
  const int bar = static_cast<int>((frame * 5) % static_cast<std::uint64_t>(h));
  for (int y = 0; y < h; ++y) {
    const int dist = std::abs(y - bar);
    const int glow = dist < 28 ? (28 - dist) * 4 : 0;
    for (int x = 0; x < w; ++x) {
      std::uint8_t* p = out.data() + (static_cast<std::size_t>(y) * w + x) * 4;
      const int shade = 22 + (x * 30) / w;
      p[0] = clamp_u8(14 + glow);
      p[1] = clamp_u8(shade + glow);
      p[2] = clamp_u8(shade + 22 + glow);
      p[3] = 255;
    }
  }
}

}  // namespace

CameraEngine::CameraEngine(int width, int height, int fps, int rotation,
                           std::string device, FrameBuffer* preview)
    : width_(width > 0 ? width : 480),
      height_(height > 0 ? height : 640),
      fps_(fps > 0 ? fps : 30),
      rotation_(((rotation % 360) + 360) % 360),
      device_(std::move(device)),
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
  // Capture resolution: a 90/270 rotation swaps the axes.
  const bool swap = (rotation_ == 90 || rotation_ == 270);
  const int cap_w = swap ? height_ : width_;
  const int cap_h = swap ? width_ : height_;

  camera_ = std::make_unique<V4l2Capture>();
  bool ok = camera_->open(device_, cap_w, cap_h, fps_) &&
            camera_->start_stream();
  if (ok) {
    has_camera_.store(true);
    LOG_INFO("camera: %s open, rotation=%d, output %dx%d", device_.c_str(),
             rotation_, width_, height_);
  } else {
    has_camera_.store(false);
    camera_.reset();
    LOG_WARN("camera: no device — using synthetic frames");
  }

  std::vector<std::uint8_t> nv12;
  std::vector<std::uint8_t> rgba;
  const auto frame_interval = std::chrono::milliseconds(1000 / fps_);
  std::uint64_t synth_frame = 0;
  auto next_synth = std::chrono::steady_clock::now();

  while (running_.load()) {
    if (camera_) {
      if (!camera_->dequeue(nv12, 500)) continue;
      const std::int64_t ts_us =
          std::chrono::duration_cast<std::chrono::microseconds>(
              std::chrono::steady_clock::now().time_since_epoch())
              .count();
      nv12_to_rgba(nv12.data(), camera_->width(), camera_->height(), rotation_,
                   width_, height_, rgba);
      deliver(rgba, ts_us);
    } else {
      make_synthetic_frame(width_, height_, synth_frame, rgba);
      deliver(rgba, static_cast<std::int64_t>(synth_frame) * 1000000 / fps_);
      ++synth_frame;
      next_synth += frame_interval;
      std::this_thread::sleep_until(next_synth);
    }
  }
}

void CameraEngine::deliver(const std::vector<std::uint8_t>& rgba,
                           std::int64_t timestamp_us) {
  if (rgba.size() != static_cast<std::size_t>(width_) * height_ * 4) return;

  if (preview_) {
    preview_->publish(width_, height_, rgba.data(), rgba.size());
  }
  if (!publishing_.load() || !video_source_) return;

  try {
    livekit::VideoFrame frame(width_, height_, livekit::VideoBufferType::RGBA,
                              rgba);
    video_source_->captureFrame(frame, timestamp_us);
  } catch (const std::exception& e) {
    LOG_DEBUG("camera: captureFrame dropped a frame: %s", e.what());
  }
}

}  // namespace jusiai

#pragma once

// Recycles raw byte buffers used to hold one frame. Avoids
// per-frame heap churn on the network thread.

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace telefacet::data {

// One set of checkerboard corners detected on-device (protocol v4+). Coordinates
// are in full-frame Y-plane pixel space, so they overlay directly on the frame.
struct CornerSet {
  std::uint8_t set_id = 0;  // 0 for `checkerboard`; row*2+col (0..3) for 2x2
  std::uint8_t flags  = 0;  // bit 0 ⇒ full-frame Y-plane coords
  std::vector<std::array<float, 2>> corners;  // {x, y} per inner corner
};

// One detected ArUco/AprilTag marker (protocol v6, `aruco` / `aruco2x2` modes).
// Coordinates are in full-frame Y-plane pixel space, like CornerSet, so they
// overlay directly on the frame.
struct ArucoMarker {
  std::int32_t marker_id = 0;  // DICT_APRILTAG_16h5 id (0..29)
  std::uint8_t quadrant  = 0;  // 0 for `aruco`; row*2+col (0..3) for `aruco2x2`
  std::uint8_t flags     = 0;  // bit 0 ⇒ full-frame Y-plane coords
  std::vector<std::array<float, 2>> corners;  // 4 corners, clockwise from TL
};

struct FrameBuffer {
  std::vector<std::uint8_t> data;  // packed bytes
  std::uint32_t frame_id          = 0;
  std::uint32_t camera_id         = 0;  // server-local id
  std::uint32_t width             = 0;
  std::uint32_t height            = 0;
  std::uint32_t bytes_per_line    = 0;
  std::uint32_t pixel_format      = 0;  // FourCC from ChunkHeader
  std::uint32_t frames_saved      = 0;
  // v3 timing metadata.
  std::uint64_t timestamp_us      = 0;  // monotonic HW capture timestamp
  std::uint32_t frame_duration_us = 0;  // IPA/ISP-reported per-frame duration
  // v5 per-frame focus metadata.
  float         lens_position     = NAN;   // dioptres; NaN when unavailable
  std::uint8_t  af_state          = 0xFF;  // libcamera AfState; 0xFF if none
  // v4 checkerboard corners (empty unless a checkerboard save mode found a board).
  std::vector<CornerSet> corner_sets;
  // v6 ArUco markers (empty unless an aruco save mode detected a marker).
  std::vector<ArucoMarker> aruco_markers;
  bool          header_only       = false;
};

class FrameBufferPool {
 public:
  FrameBufferPool() = default;

  std::shared_ptr<FrameBuffer> acquire(std::size_t at_least_bytes);
  void release(std::shared_ptr<FrameBuffer> buf);

 private:
  std::mutex mu_;
  std::vector<std::shared_ptr<FrameBuffer>> free_;
};

}  // namespace telefacet::data

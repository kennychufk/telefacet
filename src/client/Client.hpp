#pragma once

// Headless client API — the GUI-free entry point for consuming telefacet's
// camera streams from another application (e.g. a robot controller).
//
// The GUI app (src/app/App.cpp) wires MultiServerManager + CameraStore + the
// ImGui ControlPanel together by hand. `Client` is the same wiring minus the
// UI: it drives one or more cherupi-v4l2 servers through the full lifecycle
// (connect → discover → configure → set aruco2x2 → start cameras → start
// streams) and then hands out the latest per-camera AprilTag/ArUco corner
// detections through a small, thread-safe pull API suitable for a real-time
// control loop.
//
// Nothing here depends on GLFW / OpenGL / ImGui — it links only against
// telefacet_core.

#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "config/ConfigLoader.hpp"
#include "data/CameraStore.hpp"
#include "data/FrameBufferPool.hpp"  // data::ArucoMarker
#include "net/MultiServerManager.hpp"

namespace telefacet::client {

// AprilTag / ArUco detections for one camera at one frame, copied out of the
// recycled network buffer so the caller may hold it for as long as it likes.
// Corner coordinates are in full-frame Y-plane pixels (see MarkerSetHeader),
// so they map 1:1 onto the camera's calibrated intrinsics.
struct CameraMarkers {
  std::size_t   global_camera_id = 0;  // stable index into cameras()
  std::size_t   server_index     = 0;  // which configured server
  std::uint32_t local_camera_id  = 0;  // server-local id == physical cam index
  std::uint32_t frame_id         = 0;
  std::uint64_t timestamp_us     = 0;  // monotonic HW capture timestamp
  float         lens_position    = NAN;  // dioptres (drives focus-dependent K)
  std::uint32_t width            = 0;
  std::uint32_t height           = 0;
  // Empty when the detector found no marker in this frame.
  std::vector<data::ArucoMarker> markers;
};

// Options applied during start(). Defaults are tuned for pose estimation:
// subpixel corners on, no on-disk frame saving, header-only transport (we only
// need the corner block, not the pixels — a large bandwidth saving).
struct ClientOptions {
  // aruco2x2 detector params (server-side ProcessConfig).
  bool aruco_full_res_detection = false;  // detect on full-res vs 2x-subsampled
  int  aruco_num_threads        = 4;      // quadrant parallelism (1..4)
  bool aruco_corner_refine      = true;   // CORNER_REFINE_SUBPIX for accuracy

  bool save_frames = false;  // keep the Pi from writing frames to disk
  bool header_only = true;   // stream header+detection block only, no pixels

  // Optional manual camera controls applied before streaming starts.
  std::optional<double>       lens_position;     // dioptres; matches calibration
  std::optional<std::int64_t> exposure_time_us;  // manual shutter

  // How long start() waits for connect + discovery + each state transition.
  std::chrono::milliseconds start_timeout{8000};
};

class Client {
 public:
  explicit Client(config::Config cfg);
  ~Client();
  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;

  // Runs the full bring-up and blocks until every configured server is
  // streaming, or up to opts.start_timeout. Throws std::runtime_error on
  // timeout / failure. Safe to call once.
  void start(const ClientOptions& opts = {});

  // Stops streams + cameras and disconnects. Idempotent; the destructor calls
  // it. Safe to call even if start() threw partway through.
  void stop();

  bool connected() const;

  // The discovered camera roster (valid after start()). global_id values are
  // dense [0, cameras().size()) and stable for the session.
  std::vector<data::CameraInfo> cameras() const;

  // Pull the latest detections for one camera. Returns false (and leaves `out`
  // untouched) when no new frame has arrived since this call last returned true
  // for that camera. Non-blocking, latest-wins — older frames are skipped.
  bool latest(std::size_t global_camera_id, CameraMarkers& out);

  // Convenience for a control loop: returns one CameraMarkers per camera that
  // produced a fresh frame since the previous poll(). Cameras with no new frame
  // are omitted; a returned entry may still carry an empty `markers` vector
  // (a frame in which nothing was detected).
  std::vector<CameraMarkers> poll();

  // Escape hatches for advanced callers (per-camera lens/exposure, stats, ...).
  data::CameraStore&       store()   { return store_; }
  net::MultiServerManager& manager() { return *msm_; }

 private:
  void copyOut(std::size_t global_id, const data::FrameBuffer& buf,
               CameraMarkers& out) const;
  bool waitUntil(const std::function<bool()>& pred,
                 std::chrono::milliseconds timeout) const;

  config::Config                            cfg_;
  data::CameraStore                         store_;
  std::unique_ptr<net::MultiServerManager>  msm_;
  std::unordered_map<std::size_t, std::uint64_t> seen_seq_;  // per-camera cursor
  bool started_ = false;
};

}  // namespace telefacet::client

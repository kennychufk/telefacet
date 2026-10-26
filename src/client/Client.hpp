#pragma once

// Headless client API — the GUI-free entry point for consuming telefacet's
// camera streams from another application (e.g. a robot controller).
//
// The GUI app (src/app/App.cpp) wires MultiServerManager + CameraStore + the
// ImGui ControlPanel together by hand. `Client` is the same wiring minus the
// UI: it drives one or more cherupi-v4l2 servers through the full lifecycle
// (connect → discover → configure → set process mode → start cameras → start
// streams) and then hands out the latest per-camera AprilTag/ArUco corner
// detections through a small, thread-safe pull API suitable for a real-time
// control loop.
//
// The defaults target pose estimation (aruco2x2 + header-only). Callers wanting
// raw pixels — e.g. tools/stream_benchmark.cpp — set ClientOptions::save_mode
// to "none" and header_only to false, then pull whole frames off store().
//
// Nothing here depends on GLFW / OpenGL / ImGui — it links only against
// telefacet_core.

#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
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

// One camera's capture frame, as returned by triggerCapture() / stackCapture().
struct CapturedFrame {
  std::size_t   global_camera_id = 0;  // stable index into cameras()
  std::size_t   server_index     = 0;
  std::uint32_t local_camera_id  = 0;
  std::uint32_t frame_id         = 0;
  // The frame as received, owned by the caller (it is not recycled):
  // timestamp / focus metadata, the process mode's detection (corner_sets /
  // aruco_markers, empty in a non-detector mode) and, unless the request had
  // send_image false (then header_only is set and data is empty), the pixels.
  // A trigger frame is full-resolution YUV420 and its corners are full-frame
  // pixels. A stacked image is half-resolution 'SF32' float32 (protocol §5.5)
  // and its corners are in its own pixels: sensor pixel = 2b + 0.5.
  std::shared_ptr<const data::FrameBuffer> frame;
};

// trigger_capture options (protocol §4.17).
struct TriggerOptions {
  // Settling frames discarded per camera before the kept one (0..100).
  // Unset ⇒ the config's processing.trigger_skip_frames.
  std::optional<int> skip_frames;
  // false ⇒ frames arrive header + detection only — the corners without the
  // pixels, for a detector mode on a slow link.
  bool send_image = true;
};

// Result of one triggerCapture() across every connected server.
struct TriggerOutcome {
  // Every server acked and delivered all its frames before the timeout. False
  // ⇒ `captures` is partial: some server refused (see `refusals`), or did not
  // finish in time.
  bool complete  = false;
  // A server abandoned its trigger because capture stopped while it was
  // pending; that server's untaken cameras are missing from `captures`.
  bool cancelled = false;
  // "server <i>: <message>" for each server that refused the request.
  std::vector<std::string> refusals;
  // One entry per camera that delivered a frame, sorted by global_camera_id.
  std::vector<CapturedFrame> captures;

  explicit operator bool() const { return complete && !cancelled; }
};

// stack_capture options (protocol §4.19). Unset ⇒ the config's
// processing.stack_* defaults; an exposure / gain of 0 there keeps the preview's.
struct StackOptions {
  std::optional<int>          num_frames;        // 1..1024
  std::optional<std::int64_t> exposure_time_us;  // pin for the stack, then restore
  std::optional<double>       analogue_gain;     // 1..64; server: 1.0 with an exposure
  std::optional<int>          skip_frames;       // 0..100, after settings settle
  bool                        send_image = true;
};

// One camera's part of a stackCapture().
struct StackedCamera {
  std::size_t       global_camera_id = 0;
  std::size_t       server_index     = 0;
  std::uint32_t     local_camera_id  = 0;
  // The server's stack_result entry: frames summed, exposure, white level...
  // `stats.error` set ⇒ this camera failed and has no image.
  net::StackCapture stats;
  // The stacked image (see CapturedFrame::frame); null when the camera failed
  // or its image did not arrive before the timeout.
  std::shared_ptr<const data::FrameBuffer> frame;
};

struct StackOutcome {
  bool complete  = false;  // every server reported and sent its images in time
  bool cancelled = false;  // stop_cameras arrived mid-stack
  std::vector<std::string>   refusals;  // e.g. "server 0: stack_capture needs the raw stream"
  std::vector<StackedCamera> cameras;   // sorted by global_camera_id

  // Complete, and every camera stacked successfully.
  explicit operator bool() const {
    if (!complete || cancelled) return false;
    for (const auto& c : cameras)
      if (!c.stats.error.empty() || !c.frame) return false;
    return true;
  }
};

// Options applied during start(). Defaults are tuned for pose estimation:
// subpixel corners on, no on-disk frame saving, header-only transport (we only
// need the corner block, not the pixels — a large bandwidth saving).
struct ClientOptions {
  // Server-side processing mode (protocol §4.5): none|buffer|batch|
  // checkerboard|checkerboard2x2|aruco|aruco2x2. The aruco_* fields below apply
  // only to the aruco modes. Use "none" with header_only=false to stream raw
  // full frames (every captured frame — "none" has no detector to gate on).
  // triggerCapture() / stackCapture() work in every mode and get its detection.
  std::string save_mode = "aruco2x2";

  // aruco2x2 detector params (server-side ProcessConfig).
  bool aruco_full_res_detection = false;  // detect on full-res vs 2x-subsampled
  int  aruco_num_threads        = 4;      // quadrant parallelism (1..4)
  bool aruco_corner_refine      = true;   // CORNER_REFINE_SUBPIX for accuracy

  // Whether the Pi writes live frames to disk. Off by default (pose estimation
  // needs no files). Captures are never saved on the Pi: triggerCapture() and
  // stackCapture() hand their frames back instead.
  bool save_frames = false;
  bool header_only = true;   // stream header+detection block only, no pixels

  // Optional manual camera controls applied before streaming starts.
  std::optional<double>       lens_position;     // dioptres; matches calibration
  std::optional<std::int64_t> exposure_time_us;  // manual shutter
  // Locks FrameDurationLimits to this value (µs), i.e. a fixed capture
  // cadence: 500000 ⇒ 2 fps. Unset ⇒ libcamera's default range. Mirrors the
  // GUI's "Lock fps" control (protocol §4.14).
  std::optional<std::int64_t> frame_duration_us;

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

  // Take one frame per running camera, right now, and return it — the API
  // behind the GUI's Trigger button, and the call an automated calibration rig
  // makes once its arm has come to a complete stop. Works in any process mode;
  // each frame carries that mode's detection. Nothing is saved on the server.
  //
  // Blocks until every connected server has acked and delivered its frames, or
  // `timeout` elapses. The arm may move as soon as the frames are taken, which
  // is a little earlier than this returns (detection and transfer follow).
  //
  // Not reentrant: one capture (trigger or stack) at a time per Client — the
  // servers refuse a second one until the first's frames have been sent.
  TriggerOutcome triggerCapture(
      std::chrono::milliseconds timeout = std::chrono::milliseconds{10000},
      const TriggerOptions& opts = {});

  // Average raw frames per camera on every server configured with `raw`
  // (ServerCfg::raw) and return the stacked images, with the process mode's
  // detection on each — corner scatter falls as 1/sqrt(num_frames). Blocks
  // until every such server has reported and sent its images, or `timeout`.
  // Takes num_frames frame periods plus settling, so allow for it.
  StackOutcome stackCapture(
      std::chrono::milliseconds timeout = std::chrono::milliseconds{60000},
      const StackOptions& opts = {});

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

  // Capture rendezvous. Results, refusals and frames land on the WebSocket
  // receive threads (one per server) and are collected here until every
  // armed server has finished. Guarded by capture_mu_.
  struct PendingServer {
    bool          done     = false;
    bool          acked    = false;
    std::uint32_t id       = 0;  // trigger_id / stack_id
    std::size_t   expected = 0;  // frames announced by the result
    std::string   refusal;
    net::TriggerResult trigger;
    net::StackResult   stack;
    std::vector<std::shared_ptr<const data::FrameBuffer>> frames;
  };
  // Run one capture: wait out any earlier capture still in flight, arm the
  // rendezvous, `send` the request (returning the servers it reached), and
  // wait until each of them finished. Returns false on timeout or when
  // nothing could be sent; `out` holds whatever arrived, by server, and
  // `refusals` any reason it could not start.
  bool runCapture(net::CaptureKind kind,
                  const std::function<std::vector<std::size_t>()>& send,
                  std::chrono::milliseconds timeout,
                  std::unordered_map<std::size_t, PendingServer>& out,
                  std::vector<std::string>& refusals);
  // Receive-thread hooks feeding the rendezvous.
  void onCaptureResult(std::size_t server, net::CaptureKind kind,
                       std::uint32_t id, std::size_t expected,
                       const net::TriggerResult* tr, const net::StackResult* sr);
  void onCaptureRefused(std::size_t server, net::CaptureKind kind,
                        const std::string& message);
  void onCaptureFrame(std::size_t server,
                      std::shared_ptr<const data::FrameBuffer> frame);

  mutable std::mutex      capture_mu_;
  std::condition_variable capture_cv_;
  bool                    capture_active_ = false;
  net::CaptureKind        capture_kind_   = net::CaptureKind::Trigger;
  std::unordered_map<std::size_t, PendingServer> capture_pending_;  // by server
};

}  // namespace telefacet::client

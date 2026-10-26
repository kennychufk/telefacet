#pragma once

// Mirrors telefacet-web/src/services/ConfigLoader.js. Loads a YAML file with
// servers, camera_config, and processing (formerly frame_saving).

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace telefacet::config {

// Per-server settings. Sensor type and resolution are per-server (a client may
// talk to servers running different sensors), but identical across every camera
// on one server. All three are optional — an omitted field falls back to the
// connected server's own default (currently "imx519" / 2328x1748).
struct ServerCfg {
  std::string                  address;  // ws://host:port or wss://...
  std::string                  sensor;   // empty ⇒ unset (server default)
  std::optional<std::uint32_t> width;
  std::optional<std::uint32_t> height;
  // Also capture the unpacked 16-bit raw Bayer stream that stack_capture
  // averages (configure `raw`, protocol §4.3). Off by default: each raw buffer
  // is width × height × 2 bytes and costs memory and bus bandwidth even while
  // nothing is being stacked.
  bool                         raw = false;
};

struct FrameSavingCfg {
  // none|buffer|batch|checkerboard|checkerboard2x2|aruco|aruco2x2. Process
  // modes are post-processing only (protocol v8): triggering and stacking are
  // commands that work in any of them, and get the mode's detection.
  std::string mode = "none";
  // When false, detector modes still run and stream their side-products but no
  // frames are written to disk (server-side ProcessConfig::save_frames).
  bool save_frames = true;
  std::string output_dir = "camera_frames";
  bool   prepend_timestamp_to_dir = false;
  std::size_t batch_size      = 10;
  std::size_t writer_threads  = 4;
  // checkerboard / checkerboard2x2 fields
  int  checkerboard_rows = 8;
  int  checkerboard_cols = 11;
  bool checkerboard_full_res_detection = false;
  int  checkerboard_num_threads = 4;
  // cornerSubPix half-window in full-res px; unset ⇒ the server's default
  // (15). A stack uses half of it. 0 disables refinement.
  std::optional<int> checkerboard_subpix_window;
  // aruco / aruco2x2 fields
  bool aruco_full_res_detection = false;
  int  aruco_num_threads = 4;
  bool aruco_corner_refine = false;
  // trigger_capture's skip_frames (0..100): settling frames discarded per
  // camera before the one that is kept. Sent with every trigger.
  int  trigger_skip_frames = 0;
  // stack_capture defaults (protocol §4.19), sent with every stack. An
  // exposure / gain of 0 leaves the camera's preview settings in force.
  int          stack_num_frames    = 16;   // 1..1024
  std::int64_t stack_exposure_time = 0;    // µs, 0..10 000 000
  double       stack_analogue_gain = 0.0;  // 0, or 1..64
  int          stack_skip_frames   = 0;    // 0..100
  // Resource guards (server-side ProcessConfig). A mode that retains or writes
  // every frame builds a backlog whenever it outruns its sink; the server
  // bounds that backlog and refuses a mode that provably cannot keep up.
  //   backlog_max_bytes        0 => the server derives one from MemAvailable
  //   disk_write_bytes_per_sec 0 => the server measures the output filesystem
  //   allow_overcommit         start anyway when the check says infeasible
  std::size_t backlog_max_bytes = 0;
  std::size_t disk_write_bytes_per_sec = 0;
  bool allow_overcommit = false;
};

// Client-side saving of trigger / stack captures. The server saves neither
// (protocol v8), so this is the only copy. Used by the GUI; a headless Client
// gets the frames back from its capture calls and keeps what it wants.
struct CaptureSavingCfg {
  bool        save = true;
  // Each GUI session writes into <dir>/<YYYYMMDD-HHMMSS>-captures/<host_port>/.
  std::string dir  = "captures";
};

struct Config {
  std::vector<ServerCfg> servers;
  FrameSavingCfg         saving;
  CaptureSavingCfg       captures;
};

// Throws std::runtime_error on parse / validation failure.
Config loadFromFile(const std::string& path);

}  // namespace telefacet::config

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
};

struct FrameSavingCfg {
  // none|buffer|batch|trigger|checkerboard|checkerboard2x2|aruco|aruco2x2
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
  // aruco / aruco2x2 fields
  bool aruco_full_res_detection = false;
  int  aruco_num_threads = 4;
  bool aruco_corner_refine = false;
  // trigger field: frames discarded per camera before the one a
  // trigger_capture keeps (a per-request `skip_frames` overrides it).
  int  trigger_skip_frames = 0;
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

struct Config {
  std::vector<ServerCfg> servers;
  FrameSavingCfg         saving;
};

// Throws std::runtime_error on parse / validation failure.
Config loadFromFile(const std::string& path);

}  // namespace telefacet::config

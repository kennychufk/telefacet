#pragma once

// Mirrors telefacet-web/src/services/ConfigLoader.js. Loads a YAML file with
// servers, camera_config, and frame_saving.

#include <cstdint>
#include <string>
#include <vector>

namespace telefacet::config {

struct ServerCfg {
  std::string address;  // ws://host:port or wss://...
};

struct CameraCfg {
  std::uint32_t width       = 1456;
  std::uint32_t height      = 1088;
  std::uint32_t crop_width  = 1456;
  std::uint32_t crop_height = 1088;
  std::uint32_t crop_left   = 0;
  std::uint32_t crop_top    = 0;
  std::uint32_t v4l2_buffers = 4;
};

struct FrameSavingCfg {
  std::string mode = "none";  // none|buffer|batch|checkerboard
  std::string output_dir = "camera_frames";
  bool   prepend_timestamp_to_dir = false;
  std::size_t batch_size      = 10;
  std::size_t writer_threads  = 4;
  // Checkerboard-only fields
  int  checkerboard_rows = 8;
  int  checkerboard_cols = 11;
  bool checkerboard_full_res_detection = false;
  int  checkerboard_num_threads = 4;
};

struct Config {
  std::vector<ServerCfg> servers;
  CameraCfg              camera;
  FrameSavingCfg         saving;
};

// Throws std::runtime_error on parse / validation failure.
Config loadFromFile(const std::string& path);

}  // namespace telefacet::config

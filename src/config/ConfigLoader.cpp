#include "config/ConfigLoader.hpp"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <stdexcept>

namespace telefacet::config {

namespace {

void requireKey(const YAML::Node& node, const char* key) {
  if (!node[key]) {
    throw std::runtime_error(std::string("missing required key: ") + key);
  }
}

void validateAddress(const std::string& addr, std::size_t idx) {
  // Bare-bones URL check; matches the JS validator's intent.
  const bool ok =
      addr.rfind("ws://", 0) == 0 || addr.rfind("wss://", 0) == 0;
  if (!ok) {
    throw std::runtime_error("server " + std::to_string(idx) +
                             " address must use ws:// or wss://: " + addr);
  }
}

}  // namespace

Config loadFromFile(const std::string& path) {
  YAML::Node root;
  try {
    root = YAML::LoadFile(path);
  } catch (const std::exception& e) {
    throw std::runtime_error("failed to load YAML " + path + ": " + e.what());
  }

  Config cfg;

  // ---- servers ----
  requireKey(root, "servers");
  if (!root["servers"].IsSequence() || root["servers"].size() == 0) {
    throw std::runtime_error("'servers' must be a non-empty sequence");
  }
  for (std::size_t i = 0; i < root["servers"].size(); ++i) {
    const auto& s = root["servers"][i];
    requireKey(s, "address");
    ServerCfg sc;
    sc.address = s["address"].as<std::string>();
    validateAddress(sc.address, i);
    // Per-server sensor + resolution are optional (fall back to server default).
    if (s["sensor"]) sc.sensor = s["sensor"].as<std::string>();
    if (s["width"])  sc.width  = s["width"].as<std::uint32_t>();
    if (s["height"]) sc.height = s["height"].as<std::uint32_t>();
    cfg.servers.push_back(std::move(sc));
  }

  // ---- processing (optional, with defaults; formerly `frame_saving`) ----
  if (root["processing"]) {
    const auto& f = root["processing"];
    if (f["mode"]) cfg.saving.mode = f["mode"].as<std::string>();
    static const std::vector<std::string> valid = {
        "none", "buffer", "batch", "trigger", "checkerboard", "checkerboard2x2",
        "aruco", "aruco2x2"};
    if (std::find(valid.begin(), valid.end(), cfg.saving.mode) ==
        valid.end()) {
      throw std::runtime_error("processing.mode invalid: " + cfg.saving.mode);
    }
    if (f["save_frames"]) cfg.saving.save_frames = f["save_frames"].as<bool>();
    if (f["output_dir"]) cfg.saving.output_dir = f["output_dir"].as<std::string>();
    if (f["prepend_timestamp_to_dir"])
      cfg.saving.prepend_timestamp_to_dir = f["prepend_timestamp_to_dir"].as<bool>();
    if (f["batch_size"])     cfg.saving.batch_size     = f["batch_size"].as<std::size_t>();
    if (f["writer_threads"]) cfg.saving.writer_threads = f["writer_threads"].as<std::size_t>();
    if (f["backlog_max_bytes"])
      cfg.saving.backlog_max_bytes = f["backlog_max_bytes"].as<std::size_t>();
    if (f["disk_write_bytes_per_sec"])
      cfg.saving.disk_write_bytes_per_sec =
          f["disk_write_bytes_per_sec"].as<std::size_t>();
    if (f["allow_overcommit"])
      cfg.saving.allow_overcommit = f["allow_overcommit"].as<bool>();
    if (cfg.saving.mode == "checkerboard" ||
        cfg.saving.mode == "checkerboard2x2") {
      if (f["checkerboard_rows"])
        cfg.saving.checkerboard_rows = f["checkerboard_rows"].as<int>();
      if (f["checkerboard_cols"])
        cfg.saving.checkerboard_cols = f["checkerboard_cols"].as<int>();
      if (f["checkerboard_full_res_detection"])
        cfg.saving.checkerboard_full_res_detection =
            f["checkerboard_full_res_detection"].as<bool>();
      if (f["checkerboard_num_threads"])
        cfg.saving.checkerboard_num_threads =
            f["checkerboard_num_threads"].as<int>();
    }
    if (cfg.saving.mode == "trigger") {
      if (f["trigger_skip_frames"])
        cfg.saving.trigger_skip_frames = f["trigger_skip_frames"].as<int>();
    }
    if (cfg.saving.mode == "aruco" || cfg.saving.mode == "aruco2x2") {
      if (f["aruco_full_res_detection"])
        cfg.saving.aruco_full_res_detection =
            f["aruco_full_res_detection"].as<bool>();
      if (f["aruco_num_threads"])
        cfg.saving.aruco_num_threads = f["aruco_num_threads"].as<int>();
      if (f["aruco_corner_refine"])
        cfg.saving.aruco_corner_refine = f["aruco_corner_refine"].as<bool>();
    }
  }

  return cfg;
}

}  // namespace telefacet::config

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
    if (s["raw"])    sc.raw    = s["raw"].as<bool>();
    cfg.servers.push_back(std::move(sc));
  }

  // ---- processing (optional, with defaults; formerly `frame_saving`) ----
  if (root["processing"]) {
    const auto& f = root["processing"];
    if (f["mode"]) cfg.saving.mode = f["mode"].as<std::string>();
    static const std::vector<std::string> valid = {
        "none", "buffer", "batch", "checkerboard", "checkerboard2x2",
        "aruco", "aruco2x2"};
    std::string valid_list;
    for (const auto& v : valid) valid_list += (valid_list.empty() ? "" : ", ") + v;
    if (cfg.saving.mode == "trigger" || cfg.saving.mode == "stack") {
      throw std::runtime_error(
          "processing.mode '" + cfg.saving.mode +
          "' no longer exists: trigger and stack captures work in any mode "
          "(protocol v8) — use one of: " + valid_list);
    }
    if (std::find(valid.begin(), valid.end(), cfg.saving.mode) ==
        valid.end()) {
      throw std::runtime_error("processing.mode invalid: " + cfg.saving.mode +
                               " (one of: " + valid_list + ")");
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
      if (f["checkerboard_subpix_window"]) {
        const int w = f["checkerboard_subpix_window"].as<int>();
        if (w < 0 || w > 100)
          throw std::runtime_error(
              "processing.checkerboard_subpix_window must be in [0, 100]");
        cfg.saving.checkerboard_subpix_window = w;
      }
    }
    // Capture-request defaults, valid in every mode.
    auto intIn = [&](const char* key, int lo, int hi, int& out) {
      if (!f[key]) return;
      const int v = f[key].as<int>();
      if (v < lo || v > hi)
        throw std::runtime_error(std::string("processing.") + key +
                                 " must be an integer in [" +
                                 std::to_string(lo) + ", " +
                                 std::to_string(hi) + "]");
      out = v;
    };
    intIn("trigger_skip_frames", 0, 100, cfg.saving.trigger_skip_frames);
    intIn("stack_num_frames", 1, 1024, cfg.saving.stack_num_frames);
    intIn("stack_skip_frames", 0, 100, cfg.saving.stack_skip_frames);
    if (f["stack_exposure_time"]) {
      const auto v = f["stack_exposure_time"].as<std::int64_t>();
      if (v < 0 || v > 10000000)
        throw std::runtime_error(
            "processing.stack_exposure_time must be in [0, 10000000] us "
            "(0 = keep the preview's)");
      cfg.saving.stack_exposure_time = v;
    }
    if (f["stack_analogue_gain"]) {
      const double g = f["stack_analogue_gain"].as<double>();
      if (!(g == 0.0 || (g >= 1.0 && g <= 64.0)))
        throw std::runtime_error(
            "processing.stack_analogue_gain must be 0 (unpinned) or in [1, 64]");
      cfg.saving.stack_analogue_gain = g;
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

  // ---- captures (optional): client-side saving of trigger / stack frames ----
  if (root["captures"]) {
    const auto& c = root["captures"];
    if (c["save"]) cfg.captures.save = c["save"].as<bool>();
    if (c["dir"])  cfg.captures.dir  = c["dir"].as<std::string>();
    if (cfg.captures.dir.empty())
      throw std::runtime_error("captures.dir must not be empty");
  }

  return cfg;
}

}  // namespace telefacet::config

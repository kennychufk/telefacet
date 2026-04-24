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
    ServerCfg sc{s["address"].as<std::string>()};
    validateAddress(sc.address, i);
    cfg.servers.push_back(std::move(sc));
  }

  // ---- camera_config ----
  requireKey(root, "camera_config");
  const auto& cam = root["camera_config"];
  for (const char* k :
       {"width", "height", "crop_width", "crop_height", "crop_left",
        "crop_top"}) {
    requireKey(cam, k);
  }
  cfg.camera.width        = cam["width"].as<std::uint32_t>();
  cfg.camera.height       = cam["height"].as<std::uint32_t>();
  cfg.camera.crop_width   = cam["crop_width"].as<std::uint32_t>();
  cfg.camera.crop_height  = cam["crop_height"].as<std::uint32_t>();
  cfg.camera.crop_left    = cam["crop_left"].as<std::uint32_t>();
  cfg.camera.crop_top     = cam["crop_top"].as<std::uint32_t>();
  if (cam["v4l2_buffers"]) {
    cfg.camera.v4l2_buffers = cam["v4l2_buffers"].as<std::uint32_t>();
  }

  // ---- frame_saving (optional, with defaults) ----
  if (root["frame_saving"]) {
    const auto& f = root["frame_saving"];
    if (f["mode"]) cfg.saving.mode = f["mode"].as<std::string>();
    static const std::vector<std::string> valid = {"none", "buffer", "batch",
                                                   "checkerboard"};
    if (std::find(valid.begin(), valid.end(), cfg.saving.mode) ==
        valid.end()) {
      throw std::runtime_error("frame_saving.mode invalid: " + cfg.saving.mode);
    }
    if (f["output_dir"]) cfg.saving.output_dir = f["output_dir"].as<std::string>();
    if (f["prepend_timestamp_to_dir"])
      cfg.saving.prepend_timestamp_to_dir = f["prepend_timestamp_to_dir"].as<bool>();
    if (f["batch_size"])     cfg.saving.batch_size     = f["batch_size"].as<std::size_t>();
    if (f["writer_threads"]) cfg.saving.writer_threads = f["writer_threads"].as<std::size_t>();
    if (cfg.saving.mode == "checkerboard") {
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
  }

  return cfg;
}

}  // namespace telefacet::config

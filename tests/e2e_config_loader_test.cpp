#include <gtest/gtest.h>

#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "config/ConfigLoader.hpp"

#ifndef TELEFACET_TEST_FIXTURES_DIR
#define TELEFACET_TEST_FIXTURES_DIR "."
#endif

TEST(ConfigLoader, LoadsExampleYaml) {
  auto path = std::string(TELEFACET_TEST_FIXTURES_DIR) + "/test_config.yaml";
  auto cfg = telefacet::config::loadFromFile(path);

  ASSERT_EQ(cfg.servers.size(), 1u);
  EXPECT_EQ(cfg.servers[0].address, "ws://localhost:9001");
  EXPECT_EQ(cfg.servers[0].sensor, "imx519");
  ASSERT_TRUE(cfg.servers[0].width.has_value());
  ASSERT_TRUE(cfg.servers[0].height.has_value());
  EXPECT_EQ(*cfg.servers[0].width, 1456u);
  EXPECT_EQ(*cfg.servers[0].height, 1088u);

  EXPECT_EQ(cfg.saving.mode, "none");
  // The fixture's `processing` section sets save_frames: false (non-default),
  // so this also proves the renamed section + new field are parsed.
  EXPECT_FALSE(cfg.saving.save_frames);
}

TEST(ConfigLoader, LoadsCaptureYaml) {
  auto path = std::string(TELEFACET_TEST_FIXTURES_DIR) + "/capture_config.yaml";
  auto cfg = telefacet::config::loadFromFile(path);

  ASSERT_EQ(cfg.servers.size(), 2u);
  EXPECT_TRUE(cfg.servers[0].raw);
  EXPECT_FALSE(cfg.servers[1].raw);  // off unless asked for

  EXPECT_EQ(cfg.saving.mode, "checkerboard");
  ASSERT_TRUE(cfg.saving.checkerboard_subpix_window.has_value());
  EXPECT_EQ(*cfg.saving.checkerboard_subpix_window, 40);
  EXPECT_EQ(cfg.saving.trigger_skip_frames, 2);
  EXPECT_EQ(cfg.saving.stack_num_frames, 32);
  EXPECT_EQ(cfg.saving.stack_exposure_time, 50000);
  EXPECT_DOUBLE_EQ(cfg.saving.stack_analogue_gain, 1.5);
  EXPECT_EQ(cfg.saving.stack_skip_frames, 3);

  EXPECT_FALSE(cfg.captures.save);
  EXPECT_EQ(cfg.captures.dir, "calibration_captures");
}

TEST(ConfigLoader, CaptureDefaults) {
  auto path = std::string(TELEFACET_TEST_FIXTURES_DIR) + "/test_config.yaml";
  auto cfg = telefacet::config::loadFromFile(path);
  EXPECT_FALSE(cfg.servers[0].raw);
  EXPECT_FALSE(cfg.saving.checkerboard_subpix_window.has_value());
  EXPECT_EQ(cfg.saving.trigger_skip_frames, 0);
  EXPECT_EQ(cfg.saving.stack_num_frames, 16);
  EXPECT_EQ(cfg.saving.stack_exposure_time, 0);
  EXPECT_TRUE(cfg.captures.save);
  EXPECT_EQ(cfg.captures.dir, "captures");
}

namespace {

// Load `yaml` from a scratch file; returns the loader's error, "" on success.
std::string loadError(const std::string& yaml) {
  const auto path = std::filesystem::temp_directory_path() /
                    ("telefacet_cfg_" + std::to_string(::getpid()) + ".yaml");
  std::ofstream(path) << yaml;
  std::string err;
  try {
    telefacet::config::loadFromFile(path.string());
  } catch (const std::runtime_error& e) {
    err = e.what();
  }
  std::filesystem::remove(path);
  return err;
}

}  // namespace

TEST(ConfigLoader, RejectsRemovedCaptureModes) {
  // v8 removed the `trigger` and `stack` process modes: the message must say
  // where the feature went rather than just "invalid".
  for (const char* mode : {"trigger", "stack"}) {
    const auto err = loadError(
        std::string("servers: [{address: ws://localhost:9001}]\n"
                    "processing: {mode: ") + mode + "}\n");
    EXPECT_NE(err.find("no longer exists"), std::string::npos) << err;
  }
}

TEST(ConfigLoader, RejectsOutOfRangeCaptureParams) {
  const std::string head = "servers: [{address: ws://localhost:9001}]\n";
  EXPECT_FALSE(loadError(head + "processing: {trigger_skip_frames: 101}\n").empty());
  EXPECT_FALSE(loadError(head + "processing: {stack_num_frames: 0}\n").empty());
  EXPECT_FALSE(loadError(head + "processing: {stack_analogue_gain: 0.5}\n").empty());
  EXPECT_TRUE(loadError(head + "processing: {stack_analogue_gain: 0}\n").empty());
}

TEST(ConfigLoader, ThrowsOnMissingFile) {
  EXPECT_THROW(
      telefacet::config::loadFromFile("/nonexistent/path/to/config.yaml"),
      std::runtime_error);
}

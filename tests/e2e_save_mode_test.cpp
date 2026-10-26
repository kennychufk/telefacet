#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>

#include "harness.hpp"
#include "test_env.hpp"

using telefacet::test::BlockingClient;
using telefacet::test::TestCameraCfg;
using telefacet::test::serverUrl;
namespace fs = std::filesystem;

namespace {

// Sibling of tmp_output in the pytest harness — the server is on the same
// host (the Pi), so we create a directory here and pass it to the server. Use
// the current working dir; the test runner invokes us from the build tree.
fs::path makeOutputDir(const std::string& label) {
  fs::path base = fs::current_path() / "telefacet_e2e_out" / label;
  std::error_code ec;
  fs::remove_all(base, ec);
  fs::create_directories(base);
  return base;
}

bool hasYuvFile(const fs::path& dir) {
  if (!fs::exists(dir)) return false;
  for (auto& entry : fs::directory_iterator(dir)) {
    if (entry.path().extension() == ".yuv") return true;
  }
  return false;
}

}  // namespace

TEST(SaveMode, NoneProducesNoFiles) {
  auto out = makeOutputDir("none");
  BlockingClient c(serverUrl());
  c.connect();
  auto cams = c.waitForDiscovery();
  ASSERT_FALSE(cams.empty());

  TestCameraCfg cfg;
  c.sendAndExpectStatus([&] {
    return c.raw().configureCameras(cfg.width, cfg.height);
  });
  c.sendAndExpectStatus([&] {
    return c.raw().setSaveMode("none", {{"output_dir", out.string()}});
  });
  c.sendAndExpectStatus([&] { return c.raw().startCameras(); });
  c.sendAndExpectStatus([&] { return c.raw().startStream(cams[0].id); });
  std::this_thread::sleep_for(std::chrono::seconds(1));
  c.sendAndExpectStatus([&] { return c.raw().stopCameras(); },
                        "", std::chrono::seconds(15));
  EXPECT_FALSE(hasYuvFile(out));
}

TEST(SaveMode, BufferWritesFilesOnStop) {
  auto out = makeOutputDir("buffer");
  BlockingClient c(serverUrl());
  c.connect();
  auto cams = c.waitForDiscovery();
  ASSERT_FALSE(cams.empty());

  TestCameraCfg cfg;
  c.sendAndExpectStatus([&] {
    return c.raw().configureCameras(cfg.width, cfg.height);
  });
  c.sendAndExpectStatus([&] {
    return c.raw().setSaveMode("buffer", {{"output_dir", out.string()}});
  });
  c.sendAndExpectStatus([&] { return c.raw().startCameras(); });
  c.sendAndExpectStatus([&] { return c.raw().startStream(cams[0].id); });
  std::this_thread::sleep_for(std::chrono::seconds(2));
  c.sendAndExpectStatus([&] { return c.raw().stopCameras(); },
                        "", std::chrono::seconds(15));
  EXPECT_TRUE(hasYuvFile(out)) << "Expected .yuv files in " << out;
}

TEST(SaveMode, BatchWritesFilesWhileRunning) {
  auto out = makeOutputDir("batch");
  BlockingClient c(serverUrl());
  c.connect();
  auto cams = c.waitForDiscovery();
  ASSERT_FALSE(cams.empty());

  TestCameraCfg cfg;
  c.sendAndExpectStatus([&] {
    return c.raw().configureCameras(cfg.width, cfg.height);
  });
  c.sendAndExpectStatus([&] {
    return c.raw().setSaveMode("batch", {{"output_dir", out.string()},
                                         {"batch_size", 5},
                                         {"writer_threads", 2}});
  });
  c.sendAndExpectStatus([&] { return c.raw().startCameras(); });
  c.sendAndExpectStatus([&] { return c.raw().startStream(cams[0].id); });
  std::this_thread::sleep_for(std::chrono::milliseconds(2500));
  c.sendAndExpectStatus([&] { return c.raw().stopCameras(); },
                        "", std::chrono::seconds(15));
  EXPECT_TRUE(hasYuvFile(out)) << "Expected batched .yuv files in " << out;
}

TEST(SaveMode, CheckerboardRunsWithoutError) {
  auto out = makeOutputDir("checkerboard");
  BlockingClient c(serverUrl());
  c.connect();
  auto cams = c.waitForDiscovery();
  ASSERT_FALSE(cams.empty());

  TestCameraCfg cfg;
  c.sendAndExpectStatus([&] {
    return c.raw().configureCameras(cfg.width, cfg.height);
  });
  c.sendAndExpectStatus([&] {
    return c.raw().setSaveMode(
        "checkerboard",
        {{"output_dir", out.string()},
         {"checkerboard_rows", 8},
         {"checkerboard_cols", 11}});
  });
  c.sendAndExpectStatus([&] { return c.raw().startCameras(); });
  c.sendAndExpectStatus([&] { return c.raw().startStream(cams[0].id); });
  std::this_thread::sleep_for(std::chrono::seconds(2));
  // We don't assume a board is in view — we just want a clean shutdown.
  c.sendAndExpectStatus([&] { return c.raw().stopCameras(); },
                        "", std::chrono::seconds(15));
}

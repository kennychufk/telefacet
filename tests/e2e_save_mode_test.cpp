#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <optional>
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

std::size_t countYuvFiles(const fs::path& dir) {
  if (!fs::exists(dir)) return 0;
  std::size_t n = 0;
  for (auto& entry : fs::directory_iterator(dir)) {
    if (entry.path().extension() == ".yuv") ++n;
  }
  return n;
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

// TRIGGER: nothing is written until trigger_capture asks for a frame, and the
// ack only arrives once the frame really was captured (§4.17).
TEST(SaveMode, TriggerSavesOnlyOnRequest) {
  auto out = makeOutputDir("trigger");
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
        "trigger", {{"output_dir", out.string()}, {"save_frames", true}});
  });
  c.sendAndExpectStatus([&] { return c.raw().startCameras(); });
  c.sendAndExpectStatus([&] { return c.raw().startStream(cams[0].id); });

  // Frames are flowing, but none of them may be written.
  std::this_thread::sleep_for(std::chrono::seconds(1));
  EXPECT_FALSE(hasYuvFile(out)) << "trigger mode wrote frames without a trigger";

  // The ack is asynchronous — it lands on the receive thread once the camera
  // has delivered the triggered frame. The harness doesn't claim this callback.
  std::mutex mu;
  std::condition_variable cv;
  std::optional<telefacet::net::TriggerResult> ack;
  c.raw().setOnTriggerResult(
      [&](std::size_t, const telefacet::net::TriggerResult& tr) {
        {
          std::lock_guard<std::mutex> lk(mu);
          ack = tr;
        }
        cv.notify_all();
      });

  ASSERT_TRUE(c.raw().triggerCapture(cams[0].id));
  {
    std::unique_lock<std::mutex> lk(mu);
    ASSERT_TRUE(cv.wait_for(lk, std::chrono::seconds(10),
                            [&] { return ack.has_value(); }))
        << "no trigger_result within 10s";
  }
  EXPECT_FALSE(ack->cancelled);
  ASSERT_EQ(ack->captures.size(), 1u);
  EXPECT_EQ(ack->captures[0].camera_id, cams[0].id);
  EXPECT_FALSE(ack->captures[0].filename.empty());

  c.sendAndExpectStatus([&] { return c.raw().stopCameras(); },
                        "", std::chrono::seconds(15));
  EXPECT_EQ(countYuvFiles(out), 1u)
      << "expected exactly the one triggered frame in " << out;
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

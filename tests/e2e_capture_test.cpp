// On-demand captures against a live server (protocol v8, §4.17, §4.19, §5.9):
// trigger_capture and stack_capture work in any process mode and send their
// frames back to the commander after the ack, whatever its subscription or
// header-only state. Nothing is saved on the server, so — unlike the save-mode
// suites — these run fine against a server on another host.
//
// The stacking tests configure with `raw: true`; a sensor mode or server that
// cannot provide the raw stream skips them.

#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "client/Client.hpp"
#include "harness.hpp"
#include "net/Protocol.hpp"
#include "test_env.hpp"

using telefacet::test::BlockingClient;
using telefacet::test::TestCameraCfg;
using telefacet::test::serverUrl;
namespace net   = telefacet::net;
namespace proto = telefacet::proto;
using namespace std::chrono_literals;

namespace {

// Collects one connection's capture callbacks so a test can wait on them.
struct CaptureLog {
  std::mutex mu;
  std::condition_variable cv;
  std::vector<net::TriggerResult> triggers;
  std::vector<net::StackResult> stacks;
  std::vector<net::CaptureFrame> frames;
  std::vector<std::string> refusals;
  std::size_t live_frames = 0;
  std::size_t live_with_pixels = 0;

  explicit CaptureLog(BlockingClient& c) {
    auto& raw = c.raw();
    raw.setOnTriggerResult([this](std::size_t, const net::TriggerResult& tr) {
      add([&] { triggers.push_back(tr); });
    });
    raw.setOnStackResult([this](std::size_t, const net::StackResult& sr) {
      add([&] { stacks.push_back(sr); });
    });
    raw.setOnCaptureFrame([this](std::size_t, const net::CaptureFrame& cf) {
      add([&] { frames.push_back(cf); });
    });
    raw.setOnCaptureRefused(
        [this](std::size_t, net::CaptureKind, const std::string& m) {
          add([&] { refusals.push_back(m); });
        });
    // Replaces the harness's live-frame queue: count them instead.
    raw.setOnFrame([this](std::size_t, std::shared_ptr<telefacet::data::FrameBuffer> b) {
      add([&] {
        ++live_frames;
        if (!b->header_only) ++live_with_pixels;
      });
    });
  }

  template <typename F>
  void add(F f) {
    {
      std::lock_guard<std::mutex> lk(mu);
      f();
    }
    cv.notify_all();
  }

  template <typename P>
  bool waitFor(P pred, std::chrono::milliseconds timeout = 15s) {
    std::unique_lock<std::mutex> lk(mu);
    return cv.wait_for(lk, timeout, pred);
  }
};

// configure → set_process_mode → start_cameras, no stream subscription.
std::vector<net::DiscoveredCamera> bringUp(BlockingClient& c,
                                           const std::string& mode,
                                           bool raw = false) {
  c.connect();
  auto cams = c.waitForDiscovery();
  if (cams.empty()) throw std::runtime_error("no cameras discovered");
  TestCameraCfg cfg;
  c.sendAndExpectStatus(
      [&] { return c.raw().configureCameras(cfg.width, cfg.height, raw); });
  c.sendAndExpectStatus(
      [&] { return c.raw().setSaveMode(mode, {{"save_frames", false}}); });
  c.sendAndExpectStatus([&] { return c.raw().startCameras(); });
  return cams;
}

}  // namespace

// A trigger in `none` mode, with no subscription: the frame still comes back,
// full resolution with pixels, after its ack and matching it.
TEST(Capture, TriggerReturnsFrameWithoutSubscription) {
  BlockingClient c(serverUrl());
  auto cams = bringUp(c, "none");
  CaptureLog log(c);
  TestCameraCfg cfg;

  ASSERT_TRUE(c.raw().triggerCapture(cams[0].id));
  ASSERT_TRUE(log.waitFor([&] { return !log.frames.empty(); }))
      << "no capture frame within 15 s";
  ASSERT_EQ(log.triggers.size(), 1u) << "the ack must precede its frame";
  const auto& tr = log.triggers[0];
  EXPECT_FALSE(tr.cancelled);
  ASSERT_EQ(tr.captures.size(), 1u);

  const auto& f = *log.frames[0].frame;
  EXPECT_EQ(f.frame_kind, static_cast<std::uint8_t>(proto::FrameKind::Trigger));
  EXPECT_EQ(f.capture_id, static_cast<std::uint16_t>(tr.trigger_id));
  EXPECT_EQ(f.frame_id, tr.captures[0].frame_id);
  EXPECT_EQ(f.camera_id, cams[0].id);
  EXPECT_EQ(f.width, cfg.width);
  EXPECT_EQ(f.height, cfg.height);
  EXPECT_EQ(f.pixel_format, proto::kPixelFormatYU12);
  EXPECT_FALSE(f.header_only);
  EXPECT_GE(f.data.size(), static_cast<std::size_t>(f.bytes_per_line) * f.height * 3 / 2);
  EXPECT_EQ(log.frames[0].result.value("frame_id", 0u), f.frame_id);

  EXPECT_FALSE(c.raw().captureInProgress());
  EXPECT_EQ(log.live_frames, 0u) << "a capture frame was delivered as live";
}

// send_image false: header + detection only, even with header-only off; in a
// detector mode the frame says which detector ran on it.
TEST(Capture, TriggerWithoutImageInDetectorMode) {
  BlockingClient c(serverUrl());
  auto cams = bringUp(c, "checkerboard");
  CaptureLog log(c);

  ASSERT_TRUE(c.raw().triggerCapture(std::nullopt, 0, /*send_image*/ false));
  ASSERT_TRUE(log.waitFor([&] {
    return !log.triggers.empty() && log.frames.size() >= log.triggers[0].captures.size();
  }));
  ASSERT_FALSE(log.frames.empty());
  for (const auto& cf : log.frames) {
    EXPECT_TRUE(cf.frame->header_only);
    EXPECT_TRUE(cf.frame->data.empty());
    EXPECT_EQ(cf.frame->detection_kind,
              static_cast<std::uint8_t>(proto::DetectionKind::Checkerboard));
  }
}

// The reverse: a header-only live stream does not strip a capture's pixels.
TEST(Capture, TriggerSendsPixelsWhileStreamIsHeaderOnly) {
  BlockingClient c(serverUrl());
  auto cams = bringUp(c, "none");
  c.sendAndExpectStatus([&] { return c.raw().setHeaderOnly(true); });
  c.sendAndExpectStatus([&] { return c.raw().startStream(cams[0].id); });
  CaptureLog log(c);
  ASSERT_TRUE(log.waitFor([&] { return log.live_frames >= 2; }));

  ASSERT_TRUE(c.raw().triggerCapture(cams[0].id));
  ASSERT_TRUE(log.waitFor([&] { return !log.frames.empty(); }));
  EXPECT_FALSE(log.frames[0].frame->header_only);
  EXPECT_FALSE(log.frames[0].frame->data.empty());
  std::lock_guard<std::mutex> lk(log.mu);
  EXPECT_EQ(log.live_with_pixels, 0u);
}

// One capture at a time: a second request while the first is still waiting
// on its settling frames is refused, and the refusal is attributed to it.
TEST(Capture, OverlappingCaptureIsRefused) {
  BlockingClient c(serverUrl());
  auto cams = bringUp(c, "none");
  CaptureLog log(c);

  ASSERT_TRUE(c.raw().triggerCapture(cams[0].id, /*skip_frames*/ 30));
  ASSERT_TRUE(c.raw().triggerCapture(cams[0].id));
  ASSERT_TRUE(log.waitFor([&] { return !log.refusals.empty(); }, 5s));
  EXPECT_NE(log.refusals[0].find("already in progress"), std::string::npos)
      << log.refusals[0];
  EXPECT_TRUE(c.raw().captureInProgress()) << "the first capture is still pending";

  ASSERT_TRUE(log.waitFor([&] { return !log.frames.empty(); }, 20s));
  EXPECT_EQ(log.triggers.size(), 1u);
  EXPECT_FALSE(c.raw().captureInProgress());
}

TEST(Capture, StackWithoutRawIsRefused) {
  BlockingClient c(serverUrl());
  bringUp(c, "none", /*raw*/ false);
  CaptureLog log(c);
  net::StackRequest req;
  req.num_frames = 2;
  ASSERT_TRUE(c.raw().stackCapture(req));
  ASSERT_TRUE(log.waitFor([&] { return !log.refusals.empty(); }, 5s));
  EXPECT_NE(log.refusals[0].find("raw"), std::string::npos) << log.refusals[0];
  EXPECT_FALSE(c.raw().captureInProgress());
}

// A stack: the result first, then one half-resolution SF32 image per camera,
// with the process mode's detection on it.
TEST(Capture, StackReturnsSf32Image) {
  BlockingClient c(serverUrl());
  std::vector<net::DiscoveredCamera> cams;
  try {
    cams = bringUp(c, "checkerboard", /*raw*/ true);
  } catch (const std::exception& e) {
    GTEST_SKIP() << "server cannot configure the raw stream: " << e.what();
  }
  const auto rs = c.raw().rawStream();
  ASSERT_TRUE(rs.valid) << "configure reply carried no raw stream";
  CaptureLog log(c);
  TestCameraCfg cfg;

  net::StackRequest req;
  req.camera_id = cams[0].id;
  req.num_frames = 4;
  req.exposure_time_us = 20000;
  ASSERT_TRUE(c.raw().stackCapture(req));
  ASSERT_TRUE(log.waitFor([&] { return !log.frames.empty(); }, 30s))
      << "no stacked image within 30 s";
  ASSERT_EQ(log.stacks.size(), 1u);
  const auto& sr = log.stacks[0];
  ASSERT_EQ(sr.captures.size(), 1u);
  const auto& sc = sr.captures[0];
  ASSERT_TRUE(sc.error.empty()) << sc.error;
  EXPECT_EQ(sc.frames_stacked, 4);
  EXPECT_NEAR(static_cast<double>(sc.exposure_time), 20000.0, 400.0);
  EXPECT_EQ(sc.width, cfg.width / 2);

  const auto& f = *log.frames[0].frame;
  EXPECT_EQ(f.frame_kind, static_cast<std::uint8_t>(proto::FrameKind::Stack));
  EXPECT_EQ(f.capture_id, static_cast<std::uint16_t>(sr.stack_id));
  EXPECT_EQ(f.pixel_format, proto::kPixelFormatSF32);
  EXPECT_EQ(f.width, sc.width);
  EXPECT_EQ(f.height, sc.height);
  EXPECT_EQ(f.frame_id, sc.last_frame_id);
  EXPECT_GE(f.data.size(), static_cast<std::size_t>(f.width) * f.height * 4);
  EXPECT_EQ(f.detection_kind,
            static_cast<std::uint8_t>(proto::DetectionKind::Checkerboard));
  EXPECT_EQ(log.frames[0].result.value("frames_stacked", 0), 4);
  EXPECT_FALSE(c.raw().captureInProgress());
}

// The headless Client end to end: triggerCapture() and stackCapture() block
// until the frames are in hand and return them.
TEST(Capture, HeadlessClientReturnsFrames) {
  TestCameraCfg cam;
  telefacet::config::Config cfg;
  telefacet::config::ServerCfg sc;
  sc.address = serverUrl();
  sc.sensor  = telefacet::test::testSensor();
  sc.width   = cam.width;
  sc.height  = cam.height;
  sc.raw     = true;
  cfg.servers.push_back(sc);
  cfg.saving.stack_num_frames = 3;
  cfg.saving.stack_exposure_time = 20000;

  // Client::stop() leaves the server CONFIGURED; a BlockingClient's teardown
  // walks it back to IDLE for the next test, however this one ends.
  struct ResetServer {
    ~ResetServer() {
      BlockingClient c(serverUrl());
      try { c.connect(); } catch (...) {}
    }
  } reset_server;

  telefacet::client::Client client(cfg);
  telefacet::client::ClientOptions opts;
  opts.save_mode   = "aruco";
  opts.header_only = true;
  opts.start_timeout = 15s;
  try {
    client.start(opts);
  } catch (const std::exception& e) {
    GTEST_SKIP() << "could not bring up with raw: " << e.what();
  }
  const auto n = client.cameras().size();

  auto shot = client.triggerCapture(15s);
  ASSERT_TRUE(shot) << (shot.refusals.empty() ? "timed out" : shot.refusals[0]);
  ASSERT_EQ(shot.captures.size(), n);
  for (const auto& c : shot.captures) {
    ASSERT_TRUE(c.frame);
    EXPECT_FALSE(c.frame->header_only) << "send_image defaults to true";
    EXPECT_EQ(c.frame->width, cam.width);
    EXPECT_EQ(c.frame->detection_kind,
              static_cast<std::uint8_t>(proto::DetectionKind::Aruco));
  }

  telefacet::client::TriggerOptions bare;
  bare.send_image = false;
  auto shot2 = client.triggerCapture(15s, bare);
  ASSERT_TRUE(shot2);
  for (const auto& c : shot2.captures) EXPECT_TRUE(c.frame->header_only);

  auto st = client.stackCapture(30s);
  ASSERT_TRUE(st) << (st.refusals.empty() ? "incomplete" : st.refusals[0]);
  ASSERT_EQ(st.cameras.size(), n);
  for (const auto& c : st.cameras) {
    EXPECT_EQ(c.stats.frames_stacked, 3);
    ASSERT_TRUE(c.frame);
    EXPECT_EQ(c.frame->pixel_format, proto::kPixelFormatSF32);
  }
  client.stop();
}

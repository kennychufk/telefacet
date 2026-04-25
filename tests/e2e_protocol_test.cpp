#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <vector>

#include "harness.hpp"
#include "test_env.hpp"

using telefacet::test::BlockingClient;
using telefacet::test::TestCameraCfg;
using telefacet::test::serverUrl;
using telefacet::test::kFourccYU12;

namespace {

// Drive `c` into RUNNING + streaming cam[0], returning its id.
// BlockingClient is non-movable (owns a mutex/condvar), so callers allocate
// it on the stack and pass a reference here.
std::uint32_t bringUpStreaming(BlockingClient& c) {
  c.connect();
  auto cams = c.waitForDiscovery();
  if (cams.empty()) throw std::runtime_error("No cameras");

  TestCameraCfg cfg;
  c.sendAndExpectStatus([&] {
    return c.raw().configureCameras(cfg.width, cfg.height, cfg.crop_width,
                                    cfg.crop_height, cfg.crop_left,
                                    cfg.crop_top);
  });
  c.sendAndExpectStatus([&] { return c.raw().startCameras(); });
  c.sendAndExpectStatus([&] { return c.raw().startStream(cams[0].id); });
  return cams[0].id;
}

}  // namespace

TEST(Protocol, FrameMetadataIsSane) {
  BlockingClient c(serverUrl());
  bringUpStreaming(c);
  auto frame = c.nextFrame(std::chrono::seconds(5));
  ASSERT_TRUE(frame);
  EXPECT_EQ(frame->pixel_format, kFourccYU12);
  EXPECT_GT(frame->width, 0u);
  EXPECT_GT(frame->height, 0u);
  EXPECT_GE(frame->bytes_per_line, frame->width);
  EXPECT_EQ(frame->data.size(),
            static_cast<std::size_t>(frame->bytes_per_line) * frame->height *
                3 / 2);
  c.sendAndExpectStatus([&] { return c.raw().stopCameras(); },
                        "", std::chrono::seconds(15));
}

// Adaptive rate may drop frames under backpressure, so we only require
// overall progress, not strict monotonicity between adjacent frames.
TEST(Protocol, FrameIdsAdvance) {
  BlockingClient c(serverUrl());
  bringUpStreaming(c);
  std::vector<std::uint32_t> ids;
  for (int i = 0; i < 10; ++i) {
    auto f = c.nextFrame(std::chrono::seconds(5));
    ids.push_back(f->frame_id);
  }
  EXPECT_GT(ids.back(), ids.front());
  c.sendAndExpectStatus([&] { return c.raw().stopCameras(); },
                        "", std::chrono::seconds(15));
}

TEST(Protocol, FramesSavedMonotonic) {
  BlockingClient c(serverUrl());
  bringUpStreaming(c);
  std::uint32_t last = 0;
  for (int i = 0; i < 10; ++i) {
    auto f = c.nextFrame(std::chrono::seconds(5));
    EXPECT_GE(f->frames_saved, last);
    last = f->frames_saved;
  }
  c.sendAndExpectStatus([&] { return c.raw().stopCameras(); },
                        "", std::chrono::seconds(15));
}

TEST(Protocol, HeaderOnlyMode) {
  BlockingClient c(serverUrl());
  c.connect();
  auto cams = c.waitForDiscovery();
  ASSERT_FALSE(cams.empty());

  TestCameraCfg cfg;
  c.sendAndExpectStatus([&] {
    return c.raw().configureCameras(cfg.width, cfg.height, cfg.crop_width,
                                    cfg.crop_height, cfg.crop_left,
                                    cfg.crop_top);
  });
  c.sendAndExpectStatus([&] { return c.raw().setHeaderOnly(true); });
  c.sendAndExpectStatus([&] { return c.raw().startCameras(); });
  c.sendAndExpectStatus([&] { return c.raw().startStream(cams[0].id); });

  // Drop the first frame — may pre-date the toggle.
  (void)c.nextFrame(std::chrono::seconds(5));
  for (int i = 0; i < 3; ++i) {
    auto f = c.nextFrame(std::chrono::seconds(5));
    EXPECT_TRUE(f->header_only);
    EXPECT_EQ(f->data.size(), 0u);
    EXPECT_GT(f->width, 0u);
    EXPECT_GT(f->height, 0u);
  }
  c.sendAndExpectStatus([&] { return c.raw().stopCameras(); },
                        "", std::chrono::seconds(15));
}

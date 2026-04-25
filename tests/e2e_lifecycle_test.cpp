#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>

#include "harness.hpp"
#include "test_env.hpp"

using telefacet::test::BlockingClient;
using telefacet::test::CommandError;
using telefacet::test::TestCameraCfg;
using telefacet::test::serverUrl;
using telefacet::test::kFourccYU12;

namespace {

void configure(BlockingClient& c) {
  TestCameraCfg cfg;
  c.sendAndExpectStatus([&] {
    return c.raw().configureCameras(cfg.width, cfg.height, cfg.crop_width,
                                    cfg.crop_height, cfg.crop_left,
                                    cfg.crop_top);
  });
}

}  // namespace

// Full happy-path: connect → discover → configure → start → stream → stop →
// unconfigure. Each teardown leaves the server in IDLE.
TEST(Lifecycle, HappyPath) {
  BlockingClient c(serverUrl());
  c.connect();

  auto cams = c.waitForDiscovery();
  ASSERT_FALSE(cams.empty()) << "No cameras reported by the server";

  configure(c);

  c.sendAndExpectStatus([&] { return c.raw().startCameras(); });
  c.sendAndExpectStatus([&] { return c.raw().startStream(cams[0].id); });

  auto frame = c.nextFrame(std::chrono::seconds(5));
  ASSERT_TRUE(frame != nullptr);
  EXPECT_EQ(frame->camera_id, cams[0].id);
  // libcamera snaps requested resolution to the nearest supported sensor mode
  // (e.g. 1456×1088 → 2328×1748 on IMX519), so assert structural invariants
  // instead of pinning to the requested cfg values.
  EXPECT_GT(frame->width, 0u);
  EXPECT_GT(frame->height, 0u);
  EXPECT_GE(frame->bytes_per_line, frame->width);
  EXPECT_EQ(frame->pixel_format, kFourccYU12);
  EXPECT_FALSE(frame->header_only);
  EXPECT_EQ(frame->data.size(),
            static_cast<std::size_t>(frame->bytes_per_line) * frame->height *
                3 / 2);

  c.sendAndExpectStatus([&] { return c.raw().stopStream(cams[0].id); });
  c.sendAndExpectStatus([&] { return c.raw().stopCameras(); },
                        /*expect_substring=*/"", std::chrono::seconds(15));
  c.sendAndExpectStatus([&] { return c.raw().unconfigure(); });
}

// Re-configure is rejected in CONFIGURED and RUNNING; `unconfigure` is the
// documented path back to IDLE.
TEST(Lifecycle, ReconfigureRequiresUnconfigure) {
  BlockingClient c(serverUrl());
  c.connect();
  ASSERT_FALSE(c.waitForDiscovery().empty());

  configure(c);

  // In CONFIGURED — configure must error.
  EXPECT_THROW(configure(c), CommandError);

  // unconfigure → IDLE → configure succeeds again.
  c.sendAndExpectStatus([&] { return c.raw().unconfigure(); });
  configure(c);

  // In RUNNING — configure must also error.
  c.sendAndExpectStatus([&] { return c.raw().startCameras(); });
  EXPECT_THROW(configure(c), CommandError);
  c.sendAndExpectStatus([&] { return c.raw().stopCameras(); },
                        "", std::chrono::seconds(15));
}

// unconfigure is rejected outside CONFIGURED.
TEST(Lifecycle, UnconfigureRequiresConfigured) {
  BlockingClient c(serverUrl());
  c.connect();
  ASSERT_FALSE(c.waitForDiscovery().empty());

  // IDLE — unconfigure must error.
  EXPECT_THROW(
      c.sendAndExpectStatus([&] { return c.raw().unconfigure(); }),
      CommandError);

  // RUNNING — unconfigure must error.
  configure(c);
  c.sendAndExpectStatus([&] { return c.raw().startCameras(); });
  EXPECT_THROW(
      c.sendAndExpectStatus([&] { return c.raw().unconfigure(); }),
      CommandError);
  c.sendAndExpectStatus([&] { return c.raw().stopCameras(); },
                        "", std::chrono::seconds(15));
}

// start_stream with a bogus camera id must error.
TEST(Lifecycle, UnknownCameraIdErrors) {
  BlockingClient c(serverUrl());
  c.connect();
  auto cams = c.waitForDiscovery();
  ASSERT_FALSE(cams.empty());

  configure(c);
  c.sendAndExpectStatus([&] { return c.raw().startCameras(); });

  std::uint32_t bad_id = 0;
  for (const auto& cam : cams) bad_id = std::max(bad_id, cam.id);
  bad_id += 999;
  EXPECT_THROW(
      c.sendAndExpectStatus([&] { return c.raw().startStream(bad_id); }),
      CommandError);

  c.sendAndExpectStatus([&] { return c.raw().stopCameras(); },
                        "", std::chrono::seconds(15));
}

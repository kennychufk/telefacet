// Client-side capture handling that needs no server: the stacked-image helpers
// (display rendering, .npy) and the CaptureSaver's files.

#include <gtest/gtest.h>

#include <unistd.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "data/CaptureSaver.hpp"
#include "data/StackImage.hpp"
#include "net/Protocol.hpp"

namespace {

using telefacet::data::CaptureSaveState;
using telefacet::data::CaptureSaver;
using telefacet::data::FrameBuffer;
namespace fs = std::filesystem;
namespace proto = telefacet::proto;

// An SF32 frame of `w` x `h` samples, `pad` float32s of row padding.
std::shared_ptr<FrameBuffer> stackFrame(std::uint32_t w, std::uint32_t h,
                                        const std::vector<float>& px,
                                        std::uint32_t pad = 0) {
  auto f = std::make_shared<FrameBuffer>();
  f->width = w;
  f->height = h;
  f->bytes_per_line = (w + pad) * 4;
  f->pixel_format = proto::kPixelFormatSF32;
  f->frame_kind = static_cast<std::uint8_t>(proto::FrameKind::Stack);
  f->data.assign(static_cast<std::size_t>(f->bytes_per_line) * h, 0xEE);
  for (std::uint32_t y = 0; y < h; ++y)
    std::memcpy(f->data.data() + y * f->bytes_per_line, px.data() + y * w, w * 4);
  return f;
}

std::string slurp(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

fs::path scratchDir(const std::string& label) {
  auto d = fs::temp_directory_path() /
           ("telefacet_" + label + "_" + std::to_string(::getpid()));
  fs::remove_all(d);
  return d;
}

// The single session directory a saver made under `root`.
fs::path sessionDir(const fs::path& root) {
  for (const auto& e : fs::directory_iterator(root)) return e.path();
  return {};
}

}  // namespace

TEST(StackImage, NpyHeaderIsAlignedAndDescribesShape) {
  const auto h = telefacet::data::npyHeader(1520, 2028);
  ASSERT_GE(h.size(), 10u);
  EXPECT_EQ(h.size() % 64, 0u);
  EXPECT_EQ(h[0], 0x93);
  EXPECT_EQ(std::string(h.begin() + 1, h.begin() + 6), "NUMPY");
  EXPECT_EQ(h[6], 1);  // format 1.0
  const std::size_t len = h[8] | (h[9] << 8);
  EXPECT_EQ(len + 10, h.size());
  const std::string text(h.begin() + 10, h.end());
  EXPECT_NE(text.find("'descr': '<f4'"), std::string::npos);
  EXPECT_NE(text.find("'shape': (1520, 2028)"), std::string::npos);
  EXPECT_EQ(text.back(), '\n');
}

TEST(StackImage, NpyDropsRowPadding) {
  auto f = stackFrame(3, 2, {1, 2, 3, 4, 5, 6}, /*pad*/ 2);
  const auto npy = telefacet::data::stackImageNpy(*f);
  const auto hdr = telefacet::data::npyHeader(2, 3).size();
  ASSERT_EQ(npy.size(), hdr + 6 * 4);
  float v[6];
  std::memcpy(v, npy.data() + hdr, sizeof(v));
  for (int i = 0; i < 6; ++i) EXPECT_FLOAT_EQ(v[i], i + 1.0f);
}

TEST(StackImage, GrayStretchesWhiteAndClampsBelowBlack) {
  // Mostly 1000 DN, one dark and one below-black sample.
  std::vector<float> px(100, 1000.0f);
  px[0] = 0.0f;
  px[1] = -50.0f;
  px[2] = NAN;
  auto f = stackFrame(10, 10, px);
  std::vector<std::uint8_t> gray;
  const float white = telefacet::data::renderStackGray(*f, gray);
  EXPECT_NEAR(white, 1000.0f / telefacet::data::kStackWhitePoint, 1e-2f);
  ASSERT_EQ(gray.size(), 100u);
  EXPECT_EQ(gray[0], 0);
  EXPECT_EQ(gray[1], 0);
  EXPECT_EQ(gray[2], 0);
  // The board white lands at the white point through gamma 2.2: ~243.
  EXPECT_NEAR(gray[50], 255.0 * std::pow(0.9, 1 / 2.2), 1.5);
}

TEST(StackImage, TruncatedPayloadRendersNothing) {
  auto f = stackFrame(4, 4, std::vector<float>(16, 1.0f));
  f->data.resize(10);
  std::vector<std::uint8_t> gray;
  EXPECT_EQ(telefacet::data::renderStackGray(*f, gray), 0.0f);
  EXPECT_TRUE(gray.empty());
  EXPECT_TRUE(telefacet::data::stackImageNpy(*f).empty());
}

TEST(CaptureSaver, ServerDirName) {
  EXPECT_EQ(telefacet::data::serverDirName("ws://192.168.1.66:9001"),
            "192.168.1.66_9001");
  EXPECT_EQ(telefacet::data::serverDirName("wss://pi.local:9001/observer"),
            "pi.local_9001");
}

TEST(CaptureSaver, WritesTriggerStackAndHeaderOnly) {
  const auto root = scratchDir("captures");
  const std::string addr = "ws://10.0.0.5:9001";
  CaptureSaver saver(root.string());

  // A triggered I420 frame with one checkerboard corner.
  auto trig = std::make_shared<FrameBuffer>();
  trig->camera_id = 1;
  trig->frame_id = 412;
  trig->width = 4;
  trig->height = 2;
  trig->bytes_per_line = 4;
  trig->frame_kind = static_cast<std::uint8_t>(proto::FrameKind::Trigger);
  trig->capture_id = 3;
  trig->detection_kind = 1;
  trig->data.assign(12, 0x42);
  trig->corner_sets.push_back({0, 1, {{1.5f, 0.5f}}});
  auto trig_state = std::make_shared<std::atomic<CaptureSaveState>>(
      CaptureSaveState::NotSaved);
  saver.save(addr, trig, {{"camera_id", 1}, {"frame_id", 412}}, trig_state);

  // A stacked image.
  auto stack = stackFrame(2, 1, {10.0f, 20.0f});
  stack->camera_id = 0;
  stack->frame_id = 843;
  stack->capture_id = 1;
  saver.save(addr, stack, {{"camera_id", 0}, {"frames_stacked", 32}});

  // A header-only trigger (send_image false): sidecar only.
  auto bare = std::make_shared<FrameBuffer>(*trig);
  bare->frame_id = 500;
  bare->capture_id = 4;
  bare->header_only = true;
  bare->data.clear();
  saver.save(addr, bare, nullptr);

  saver.flush();
  EXPECT_EQ(trig_state->load(), CaptureSaveState::Saved);
  const auto st = saver.status();
  EXPECT_EQ(st.saved, 3u);
  EXPECT_EQ(st.errors, 0u);

  const auto dir = sessionDir(root) / "10.0.0.5_9001";
  EXPECT_NE(sessionDir(root).filename().string().find("-captures"),
            std::string::npos);
  EXPECT_EQ(slurp(dir / "cam1-412.yuv"), std::string(12, '\x42'));
  const auto tj = nlohmann::json::parse(slurp(dir / "cam1-412.json"));
  EXPECT_EQ(tj["kind"], "trigger");
  EXPECT_EQ(tj["trigger_id"], 3);
  EXPECT_EQ(tj["image"]["file"], "cam1-412.yuv");
  EXPECT_EQ(tj["image"]["stride"], 4);
  EXPECT_EQ(tj["detection"]["kind"], "checkerboard");
  EXPECT_FLOAT_EQ(tj["detection"]["sets"][0]["corners"][0][0].get<float>(), 1.5f);
  EXPECT_EQ(tj["result"]["frame_id"], 412);
  EXPECT_TRUE(tj["lens_position"].is_null());  // NaN on the wire

  const auto npy = slurp(dir / "cam0-843.npy");
  EXPECT_EQ(npy.substr(1, 5), "NUMPY");
  const auto sj = nlohmann::json::parse(slurp(dir / "cam0-843.json"));
  EXPECT_EQ(sj["kind"], "stack");
  EXPECT_EQ(sj["stack_id"], 1);
  EXPECT_EQ(sj["result"]["frames_stacked"], 32);

  EXPECT_FALSE(fs::exists(dir / "cam1-500.yuv"));
  const auto bj = nlohmann::json::parse(slurp(dir / "cam1-500.json"));
  EXPECT_TRUE(bj["image"].is_null());
  EXPECT_TRUE(bj["result"].is_null());
  fs::remove_all(root);
}

TEST(CaptureSaver, RepeatedCaptureIdStartsANewRun) {
  // start_cameras resets capture ids, so trigger #1 can come twice for one
  // camera; the second must not overwrite the first.
  const auto root = scratchDir("runs");
  CaptureSaver saver(root.string());
  auto f = std::make_shared<FrameBuffer>();
  f->frame_kind = static_cast<std::uint8_t>(proto::FrameKind::Trigger);
  f->capture_id = 1;
  f->frame_id = 10;
  f->header_only = true;
  saver.save("ws://h:1", f, nullptr);
  saver.save("ws://h:1", f, nullptr);
  saver.flush();
  const auto dir = sessionDir(root) / "h_1";
  EXPECT_TRUE(fs::exists(dir / "cam0-10.json"));
  EXPECT_TRUE(fs::exists(dir / "cam0-10-run1.json"));
  fs::remove_all(root);
}

TEST(CaptureSaver, DisabledWritesNothing) {
  const auto root = scratchDir("off");
  CaptureSaver saver(root.string(), /*enabled*/ false);
  auto f = std::make_shared<FrameBuffer>();
  f->frame_kind = static_cast<std::uint8_t>(proto::FrameKind::Trigger);
  auto state = std::make_shared<std::atomic<CaptureSaveState>>(
      CaptureSaveState::NotSaved);
  saver.save("ws://h:1", f, nullptr, state);
  saver.flush();
  EXPECT_FALSE(fs::exists(root));
  EXPECT_EQ(state->load(), CaptureSaveState::NotSaved);
}

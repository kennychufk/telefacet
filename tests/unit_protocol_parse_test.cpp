// Byte-level ChunkReassembler parsing tests. Unlike the e2e_* suites these hit
// no hardware: they hand-build CHUN start messages (header-only, so no CHNK
// data packets are needed) and feed them straight into ChunkReassembler,
// asserting the parsed detection block lands in the right FrameBuffer field.

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

#include "data/FrameBufferPool.hpp"
#include "net/ChunkReassembler.hpp"
#include "net/Protocol.hpp"

namespace {

using telefacet::data::FrameBuffer;
using telefacet::data::FrameBufferPool;
using telefacet::net::ChunkReassembler;
namespace proto = telefacet::proto;

// Append raw bytes of a trivially-copyable value to a byte buffer.
template <typename T>
void appendPod(std::vector<std::uint8_t>& out, const T& v) {
  const auto* p = reinterpret_cast<const std::uint8_t*>(&v);
  out.insert(out.end(), p, p + sizeof(T));
}

void appendXY(std::vector<std::uint8_t>& out, float x, float y) {
  appendPod(out, x);
  appendPod(out, y);
}

// Build a header-only CHUN start message (total_chunks/total_size = 0, so the
// reassembler emits the frame immediately) carrying `block` as the detection
// block. `version`, `detection_kind` and `num_corner_sets` are caller-set so a
// test can exercise the version gate and the parse branch.
std::vector<std::uint8_t> buildStart(std::uint32_t version,
                                     std::uint8_t detection_kind,
                                     std::uint16_t num_sets,
                                     const std::vector<std::uint8_t>& block,
                                     std::int32_t focus_fom = -1,
                                     std::uint8_t frame_kind = 0,
                                     std::uint16_t capture_id = 0) {
  proto::ChunkStartMarker marker{};
  marker.magic = proto::kChunkStartMagic;
  marker.version = version;

  proto::ChunkHeader hdr{};
  hdr.frame_uuid = 0xABCD1234u;
  hdr.frame_id = 42;
  hdr.camera_id = 0;
  hdr.total_chunks = 0;  // header-only ⇒ delivered without CHNK packets
  hdr.total_size = 0;
  hdr.width = 640;
  hdr.height = 480;
  hdr.bytes_per_line = 640;
  hdr.pixel_format = 0x32315559u;
  hdr.corner_block_size = static_cast<std::uint32_t>(block.size());
  hdr.num_corner_sets = num_sets;
  hdr.detection_kind = detection_kind;
  hdr.focus_fom = focus_fom;
  hdr.frame_kind = frame_kind;
  hdr.capture_id = capture_id;

  std::vector<std::uint8_t> out;
  appendPod(out, marker);
  appendPod(out, hdr);
  out.insert(out.end(), block.begin(), block.end());
  return out;
}

// Drive one start message through a reassembler and return the emitted frame.
std::shared_ptr<FrameBuffer> parseOne(const std::vector<std::uint8_t>& msg) {
  FrameBufferPool pool;
  std::shared_ptr<FrameBuffer> got;
  ChunkReassembler reasm(pool, [&](std::shared_ptr<FrameBuffer> b) {
    got = std::move(b);
  });
  reasm.onBinary(msg.data(), msg.size());
  return got;
}

}  // namespace

TEST(ProtocolParse, ArucoMarkerBlockParses) {
  // Two markers: id 17 quadrant 0, id 3 quadrant 2. 4 corners each.
  std::vector<std::uint8_t> block;
  proto::MarkerSetHeader m0{};
  m0.marker_id = 17;
  m0.quadrant = 0;
  m0.flags = 0x01;
  m0.num_corners = 4;
  appendPod(block, m0);
  appendXY(block, 10, 20);
  appendXY(block, 50, 20);
  appendXY(block, 50, 60);
  appendXY(block, 10, 60);

  proto::MarkerSetHeader m1{};
  m1.marker_id = 3;
  m1.quadrant = 2;
  m1.flags = 0x01;
  m1.num_corners = 4;
  appendPod(block, m1);
  appendXY(block, 100, 110);
  appendXY(block, 140, 110);
  appendXY(block, 140, 150);
  appendXY(block, 100, 150);

  auto f = parseOne(buildStart(8, /*aruco*/ 2, /*num_sets*/ 2, block));
  ASSERT_TRUE(f);
  EXPECT_TRUE(f->corner_sets.empty());
  ASSERT_EQ(f->aruco_markers.size(), 2u);

  EXPECT_EQ(f->aruco_markers[0].marker_id, 17);
  EXPECT_EQ(f->aruco_markers[0].quadrant, 0u);
  EXPECT_EQ(f->aruco_markers[0].flags & 0x01, 0x01);
  ASSERT_EQ(f->aruco_markers[0].corners.size(), 4u);
  EXPECT_FLOAT_EQ(f->aruco_markers[0].corners[0][0], 10.0f);
  EXPECT_FLOAT_EQ(f->aruco_markers[0].corners[2][1], 60.0f);

  EXPECT_EQ(f->aruco_markers[1].marker_id, 3);
  EXPECT_EQ(f->aruco_markers[1].quadrant, 2u);
  EXPECT_FLOAT_EQ(f->aruco_markers[1].corners[1][0], 140.0f);
}

TEST(ProtocolParse, CheckerboardBlockStillParses) {
  // detection_kind == 1 must land in corner_sets, leaving aruco_markers empty.
  std::vector<std::uint8_t> block;
  proto::CornerSetHeader csh{};
  csh.set_id = 0;
  csh.flags = 0x01;
  csh.num_corners = 3;
  appendPod(block, csh);
  appendXY(block, 1, 2);
  appendXY(block, 3, 4);
  appendXY(block, 5, 6);

  auto f = parseOne(buildStart(8, /*checkerboard*/ 1, /*num_sets*/ 1, block));
  ASSERT_TRUE(f);
  EXPECT_TRUE(f->aruco_markers.empty());
  ASSERT_EQ(f->corner_sets.size(), 1u);
  ASSERT_EQ(f->corner_sets[0].corners.size(), 3u);
  EXPECT_FLOAT_EQ(f->corner_sets[0].corners[2][1], 6.0f);
}

TEST(ProtocolParse, NoDetectionLeavesBothEmpty) {
  auto f = parseOne(buildStart(8, /*none*/ 0, /*num_sets*/ 0, {}));
  ASSERT_TRUE(f);
  EXPECT_TRUE(f->corner_sets.empty());
  EXPECT_TRUE(f->aruco_markers.empty());
}

TEST(ProtocolParse, VersionGateRejectsV7) {
  // A v7 frame must be dropped (no callback) now that the client is v8-only:
  // offset 58 meant `reserved` there, and frame_kind 1 meant a `stack` mode.
  auto f = parseOne(buildStart(7, /*none*/ 0, /*num_sets*/ 0, {}));
  EXPECT_FALSE(f);
}

TEST(ProtocolParse, VersionGateAcceptsV8) {
  auto f = parseOne(buildStart(8, /*none*/ 0, /*num_sets*/ 0, {}));
  EXPECT_TRUE(f);
}

TEST(ProtocolParse, HeaderLayoutMatchesV8Offsets) {
  // §5.3: capture_id at 58, frame_kind at 66, focus_fom at 68.
  EXPECT_EQ(offsetof(proto::ChunkHeader, capture_id), 58u);
  EXPECT_EQ(offsetof(proto::ChunkHeader, frame_kind), 66u);
  EXPECT_EQ(offsetof(proto::ChunkHeader, focus_fom), 68u);
}

TEST(ProtocolParse, LiveFrameHasNoCaptureId) {
  auto f = parseOne(buildStart(8, /*none*/ 0, /*num_sets*/ 0, {}));
  ASSERT_TRUE(f);
  EXPECT_EQ(f->frame_kind, 0u);
  EXPECT_EQ(f->capture_id, 0u);
  EXPECT_EQ(f->detection_kind, 0u);
}

TEST(ProtocolParse, TriggerFrameCarriesCaptureId) {
  // A header-only triggered frame (send_image false) with its board corners.
  std::vector<std::uint8_t> block;
  proto::CornerSetHeader csh{};
  csh.flags = 0x01;
  csh.num_corners = 1;
  appendPod(block, csh);
  appendXY(block, 7, 8);
  auto f = parseOne(buildStart(8, /*checkerboard*/ 1, 1, block, 1234,
                               /*trigger*/ 2, /*capture_id*/ 3));
  ASSERT_TRUE(f);
  EXPECT_TRUE(f->header_only);
  EXPECT_EQ(f->frame_kind, 2u);
  EXPECT_TRUE(proto::isCaptureFrameKind(f->frame_kind));
  EXPECT_EQ(f->capture_id, 3u);
  EXPECT_EQ(f->detection_kind, 1u);
  ASSERT_EQ(f->corner_sets.size(), 1u);
  EXPECT_FLOAT_EQ(f->corner_sets[0].corners[0][1], 8.0f);
}

TEST(ProtocolParse, StackFrameReassemblesFromChunks) {
  // A 2x2 SF32 stacked image (frame_kind 1) split over two CHNK packets.
  const std::vector<float> px = {1.0f, 2.0f, 3.0f, 4.0f};
  std::vector<std::uint8_t> payload(px.size() * 4);
  std::memcpy(payload.data(), px.data(), payload.size());

  proto::ChunkStartMarker marker{proto::kChunkStartMagic, 8};
  proto::ChunkHeader hdr{};
  hdr.frame_uuid = 77;
  hdr.frame_id = 843;
  hdr.total_chunks = 2;
  hdr.total_size = static_cast<std::uint32_t>(payload.size());
  hdr.width = 2;
  hdr.height = 2;
  hdr.bytes_per_line = 8;
  hdr.pixel_format = proto::kPixelFormatSF32;
  hdr.frame_kind = 1;
  hdr.capture_id = 9;
  std::vector<std::uint8_t> start;
  appendPod(start, marker);
  appendPod(start, hdr);

  FrameBufferPool pool;
  std::shared_ptr<FrameBuffer> got;
  ChunkReassembler reasm(pool, [&](std::shared_ptr<FrameBuffer> b) {
    got = std::move(b);
  });
  reasm.onBinary(start.data(), start.size());
  for (std::uint32_t i = 0; i < 2; ++i) {
    proto::ChunkData cd{proto::kChunkDataMagic, 77, i, 8};
    std::vector<std::uint8_t> msg;
    appendPod(msg, cd);
    msg.insert(msg.end(), payload.begin() + i * 8, payload.begin() + i * 8 + 8);
    EXPECT_FALSE(got) << "frame emitted before its last chunk";
    reasm.onBinary(msg.data(), msg.size());
  }
  ASSERT_TRUE(got);
  EXPECT_EQ(got->frame_kind, 1u);
  EXPECT_EQ(got->capture_id, 9u);
  EXPECT_EQ(got->pixel_format, proto::kPixelFormatSF32);
  ASSERT_EQ(got->data.size(), 16u);
  float last;
  std::memcpy(&last, got->data.data() + 12, 4);
  EXPECT_FLOAT_EQ(last, 4.0f);
}

TEST(ProtocolParse, FocusFomIsParsed) {
  auto f = parseOne(buildStart(8, /*none*/ 0, /*num_sets*/ 0, {}, 98765));
  ASSERT_TRUE(f);
  EXPECT_EQ(f->focus_fom, 98765);
  // The region is not on the wire; the owning WebSocketClient stamps it.
  EXPECT_FALSE(f->focus_fom_region.valid);

  auto none = parseOne(buildStart(8, /*none*/ 0, /*num_sets*/ 0, {}));
  ASSERT_TRUE(none);
  EXPECT_EQ(none->focus_fom, -1);
}

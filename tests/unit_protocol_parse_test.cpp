// Byte-level ChunkReassembler parsing tests. Unlike the e2e_* suites these hit
// no hardware: they hand-build CHUN start messages (header-only, so no CHNK
// data packets are needed) and feed them straight into ChunkReassembler,
// asserting the parsed detection block lands in the right FrameBuffer field.

#include <gtest/gtest.h>

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
                                     const std::vector<std::uint8_t>& block) {
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

  auto f = parseOne(buildStart(6, /*aruco*/ 2, /*num_sets*/ 2, block));
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

  auto f = parseOne(buildStart(6, /*checkerboard*/ 1, /*num_sets*/ 1, block));
  ASSERT_TRUE(f);
  EXPECT_TRUE(f->aruco_markers.empty());
  ASSERT_EQ(f->corner_sets.size(), 1u);
  ASSERT_EQ(f->corner_sets[0].corners.size(), 3u);
  EXPECT_FLOAT_EQ(f->corner_sets[0].corners[2][1], 6.0f);
}

TEST(ProtocolParse, NoDetectionLeavesBothEmpty) {
  auto f = parseOne(buildStart(6, /*none*/ 0, /*num_sets*/ 0, {}));
  ASSERT_TRUE(f);
  EXPECT_TRUE(f->corner_sets.empty());
  EXPECT_TRUE(f->aruco_markers.empty());
}

TEST(ProtocolParse, VersionGateRejectsV5) {
  // A v5 frame must be dropped (no callback) now that the client is v6-only.
  auto f = parseOne(buildStart(5, /*none*/ 0, /*num_sets*/ 0, {}));
  EXPECT_FALSE(f);
}

TEST(ProtocolParse, VersionGateAcceptsV6) {
  auto f = parseOne(buildStart(6, /*none*/ 0, /*num_sets*/ 0, {}));
  EXPECT_TRUE(f);
}

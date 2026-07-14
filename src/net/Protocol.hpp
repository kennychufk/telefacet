#pragma once

// Wire protocol for cherupi-v4l2 chunked frame transport. Mirrors the packed
// structs in cherupi-v4l2/types.hpp. Multi-byte fields are little-endian; on
// any little-endian host (the only platforms we target) the structs map
// directly to their on-the-wire representation.
//
// Protocol v5 (see cherupi-v4l2/docs/websocket-protocol.md §8). The 68-byte
// ChunkHeader is a strict extension of the original v2 40-byte header — the
// first 40 bytes are byte-identical — plus an optional variable-size
// CornerBlock that follows the header in the same WS message when the server's
// save mode is `checkerboard` / `checkerboard2x2`.

#include <cstdint>

namespace telefacet::proto {

inline constexpr std::uint32_t kChunkStartMagic = 0x4348554Eu;  // 'CHUN'
inline constexpr std::uint32_t kChunkDataMagic  = 0x43484E4Bu;  // 'CHNK'
inline constexpr std::uint32_t kChunkVersion    = 5u;

// 8 bytes
struct ChunkStartMarker {
  std::uint32_t magic;
  std::uint32_t version;
} __attribute__((packed));

// 68 bytes. Fields 0..36 match the legacy v2 layout exactly; everything from
// timestamp_us onward was added across v3 (timing), v4 (corner block) and v5
// (per-frame focus metadata).
struct ChunkHeader {
  std::uint32_t frame_uuid;         //  0
  std::uint32_t frame_id;           //  4
  std::uint32_t camera_id;          //  8
  std::uint32_t total_chunks;       // 12  0 ⇒ header-only frame
  std::uint32_t total_size;         // 16
  std::uint32_t bytes_per_line;     // 20  stride of Y plane
  std::uint32_t width;              // 24
  std::uint32_t height;             // 28
  std::uint32_t pixel_format;       // 32  FourCC; 0x32315559 = YU12/I420
  std::uint32_t frames_saved;       // 36
  std::uint64_t timestamp_us;       // 40  monotonic HW capture timestamp (v3)
  std::uint32_t frame_duration_us;  // 48  IPA/ISP-reported duration (v3)
  std::uint32_t corner_block_size;  // 52  bytes of CornerBlock following (v4)
  std::uint16_t num_corner_sets;    // 56  CornerSetHeader count in block (v4)
  std::uint16_t reserved;           // 58  always 0
  float         lens_position;      // 60  dioptres; NaN if unavailable (v5)
  std::uint8_t  af_state;           // 64  libcamera AfState; 0xFF if none (v5)
  std::uint8_t  reserved2[3];       // 65  padding; always 0
} __attribute__((packed));

// 4 bytes — header for one set of checkerboard corners inside the CornerBlock.
// Followed by num_corners × { float x; float y; } (full-frame Y-plane pixels).
struct CornerSetHeader {
  std::uint8_t  set_id;       // 0 for `checkerboard`; row*2+col (0..3) for 2x2
  std::uint8_t  flags;        // bit 0 ⇒ full-frame Y-plane coords (always 1)
  std::uint16_t num_corners;  // = checkerboard_rows × checkerboard_cols
} __attribute__((packed));

// 16 bytes (followed by chunk_size payload bytes)
struct ChunkData {
  std::uint32_t magic;
  std::uint32_t frame_uuid;
  std::uint32_t chunk_index;
  std::uint32_t chunk_size;
} __attribute__((packed));

// Minimum size of a start message: marker + header. A CornerBlock of
// corner_block_size bytes may follow in the same message, so the total is
// kChunkStartMinSize + ChunkHeader::corner_block_size.
inline constexpr std::size_t kChunkStartMinSize =
    sizeof(ChunkStartMarker) + sizeof(ChunkHeader);  // 76 bytes
inline constexpr std::size_t kChunkDataHeaderSize = sizeof(ChunkData);  // 16

// JSON command names accepted by the server (cherupi-v4l2 types.hpp Protocol).
namespace cmd {
inline constexpr const char* kDiscover         = "discover";
inline constexpr const char* kConfigure        = "configure";
inline constexpr const char* kUnconfigure      = "unconfigure";
inline constexpr const char* kSetSaveMode      = "set_save_mode";
inline constexpr const char* kStartCameras     = "start_cameras";
inline constexpr const char* kStartStream      = "start_stream";
inline constexpr const char* kStopStream       = "stop_stream";
inline constexpr const char* kStopCameras      = "stop_cameras";
inline constexpr const char* kResetFrameCounts = "reset_frame_counts";
inline constexpr const char* kSetHeaderOnly    = "set_header_only";
}  // namespace cmd

}  // namespace telefacet::proto

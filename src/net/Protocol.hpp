#pragma once

// Wire protocol for cherupi-v4l2 chunked frame transport. Mirrors the packed
// structs in cherupi-v4l2/types.hpp. Multi-byte fields are little-endian; on
// any little-endian host (the only platforms we target) the structs map
// directly to their on-the-wire representation.

#include <cstdint>

namespace telefacet::proto {

inline constexpr std::uint32_t kChunkStartMagic = 0x4348554Eu;  // 'CHUN'
inline constexpr std::uint32_t kChunkDataMagic  = 0x43484E4Bu;  // 'CHNK'
inline constexpr std::uint32_t kChunkVersion    = 2u;

// 8 bytes
struct ChunkStartMarker {
  std::uint32_t magic;
  std::uint32_t version;
} __attribute__((packed));

// 40 bytes
struct ChunkHeader {
  std::uint32_t frame_uuid;
  std::uint32_t frame_id;
  std::uint32_t camera_id;
  std::uint32_t total_chunks;
  std::uint32_t total_size;
  std::uint32_t bytes_per_line;
  std::uint32_t width;
  std::uint32_t height;
  std::uint32_t pixel_format;  // FourCC, little-endian; 0x32315559 = YU12/I420
  std::uint32_t frames_saved;
} __attribute__((packed));

// 16 bytes (followed by chunk_size payload bytes)
struct ChunkData {
  std::uint32_t magic;
  std::uint32_t frame_uuid;
  std::uint32_t chunk_index;
  std::uint32_t chunk_size;
} __attribute__((packed));

inline constexpr std::size_t kChunkStartTotalSize =
    sizeof(ChunkStartMarker) + sizeof(ChunkHeader);  // 44 bytes
inline constexpr std::size_t kChunkDataHeaderSize = sizeof(ChunkData);  // 16

// JSON command names accepted by the server (cherupi-v4l2 types.hpp Protocol).
namespace cmd {
inline constexpr const char* kDiscover         = "discover";
inline constexpr const char* kConfigure        = "configure";
inline constexpr const char* kSetSaveMode      = "set_save_mode";
inline constexpr const char* kStartCameras     = "start_cameras";
inline constexpr const char* kStartStream      = "start_stream";
inline constexpr const char* kStopStream       = "stop_stream";
inline constexpr const char* kStopCameras      = "stop_cameras";
inline constexpr const char* kResetFrameCounts = "reset_frame_counts";
inline constexpr const char* kSetHeaderOnly    = "set_header_only";
}  // namespace cmd

}  // namespace telefacet::proto

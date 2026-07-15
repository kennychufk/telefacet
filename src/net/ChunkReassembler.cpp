#include "net/ChunkReassembler.hpp"

#include <spdlog/spdlog.h>

#include <cstring>

#include "net/Protocol.hpp"

namespace telefacet::net {

namespace {

// Parse the variable-size CornerBlock (protocol §5.4) that may follow the
// ChunkHeader in the same WS message. `block` points at the first byte after
// the header; `block_size` is ChunkHeader.corner_block_size. Bounds-checked
// against a malformed/oversized set. Coordinates are full-frame Y-plane pixels.
void parseCornerBlock(const std::uint8_t* block, std::size_t block_size,
                      std::uint16_t expected_sets,
                      std::vector<data::CornerSet>& out) {
  out.clear();
  if (block_size == 0 || expected_sets == 0) return;

  std::size_t off = 0;
  while (off + sizeof(proto::CornerSetHeader) <= block_size &&
         out.size() < static_cast<std::size_t>(expected_sets)) {
    proto::CornerSetHeader csh{};
    std::memcpy(&csh, block + off, sizeof(csh));
    off += sizeof(csh);
    const std::uint16_t num_corners = csh.num_corners;
    const std::size_t corner_bytes = static_cast<std::size_t>(num_corners) * 8;
    if (off + corner_bytes > block_size) {
      spdlog::error("corner block overrun: set {} claims {} corners, {} left",
                    out.size(), num_corners, block_size - off);
      return;
    }
    data::CornerSet set;
    set.set_id = csh.set_id;
    set.flags  = csh.flags;
    set.corners.resize(num_corners);
    for (std::uint16_t i = 0; i < num_corners; ++i) {
      float xy[2];
      std::memcpy(xy, block + off, sizeof(xy));
      set.corners[i] = {xy[0], xy[1]};
      off += sizeof(xy);
    }
    out.push_back(std::move(set));
  }
  if (out.size() != static_cast<std::size_t>(expected_sets)) {
    spdlog::warn("parsed {} corner sets, expected {}", out.size(),
                 expected_sets);
  }
}

// Parse the variable-size detection block (protocol §5.4.2) when
// detection_kind == Aruco. Sibling of parseCornerBlock: same block bytes and
// bounds-checking, but each record is a MarkerSetHeader (marker id + quadrant)
// followed by num_corners × {float x, float y}. Coordinates are full-frame
// Y-plane pixels, identical to the checkerboard path.
void parseMarkerBlock(const std::uint8_t* block, std::size_t block_size,
                      std::uint16_t expected_sets,
                      std::vector<data::ArucoMarker>& out) {
  out.clear();
  if (block_size == 0 || expected_sets == 0) return;

  std::size_t off = 0;
  while (off + sizeof(proto::MarkerSetHeader) <= block_size &&
         out.size() < static_cast<std::size_t>(expected_sets)) {
    proto::MarkerSetHeader msh{};
    std::memcpy(&msh, block + off, sizeof(msh));
    off += sizeof(msh);
    const std::uint16_t num_corners = msh.num_corners;
    const std::size_t corner_bytes = static_cast<std::size_t>(num_corners) * 8;
    if (off + corner_bytes > block_size) {
      spdlog::error("marker block overrun: marker {} claims {} corners, {} left",
                    out.size(), num_corners, block_size - off);
      return;
    }
    data::ArucoMarker marker;
    marker.marker_id = msh.marker_id;
    marker.quadrant  = msh.quadrant;
    marker.flags     = msh.flags;
    marker.corners.resize(num_corners);
    for (std::uint16_t i = 0; i < num_corners; ++i) {
      float xy[2];
      std::memcpy(xy, block + off, sizeof(xy));
      marker.corners[i] = {xy[0], xy[1]};
      off += sizeof(xy);
    }
    out.push_back(std::move(marker));
  }
  if (out.size() != static_cast<std::size_t>(expected_sets)) {
    spdlog::warn("parsed {} markers, expected {}", out.size(), expected_sets);
  }
}

}  // namespace

ChunkReassembler::ChunkReassembler(data::FrameBufferPool& pool,
                                   FrameCallback cb)
    : pool_(pool), cb_(std::move(cb)) {}

void ChunkReassembler::onBinary(const void* data, std::size_t len) {
  if (len < 8) {
    spdlog::warn("binary too small: {} bytes", len);
    return;
  }
  const auto* bytes = static_cast<const std::uint8_t*>(data);
  std::uint32_t magic;
  std::memcpy(&magic, bytes, sizeof(magic));
  if (magic == proto::kChunkStartMagic) {
    handleStart(bytes, len);
  } else if (magic == proto::kChunkDataMagic) {
    handleChunk(bytes, len);
  } else {
    spdlog::warn("unknown binary magic: 0x{:08x}", magic);
  }

  // Periodic sweep — keep timed-out buffers from leaking.
  const auto now = std::chrono::steady_clock::now();
  if (now - last_sweep_ > std::chrono::seconds(1)) {
    last_sweep_ = now;
    sweepStale(std::chrono::seconds(5));
  }
}

void ChunkReassembler::handleStart(const std::uint8_t* data, std::size_t len) {
  if (len < proto::kChunkStartMinSize) {
    spdlog::error("invalid chunk start size: {} (expected >= {})", len,
                  proto::kChunkStartMinSize);
    return;
  }
  proto::ChunkStartMarker marker{};
  proto::ChunkHeader hdr{};
  std::memcpy(&marker, data, sizeof(marker));
  std::memcpy(&hdr, data + sizeof(marker), sizeof(hdr));
  const std::uint32_t version = marker.version;
  if (version != proto::kChunkVersion) {
    spdlog::error("unsupported chunk version: {}", version);
    return;
  }

  // Copy packed fields to locals before referencing/passing.
  const std::uint32_t frame_uuid        = hdr.frame_uuid;
  const std::uint32_t frame_id          = hdr.frame_id;
  const std::uint32_t camera_id         = hdr.camera_id;
  const std::uint32_t width             = hdr.width;
  const std::uint32_t height            = hdr.height;
  const std::uint32_t bytes_per_line    = hdr.bytes_per_line;
  const std::uint32_t pixel_format      = hdr.pixel_format;
  const std::uint32_t frames_saved      = hdr.frames_saved;
  const std::uint32_t total_chunks_v    = hdr.total_chunks;
  const std::uint32_t total_size_v      = hdr.total_size;
  const std::uint64_t timestamp_us      = hdr.timestamp_us;
  const std::uint32_t frame_duration_us = hdr.frame_duration_us;
  const std::uint32_t corner_block_size = hdr.corner_block_size;
  const std::uint16_t num_corner_sets   = hdr.num_corner_sets;
  const float         lens_position     = hdr.lens_position;
  const std::uint8_t  af_state          = hdr.af_state;
  const std::uint8_t  detection_kind    = hdr.detection_kind;

  // The start message carries the header plus an optional detection block. Its
  // byte size is corner_block_size regardless of detection_kind, so the length
  // invariant is unchanged from the checkerboard-only path.
  if (len != proto::kChunkStartMinSize + corner_block_size) {
    spdlog::error("chunk start length mismatch: got {}, expected {}", len,
                  proto::kChunkStartMinSize + corner_block_size);
    return;
  }
  // Branch on detection_kind: checkerboard fills corner_sets, aruco fills
  // aruco_markers. Whichever detector didn't run leaves its vector empty.
  std::vector<data::CornerSet> corner_sets;
  std::vector<data::ArucoMarker> aruco_markers;
  const auto* block = data + proto::kChunkStartMinSize;
  switch (static_cast<proto::DetectionKind>(detection_kind)) {
    case proto::DetectionKind::Checkerboard:
      parseCornerBlock(block, corner_block_size, num_corner_sets, corner_sets);
      break;
    case proto::DetectionKind::Aruco:
      parseMarkerBlock(block, corner_block_size, num_corner_sets, aruco_markers);
      break;
    case proto::DetectionKind::None:
      break;  // no block
  }

  // Populate the frame metadata shared by both delivery paths. Buffers come
  // from a pool that does not clear fields, so every field is set explicitly.
  auto fill = [&](data::FrameBuffer& b) {
    b.frame_id          = frame_id;
    b.camera_id         = camera_id;
    b.width             = width;
    b.height            = height;
    b.bytes_per_line    = bytes_per_line;
    b.pixel_format      = pixel_format;
    b.frames_saved      = frames_saved;
    b.timestamp_us      = timestamp_us;
    b.frame_duration_us = frame_duration_us;
    b.lens_position     = lens_position;
    b.af_state          = af_state;
    b.corner_sets       = corner_sets;
    b.aruco_markers     = aruco_markers;
  };

  // Header-only frame (set_header_only mode): no CHNK packets follow.
  if (total_chunks_v == 0 && total_size_v == 0) {
    auto buf = pool_.acquire(0);
    fill(*buf);
    buf->header_only = true;
    if (cb_) cb_(std::move(buf));
    return;
  }

  // Multi-chunk frame: hold metadata + per-chunk staging until completion.
  InFlight inf;
  inf.buf = pool_.acquire(0);
  fill(*inf.buf);
  inf.buf->header_only = false;
  inf.total_chunks     = total_chunks_v;
  inf.total_size       = total_size_v;
  inf.chunks.resize(total_chunks_v);
  in_flight_.emplace(frame_uuid, std::move(inf));
}

void ChunkReassembler::handleChunk(const std::uint8_t* data, std::size_t len) {
  if (len < proto::kChunkDataHeaderSize) {
    spdlog::error("chunk data too small: {}", len);
    return;
  }
  proto::ChunkData chd{};
  std::memcpy(&chd, data, sizeof(chd));
  // Copy packed fields to locals so spdlog/formatters can take references.
  const std::uint32_t frame_uuid  = chd.frame_uuid;
  const std::uint32_t chunk_index = chd.chunk_index;
  const std::uint32_t chunk_size  = chd.chunk_size;
  if (len != proto::kChunkDataHeaderSize + chunk_size) {
    spdlog::error("chunk size mismatch: got {}, expected {}", len,
                  proto::kChunkDataHeaderSize + chunk_size);
    return;
  }
  auto it = in_flight_.find(frame_uuid);
  if (it == in_flight_.end()) {
    spdlog::warn("chunk for unknown frame uuid {}", frame_uuid);
    return;
  }
  auto& inf = it->second;
  if (chunk_index >= inf.total_chunks) {
    spdlog::error("chunk index {} out of range (total {})", chunk_index,
                  inf.total_chunks);
    return;
  }
  auto& slot = inf.chunks[chunk_index];
  if (!slot.empty()) {
    spdlog::warn("duplicate chunk {} for frame {}", chunk_index,
                 inf.buf->frame_id);
    return;
  }
  slot.assign(data + sizeof(chd), data + sizeof(chd) + chunk_size);
  inf.received++;

  if (inf.received == inf.total_chunks) {
    // Concatenate in chunk-index order into the destination FrameBuffer.
    inf.buf->data.resize(inf.total_size);
    std::size_t offset = 0;
    for (std::uint32_t i = 0; i < inf.total_chunks; ++i) {
      const auto& src = inf.chunks[i];
      if (offset + src.size() > inf.buf->data.size()) {
        spdlog::error("assembly overflow at chunk {}: off={} sz={} cap={}", i,
                      offset, src.size(), inf.buf->data.size());
        in_flight_.erase(it);
        return;
      }
      std::memcpy(inf.buf->data.data() + offset, src.data(), src.size());
      offset += src.size();
    }
    if (offset != inf.total_size) {
      spdlog::warn("assembled size {} != declared {}", offset, inf.total_size);
    }
    auto out = std::move(inf.buf);
    in_flight_.erase(it);
    if (cb_) cb_(std::move(out));
  }
}

void ChunkReassembler::sweepStale(std::chrono::milliseconds timeout) {
  const auto now = std::chrono::steady_clock::now();
  for (auto it = in_flight_.begin(); it != in_flight_.end();) {
    if (now - it->second.started > timeout) {
      spdlog::warn("dropping stale frame uuid {} ({}/{} chunks)", it->first,
                   it->second.received, it->second.total_chunks);
      pool_.release(std::move(it->second.buf));
      it = in_flight_.erase(it);
    } else {
      ++it;
    }
  }
}

void ChunkReassembler::reset() {
  for (auto& kv : in_flight_) pool_.release(std::move(kv.second.buf));
  in_flight_.clear();
}

}  // namespace telefacet::net

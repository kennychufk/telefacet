#include "net/ChunkReassembler.hpp"

#include <spdlog/spdlog.h>

#include <cstring>

#include "net/Protocol.hpp"

namespace telefacet::net {

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
  if (len != proto::kChunkStartTotalSize) {
    spdlog::error("invalid chunk start size: {} (expected {})", len,
                  proto::kChunkStartTotalSize);
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

  // Header-only frame (set_header_only mode).
  if (hdr.total_chunks == 0 && hdr.total_size == 0) {
    auto buf = pool_.acquire(0);
    buf->frame_id       = hdr.frame_id;
    buf->camera_id      = hdr.camera_id;
    buf->width          = hdr.width;
    buf->height         = hdr.height;
    buf->bytes_per_line = hdr.bytes_per_line;
    buf->pixel_format   = hdr.pixel_format;
    buf->frames_saved   = hdr.frames_saved;
    buf->header_only    = true;
    if (cb_) cb_(std::move(buf));
    return;
  }

  // Copy packed fields to locals before referencing/passing.
  const std::uint32_t frame_uuid     = hdr.frame_uuid;
  const std::uint32_t frame_id       = hdr.frame_id;
  const std::uint32_t camera_id      = hdr.camera_id;
  const std::uint32_t width          = hdr.width;
  const std::uint32_t height         = hdr.height;
  const std::uint32_t bytes_per_line = hdr.bytes_per_line;
  const std::uint32_t pixel_format   = hdr.pixel_format;
  const std::uint32_t frames_saved   = hdr.frames_saved;
  const std::uint32_t total_chunks_v = hdr.total_chunks;
  const std::uint32_t total_size_v   = hdr.total_size;

  // Multi-chunk frame: hold metadata + per-chunk staging until completion.
  InFlight inf;
  inf.buf = pool_.acquire(0);
  inf.buf->frame_id       = frame_id;
  inf.buf->camera_id      = camera_id;
  inf.buf->width          = width;
  inf.buf->height         = height;
  inf.buf->bytes_per_line = bytes_per_line;
  inf.buf->pixel_format   = pixel_format;
  inf.buf->frames_saved   = frames_saved;
  inf.buf->header_only    = false;
  inf.total_chunks        = total_chunks_v;
  inf.total_size          = total_size_v;
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

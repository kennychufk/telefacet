#pragma once

// Reassembles chunked binary frames from cherupi-v4l2. Mirrors the JS
// implementation in telefacet-web/src/services/WebSocketManager.js.

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

#include "data/FrameBufferPool.hpp"

namespace telefacet::net {

class ChunkReassembler {
 public:
  using FrameCallback =
      std::function<void(std::shared_ptr<data::FrameBuffer>)>;

  explicit ChunkReassembler(data::FrameBufferPool& pool, FrameCallback cb);

  // Feed a binary WebSocket message; thread-safe to call from a single
  // network thread (per-connection only).
  void onBinary(const void* data, std::size_t len);

  // Drop in-flight buffers older than `timeout`.
  void sweepStale(std::chrono::milliseconds timeout);

  // Wipe all in-flight buffers (e.g. on disconnect).
  void reset();

 private:
  struct InFlight {
    std::shared_ptr<data::FrameBuffer> buf;  // metadata only until assembly
    std::uint32_t total_chunks = 0;
    std::uint32_t total_size   = 0;
    std::uint32_t received     = 0;
    // One byte vector per chunk index, populated as chunks arrive. We
    // concatenate in order on completion (matches the JS reassembler).
    std::vector<std::vector<std::uint8_t>> chunks;
    std::chrono::steady_clock::time_point started =
        std::chrono::steady_clock::now();
  };

  void handleStart(const std::uint8_t* data, std::size_t len);
  void handleChunk(const std::uint8_t* data, std::size_t len);

  data::FrameBufferPool& pool_;
  FrameCallback cb_;
  std::unordered_map<std::uint32_t, InFlight> in_flight_;  // by frame_uuid
  std::chrono::steady_clock::time_point last_sweep_ =
      std::chrono::steady_clock::now();
};

}  // namespace telefacet::net

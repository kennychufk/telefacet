#pragma once

// Recycles raw byte buffers used to hold one packed-Bayer frame. Avoids
// per-frame heap churn on the network thread.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace telefacet::data {

struct FrameBuffer {
  std::vector<std::uint8_t> data;  // packed bytes
  std::uint32_t frame_id       = 0;
  std::uint32_t camera_id      = 0;  // server-local id
  std::uint32_t width          = 0;
  std::uint32_t height         = 0;
  std::uint32_t bytes_per_line = 0;
  std::uint32_t frames_saved   = 0;
  bool          header_only    = false;
};

class FrameBufferPool {
 public:
  FrameBufferPool() = default;

  std::shared_ptr<FrameBuffer> acquire(std::size_t at_least_bytes);
  void release(std::shared_ptr<FrameBuffer> buf);

 private:
  std::mutex mu_;
  std::vector<std::shared_ptr<FrameBuffer>> free_;
};

}  // namespace telefacet::data

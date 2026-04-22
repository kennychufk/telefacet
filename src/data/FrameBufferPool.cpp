#include "data/FrameBufferPool.hpp"

namespace telefacet::data {

std::shared_ptr<FrameBuffer> FrameBufferPool::acquire(std::size_t at_least) {
  std::lock_guard<std::mutex> lk(mu_);
  if (!free_.empty()) {
    auto buf = std::move(free_.back());
    free_.pop_back();
    if (buf->data.size() < at_least) buf->data.resize(at_least);
    return buf;
  }
  auto buf = std::make_shared<FrameBuffer>();
  buf->data.resize(at_least);
  return buf;
}

void FrameBufferPool::release(std::shared_ptr<FrameBuffer> buf) {
  if (!buf) return;
  std::lock_guard<std::mutex> lk(mu_);
  // Cap pool growth so a transient burst doesn't permanently bloat memory.
  if (free_.size() < 16) free_.push_back(std::move(buf));
}

}  // namespace telefacet::data

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
  // Recycle only a buffer we are the sole owner of. If any other owner still
  // references it — the GL thread mid-upload holding a consumeIfNew() copy, or
  // a camera slot's latest-frame pointer — returning it to the free list would
  // let a network thread reacquire and overwrite its bytes while they are still
  // being read (glTexSubImage2D), tearing that frame. This is why two streams
  // garble but one doesn't: the shared pool + two independent network threads
  // make the reacquire-during-upload collision actually land. Dropping our
  // reference here is safe and non-leaking: the buffer returns to the pool on
  // whichever release later observes the final reference (e.g. eviction, once
  // the GL thread's copy is gone). Checked before the lock: the buffer is not
  // in free_ yet, so no acquire() can hand it out, and once it has left its
  // slot it can never be re-referenced — so use_count()==1 is a stable answer.
  if (buf.use_count() > 1) return;
  std::lock_guard<std::mutex> lk(mu_);
  // Cap pool growth so a transient burst doesn't permanently bloat memory.
  if (free_.size() < 16) free_.push_back(std::move(buf));
}

}  // namespace telefacet::data

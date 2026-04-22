#include "data/CameraStore.hpp"

#include <algorithm>

namespace telefacet::data {

void CameraStore::clear() {
  {
    std::lock_guard<std::mutex> lk(roster_mu_);
    roster_.clear();
  }
  std::lock_guard<std::mutex> lk(slots_mu_);
  slots_.clear();
  stats_.clear();
}

std::size_t CameraStore::addCamera(std::size_t server_index,
                                   std::uint32_t local_camera_id,
                                   const std::string& sensor_type) {
  std::lock_guard<std::mutex> lk(roster_mu_);
  // Replace existing entry on rediscovery.
  for (auto& c : roster_) {
    if (c.server_index == server_index &&
        c.local_camera_id == local_camera_id) {
      c.sensor_type = sensor_type;
      return c.global_id;
    }
  }
  CameraInfo info;
  info.server_index    = server_index;
  info.local_camera_id = local_camera_id;
  info.sensor_type     = sensor_type;
  info.global_id       = roster_.size();  // tentative; rebuild() finalizes
  roster_.push_back(info);
  return info.global_id;
}

void CameraStore::rebuildGlobalIdsSorted() {
  std::lock_guard<std::mutex> rlk(roster_mu_);
  std::sort(roster_.begin(), roster_.end(),
            [](const CameraInfo& a, const CameraInfo& b) {
              if (a.server_index != b.server_index)
                return a.server_index < b.server_index;
              return a.local_camera_id < b.local_camera_id;
            });
  std::lock_guard<std::mutex> slk(slots_mu_);
  slots_.clear();
  for (std::size_t i = 0; i < roster_.size(); ++i) {
    roster_[i].global_id = i;
    roster_[i].label     = "cam" + std::to_string(i);
    if (!stats_.count(i)) stats_[i] = std::make_unique<CameraLiveStats>();
  }
}

std::vector<CameraInfo> CameraStore::snapshot() const {
  std::lock_guard<std::mutex> lk(roster_mu_);
  return roster_;
}

CameraInfo* CameraStore::find(std::size_t global_id) {
  std::lock_guard<std::mutex> lk(roster_mu_);
  for (auto& c : roster_) {
    if (c.global_id == global_id) return &c;
  }
  return nullptr;
}

CameraInfo* CameraStore::findByServer(std::size_t server_index,
                                       std::uint32_t local_id) {
  std::lock_guard<std::mutex> lk(roster_mu_);
  for (auto& c : roster_) {
    if (c.server_index == server_index && c.local_camera_id == local_id)
      return &c;
  }
  return nullptr;
}

void CameraStore::publishFrame(std::size_t global_id,
                               std::shared_ptr<FrameBuffer> buf) {
  if (!buf) return;
  std::shared_ptr<FrameBuffer> evicted;
  {
    std::lock_guard<std::mutex> lk(slots_mu_);
    auto& slot = slots_[global_id];
    evicted = std::move(slot.latest);
    slot.latest = std::move(buf);
    slot.seq++;

    // Update live stats (frame_id, frames_saved, fps).
    auto it = stats_.find(global_id);
    if (it == stats_.end()) {
      it = stats_.emplace(global_id, std::make_unique<CameraLiveStats>())
               .first;
    }
    auto& s = *it->second;
    s.last_frame_id.store(slot.latest->frame_id, std::memory_order_relaxed);
    s.frames_saved.store(slot.latest->frames_saved,
                         std::memory_order_relaxed);
    {
      std::lock_guard<std::mutex> mlk(s.mu);
      s.fps_window_count++;
      const auto now = std::chrono::steady_clock::now();
      const auto elapsed_ms =
          std::chrono::duration_cast<std::chrono::milliseconds>(
              now - s.fps_window_start)
              .count();
      if (elapsed_ms >= 1000) {
        const float fps = (s.fps_window_count * 1000.0f) /
                          static_cast<float>(elapsed_ms);
        s.fps.store(fps, std::memory_order_relaxed);
        s.fps_window_count = 0;
        s.fps_window_start = now;
      }
    }
  }
  if (evicted) pool_.release(std::move(evicted));
}

std::shared_ptr<FrameBuffer> CameraStore::consumeIfNew(
    std::size_t global_id, std::uint64_t& seen_seq_inout) {
  std::lock_guard<std::mutex> lk(slots_mu_);
  auto it = slots_.find(global_id);
  if (it == slots_.end() || !it->second.latest) return nullptr;
  if (it->second.seq == seen_seq_inout) return nullptr;
  seen_seq_inout = it->second.seq;
  return it->second.latest;
}

CameraLiveStats* CameraStore::stats(std::size_t global_id) {
  std::lock_guard<std::mutex> lk(slots_mu_);
  auto it = stats_.find(global_id);
  return it == stats_.end() ? nullptr : it->second.get();
}

}  // namespace telefacet::data

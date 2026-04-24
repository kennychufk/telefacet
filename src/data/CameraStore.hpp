#pragma once

// CameraStore — global camera id ↔ latest frame slot + per-camera live stats.
// The network threads publish frames; the GL thread consumes them.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "data/FrameBufferPool.hpp"

namespace telefacet::data {

struct CameraInfo {
  std::size_t global_id = 0;
  std::size_t server_index = 0;
  std::uint32_t local_camera_id = 0;
  std::string label;            // e.g. "cam0"
  std::string sensor_type;      // from discovery, e.g. "IMX296"

  // Streaming UI flags (mutated from main/UI thread).
  bool streaming = false;
};

struct CameraLiveStats {
  std::atomic<std::uint32_t> last_frame_id{0};
  std::atomic<std::uint32_t> frames_saved{0};
  std::atomic<float>          fps{0.0f};

  // Internal FPS computation
  std::mutex mu;
  std::uint64_t fps_window_count = 0;
  std::chrono::steady_clock::time_point fps_window_start =
      std::chrono::steady_clock::now();
};

class CameraStore {
 public:
  // Lookup / mutation of the camera roster (called from main thread).
  void clear();
  std::size_t addCamera(std::size_t server_index,
                        std::uint32_t local_camera_id,
                        const std::string& sensor_type);
  void rebuildGlobalIdsSorted();
  std::vector<CameraInfo> snapshot() const;
  CameraInfo* find(std::size_t global_id);
  CameraInfo* findByServer(std::size_t server_index, std::uint32_t local_id);

  // Frame publication / consumption.
  void publishFrame(std::size_t global_id, std::shared_ptr<FrameBuffer> buf);
  // Returns the latest frame for `global_id`, or nullptr if no new frame
  // has arrived since the last call from this consumer (latest-wins semantics
  // — older frames are dropped).
  std::shared_ptr<FrameBuffer> consumeIfNew(std::size_t global_id,
                                            std::uint64_t& seen_seq_inout);

  // Live stats accessor (returns nullptr if camera not present).
  CameraLiveStats* stats(std::size_t global_id);

  FrameBufferPool& pool() { return pool_; }

 private:
  struct Slot {
    std::shared_ptr<FrameBuffer> latest;
    std::uint64_t seq = 0;  // monotonically increasing
  };

  mutable std::mutex roster_mu_;
  std::vector<CameraInfo> roster_;  // ordered by global_id

  std::mutex slots_mu_;  // guards slots_ map operations
  std::unordered_map<std::size_t, Slot> slots_;
  std::unordered_map<std::size_t, std::unique_ptr<CameraLiveStats>> stats_;

  FrameBufferPool pool_;
};

}  // namespace telefacet::data

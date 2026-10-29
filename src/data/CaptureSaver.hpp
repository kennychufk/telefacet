#pragma once

// CaptureSaver — writes every trigger / stack capture frame to this machine.
//
// The server saves neither (protocol v8: captures are commands that deliver
// their frames to the commander, §5.9), so this is the only copy. Mirrors
// telefacet-web's services/CaptureSaver.js, same layout and sidecar:
//
//   <root>/<YYYYMMDD-HHMMSS>-captures/<host_port>/cam<cam>-<frame_id>.yuv   (trigger)
//                                              /cam<cam>-<frame_id>.npy   (stack)
//                                              /cam<cam>-<frame_id>.json  (either)
//
// one session directory per CaptureSaver, made on the first capture and named
// to the second so two runs in one minute never write over each other. <cam>
// is the server-local camera id. The .yuv is the payload as received, stride
// padding included (yuv420_convert --stride reads it); the .npy is the float32
// stack with its row padding dropped. A header-only capture (send_image false)
// writes only the .json, which holds the geometry, the header's metadata, the
// detection the server ran on that frame, and the camera's entry from the
// trigger_result / stack_result.
//
// Capture ids restart at 1 on every start_cameras, so a capture seen again for
// the same camera starts a new run, and later runs' files carry `-run<r>`.
//
// Writes happen on one worker thread, so a 12 MB stack never stalls the
// network or GL thread that hands it over.

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <ctime>
#include <deque>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>

#include "data/CameraStore.hpp"
#include "data/FrameBufferPool.hpp"

namespace telefacet::data {

// `20261023-123846-captures`, in local time.
std::string captureSessionDirName(std::time_t t);
// `192.168.1.66_9001` for ws://192.168.1.66:9001 — the server's subdirectory.
std::string serverDirName(const std::string& address);
// The .json written beside a capture frame. `image_file` is empty for a
// header-only capture; `result` is the camera's result entry, or null.
nlohmann::json captureSidecar(const FrameBuffer& frame,
                              const nlohmann::json& result,
                              const std::string& address,
                              const std::string& image_file);

class CaptureSaver {
 public:
  explicit CaptureSaver(std::string root, bool enabled = true);
  ~CaptureSaver();  // writes what is queued, then stops

  CaptureSaver(const CaptureSaver&) = delete;
  CaptureSaver& operator=(const CaptureSaver&) = delete;

  const std::string& root() const { return root_; }
  void setEnabled(bool on) { enabled_.store(on); }
  bool enabled() const { return enabled_.load(); }

  // Queue one capture frame for writing and return at once. `state` (may be
  // null) follows the write: Saving, then Saved or Failed; it is left
  // NotSaved when saving is off.
  void save(const std::string& address, std::shared_ptr<const FrameBuffer> frame,
            nlohmann::json result,
            std::shared_ptr<std::atomic<CaptureSaveState>> state = nullptr);

  // Block until everything queued so far has been written (or failed).
  void flush();

  struct Status {
    bool        enabled = false;
    std::string session_path;  // empty until the first capture
    std::size_t saved   = 0;
    std::size_t errors  = 0;
    std::size_t pending = 0;
    std::string last_file;     // <host_port>/<name>, relative to the session
    std::string last_error;
  };
  Status status() const;

 private:
  struct Job {
    std::string                                     address;
    std::shared_ptr<const FrameBuffer>              frame;
    nlohmann::json                                  result;
    std::shared_ptr<std::atomic<CaptureSaveState>>  state;
  };
  struct ServerDir {
    std::string           subdir;
    int                   run = 0;
    std::set<std::string> seen;  // "<kind>:<capture_id>:<camera>" this run
  };

  void loop();
  // Returns the image file name, or the sidecar's for a header-only frame.
  // Throws std::runtime_error when something could not be written.
  std::string write(const Job& job);

  const std::string  root_;
  std::atomic<bool>  enabled_;

  mutable std::mutex      mu_;
  std::condition_variable cv_;       // work queued, or stopping
  std::condition_variable idle_cv_;  // queue drained
  std::deque<Job>         queue_;
  bool                    busy_ = false;  // the worker holds a job
  bool                    stop_ = false;
  Status                  status_;

  // Worker-thread only.
  std::string                                session_dir_;
  std::unordered_map<std::string, ServerDir> servers_;  // by address

  std::thread worker_;
};

}  // namespace telefacet::data

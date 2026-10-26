#include "data/CaptureSaver.hpp"

#include <spdlog/spdlog.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>

#include "data/StackImage.hpp"
#include "net/Protocol.hpp"

namespace telefacet::data {

namespace fs = std::filesystem;

namespace {

bool isStack(const FrameBuffer& f) {
  return f.frame_kind == static_cast<std::uint8_t>(proto::FrameKind::Stack);
}

const char* kindName(const FrameBuffer& f) {
  switch (static_cast<proto::FrameKind>(f.frame_kind)) {
    case proto::FrameKind::Stack:   return "stack";
    case proto::FrameKind::Trigger: return "trigger";
    default:                        return "live";
  }
}

const char* detectionName(std::uint8_t kind) {
  switch (static_cast<proto::DetectionKind>(kind)) {
    case proto::DetectionKind::Checkerboard: return "checkerboard";
    case proto::DetectionKind::Aruco:        return "aruco";
    default:                                 return "none";
  }
}

// `2026-10-24T03:12:45.123Z`
std::string isoUtcNow() {
  const auto now = std::chrono::system_clock::now();
  const std::time_t t = std::chrono::system_clock::to_time_t(now);
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      now.time_since_epoch()).count() % 1000;
  std::tm tm{};
  gmtime_r(&t, &tm);
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
                tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
                tm.tm_min, tm.tm_sec, static_cast<int>(ms));
  return buf;
}

nlohmann::json cornersJson(const std::vector<std::array<float, 2>>& cs) {
  nlohmann::json out = nlohmann::json::array();
  for (const auto& c : cs) out.push_back({c[0], c[1]});
  return out;
}

void writeFile(const fs::path& path, const std::uint8_t* data, std::size_t n) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) throw std::runtime_error("cannot open " + path.string());
  out.write(reinterpret_cast<const char*>(data),
            static_cast<std::streamsize>(n));
  out.close();
  if (!out) throw std::runtime_error("failed writing " + path.string());
}

}  // namespace

std::string captureSessionDirName(std::time_t t) {
  std::tm tm{};
  localtime_r(&t, &tm);
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%04d%02d%02d-%02d%02d%02d-captures",
                tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
                tm.tm_min, tm.tm_sec);
  return buf;
}

std::string serverDirName(const std::string& address) {
  std::string rest = address;
  for (const char* scheme : {"ws://", "wss://"}) {
    if (rest.rfind(scheme, 0) == 0) {
      rest = rest.substr(std::char_traits<char>::length(scheme));
      break;
    }
  }
  rest = rest.substr(0, rest.find('/'));
  for (char& c : rest) {
    const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                    (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
    if (!ok) c = '_';
  }
  return rest.empty() ? "server" : rest;
}

nlohmann::json captureSidecar(const FrameBuffer& f, const nlohmann::json& result,
                              const std::string& address,
                              const std::string& image_file) {
  const bool stack = isStack(f);
  nlohmann::json image = nullptr;
  if (!image_file.empty()) {
    if (stack) {
      image = {{"file", image_file},
               {"format",
                "float32 .npy, shape (height, width): per 2x2 Bayer quad, the "
                "mean raw value minus black, in left-justified 16-bit DN"},
               {"width", f.width},
               {"height", f.height}};
    } else {
      image = {{"file", image_file},
               {"format", "I420 (YUV420 planar, Y-plane stride padding preserved)"},
               {"width", f.width},
               {"height", f.height},
               {"stride", f.bytes_per_line},
               {"convert", "yuv420_convert --width " + std::to_string(f.width) +
                               " --height " + std::to_string(f.height) +
                               " --stride " + std::to_string(f.bytes_per_line) +
                               " " + image_file + " out.png"}};
    }
  }

  nlohmann::json detection = {
      {"kind", detectionName(f.detection_kind)},
      {"coordinates",
       stack ? "stack pixels: stack pixel (u, v) is centred on sensor pixel "
               "(2u + 0.5, 2v + 0.5)"
             : "frame pixels (full-resolution Y plane)"}};
  if (!f.aruco_markers.empty()) {
    nlohmann::json markers = nlohmann::json::array();
    for (const auto& m : f.aruco_markers)
      markers.push_back({{"marker_id", m.marker_id},
                         {"quadrant", m.quadrant},
                         {"corners", cornersJson(m.corners)}});
    detection["markers"] = std::move(markers);
  } else {
    nlohmann::json sets = nlohmann::json::array();
    for (const auto& cs : f.corner_sets)
      sets.push_back({{"set_id", cs.set_id}, {"corners", cornersJson(cs.corners)}});
    detection["sets"] = std::move(sets);
  }

  nlohmann::json j = {
      {"written_by", "telefacet"},
      {"saved_at", isoUtcNow()},
      {"server", address},
      {"kind", kindName(f)},
      {stack ? "stack_id" : "trigger_id", f.capture_id},
      {"camera_id", f.camera_id},
      {"frame_id", f.frame_id},
      {"timestamp_us", f.timestamp_us},
      {"frame_duration_us", f.frame_duration_us},
      {"lens_position", nullptr},
      {"af_state", nullptr},
      {"focus_fom", nullptr},
      {"image", std::move(image)},
      {"detection", std::move(detection)},
      {"result", result},
  };
  if (std::isfinite(f.lens_position)) j["lens_position"] = f.lens_position;
  if (f.af_state != 0xFF) j["af_state"] = f.af_state;
  if (f.focus_fom >= 0) j["focus_fom"] = f.focus_fom;
  return j;
}

CaptureSaver::CaptureSaver(std::string root, bool enabled)
    : root_(std::move(root)), enabled_(enabled) {
  worker_ = std::thread([this] { loop(); });
}

CaptureSaver::~CaptureSaver() {
  {
    std::lock_guard<std::mutex> lk(mu_);
    stop_ = true;
  }
  cv_.notify_all();
  if (worker_.joinable()) worker_.join();
}

void CaptureSaver::save(const std::string& address,
                        std::shared_ptr<const FrameBuffer> frame,
                        nlohmann::json result,
                        std::shared_ptr<std::atomic<CaptureSaveState>> state) {
  if (!enabled_.load() || !frame) return;
  if (state) state->store(CaptureSaveState::Saving);
  {
    std::lock_guard<std::mutex> lk(mu_);
    queue_.push_back({address, std::move(frame), std::move(result), std::move(state)});
    status_.pending = queue_.size() + (busy_ ? 1 : 0);
  }
  cv_.notify_all();
}

void CaptureSaver::flush() {
  std::unique_lock<std::mutex> lk(mu_);
  idle_cv_.wait(lk, [this] { return queue_.empty() && !busy_; });
}

CaptureSaver::Status CaptureSaver::status() const {
  std::lock_guard<std::mutex> lk(mu_);
  Status s = status_;
  s.enabled = enabled_.load();
  return s;
}

void CaptureSaver::loop() {
  for (;;) {
    Job job;
    {
      std::unique_lock<std::mutex> lk(mu_);
      cv_.wait(lk, [this] { return stop_ || !queue_.empty(); });
      if (queue_.empty()) return;  // stopping, and nothing left to write
      job = std::move(queue_.front());
      queue_.pop_front();
      busy_ = true;
    }
    std::string file, error;
    try {
      file = write(job);
    } catch (const std::exception& e) {
      error = e.what();
      spdlog::error("capture save failed: {}", error);
    }
    if (job.state)
      job.state->store(error.empty() ? CaptureSaveState::Saved
                                     : CaptureSaveState::Failed);
    {
      std::lock_guard<std::mutex> lk(mu_);
      busy_ = false;
      status_.pending = queue_.size();
      if (error.empty()) {
        ++status_.saved;
        status_.last_file  = file;
        status_.last_error.clear();
      } else {
        ++status_.errors;
        status_.last_error = error;
      }
    }
    idle_cv_.notify_all();
  }
}

std::string CaptureSaver::write(const Job& job) {
  const FrameBuffer& f = *job.frame;
  if (session_dir_.empty()) {
    session_dir_ = (fs::path(root_) / captureSessionDirName(std::time(nullptr)))
                       .string();
    std::lock_guard<std::mutex> lk(mu_);
    status_.session_path = session_dir_;
  }
  auto it = servers_.find(job.address);
  if (it == servers_.end())
    it = servers_.emplace(job.address, ServerDir{serverDirName(job.address), 0, {}}).first;
  ServerDir& sd = it->second;
  const fs::path dir = fs::path(session_dir_) / sd.subdir;
  std::error_code ec;
  fs::create_directories(dir, ec);
  if (ec) throw std::runtime_error("cannot create " + dir.string() + ": " + ec.message());

  // A capture id seen again for the same camera means start_cameras ran in
  // between: a new run.
  const std::string key = std::to_string(f.frame_kind) + ":" +
                          std::to_string(f.capture_id) + ":" +
                          std::to_string(f.camera_id);
  if (sd.seen.count(key)) {
    ++sd.run;
    sd.seen.clear();
  }
  sd.seen.insert(key);
  std::string base = "cam" + std::to_string(f.camera_id) + "-" +
                     std::to_string(f.frame_id);
  if (sd.run > 0) base += "-run" + std::to_string(sd.run);

  std::string image_file;
  if (!f.header_only && !f.data.empty()) {
    if (isStack(f)) {
      const auto npy = stackImageNpy(f);
      if (npy.empty()) throw std::runtime_error("stacked image " + base + " is truncated");
      image_file = base + ".npy";
      writeFile(dir / image_file, npy.data(), npy.size());
    } else {
      image_file = base + ".yuv";
      writeFile(dir / image_file, f.data.data(), f.data.size());
    }
  }
  const std::string text =
      captureSidecar(f, job.result, job.address, image_file).dump(2);
  writeFile(dir / (base + ".json"),
            reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
  return sd.subdir + "/" + (image_file.empty() ? base + ".json" : image_file);
}

}  // namespace telefacet::data

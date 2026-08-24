// lowfps_repro — headless reproduction of the "server hangs after dozens of
// frames" report: manual focus, a short manual shutter and a locked low frame
// rate, driving the exact `processing:` block from a config file.
//
// This is the GUI's bring-up sequence (connect → configure → focus/exposure/
// frame-duration → set_process_mode → start → start_stream) with no GLFW, no
// window and no clicks, so a hang can be reproduced over SSH and correlated
// against the server's own logs.
//
//   ./build/lowfps_repro <config.yaml> [seconds] [fps] [exposure_us] [dioptres]
//                        [recover]
//
// Defaults mirror the report: 2 fps, 1000 µs shutter, 0 dpt, 180 s.
// It prints a line per second (frames received, wire throughput, frame-id gaps)
// and shouts as soon as the stream goes quiet for longer than 3 frame periods.
//
// `recover` (0/1, default 0): on detecting a stall, run the recovery the server
// documents for a capture timeout — stop_cameras, start_cameras, start_stream —
// and report whether frames come back. Exercises that a stalled camera has not
// been wedged out of its own lifecycle.

#include <spdlog/spdlog.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "client/Client.hpp"
#include "config/ConfigLoader.hpp"
#include "util/Log.hpp"

using clock_t_ = std::chrono::steady_clock;
using namespace std::chrono_literals;

namespace {

double secondsSince(clock_t_::time_point t0) {
  return std::chrono::duration<double>(clock_t_::now() - t0).count();
}

}  // namespace

int main(int argc, char** argv) {
  telefacet::log::init();
  if (argc < 2) {
    std::fprintf(stderr,
                 "usage: %s <config.yaml> [seconds] [fps] [exposure_us] "
                 "[dioptres]\n",
                 argv[0]);
    return EXIT_FAILURE;
  }
  const std::string cfg_path = argv[1];
  const double  duration_s = argc > 2 ? std::atof(argv[2]) : 180.0;
  const double  fps        = argc > 3 ? std::atof(argv[3]) : 2.0;
  const int64_t exposure   = argc > 4 ? std::atoll(argv[4]) : 1000;
  const double  dioptres   = argc > 5 ? std::atof(argv[5]) : 0.0;
  const bool    recover    = argc > 6 ? std::atoi(argv[6]) != 0 : false;
  const int64_t frame_dur  = static_cast<int64_t>(1e6 / fps);

  try {
    auto cfg = telefacet::config::loadFromFile(cfg_path);
    spdlog::info(
        "lowfps_repro: {} server(s), mode='{}' save_frames={} output_dir='{}'",
        cfg.servers.size(), cfg.saving.mode, cfg.saving.save_frames,
        cfg.saving.output_dir);
    spdlog::info("lowfps_repro: {} fps (frame_duration={} us), shutter={} us, "
                 "focus={} dpt, run={} s",
                 fps, frame_dur, exposure, dioptres, duration_s);

    telefacet::client::Client client(std::move(cfg));

    telefacet::client::ClientOptions opts;
    // Drive exactly what the config asks for, not the pose-estimation defaults.
    opts.save_mode        = client.manager().configuredSaveMode();
    opts.save_frames      = client.manager().savingConfig().save_frames;
    opts.header_only      = false;  // pull the pixels, like the GUI viewer does
    opts.lens_position    = dioptres;
    opts.exposure_time_us = exposure;
    opts.frame_duration_us = frame_dur;
    opts.start_timeout    = 15s;
    client.start(opts);

    const auto cams = client.cameras();

    // In trigger mode, exercise the shutter: this is the mode a calibration
    // rig actually uses (move, settle, trigger), so a live run should prove a
    // frame really lands on disk per trigger.
    if (opts.save_mode == "trigger") {
      for (int shot = 0; shot < 3; ++shot) {
        const auto outcome = client.triggerCapture(10s);
        spdlog::info("trigger {}: complete={} cancelled={} captures={}", shot,
                     outcome.complete, outcome.cancelled,
                     outcome.captures.size());
        for (const auto& c : outcome.captures)
          spdlog::info("  cam{} frame#{} -> {}", c.global_camera_id, c.frame_id,
                       c.filename);
      }
    }

    std::unordered_map<std::size_t, std::uint64_t> cursor;
    std::unordered_map<std::size_t, std::int64_t>  last_frame_id;

    const auto t0 = clock_t_::now();
    auto next_report = t0 + 1s;
    auto last_frame_at = t0;

    std::uint64_t total_frames = 0, total_bytes = 0, total_gaps = 0;
    std::uint64_t win_frames = 0, win_bytes = 0;
    bool stall_reported = false;
    const double stall_s = std::max(3.0, 3.0 / fps);
    bool recovering = false;
    std::uint64_t recovery_baseline = 0;
    auto recovery_deadline = clock_t_::now();

    while (secondsSince(t0) < duration_s) {
      bool got_any = false;
      for (const auto& info : cams) {
        auto buf = client.store().consumeIfNew(info.global_id,
                                               cursor[info.global_id]);
        if (!buf) continue;
        got_any = true;
        ++total_frames;
        ++win_frames;
        total_bytes += buf->data.size();
        win_bytes   += buf->data.size();
        auto& last = last_frame_id[info.global_id];
        if (last >= 0) {
          const std::int64_t gap =
              static_cast<std::int64_t>(buf->frame_id) - last - 1;
          if (gap > 0) total_gaps += static_cast<std::uint64_t>(gap);
        }
        last = buf->frame_id;
      }
      if (got_any) {
        last_frame_at = clock_t_::now();
        stall_reported = false;
      } else if (!stall_reported && secondsSince(last_frame_at) > stall_s) {
        spdlog::error(
            "STALL: no frame for {:.1f} s (> {:.1f} s) at t={:.1f} s after "
            "{} frame(s) — connected={}",
            secondsSince(last_frame_at), stall_s, secondsSince(t0),
            total_frames, client.connected());
        stall_reported = true;

        if (recover) {
          // The documented recovery for a capture_timeout. Reconfiguring is
          // not needed, so this must work with the pipeline still allocated.
          const std::uint64_t before = total_frames;
          spdlog::warn("RECOVER: stop_cameras -> start_cameras");
          client.manager().stopAllCameras();
          std::this_thread::sleep_for(2s);
          client.manager().startAllCameras();
          std::this_thread::sleep_for(2s);
          for (const auto& info : cams) client.manager().startStream(info.global_id);
          recovery_deadline = clock_t_::now() + 15s;
          recovery_baseline = before;
          recovering = true;
          last_frame_at = clock_t_::now();
        }
      }

      if (recovering && total_frames > recovery_baseline) {
        spdlog::info("RECOVER: streaming resumed after {} more frame(s)",
                     total_frames - recovery_baseline);
        recovering = false;
      } else if (recovering && clock_t_::now() > recovery_deadline) {
        spdlog::error("RECOVER: FAILED — no frame within 15 s of restart");
        recovering = false;
      }

      if (clock_t_::now() >= next_report) {
        spdlog::info(
            "t={:5.1f}s  frames={:4d} (+{:2d})  {:.2f} fps  {:6.1f} Mbit/s  "
            "gaps={}  quiet={:.1f}s",
            secondsSince(t0), total_frames, win_frames,
            static_cast<double>(win_frames), win_bytes * 8.0 / 1e6, total_gaps,
            secondsSince(last_frame_at));
        win_frames = 0;
        win_bytes = 0;
        next_report += 1s;
      }
      std::this_thread::sleep_for(1ms);
    }

    const double elapsed = secondsSince(t0);
    spdlog::info("--- summary ---");
    spdlog::info("elapsed      : {:.1f} s", elapsed);
    spdlog::info("frames       : {} ({:.2f} fps)", total_frames,
                 total_frames / elapsed);
    spdlog::info("bytes        : {} ({:.1f} Mbit/s)", total_bytes,
                 total_bytes * 8.0 / 1e6 / elapsed);
    spdlog::info("frame-id gaps: {}", total_gaps);
    spdlog::info("last frame   : {:.1f} s ago", secondsSince(last_frame_at));
    spdlog::info("connected    : {}", client.connected());

    client.stop();
    return EXIT_SUCCESS;
  } catch (const std::exception& e) {
    spdlog::critical("fatal: {}", e.what());
    return EXIT_FAILURE;
  }
}

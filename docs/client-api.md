# Headless client API (`telefacet::client::Client`)

`src/client/Client.{hpp,cpp}` is the GUI-free entry point for **consuming**
telefacet's camera streams from another C++ application — e.g. a robot
controller that needs on-device AprilTag corner detections but no window.

It is the same wiring the ImGui app does in `src/app/App.cpp`
(`MultiServerManager` + `CameraStore` + the lifecycle the `ControlPanel`
drives), packaged behind a small class and stripped of every GLFW / OpenGL /
ImGui dependency. It links only against `telefacet_core`.

## What `start()` does

Given a loaded `config::Config` (just the `servers:` list is required), one
call runs the whole bring-up and blocks until every server is streaming:

```
connectAll → wait for discovery → configureAll → lens/exposure → set save_mode
           → (header-only, no disk saving) → startAllCameras → startStream(all)
```

`ClientOptions` defaults are tuned for pose estimation:

| option | default | why |
|---|---|---|
| `save_mode` | `"aruco2x2"` | server-side processing mode; any mode from protocol §4.5. Captures (below) work in all of them |
| `aruco_corner_refine` | `true` | subpixel corners → better PnP |
| `save_frames` | `false` | don't fill the Pi's disk |
| `header_only` | `true` | stream only the header + corner block, **not** the pixels — a large bandwidth saving; the detection block rides every frame regardless |
| `lens_position` | unset | set it to the diopter your intrinsics were calibrated at for a locked focus |

Lens/exposure are applied *after* configure — the server rejects those controls
while the cameras are still IDLE.

### Streaming raw frames instead of detections

Set `save_mode = "none"` and `header_only = false` to stream whole YUV420 frames.
`poll()`/`latest()` only copy out marker data, so raw pixels come off the store
directly — `client.store().consumeIfNew(global_id, cursor)` hands back the
`FrameBuffer` with `.data`.

Note that `"none"` is the only ungated mode: detector modes stream only the
frames the on-device detector finished, so they drop frames under load by design.

## On-demand captures: `triggerCapture()` and `stackCapture()`

Since protocol v8 both captures are commands that work in **any** process mode:
each frame they produce gets that mode's detection, nothing is saved on the
server, and the frames come back to this client (§5.9) — whatever the live
subscription or header-only state. One capture of either kind at a time; the
servers refuse an overlapping one.

### `triggerCapture()`

The automated-calibration path: move the camera with a robot arm, wait for it
to come to a complete stop, then trigger, so no frame carries motion blur.

```cpp
telefacet::client::ClientOptions opts;
opts.save_mode = "checkerboard";   // the frames come back with their corners
client.start(opts);

for (const Pose& pose : calibration_poses) {
  arm.moveTo(pose);
  arm.waitUntilStopped();

  auto shot = client.triggerCapture(std::chrono::seconds(10));
  if (!shot) { /* refused, timed out or cancelled — retry this pose */ }
  for (const auto& c : shot.captures) {
    const telefacet::data::FrameBuffer& f = *c.frame;   // yours to keep
    // f.data: full-resolution I420 (bytes_per_line × height Y plane, then U, V)
    // f.corner_sets / f.aruco_markers: the mode's detection, full-frame pixels
  }
}
```

It arms all running cameras on all connected servers, so one call yields one
frame per camera, all at the same pose, and blocks until every server has
acked **and** its frames have arrived. The arm may move as soon as the frames
are taken, which is a little earlier (detection and transfer follow).

| field | meaning |
|---|---|
| `complete` | every server acked and delivered before the timeout |
| `cancelled` | a server abandoned its trigger because capture stopped mid-flight; its untaken cameras are missing |
| `refusals` | `"server <i>: <message>"` for each server that refused (not RUNNING, a capture already in progress, ...) — the call returns at once rather than waiting out the timeout |
| `captures` | one `CapturedFrame` per camera that delivered, sorted by `global_camera_id`: ids, `frame_id` and the `frame` itself |

`TriggerOptions` (2nd arg): `skip_frames` (0..100, default the config's
`processing.trigger_skip_frames`) discards settling frames per camera before the
kept one; `send_image = false` returns header + detection only — the corners
without the pixels, for a slow link.

### `stackCapture()`

Averages raw Bayer frames per camera on every server configured with
`raw: true` (`ServerCfg::raw`) and returns one stacked image each. On a still
rig a corner's frame-to-frame scatter is photon noise, so it falls as
1/√`num_frames` (measured on an IMX477 at 4056×3040: 0.0154 px for one frame,
0.0029 px for 32).

```cpp
telefacet::client::StackOptions so;
so.num_frames       = 32;
so.exposure_time_us = 50000;   // pinned for the stack, then restored
auto st = client.stackCapture(std::chrono::seconds(60), so);
for (const auto& c : st.cameras) {
  if (!c.stats.error.empty()) continue;           // this camera failed
  // c.stats.white_fraction: aim for ~0.8; c.stats.clipped_fraction: keep at 0
  const telefacet::data::FrameBuffer& img = *c.frame;
  // img.data: width × height float32 (pixel_format 'SF32'), linear DN above
  // black; img.corner_sets in stack pixels: sensor pixel = 2·b + 0.5
}
```

Unset `StackOptions` fields take the config's `processing.stack_*` defaults.
`StackOutcome` converts to `true` only when every camera stacked and its image
arrived.

## Consuming detections

The pull API is latest-wins and non-blocking, matching the live-viewer
semantics (older frames are dropped). Corner coordinates are full-frame Y-plane
pixels, so they drop straight into your calibrated intrinsics.

```cpp
#include "client/Client.hpp"

telefacet::config::Config cfg = telefacet::config::loadFromFile("servers.yaml");
telefacet::client::Client client(std::move(cfg));

telefacet::client::ClientOptions opts;
opts.lens_position = 4.0;          // locked focus @ 4 dpt
client.start(opts);

// In your control loop:
for (const telefacet::client::CameraMarkers& cm : client.poll()) {
  // cm.global_camera_id, cm.local_camera_id, cm.lens_position, cm.timestamp_us
  for (const telefacet::data::ArucoMarker& m : cm.markers) {
    // m.marker_id, m.quadrant, m.corners  (4× {x,y}, clockwise from top-left)
  }
}
```

`poll()` returns one `CameraMarkers` per camera that produced a **fresh** frame
since the previous poll; an entry may carry an empty `markers` vector (a frame
in which nothing was detected). Use `latest(global_camera_id, out)` to pull a
single camera. `cameras()` returns the discovered roster; `global_id` values
are dense `[0, N)` and stable for the session.

> **aruco2x2 note.** "2×2" is a per-frame quadrant tiling the server uses for
> detection parallelism — `quadrant` (0..3 = row·2+col) just says which
> sub-region found the marker. Corners are always full-frame pixels, and the
> same physical marker can appear in more than one quadrant record near a
> boundary; dedupe by `marker_id` if you care.

## Threading

`poll()` / `latest()` copy the detection out of the recycled network buffer and
return by value, so the returned `CameraMarkers` is yours to keep. Call them
from your control thread; the network threads (one per server, owned by
`ixwebsocket`) publish frames independently.

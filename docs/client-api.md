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
| `save_mode` | `"aruco2x2"` | server-side processing mode; any mode from protocol §4.5 |
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

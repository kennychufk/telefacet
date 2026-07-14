# Stage 4 handoff — Display parity: overlays, dual FPS, streaming-only grid (P2b)

Read `PLAN.md` first. Stages 1–3 are done: the client speaks v5, the command
layer is complete, and the control panel is a full pipeline state machine with
focus / exposure / frame-duration / save-mode controls — all verified against
live Pi hardware (192.168.1.239, IMX519). Stage 4 is the **display side**: the
camera views. No new protocol work — every field you need is already parsed
into `FrameBuffer` (see below).

## What Stage 3 changed (context)
- `src/ui/ControlPanel.{hpp,cpp}` — rewritten from a flat button panel into
  sectioned UI: Servers · Pipeline · Cameras · Options · Exposure · Focus · Save
  mode. Pipeline is a Discover→Configure→Start(→Stream) state machine with
  advance/retreat, a LIVE indicator, and a two-button row (`< Stop/Reset/Back`
  and `Configure/Start/Stream >`), all driven by an aggregate of each server's
  `serverState()`. Focus/exposure/frame-duration sliders read the server's real
  limits (`lensPositionLimits()`, `frameDurationLimits()`), gated to
  CONFIGURED/RUNNING, with the neg=auto sign conventions. Save mode has
  live-editable params (output_dir / batch / threads / checkerboard rows·cols·
  full-res·threads) seeded from YAML, and the Cameras section shows per-camera
  `frames_saved` in checkerboard modes.
- **`MultiServerManager::getStateAll()` (new)** — the one non-UI change. The
  server signals lifecycle transitions with `status`, **not** a proactive
  `state`, so `serverState()` went stale after configure/start/stop and the
  pipeline never advanced. The panel now polls `get_state` on a ~0.75 s timer
  and once immediately after each advance/retreat. If you add your own periodic
  network chatter in Stage 4, be aware this poll already runs.
- `ControlPanel::Stage` is public (an anonymous-namespace helper needs it).

## What's already plumbed for you (no parsing needed)
`data::FrameBuffer` (`src/data/FrameBufferPool.hpp`) already carries, per frame:
`timestamp_us`, `frame_duration_us`, `lens_position` (dioptres, `NaN` if none),
`af_state` (libcamera AfState, `0xFF` if none), and `corner_sets`
(`std::vector<CornerSet>`; each has `set_id`, `flags` bit0 ⇒ full-frame Y-plane
coords, and `corners` = `{x,y}` inner-corner list). `CameraView::uploadIfNew()`
already copies `frame_id`/`frames_saved`/`header_only`/`width`/`height` off the
frame — extend it to also stash `timestamp_us`, `frame_duration_us`,
`lens_position`, `af_state`, and `corner_sets` for the draw pass.

`data::CameraLiveStats` currently has only client `fps`. Add server-side fps
there (atomic), computed on the network thread as frames publish.

## Spec / reference (web)
- `../telefacet-web/src/services/WebSocketManager.js` → `updateServerFpsStats`:
  server fps = frame-id gap ÷ (timestamp_us delta), i.e. normalize the HW
  timestamp delta by the number of frames actually captured between two received
  frames (handles dropped frames). Port that math.
- `../telefacet-web/src/components/CameraView.vue` — corner overlay (map
  full-frame Y-plane corner coords → the displayed image rect) and the
  lens/AF hover overlay (shows `lens_position` dpt + AfState name on hover).
- `../telefacet-web/src/stores/cameraStore.js` → `getGridDimensions()` +
  `streamingCameras` — the grid lays out **only streaming** cameras.

## Tasks
1. **Dual FPS.** Add `server_fps` to `CameraLiveStats`; compute it in
   `CameraStore::publishFrame` (or wherever `fps` is computed) from
   `timestamp_us` + `frame_id` gap (port `updateServerFpsStats`; guard the first
   frame, zero/negative deltas, and frame-id wrap). Show both client and server
   fps in the `CameraView` overlay and in the ControlPanel Cameras row (it
   currently shows client fps only — the web renders `clientFps / serverFps`).
2. **Checkerboard corner overlay.** In `CameraView::drawWindow`, when
   `corner_sets` is non-empty, map each corner from full-frame Y-plane pixels to
   the displayed image rect (you already compute `disp`/`padding`/`cursor` for
   the letterboxed `ImGui::Image`; reuse that transform) and draw points/lines
   with `ImGui::GetWindowDrawList()`. Respect `CornerSet::flags` bit0; color by
   `set_id` for the 2x2 case (0..3).
3. **Lens / AF hover overlay.** On image hover, show `lens_position` (dpt) and a
   human AfState label (map the `af_state` byte). NaN/`0xFF` ⇒ hide/"—".
4. **Streaming-only grid.** `CameraGrid` currently opens a window for **every**
   discovered camera (`sync()` over the full roster). Lay out / show only
   cameras with `info.streaming` (port `getGridDimensions` for arrangement).
   This also fixes the current cosmetic overlap where idle-camera windows sit on
   top of the control panel.
5. Keep the P hotkey / dockspace. Build clean.

## Watch out for
- `bytes_per_line` (Y stride) ≠ `width`; the frame is padded. Corner coords are
  in Y-plane **pixel** space (use `width`/`height`, not stride, for the map).
- FrameBuffers are pooled and released right after upload — copy anything you
  need for drawing into the `CameraView` before `store_.pool().release(...)`.
- Server fps math must run on the network thread with the two most recent
  frames' `timestamp_us`; store the previous timestamp+frame_id in the stats.

## Verify (live, per PLAN)
A real server is available: `ssh hkqai@192.168.1.239`, binary at
`/home/hkqai/workspace/cppWs/cherupi-v4l2/build/camera_ws_server` (start it with
`setsid nohup ./camera_ws_server >/tmp/cherupi_server.log 2>&1 &` from its build
dir; it listens on `:9001`, note the process name truncates to
`camera_ws_serve` for `pkill -x`). Run the client with
`../telefacet-web/arm0.yaml` (imx519 @ 2328x1748 — a **valid** mode; see gap
below). Drive Configure→Start→Stream and confirm: dual fps both non-zero,
corner overlay tracks a physical checkerboard in `checkerboard2x2` mode, hover
shows lens/AF, and the grid holds only streaming cameras. There's a display at
`:0`; screenshot a window region with `ffmpeg -f x11grab` (find geometry via
`xwininfo -root -tree | grep '"telefacet"'`). Bring the panel above the camera
windows by clicking its title bar, or just resize the OS window.

## Known gaps carried in (not Stage 3's doing)
- The **e2e suite** (`tests/`) fails against this hardware: the fixture
  resolution `1456x1088` (`tests/test_env.hpp` `TestCameraCfg`) is rejected by
  the IMX519 board — server logs `Invalid sensor configuration: bitDepth/size
  mismatch`. `2328x1748` (arm0.yaml) configures fine (verified). The two
  `ConfigLoader` tests pass; the 12 lifecycle/protocol/savemode tests need the
  fixture bumped to a valid 10-bit SRGGB mode before they can go green. This
  predates Stage 3 (Stage 2 never smoke-tested e2e on hardware) and is left for
  whoever owns the test fixtures — flag it, don't silently rely on it.

## Done when
Display parity with telefacet-web: dual fps, corner + lens/AF overlays, and a
streaming-only grid. Upgrade complete — update `PLAN.md` Status to note Stage 4
done and the full v2→v5 parity upgrade finished.

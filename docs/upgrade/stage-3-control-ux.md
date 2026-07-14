# Stage 3 handoff — Control UX + focus/exposure/frame-duration (P2a)

Read `PLAN.md` first. Stages 1–2 are done: the client speaks v5, parses all
responses, and the command layer is complete on `WebSocketClient` /
`MultiServerManager` (see below). Stage 3 is **UI only** — no new protocol work.

**Goal:** replace the flat button panel with the web client's pipeline
state-machine UX, and add focus / exposure / frame-duration controls that use
the server-reported limits already being parsed.

## What the command layer already gives you (Stage 2)
On `MultiServerManager`: `configureAll`, `unconfigureAll`, `startAllCameras`,
`stopAllCameras`, `startStream`/`stopStream(globalId)`, `setSaveModeAll`,
`savingParamsFromConfig()`, `configuredSaveMode()`, `setHeaderOnlyAll`,
`resetFrameCountsAll`, `setLensPositionAll(double)`, `setExposureTimeAll(int64)`,
`setFrameDurationAll(int64)`, `getFrameDurationLimits()`, `getLensPositionLimits()`.
On each `WebSocketClient` (poll these per UI frame): `serverState()` →
`""|"idle"|"configured"|"running"`, `frameDurationLimits()` → `FrameDurationLimits`,
`lensPositionLimits()` → `LensPositionLimits` (fields are `std::optional`, null
⇒ no focuser). `get_state` is already issued on connect.

## Spec / reference
Web: `../telefacet-web/src/components/Pipeline.vue` (state machine + advance/
retreat semantics — already summarised: stages Discover→Configure→Start→Stream;
retreat = Stop/Reset/Back), `FocusSection.vue`, `ExposureSection.vue`,
`src/stores/cameraStore.js` (how state drives the UI). Protocol §4.12–4.16 for
control semantics and sign conventions (lens<0 continuous AF, exposure<0 auto AE,
frame_duration<=0 unset).

## Tasks
1. **Pipeline state view** — port `Pipeline.vue`'s stage machine into ImGui in
   `ControlPanel` (or a new `Pipeline` widget). Drive `idx` from the connected
   servers' `serverState()` (aggregate: show the common state; if servers
   disagree, surface that). Advance = discover→configure→start→stream; retreat =
   stop/unconfigure/back. Wire buttons to the existing MSM broadcast helpers.
   Show a LIVE indicator when running && any camera streaming.
2. **Focus panel** — auto (continuous AF) vs manual toggle; manual slider over
   `lensPositionLimits()` min/max (fall back to a sane range when `std::nullopt`);
   0.05 dpt step (web `fd0c8f`); call `setLensPositionAll` (−1 for continuous).
   Call `getLensPositionLimits()` once cameras reach CONFIGURED/RUNNING.
3. **Exposure + frame-duration panel** — auto AE vs manual shutter (µs, clamp
   [1, 1e6]); frame-duration lock (µs; 0/neg = unset) using
   `frameDurationLimits()` for the slider range; call `setExposureTimeAll` /
   `setFrameDurationAll`. Query `getFrameDurationLimits()` when
   CONFIGURED/RUNNING.
4. **Save-mode UI** — the combo already includes `checkerboard2x2` and sends
   `savingParamsFromConfig()`. Add editable checkerboard params (rows/cols/
   full-res/threads, output_dir, batch_size) so the user can change them live,
   and show per-camera saved-frame count prominently in checkerboard modes
   (web `d50c08a`) — `CameraLiveStats::frames_saved` already tracks it.
5. Keep the P hotkey / dockspace. Build clean.

## Watch out for
- `serverState()`/limits are updated from the network thread; they're already
  mutex-guarded — just poll copies each frame, don't hold references.
- Limits queries only valid in CONFIGURED/RUNNING (§4.15/§4.16) — gate them.
- Don't block the UI thread waiting for a limits reply; poll until `valid`.

## Done when
- The control panel is a working pipeline (advance/retreat) driven by real
  server state, with focus / exposure / frame-duration controls that read the
  server's limits. Control parity with telefacet-web.

Then write `stage-4-display-parity.md` (overlays, dual FPS, streaming-only grid)
and update `PLAN.md` Status.

# Stage 2 handoff — Commands, per-server config, cleanup (P1)

Read `PLAN.md` first. Stage 1 (v5 wire/data core) is done: the client now parses
v5 headers + CornerBlock and carries `timestamp_us`, `frame_duration_us`,
`lens_position`, `af_state`, `corner_sets` on `FrameBuffer` (not yet displayed).

**Goal of Stage 2:** make every current server feature reachable and make config
correct — new text commands, per-server sensor/resolution, honoured save-mode
params, dead-code removal. UI can stay the simple button panel this stage (the
pipeline UX + control sliders are Stage 3); commands just need to exist and be
callable, and connect-time state sync needs to work.

## Before you start
- **Live smoke test Stage 1** against a real v5 server if not already done:
  `./build/telefacet configs/<cfg>.yaml` should render frames. If a server is
  reachable, run the e2e suite too (`-DTELEFACET_BUILD_TESTS=ON`, set
  `TELEFACET_WS_URL`). A hardware-free synthetic-frame check lives at
  `scratchpad/verify_v5.cpp` from the Stage 1 session (reassembler-level) — worth
  promoting into `tests/` as a non-hardware unit test at some point.

## Spec references
`../cherupi-v4l2/docs/websocket-protocol.md` §4.2 (`get_state`), §4.5 save modes
(incl. `checkerboard2x2`), §4.12–4.16 (lens/exposure/frame-duration + limits),
§4.1 (`discover` `sensor` param). Cropping removal: server commit `5755bd3`.
Port command shapes + response handling from
`../telefacet-web/src/services/WebSocketManager.js` (methods `setLensPosition`,
`setExposureTime`, `setFrameDuration`, `getFrameDurationLimits`,
`getLensPositionLimits`, `getState`, `configureCameras`, `setSaveMode`) and
per-server config from `../telefacet-web/src/services/ConfigLoader.js`.

## Tasks
1. **New commands** — `src/net/Protocol.hpp cmd::` add `kGetState`,
   `kSetLensPosition`, `kSetExposureTime`, `kSetFrameDuration`,
   `kGetFrameDurationLimits`, `kGetLensPositionLimits`. Add matching methods on
   `WebSocketClient` (mirror the JS payloads exactly — sign conventions: lens<0
   continuous AF, exposure<0 auto AE, frame_duration<=0 unset).
2. **Response handling** — `WebSocketClient::handleText` currently handles
   `discovery`/`status`/`error` (verify). Add `state`, `frame_duration_limits`
   (`min`/`max`/`num_cameras`/`current`), `lens_position_limits`
   (`min`/`max`/`default`/`num_cameras`, JSON `null` → unavailable). Add
   callbacks + store latest values per client for the UI (Stage 3 consumes them).
3. **get_state on connect** — issue `get_state` right after `discover` on
   connect; store server state per client (drives Stage 3's pipeline UI). Don't
   assume IDLE (protocol §4.2).
4. **discover sensor param** — send `{"cmd":"discover","params":{"sensor":...}}`
   when a per-server sensor is configured (§4.1).
5. **Per-server config** — `src/config/ConfigLoader.{hpp,cpp}`: move
   `sensor`/`width`/`height` onto `ServerCfg` (optional; fall back to server
   defaults). `MultiServerManager::configureAll` uses each server's own
   resolution; `discover` uses each server's sensor. Update `configs/example.yaml`
   to the per-server shape (see telefacet-web `arm0.yaml`).
6. **Drop cropping** — remove `crop_*` from `CameraCfg`,
   `WebSocketClient::configureCameras` (make it `(width,height)`), `example.yaml`,
   and the tests' `TestCameraCfg` (`tests/test_env.hpp`) + call sites in
   `tests/e2e_*` (they pass crop args today).
7. **Forward save-mode params** — `ControlPanel`/`MultiServerManager` must send
   the YAML `frame_saving` params (output_dir, batch_size, writer_threads,
   prepend_timestamp_to_dir, checkerboard_*) with `set_save_mode`, not `{}`.
   Add `checkerboard2x2` to `FrameSavingCfg` + the save-mode list + validation.
8. **Delete legacy dead code** — top-level `telefacet.cpp`, `include/`, `lib/`
   (not referenced by `CMakeLists.txt`). Also check `src/gl/Debayer.{cpp,hpp}` —
   not in the GUI target's source list; confirm dead and remove if so.
9. Build clean (GUI + tests). Update README if command surface is described.

## Watch out for
- Packed-struct field access: copy to locals before passing to spdlog/functions.
- `frame_duration_limits`/`lens_position_limits` are only valid in
  CONFIGURED/RUNNING (§4.15/§4.16) — don't query them in IDLE.
- Keep the crop removal atomic across client + tests or the e2e build breaks.

## Done when
- All six commands send correctly and their responses are parsed/stored.
- `get_state` runs on connect; per-server sensor + resolution honoured.
- `set_save_mode` carries real params; `checkerboard2x2` selectable.
- No `crop_*` anywhere; legacy files gone; build clean.

Then write `stage-3-control-ux.md` and update `PLAN.md` Status.

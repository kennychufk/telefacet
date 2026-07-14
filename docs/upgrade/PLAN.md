# telefacet C++ client — v2 → v5 upgrade plan

Master reference for bringing the native C++/OpenGL client to **full feature
parity** with `telefacet-web` against the current `cherupi-v4l2` server
(WebSocket **protocol v5**).

This work is **staged across multiple Claude Code sessions**. Each stage is
self-contained and independently testable. When advancing from one stage to
the next, write a handoff doc (`stage-N-*.md` in this folder) so a fresh
session can continue without re-deriving context.

## Related repos (siblings of this one)

| Repo | Role | Key files |
|---|---|---|
| `../cherupi-v4l2` | The server | `docs/websocket-protocol.md` (authoritative wire spec), `src/types.hpp` (packed structs) |
| `../telefacet-web` | Reference client, already fully on v5 | see "Web reference map" below |
| this repo (`telefacet`) | The client we are upgrading | live code under `src/` only |

### Web reference map (what to port from)
- `src/services/WebSocketManager.js` — v5 binary parse, CornerBlock parse, dual-FPS math, all text commands.
- `src/services/ConfigLoader.js` — per-server `sensor`/`width`/`height`, save-mode validation.
- `src/components/Pipeline.vue` — the pipeline state-machine control UX (advance/retreat).
- `src/components/FocusSection.vue`, `ExposureSection.vue` — focus / exposure / frame-duration slider UIs.
- `src/components/CameraView.vue` — corner overlay + lens/AF hover overlay.
- `src/stores/cameraStore.js` — per-camera state, streaming-only grid layout.

## The core problem

The client is on **protocol v2** (40-byte `ChunkHeader`, `version == 2`); the
server sends **v5** (68-byte header + optional variable `CornerBlock`, 76-byte
start message). Every frame is dropped today at
`src/net/ChunkReassembler.cpp:40` (`len != 44` size check) — the client renders
nothing against the current server. The first 40 bytes of the v5 header are
byte-identical to v2, so v5 is a strict **extension**, not a re-layout.

### Protocol evolution (see `../cherupi-v4l2/docs/websocket-protocol.md` §8)
| Ver | Header | Added |
|---|---|---|
| v2 | 40 B | baseline this client targets |
| v3 | 52 B | `timestamp_us` (u64 @40), `frame_duration_us` (u32 @48); `num_cameras` in `frame_duration_limits` |
| v4 | 60 B | `corner_block_size` (u32 @52), `num_corner_sets` (u16 @56), `reserved` (u16 @58) + variable `CornerBlock` |
| v5 | 68 B | `lens_position` (float @60), `af_state` (u8 @64), `reserved2` (u8[3] @65) |

Doc caveat: `websocket-protocol.md` §5.2 stale-lists the marker `version` value
as `4`; §8 + history table + web client (`=== 5`) are authoritative → **use 5**.

New text commands the client lacks: `get_state`, `set_lens_position`,
`set_exposure_time`, `set_frame_duration`, `get_frame_duration_limits`,
`get_lens_position_limits`. Also: `discover` gained a `sensor` param;
`checkerboard2x2` save mode added; **cropping removed** (client still carries
dead `crop_*` fields).

## Current-state audit (live code under `src/`)
- `src/net/Protocol.hpp` — `kChunkVersion=2`; `ChunkHeader` 40 B; `cmd::` namespace missing the 6 new commands.
- `src/net/ChunkReassembler.cpp` — `handleStart` hard-rejects non-44-byte start msgs and non-v2 version; discards trailing/corner bytes.
- `src/data/FrameBufferPool.hpp` `FrameBuffer` — no `timestamp_us`/`frame_duration_us`/`lens_position`/`af_state`/corner fields.
- `src/net/WebSocketClient.*` — `configureCameras(...)` still takes `crop_*`; `discover()` sends no `sensor`; no focus/exposure/frame-duration/get_state/limits commands; no `frame_duration_limits`/`lens_position_limits`/`state` response handling.
- `src/config/ConfigLoader.hpp` — `ServerCfg{address}` only; single global `CameraCfg` with `crop_*`; `FrameSavingCfg` has no `checkerboard2x2`.
- `src/data/CameraStore.hpp` `CameraLiveStats` — single client `fps`; no server-side fps.
- `src/ui/ControlPanel.cpp` — flat buttons (no state machine); `setSaveModeAll(mode, {})` sends **empty** params (YAML `frame_saving` never reaches server); save-mode list missing `checkerboard2x2`.
- `src/ui/CameraView.cpp` — no corner/lens/AF overlays.
- **Legacy dead code** (not in `CMakeLists.txt`, pre-rewrite): top-level `telefacet.cpp`, `include/`, `lib/` → delete in Stage 2.

## Stages

### Stage 1 — Protocol v5 core (P0) · makes frames render again
- Extend `Protocol.hpp` `ChunkHeader` to the 68-byte v5 layout; `kChunkVersion = 5`; add start-msg min size (76) constant.
- Rewrite `ChunkReassembler::handleStart` to accept `len == 76 + corner_block_size`, parse the `CornerBlock` (§5.4), and validate accordingly.
- Extend `FrameBuffer` with `timestamp_us`, `frame_duration_us`, `lens_position`, `af_state`, and parsed corner sets; plumb through `handleStart`/header-only path.
- Update README + `configs/example.yaml` "protocol v2"/"10-bit SRGGB" notes.
- **Done when:** client renders live frames from a v5 server. No new UI/controls yet; new fields plumbed but not yet displayed.
- **Handoff:** `stage-1-protocol-v5.md` (already drafted).

### Stage 2 — Commands, per-server config, cleanup (P1)
- Add the 6 new commands to `Protocol.hpp cmd::` + `WebSocketClient` methods, and `state`/`frame_duration_limits`/`lens_position_limits` response handling (+ callbacks).
- `discover` sends `sensor`; `get_state` issued on connect; store server state per client.
- `ConfigLoader`: per-server `sensor`/`width`/`height` (`ServerCfg`); `configureCameras` uses per-server resolution; drop `crop_*` everywhere.
- `set_save_mode` actually forwards the YAML `frame_saving` params; add `checkerboard2x2` to `FrameSavingCfg` + validation.
- Delete legacy `telefacet.cpp`, `include/`, `lib/`.
- **Done when:** every server feature is reachable and config is honoured (UI may still be the simple panel). Build clean, no dead code.

### Stage 3 — Control UX + focus/exposure/frame-duration (P2a)
- Replicate `Pipeline.vue` state machine in ImGui: Discover → Configure → Start → Stream with advance/retreat (Stop/Reset/Back), live indicator, driven by real server state (from Stage 2 `get_state`).
- Focus / exposure / frame-duration slider panels using server-reported limits (`get_*_limits`); auto vs manual toggles matching web semantics (neg = auto/continuous).
- Save-mode UI: `checkerboard2x2`, editable checkerboard params, per-camera saved-frame count in checkerboard modes.
- **Done when:** control parity with the web client.

### Stage 4 — Display parity: overlays, dual FPS, grid (P2b)
- Server-side fps from `timestamp_us` normalized by frame-id gap (port `updateServerFpsStats`); show client + server fps.
- Checkerboard corner overlay on the rendered frame (full-frame Y-plane coords → display-rect mapping).
- Lens-position / AF-state hover overlay.
- Grid lays out only streaming cameras.
- **Done when:** display parity with the web client. Upgrade complete.

## Verification (every stage)
- Build: `cmake -B build -S . && cmake --build build -j` (GUI). Headless: `-DTELEFACET_BUILD_TESTS=ON`, then the `tests/` e2e suite.
- Live check: run against a v5 server (`./build/telefacet configs/<cfg>.yaml`); confirm the stage's "Done when" behaviour before writing the handoff.
- The `/verify` skill drives the app end-to-end — use it on stages with runtime surface.

## Handoff protocol
When finishing a stage: (1) confirm "Done when" holds, (2) write `stage-<next>-*.md`
capturing what changed, what's verified, known gaps, and the exact next tasks,
(3) update the "Status" line below, (4) commit only if the user asks.

**Status:** Stage 1 (protocol v5 core) **code-complete and verified** — GUI +
e2e binaries build clean; v5 wire parsing (multi-chunk + CornerBlock,
header-only, NaN lens, legacy-message rejection) verified end-to-end through the
real `ChunkReassembler` with synthetic frames. **Not yet smoke-tested against
live Pi hardware** (no camera server on the dev box) — do that before Stage 2.
Next: Stage 2 (`stage-2-commands-config.md`).

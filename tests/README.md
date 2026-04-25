# End-to-End Tests (C++ client)

Headless GoogleTest binary that exercises the telefacet client against a live
`camera_ws_server`. Links only `libtelefacet_core` (networking, config, data)
— no GLFW, ImGui, or OpenGL. Proves that the C++ client's wire-level layer
stays in sync with the server.

## Layout

| File | Purpose |
|---|---|
| `harness.{hpp,cpp}` | `BlockingClient` — synchronous wrapper around `WebSocketClient` |
| `test_env.hpp` | `serverUrl()` (reads `$TELEFACET_WS_URL`) and `TestCameraCfg` |
| `e2e_lifecycle_test.cpp` | connect → discover → configure → start → stream → stop |
| `e2e_protocol_test.cpp` | CHUN/CHNK frame metadata, header-only mode, monotonic counters |
| `e2e_save_mode_test.cpp` | NONE / BUFFER / BATCH / CHECKERBOARD on-disk effects |
| `e2e_config_loader_test.cpp` | YAML config parsing (no network) |
| `fixtures/test_config.yaml` | Sample config for the loader test |

## Build

GUI deps are skipped when we only want the tests; pass `-DTELEFACET_BUILD_GUI=OFF`.

```bash
cd telefacet
cmake -B build -DTELEFACET_BUILD_TESTS=ON -DTELEFACET_BUILD_GUI=OFF
cmake --build build --target telefacet_e2e_tests
```

To build both the GUI and the tests:

```bash
cmake -B build -DTELEFACET_BUILD_TESTS=ON
cmake --build build
```

## Run

```bash
# Server must be up and connected to IMX519 cameras.
TELEFACET_WS_URL=ws://pi.local:9001 ./build/tests/telefacet_e2e_tests

# Or via ctest:
ctest --test-dir build -L e2e --output-on-failure
```

The save-mode tests write files to `./telefacet_e2e_out/<label>/` under the
current working directory — the server must be able to resolve that path
(the server is the writer). When the server is on a different host,
`--gtest_filter=-*SaveMode*` to skip them.

## Notes

- Tests are *not* parallel-safe — they share the single-client server slot.
  Run them serially (the default for ctest when you don't pass `-j`).
- `sendAndExpectStatus` clears the status queue before sending so tests stay
  resynchronized even if a prior test left the pipeline in an unexpected
  state. Each test creates a fresh `BlockingClient` so state leakage across
  tests is bounded by the server's process lifetime (which may keep cameras
  running after a disconnect — the first command of the next test then
  operates from CONFIGURED rather than IDLE, so we send `stopCameras`
  opportunistically in teardown).

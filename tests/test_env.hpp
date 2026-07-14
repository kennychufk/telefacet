#pragma once

// Shared test helpers: server URL resolution, standard test camera config.

#include <cstdlib>
#include <string>

namespace telefacet::test {

inline std::string serverUrl() {
  if (const char* e = std::getenv("TELEFACET_WS_URL")) return std::string(e);
  return "ws://localhost:9001";
}

// Shared camera config used across tests. Low enough resolution that per-frame
// payload is ~2.3 MB, so tests finish in under a few seconds even on modest
// links.
struct TestCameraCfg {
  std::uint32_t width  = 1456;
  std::uint32_t height = 1088;
};

inline constexpr std::uint32_t kFourccYU12 = 0x32315559u;

}  // namespace telefacet::test

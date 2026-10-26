#pragma once

// Shared test helpers: server URL resolution, standard test camera config.

#include <cstdint>
#include <cstdlib>
#include <string>

namespace telefacet::test {

inline std::string serverUrl() {
  if (const char* e = std::getenv("TELEFACET_WS_URL")) return std::string(e);
  return "ws://localhost:9001";
}

inline std::uint32_t envU32(const char* name, std::uint32_t def) {
  if (const char* e = std::getenv(name)) return static_cast<std::uint32_t>(std::strtoul(e, nullptr, 10));
  return def;
}

// Shared camera config used across tests. Low enough resolution that per-frame
// payload is ~2.3 MB, so tests finish in under a few seconds even on modest
// links. It must be one of the sensor's native modes: override with
// $TELEFACET_TEST_WIDTH / $TELEFACET_TEST_HEIGHT for a sensor without this one
// (e.g. 2028x1520 on an IMX477).
struct TestCameraCfg {
  std::uint32_t width  = envU32("TELEFACET_TEST_WIDTH", 1456);
  std::uint32_t height = envU32("TELEFACET_TEST_HEIGHT", 1088);
};

// The sensor substring for discover ($TELEFACET_TEST_SENSOR; empty ⇒ the
// server's default, imx519).
inline std::string testSensor() {
  if (const char* e = std::getenv("TELEFACET_TEST_SENSOR")) return e;
  return {};
}

inline constexpr std::uint32_t kFourccYU12 = 0x32315559u;

}  // namespace telefacet::test

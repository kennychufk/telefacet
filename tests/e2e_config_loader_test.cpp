#include <gtest/gtest.h>

#include <string>

#include "config/ConfigLoader.hpp"

#ifndef TELEFACET_TEST_FIXTURES_DIR
#define TELEFACET_TEST_FIXTURES_DIR "."
#endif

TEST(ConfigLoader, LoadsExampleYaml) {
  auto path = std::string(TELEFACET_TEST_FIXTURES_DIR) + "/test_config.yaml";
  auto cfg = telefacet::config::loadFromFile(path);

  ASSERT_EQ(cfg.servers.size(), 1u);
  EXPECT_EQ(cfg.servers[0].address, "ws://localhost:9001");
  EXPECT_EQ(cfg.servers[0].sensor, "imx519");
  ASSERT_TRUE(cfg.servers[0].width.has_value());
  ASSERT_TRUE(cfg.servers[0].height.has_value());
  EXPECT_EQ(*cfg.servers[0].width, 1456u);
  EXPECT_EQ(*cfg.servers[0].height, 1088u);

  EXPECT_EQ(cfg.saving.mode, "none");
}

TEST(ConfigLoader, ThrowsOnMissingFile) {
  EXPECT_THROW(
      telefacet::config::loadFromFile("/nonexistent/path/to/config.yaml"),
      std::runtime_error);
}

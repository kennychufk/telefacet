#include <spdlog/spdlog.h>

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>

#include "app/App.hpp"
#include "config/ConfigLoader.hpp"
#include "util/Log.hpp"

int main(int argc, char** argv) {
  telefacet::log::init();
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <config.yaml>\n", argv[0]);
    return EXIT_FAILURE;
  }
  try {
    auto cfg = telefacet::config::loadFromFile(argv[1]);
    spdlog::info("loaded config: {} server(s)", cfg.servers.size());
    telefacet::app::App app(std::move(cfg));
    return app.run();
  } catch (const std::exception& e) {
    spdlog::critical("fatal: {}", e.what());
    return EXIT_FAILURE;
  }
}

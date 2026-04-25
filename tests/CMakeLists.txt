# Headless end-to-end test binary. Links against libtelefacet_core (no GUI
# deps) and drives a live camera_ws_server over the real CHUN/CHNK protocol.
# Requires IMX519 hardware attached to the server.

add_executable(telefacet_e2e_tests
  harness.cpp
  e2e_lifecycle_test.cpp
  e2e_protocol_test.cpp
  e2e_save_mode_test.cpp
  e2e_config_loader_test.cpp
)

target_include_directories(telefacet_e2e_tests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(telefacet_e2e_tests PRIVATE
  telefacet_core
  GTest::gtest
  GTest::gtest_main
)

target_compile_definitions(telefacet_e2e_tests PRIVATE
  TELEFACET_TEST_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
)

include(GoogleTest)
gtest_discover_tests(telefacet_e2e_tests
  PROPERTIES LABELS e2e
  DISCOVERY_MODE PRE_TEST
)

// Switching sensors without restarting the server or this client
// (cherupi-v4l2 protocol §4.1): a commander's `discover` naming another
// sensor releases the current cameras, so the same Pi moves between, say, an
// IMX477 and an IMX708 config.
//
// Opt-in: needs a server with two different sensors attached.
// TELEFACET_TEST_SENSOR / _WIDTH / _HEIGHT name the first (as for the other
// tests) and TELEFACET_TEST_SENSOR2 / _WIDTH2 / _HEIGHT2 the second; each size
// must be one of that sensor's native modes. Skipped unless SENSOR2 is set.

#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "config/ConfigLoader.hpp"
#include "data/CameraStore.hpp"
#include "harness.hpp"
#include "net/MultiServerManager.hpp"
#include "test_env.hpp"

using namespace std::chrono_literals;
using telefacet::test::BlockingClient;
using telefacet::test::envU32;
using telefacet::test::serverUrl;

namespace {

struct Sensor {
  std::string   name;
  std::uint32_t width  = 0;
  std::uint32_t height = 0;
};

Sensor first() {
  telefacet::test::TestCameraCfg cam;
  return {telefacet::test::testSensor(), cam.width, cam.height};
}

Sensor second() {
  const char* s = std::getenv("TELEFACET_TEST_SENSOR2");
  return {s ? s : "", envU32("TELEFACET_TEST_WIDTH2", 0),
          envU32("TELEFACET_TEST_HEIGHT2", 0)};
}

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return s;
}

bool waitUntil(const std::function<bool()>& pred,
               std::chrono::milliseconds timeout = 10s) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (pred()) return true;
    std::this_thread::sleep_for(20ms);
  }
  return pred();
}

class SensorSwitch : public ::testing::Test {
 protected:
  void SetUp() override {
    a_ = first();
    b_ = second();
    if (b_.name.empty())
      GTEST_SKIP() << "set TELEFACET_TEST_SENSOR2 (and _WIDTH2 / _HEIGHT2) "
                      "to a second sensor on the server";
    ASSERT_FALSE(a_.name.empty()) << "TELEFACET_TEST_SENSOR names the first";
    ASSERT_NE(lower(a_.name), lower(b_.name));
    ASSERT_GT(b_.width, 0u);
    ASSERT_GT(b_.height, 0u);
  }

  // Walk the server back to IDLE however a test ends: a BlockingClient's
  // teardown stops and unconfigures (its discover names the first sensor,
  // which releases the second's cameras if they are the active ones).
  void TearDown() override {
    if (b_.name.empty()) return;
    BlockingClient c(serverUrl());
    try { c.connect(); } catch (...) {}
  }

  Sensor a_, b_;
};

// configure → start → stream camera 0 → one frame of the sensor's size.
void bringUp(BlockingClient& c, const Sensor& s, std::uint32_t camera_id) {
  c.sendAndExpectStatus(
      [&] { return c.raw().configureCameras(s.width, s.height); });
  c.sendAndExpectStatus([&] { return c.raw().startCameras(); });
  c.drainFrames();
  c.sendAndExpectStatus([&] { return c.raw().startStream(camera_id); });
  auto f = c.nextFrame(10s);
  ASSERT_TRUE(f);
  EXPECT_EQ(f->width, s.width) << s.name;
  EXPECT_EQ(f->height, s.height) << s.name;
}

// The GUI's session for one config: what App builds on a Load.
struct Session {
  telefacet::config::Config                         cfg;
  std::unique_ptr<telefacet::net::MultiServerManager> msm;

  Session(telefacet::data::CameraStore& store, const Sensor& s) {
    telefacet::config::ServerCfg sc;
    sc.address = serverUrl();
    sc.sensor  = s.name;
    sc.width   = s.width;
    sc.height  = s.height;
    cfg.servers.push_back(sc);
    msm = std::make_unique<telefacet::net::MultiServerManager>(store, cfg);
  }
  telefacet::net::WebSocketClient& client() { return *msm->client(0); }
  std::string state() { return client().serverState(); }
};

// The latest frame of global camera 0 within `timeout`, or null.
std::shared_ptr<telefacet::data::FrameBuffer> nextFrame(
    telefacet::data::CameraStore& store, std::chrono::milliseconds timeout) {
  std::uint64_t seen = 0;
  std::shared_ptr<telefacet::data::FrameBuffer> f;
  waitUntil([&] { return (f = store.consumeIfNew(0, seen)) != nullptr; },
            timeout);
  return f;
}

}  // namespace

// One connection: discover the other sensor, from RUNNING with a stream open,
// and back. Each discover releases the cameras (the server pushes `state`
// IDLE) and the new sensor comes up at its own size.
TEST_F(SensorSwitch, DiscoverOnOneConnection) {
  BlockingClient c(serverUrl());
  c.connect();
  auto cams = c.waitForDiscovery();
  ASSERT_FALSE(cams.empty()) << a_.name << " not found";
  bringUp(c, a_, cams[0].id);
  if (HasFatalFailure()) return;

  for (const Sensor& s : {b_, a_}) {
    SCOPED_TRACE(s.name);
    ASSERT_TRUE(c.raw().sendCommand(
        {{"cmd", "discover"}, {"params", {{"sensor", s.name}}}}));
    cams = c.waitForDiscovery();
    ASSERT_FALSE(cams.empty()) << s.name << " not found";
    EXPECT_NE(lower(cams[0].sensor_type).find(lower(s.name)), std::string::npos)
        << cams[0].sensor_type;
    EXPECT_TRUE(waitUntil([&] { return c.raw().serverState() == "idle"; }, 5s))
        << "server state: " << c.raw().serverState();
    bringUp(c, s, cams[0].id);
    if (HasFatalFailure()) return;
  }
}

// Loading another config in the GUI: the session is closed while streaming
// and the new config's opened at once, as App::loadPendingConfig does. The new
// connection must get the commander slot on its first attempt (no 503 from
// the old one still holding it), find the cameras released, and stream the
// new sensor.
TEST_F(SensorSwitch, LoadAnotherConfigWhileStreaming) {
  telefacet::data::CameraStore store;
  bool first_session = true;
  for (const Sensor& s : {b_, a_, b_}) {
    SCOPED_TRACE(s.name);
    Session session(store, s);
    session.msm->connectAll();
    ASSERT_TRUE(waitUntil([&] {
      return session.client().connected() && !store.snapshot().empty() &&
             !session.state().empty();
    })) << "connected " << session.client().connected();
    EXPECT_EQ(session.client().refusedUpgrades(), 0)
        << "the previous session still held the commander slot";

    if (first_session) {
      // Whatever an earlier run left: the same sensor keeps its cameras.
      if (session.state() == "running") session.msm->stopAllCameras();
      waitUntil([&] { return session.state() != "running"; }, 15s);
      if (session.state() == "configured") session.msm->unconfigureAll();
      first_session = false;
    }
    ASSERT_TRUE(waitUntil([&] { return session.state() == "idle"; }, 5s))
        << "server state: " << session.state();

    session.msm->configureAll();
    ASSERT_TRUE(waitUntil([&] { return session.state() == "configured"; }))
        << session.state();
    session.msm->startAllCameras();
    ASSERT_TRUE(waitUntil([&] { return session.state() == "running"; }))
        << session.state();
    ASSERT_TRUE(session.msm->startStream(0));
    auto f = nextFrame(store, 10s);
    ASSERT_TRUE(f) << "no frame";
    EXPECT_EQ(f->width, s.width);
    EXPECT_EQ(f->height, s.height);

    // Load: close for good (still streaming), forget the roster.
    session.msm->disconnectAll();
    session.msm.reset();
    store.clear();
  }
}

// Loading the same config again: the sensor does not change, so the server
// keeps its cameras running and the new session only reopens the stream.
TEST_F(SensorSwitch, LoadTheSameConfigKeepsTheCamerasRunning) {
  telefacet::data::CameraStore store;
  {
    Session session(store, a_);
    session.msm->connectAll();
    ASSERT_TRUE(waitUntil([&] {
      return !store.snapshot().empty() && !session.state().empty();
    }));
    if (session.state() == "idle") session.msm->configureAll();
    ASSERT_TRUE(waitUntil([&] { return session.state() == "configured" ||
                                       session.state() == "running"; }));
    if (session.state() == "configured") session.msm->startAllCameras();
    ASSERT_TRUE(waitUntil([&] { return session.state() == "running"; }));
    ASSERT_TRUE(session.msm->startStream(0));
    ASSERT_TRUE(nextFrame(store, 10s));
    session.msm->disconnectAll();
  }
  store.clear();

  Session again(store, a_);
  again.msm->connectAll();
  ASSERT_TRUE(waitUntil([&] {
    return !store.snapshot().empty() && !again.state().empty();
  }));
  EXPECT_EQ(again.client().refusedUpgrades(), 0);
  EXPECT_EQ(again.state(), "running");
  ASSERT_TRUE(again.msm->startStream(0));
  auto f = nextFrame(store, 10s);
  ASSERT_TRUE(f);
  EXPECT_EQ(f->width, a_.width);
}

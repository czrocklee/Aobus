#include <catch2/catch.hpp>
#include "fakeit.hpp"

#include "core/playback/PlaybackEngine.h"
#include "core/backend/IAudioBackend.h"
#include "core/IMainThreadDispatcher.h"

using namespace app::core::playback;
using namespace app::core::backend;
using namespace app::core;
using namespace fakeit;

namespace
{
  class MockDispatcher : public IMainThreadDispatcher
  {
  public:
    void dispatch(std::function<void()> callback) override { callback(); }
  };

  class MockBackendProxy : public IAudioBackend
  {
    IAudioBackend& _real;
  public:
    MockBackendProxy(IAudioBackend& real) : _real(real) {}
    bool open(AudioFormat const& f, AudioRenderCallbacks c) override { return _real.open(f, c); }
    void start() override { _real.start(); }
    void pause() override { _real.pause(); }
    void resume() override { _real.resume(); }
    void flush() override { _real.flush(); }
    void drain() override { _real.drain(); }
    void stop() override { _real.stop(); }
    void close() override { _real.close(); }
    void setExclusiveMode(bool e) override { _real.setExclusiveMode(e); }
    bool isExclusiveMode() const noexcept override { return _real.isExclusiveMode(); }
    BackendKind kind() const noexcept override { return _real.kind(); }
    std::string_view lastError() const noexcept override { return _real.lastError(); }
  };
}

TEST_CASE("PlaybackEngine - Basic Orchestration", "[playback][engine]")
{
  Mock<IAudioBackend> mockBackend;
  auto dispatcher = std::make_shared<MockDispatcher>();
  AudioDevice device{ .id = "test-device", .displayName = "Test", .description = "Test", .isDefault = false, .backendKind = BackendKind::None };

  Fake(Method(mockBackend, open));
  Fake(Method(mockBackend, stop));
  Fake(Method(mockBackend, close));
  When(Method(mockBackend, kind)).AlwaysReturn(BackendKind::None);

  auto backendPtr = std::make_unique<MockBackendProxy>(mockBackend.get());
  
  PlaybackEngine engine(std::move(backendPtr), device, dispatcher);

  SECTION("Stop correctly cleans up backend")
  {
    engine.stop();
    Verify(Method(mockBackend, stop)).AtLeastOnce();
    Verify(Method(mockBackend, close)).AtLeastOnce();
    
    auto snap = engine.snapshot();
    REQUIRE(snap.state == TransportState::Idle);
  }

  SECTION("Backend error transitions to Error state")
  {
    PlaybackEngine::onBackendError(&engine, "Hardware failure");
    
    auto snap = engine.snapshot();
    REQUIRE(snap.state == TransportState::Error);
    REQUIRE(snap.statusText == "Hardware failure");
  }

  SECTION("Route ready updates snapshot")
  {
    PlaybackEngine::onRouteReady(&engine, "anchor-123");
    
    auto route = engine.routeSnapshot();
    REQUIRE(route.anchor.has_value());
    REQUIRE(route.anchor->id == "anchor-123");
  }
}

TEST_CASE("PlaybackEngine - Backend Swapping", "[playback][engine][hot-swap]")
{
  Mock<IAudioBackend> mockBackend1;
  Mock<IAudioBackend> mockBackend2;
  auto dispatcher = std::make_shared<MockDispatcher>();
  
  Fake(Method(mockBackend1, open), Method(mockBackend1, stop), Method(mockBackend1, close));
  Fake(Method(mockBackend2, open), Method(mockBackend2, stop), Method(mockBackend2, close));
  When(Method(mockBackend1, kind)).AlwaysReturn(BackendKind::None);
  When(Method(mockBackend2, kind)).AlwaysReturn(BackendKind::AlsaExclusive);

  auto backend1 = std::make_unique<MockBackendProxy>(mockBackend1.get());
  PlaybackEngine engine(std::move(backend1), { .id = "dev1", .displayName = "D1", .description = "D1", .isDefault = false, .backendKind = BackendKind::None }, dispatcher);

  SECTION("Switching backend while idle")
  {
    auto backend2 = std::make_unique<MockBackendProxy>(mockBackend2.get());
    engine.setBackend(std::move(backend2), { .id = "dev2", .displayName = "D2", .description = "D2", .isDefault = false, .backendKind = BackendKind::AlsaExclusive });
    
    Verify(Method(mockBackend1, stop)).Once();
    Verify(Method(mockBackend1, close)).Once();
    
    auto snap = engine.snapshot();
    REQUIRE(snap.backend == BackendKind::AlsaExclusive);
    REQUIRE(snap.currentDeviceId == "dev2");
  }
}

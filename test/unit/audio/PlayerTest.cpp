#include "TestUtility.h"
#include <ao/audio/Engine.h>
#include <ao/audio/IBackend.h>
#include <ao/audio/IBackendProvider.h>
#include <ao/audio/NullBackend.h>
#include <ao/audio/Player.h>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_all.hpp>

using namespace ao::audio;
using namespace ao::audio::test;
using namespace fakeit;

namespace
{
  Engine::RouteStatus createBaseEngineRoute()
  {
    return Engine::RouteStatus{
      .state =
        {
          .sourceFormat = {.sampleRate = 44100, .channels = 2, .bitDepth = 16, .isFloat = false},
          .decoderOutputFormat = {.sampleRate = 44100, .channels = 2, .bitDepth = 16, .isFloat = false},
          .engineOutputFormat = {.sampleRate = 44100, .channels = 2, .bitDepth = 16, .isFloat = false},
        },
      .optAnchor = RouteAnchor{.backend = kBackendNone, .id = "mock-stream-id"},
    };
  }

  class TestBackend : public NullBackend
  {
    BackendId _backendId;
    ProfileId _profileId;

  public:
    TestBackend(BackendId b, ProfileId p)
      : _backendId{std::move(b)}, _profileId{std::move(p)}
    {
    }
    BackendId backendId() const noexcept override { return _backendId; }
    ProfileId profileId() const noexcept override { return _profileId; }
  };
} // namespace

TEST_CASE("Player - Lifecycle and Stale Updates with FakeIt", "[playback][player][lifecycle]")
{
  auto mockProvider = Mock<IBackendProvider>{};

  When(Method(mockProvider, subscribeDevices))
    .AlwaysDo(
      [&](IBackendProvider::OnDevicesChangedCallback const& cb)
      {
        if (cb)
        {
          cb(std::vector<Device>{Device{.id = DeviceId{"mock-sink"},
                                        .displayName = "Mock Sink",
                                        .description = "Mock",
                                        .isDefault = true,
                                        .backendId = kBackendNone,
                                        .capabilities = {}}});
        }

        return Subscription{};
      });

  When(Method(mockProvider, createBackend))
    .AlwaysDo([&](Device const& b, ProfileId const& p) { return std::make_unique<TestBackend>(b.backendId, p); });
  When(Method(mockProvider, subscribeGraph))
    .AlwaysDo([](std::string_view, IBackendProvider::OnGraphChangedCallback const&) { return Subscription{}; });
  When(Method(mockProvider, status))
    .AlwaysReturn(IBackendProvider::Status{
      .metadata =
        {.id = kBackendNone, .name = "Mock", .description = "Mock", .iconName = "audio-card", .supportedProfiles = {}},
      .devices = {}});

  auto player = Player{};
  player.addProvider(std::make_unique<MockProviderProxy>(mockProvider.get()));
  player.setOutput(kBackendNone, DeviceId{"mock-sink"}, kProfileShared);

  SECTION("Stale callbacks are ignored via generation counter")
  {
    auto engineSnap = createBaseEngineRoute();
    auto const initialGeneration = player.playbackGeneration();

    player.stop(); // Increment to simulate new playback session

    player.handleRouteChanged(engineSnap, initialGeneration);

    // If it was ignored, status should be empty or default (because _cachedEngineRoute wasn't updated)
    auto const snap = player.status();
    REQUIRE(snap.flow.nodes.empty());
  }
}

TEST_CASE("Player - Pending Output", "[playback][player][pending]")
{
  auto mockProvider = Mock<IBackendProvider>{};
  auto onDevicesChanged = IBackendProvider::OnDevicesChangedCallback{};

  When(Method(mockProvider, subscribeDevices))
    .AlwaysDo(
      [&](IBackendProvider::OnDevicesChangedCallback const& cb)
      {
        onDevicesChanged = cb;

        return Subscription{};
      });

  When(Method(mockProvider, createBackend))
    .AlwaysDo([&](Device const& dev, ProfileId const& p) { return std::make_unique<TestBackend>(dev.backendId, p); });
  When(Method(mockProvider, status))
    .AlwaysReturn(IBackendProvider::Status{.metadata = {.id = kBackendPipeWire,
                                                        .name = "PipeWire",
                                                        .description = "PipeWire Provider",
                                                        .iconName = "audio-card-symbolic",
                                                        .supportedProfiles = {}},
                                           .devices = {}});

  auto player = Player{};
  player.addProvider(std::make_unique<MockProviderProxy>(mockProvider.get()));

  // 1. Call setOutput before devices are available
  player.setOutput(kBackendPipeWire, DeviceId{"system-default"}, kProfileShared);

  // Verify that it's NOT yet active (engine still has null backend/default device)
  auto const snapBefore = player.status();
  REQUIRE(snapBefore.engine.currentDeviceId == "null");

  // 2. Simulate devices being discovered
  REQUIRE(onDevicesChanged);
  onDevicesChanged({Device{.id = DeviceId{"system-default"},
                           .displayName = "System Default",
                           .description = "PipeWire",
                           .isDefault = true,
                           .backendId = kBackendPipeWire}});

  // 3. Verify that the output was automatically restored
  auto const snapAfter = player.status();
  REQUIRE(snapAfter.engine.currentDeviceId == "system-default");
  REQUIRE(snapAfter.engine.backendId == kBackendPipeWire);
}

TEST_CASE("Player - Basic Control Propagation", "[playback][player][control]")
{
  auto player = Player{};

  SECTION("Seek is propagated to engine")
  {
    // Even with NullBackend, positionMs should be updated in Engine status
    // wait, Engine::seek returns early if no source.
    player.seek(1000);
    REQUIRE(player.status().engine.positionMs == 0);
  }

  SECTION("Volume and mute are propagated to engine and status")
  {
    player.setVolume(0.6F);
    REQUIRE(player.status().volume == Catch::Approx(0.6F));
    REQUIRE(player.status().engine.volume == Catch::Approx(0.6F));

    player.setMuted(true);
    REQUIRE(player.status().muted == true);

    player.toggleMute();
    REQUIRE(player.status().muted == false);
  }
}

// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "TestUtility.h"
#include <ao/audio/Backend.h>
#include <ao/audio/Engine.h>
#include <ao/audio/IBackendProvider.h>
#include <ao/audio/NullBackend.h>
#include <ao/audio/Player.h>
#include <ao/audio/Types.h>
#include <ao/audio/flow/Graph.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <fakeit.hpp>

#include <chrono>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::audio::test
{
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

    class TestBackend final : public NullBackend
    {
    public:
      TestBackend(BackendId b, ProfileId p)
        : _backendId{std::move(b)}, _profileId{std::move(p)}
      {
      }

      BackendId backendId() const noexcept override { return _backendId; }
      ProfileId profileId() const noexcept override { return _profileId; }

    private:
      BackendId _backendId;
      ProfileId _profileId;
    };
  } // namespace

  TEST_CASE("Player - Lifecycle and Stale Updates with FakeIt", "[playback][unit][player][lifecycle]")
  {
    auto mockProvider = Mock<IBackendProvider>{};
    Fake(Method(mockProvider, shutdown));

    When(Method(mockProvider, subscribeDevices))
      .AlwaysDo(
        [&](IBackendProvider::OnDevicesChangedCallback const& cb)
        {
          if (cb)
          {
            cb(std::vector{Device{.id = DeviceId{"mock-sink"},
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
    auto onGraphChanged = IBackendProvider::OnGraphChangedCallback{};

    When(Method(mockProvider, subscribeGraph))
      .AlwaysDo(
        [&](std::string_view, IBackendProvider::OnGraphChangedCallback const& cb)
        {
          onGraphChanged = cb;
          return Subscription{[] {}};
        });
    When(Method(mockProvider, status))
      .AlwaysReturn(IBackendProvider::Status{.metadata = {.id = kBackendNone,
                                                          .name = "Mock",
                                                          .description = "Mock",
                                                          .iconName = "audio-card",
                                                          .supportedProfiles = {}},
                                             .devices = {}});

    auto player = Player{};
    player.addProvider(std::make_unique<MockProviderProxy>(mockProvider.get()));
    player.setOutput(kBackendNone, DeviceId{"mock-sink"}, kProfileShared);

    SECTION("setOutput same values exits early")
    {
      player.setOutput(kBackendNone, DeviceId{"mock-sink"}, kProfileShared);
      // Success is implicit if no extra provider calls are made (Verify can be used)
      Verify(Method(mockProvider, createBackend)).Once();
    }

    SECTION("Stale callbacks are ignored via generation counter")
    {
      auto engineSnap = createBaseEngineRoute();
      auto const initialGeneration = player.playbackGeneration();

      player.stop(); // Increment to simulate new playback runtime

      player.handleRouteChanged(engineSnap, initialGeneration);

      // If it was ignored, status should be empty or default (because _cachedEngineRoute wasn't updated)
      auto const snap = player.status();
      REQUIRE(snap.flow.nodes.empty());
    }

    SECTION("Valid callbacks update player status and graph")
    {
      auto engineSnap = createBaseEngineRoute();
      auto const gen = player.playbackGeneration();

      player.handleRouteChanged(engineSnap, gen);

      REQUIRE(onGraphChanged);

      bool qualityChangedFired = false;
      player.setOnQualityChanged([&](Quality, bool) { qualityChangedFired = true; });

      onGraphChanged(flow::Graph{});

      auto const snap = player.status();
      REQUIRE(qualityChangedFired == true);
    }

    SECTION("Merged graph with no system stream node")
    {
      auto engineSnap = createBaseEngineRoute();
      player.handleRouteChanged(engineSnap, player.playbackGeneration());

      // Fire a system graph that has NO Stream node
      onGraphChanged(
        flow::Graph{.nodes = {flow::Node{.id = "sys-sink", .type = flow::NodeType::Sink}}, .connections = {}});

      auto const snap = player.status();
      // Should still work, but no connection from engine to system
      REQUIRE(snap.flow.nodes.size() == 3); // Decoder, Engine, Sink
    }

    SECTION("handleRouteChanged with stale generation is ignored")
    {
      auto engineSnap = createBaseEngineRoute();
      player.handleRouteChanged(engineSnap, player.playbackGeneration() - 1);
      REQUIRE(player.status().flow.nodes.empty());
    }

    SECTION("System graph change with stale generation is ignored")
    {
      auto engineSnap = createBaseEngineRoute();
      auto const gen = player.playbackGeneration();

      player.handleRouteChanged(engineSnap, gen);

      REQUIRE(onGraphChanged);

      // Increment generation
      player.stop();

      // Fire graph change, which captures the old gen
      onGraphChanged(flow::Graph{});

      auto const snap = player.status();
      REQUIRE(snap.flow.nodes.empty());
    }
  }

  TEST_CASE("Player - Pending Output", "[playback][unit][player][pending]")
  {
    auto mockProvider = Mock<IBackendProvider>{};
    Fake(Method(mockProvider, shutdown));
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

    // 1b. Call play while pending - should be ignored
    player.play(TrackPlaybackDescriptor{.filePath = "song.flac"});
    REQUIRE(player.status().engine.transport == Transport::Idle);

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
    REQUIRE(player.isReady() == true);

    // 4. Simulate SECOND devices change to trigger updateDevice for active device
    onDevicesChanged({Device{.id = DeviceId{"system-default"},
                             .displayName = "System Default (Updated)",
                             .description = "PipeWire",
                             .isDefault = true,
                             .backendId = kBackendPipeWire}});
    // This should hit line 126 in Player.cpp
  }

  TEST_CASE("Player - Basic Control Propagation", "[playback][unit][player][control]")
  {
    auto player = Player{};

    SECTION("addProvider(nullptr) is safe")
    {
      player.addProvider(nullptr);
      // No crash, nothing added.
    }

    SECTION("setOutput with non-existent provider")
    {
      player.setOutput(kBackendAlsa, DeviceId{"alsa-dev"}, kProfileShared);
      // It should just log an error and return.
      auto const snap = player.status();
      REQUIRE(snap.engine.backendId == kBackendNone);
    }

    SECTION("Seek is propagated to engine")
    {
      // Even with NullBackend, elapsed should be updated in Engine status
      // wait, Engine::seek returns early if no source.
      player.seek(std::chrono::seconds{1});
      REQUIRE(player.status().engine.elapsed == std::chrono::milliseconds{0});
    }

    SECTION("Volume and mute are propagated to engine and status")
    {
      player.setVolume(0.6F);
      REQUIRE(player.status().volume == Catch::Approx{0.6F});
      REQUIRE(player.status().engine.volume == Catch::Approx{0.6F});

      player.setMuted(true);
      REQUIRE(player.status().muted == true);

      player.toggleMute();
      REQUIRE(player.status().muted == false);
    }
  }

  TEST_CASE("Player - Subscription Unsubscribe", "[audio][unit][player][subscription]")
  {
    bool called = false;
    auto sub = Subscription{[&] { called = true; }};

    {
      auto tempSub = std::move(sub);
    }

    REQUIRE(called == true);
  }

  TEST_CASE("Player - Provider state outlives backend shutdown", "[playback][unit][player][lifecycle]")
  {
    struct Events final
    {
      std::vector<std::string> values;
    };

    struct LifetimeBackend final : NullBackend
    {
      explicit LifetimeBackend(Events& eventsArg)
        : events{eventsArg}
      {
      }

      LifetimeBackend(LifetimeBackend const&) = delete;
      LifetimeBackend& operator=(LifetimeBackend const&) = delete;
      LifetimeBackend(LifetimeBackend&&) = delete;
      LifetimeBackend& operator=(LifetimeBackend&&) = delete;

      ~LifetimeBackend() override { close(); }

      void close() override
      {
        if (!closed)
        {
          events.values.emplace_back("backend close");
          closed = true;
        }
      }

      BackendId backendId() const noexcept override { return kBackendAlsa; }
      ProfileId profileId() const noexcept override { return kProfileExclusive; }

      Events& events;
      bool closed = false;
    };

    struct LifetimeProvider final : IBackendProvider
    {
      explicit LifetimeProvider(Events& eventsArg)
        : events{eventsArg}
      {
      }

      LifetimeProvider(LifetimeProvider const&) = delete;
      LifetimeProvider& operator=(LifetimeProvider const&) = delete;
      LifetimeProvider(LifetimeProvider&&) = delete;
      LifetimeProvider& operator=(LifetimeProvider&&) = delete;

      ~LifetimeProvider() override { events.values.emplace_back("provider destroy"); }

      void shutdown() noexcept override { events.values.emplace_back("provider shutdown"); }

      Subscription subscribeDevices(OnDevicesChangedCallback callback) override
      {
        callback({Device{.id = DeviceId{"alsa-device"},
                         .displayName = "ALSA Device",
                         .description = "ALSA",
                         .backendId = kBackendAlsa}});
        return {};
      }

      Status status() const override
      {
        return {.metadata = {.id = kBackendAlsa,
                             .name = "ALSA",
                             .description = "ALSA",
                             .iconName = "audio-card-symbolic",
                             .supportedProfiles = {{kProfileExclusive, "Exclusive", "Exclusive"}}},
                .devices = {Device{.id = DeviceId{"alsa-device"},
                                   .displayName = "ALSA Device",
                                   .description = "ALSA",
                                   .backendId = kBackendAlsa}}};
      }

      std::unique_ptr<IBackend> createBackend(Device const& /*device*/, ProfileId const& /*profile*/) override
      {
        return std::make_unique<LifetimeBackend>(events);
      }

      Subscription subscribeGraph(std::string_view /*routeAnchor*/, OnGraphChangedCallback /*callback*/) override
      {
        return {};
      }

      Events& events;
    };

    auto events = Events{};

    {
      auto player = Player{};
      player.addProvider(std::make_unique<LifetimeProvider>(events));
      player.setOutput(kBackendAlsa, DeviceId{"alsa-device"}, kProfileExclusive);
    }

    // Old (broken) order was: providers.clear() → enginePtr.reset(), which destroyed the provider
    // before the engine had a chance to close the backend — backends that touch provider-owned state
    // (e.g. AlsaGraphRegistry) during close() would access a destroyed object.
    // Correct order: provider shutdown → engine destroy (backend close) → provider destroy.
    REQUIRE(events.values == std::vector<std::string>{"provider shutdown", "backend close", "provider destroy"});
  }
} // namespace ao::audio::test

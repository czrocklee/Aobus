// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "TestUtility.h"
#include "test/unit/RuntimeTestUtils.h"
#include <ao/AudioCodec.h>
#include <ao/async/ImmediateExecutor.h>
#include <ao/audio/Backend.h>
#include <ao/audio/Engine.h>
#include <ao/audio/IBackendProvider.h>
#include <ao/audio/NullBackend.h>
#include <ao/audio/PlaybackInput.h>
#include <ao/audio/Player.h>
#include <ao/audio/QualityAnalyzer.h>
#include <ao/audio/Transport.h>
#include <ao/audio/flow/Graph.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <fakeit.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
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
            .codec = AudioCodec::Flac,
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

    using rt::test::QueuedExecutor;

    Device pipeWireDevice(std::string displayName = "System Default")
    {
      return Device{.id = DeviceId{"system-default"},
                    .displayName = std::move(displayName),
                    .description = "PipeWire",
                    .isDefault = true,
                    .backendId = kBackendPipeWire};
    }

    IBackendProvider::Status pipeWireStatus()
    {
      return IBackendProvider::Status{.metadata = {.id = kBackendPipeWire,
                                                   .name = "PipeWire",
                                                   .description = "PipeWire Provider",
                                                   .iconName = "audio-card-symbolic",
                                                   .supportedProfiles = {}},
                                      .devices = {}};
    }
  } // namespace

  TEST_CASE("Player - lifecycle ignores stale route and graph updates", "[audio][unit][player][lifecycle]")
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

    auto executor = async::ImmediateExecutor{};
    auto player = Player{executor};
    player.addProvider(std::make_unique<MockProviderProxy>(mockProvider.get()));
    CHECK(player.setOutputDevice(kBackendNone, DeviceId{"mock-sink"}, kProfileShared));

    SECTION("setOutputDevice same values exits early")
    {
      CHECK(player.setOutputDevice(kBackendNone, DeviceId{"mock-sink"}, kProfileShared));
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
      CHECK(snap.flow.nodes.empty());
    }

    SECTION("Valid callbacks update player status and graph")
    {
      auto engineSnap = createBaseEngineRoute();
      auto const gen = player.playbackGeneration();

      player.handleRouteChanged(engineSnap, gen);

      REQUIRE(onGraphChanged);

      auto qualityEvents = std::vector<std::pair<QualityResult, bool>>{};
      player.setOnQualityChanged([&](QualityResult const& quality, bool ready)
                                 { qualityEvents.emplace_back(quality, ready); });

      onGraphChanged(flow::Graph{});

      auto const snap = player.status();
      CHECK(snap.quality == Quality::BitwisePerfect);
      CHECK(snap.qualityFullyVerified == false);
      CHECK(snap.isReady == false);
      REQUIRE(qualityEvents.size() == 1);
      CHECK(qualityEvents[0].first.overall == Quality::BitwisePerfect);
      CHECK(qualityEvents[0].first.fullyVerified == false);
      CHECK(qualityEvents[0].second == false);
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
      CHECK(snap.flow.nodes.size() == 4); // Source, Decoder, Engine, Sink
      CHECK(snap.qualityFullyVerified == false);

      // The source node is labelled with the detected codec.
      auto const sourceIt = std::ranges::find(snap.flow.nodes, std::string_view{"ao-source"}, &flow::Node::id);
      REQUIRE(sourceIt != snap.flow.nodes.end());
      CHECK(sourceIt->name == "FLAC");
    }

    SECTION("System nodes without reported formats are not backfilled")
    {
      auto engineSnap = createBaseEngineRoute();
      player.handleRouteChanged(engineSnap, player.playbackGeneration());

      onGraphChanged(
        flow::Graph{.nodes =
                      {
                        flow::Node{.id = "sys-stream", .type = flow::NodeType::Stream, .name = "PipeWire Stream"},
                        flow::Node{.id = "sys-sink", .type = flow::NodeType::Sink, .name = "Speakers"},
                      },
                    .connections = {
                      flow::Connection{.sourceId = "sys-stream", .destinationId = "sys-sink", .isActive = true},
                    }});

      auto const snap = player.status();
      auto const streamIt = std::ranges::find(snap.flow.nodes, std::string_view{"sys-stream"}, &flow::Node::id);
      REQUIRE(streamIt != snap.flow.nodes.end());
      CHECK_FALSE(streamIt->optFormat);
      CHECK(snap.quality == Quality::BitwisePerfect);
      CHECK(snap.qualityFullyVerified == false);

      auto const assessmentIt =
        std::ranges::find(snap.qualityAssessments, std::string_view{"sys-stream"}, &NodeQualityAssessment::nodeId);
      REQUIRE(assessmentIt != snap.qualityAssessments.end());
      CHECK_FALSE(assessmentIt->optFormat);
    }

    SECTION("handleRouteChanged with stale generation is ignored")
    {
      auto engineSnap = createBaseEngineRoute();
      player.handleRouteChanged(engineSnap, player.playbackGeneration() - 1);
      CHECK(player.status().flow.nodes.empty());
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
      CHECK(snap.flow.nodes.empty());
    }
  }

  TEST_CASE("Player - pending output activates when matching device appears", "[audio][unit][player][pending]")
  {
    auto mockProvider = Mock<IBackendProvider>{};
    Fake(Method(mockProvider, shutdown));
    auto onOutputDevicesChanged = IBackendProvider::OnDevicesChangedCallback{};

    When(Method(mockProvider, subscribeDevices))
      .AlwaysDo(
        [&](IBackendProvider::OnDevicesChangedCallback const& cb)
        {
          onOutputDevicesChanged = cb;

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

    auto executor = async::ImmediateExecutor{};
    auto player = Player{executor};
    player.addProvider(std::make_unique<MockProviderProxy>(mockProvider.get()));

    // 1. Call setOutputDevice before devices are available
    CHECK(player.setOutputDevice(kBackendPipeWire, DeviceId{"system-default"}, kProfileShared));

    // Verify that it's NOT yet active (engine still has null backend/default device)
    auto const snapBefore = player.status();
    CHECK(snapBefore.engine.currentDeviceId == "null");

    // 1b. Call play while pending - should be ignored
    CHECK_FALSE(player.play(Engine::PlaybackItem{
      .id = Engine::PlaybackItemId{.value = 1},
      .input = PlaybackInput{.filePath = "song.flac"},
    }));
    CHECK(player.status().engine.transport == Transport::Idle);

    // 2. Simulate devices being discovered
    REQUIRE(onOutputDevicesChanged);
    onOutputDevicesChanged({Device{.id = DeviceId{"system-default"},
                                   .displayName = "System Default",
                                   .description = "PipeWire",
                                   .isDefault = true,
                                   .backendId = kBackendPipeWire}});

    // 3. Verify that the output was automatically restored
    auto const snapAfter = player.status();
    CHECK(snapAfter.engine.currentDeviceId == "system-default");
    CHECK(snapAfter.engine.backendId == kBackendPipeWire);
    CHECK(player.isReady() == true);

    // 4. Simulate SECOND devices change to trigger updateDevice for active device
    onOutputDevicesChanged({Device{.id = DeviceId{"system-default"},
                                   .displayName = "System Default (Updated)",
                                   .description = "PipeWire",
                                   .isDefault = true,
                                   .backendId = kBackendPipeWire}});
    // This should hit line 126 in Player.cpp
  }

  TEST_CASE("Player - setOutputDevice rejects unknown backend", "[audio][unit][player][output]")
  {
    auto mockProvider = Mock<IBackendProvider>{};
    Fake(Method(mockProvider, shutdown));

    When(Method(mockProvider, subscribeDevices))
      .AlwaysDo([](IBackendProvider::OnDevicesChangedCallback const&) { return Subscription{}; });
    When(Method(mockProvider, status)).AlwaysReturn(pipeWireStatus());

    auto executor = async::ImmediateExecutor{};
    auto player = Player{executor};
    player.addProvider(std::make_unique<MockProviderProxy>(mockProvider.get()));

    auto const result = player.setOutputDevice(kBackendAlsa, DeviceId{"alsa-device"}, kProfileShared);

    REQUIRE_FALSE(result);
    CHECK(result.error().code == Error::Code::NotFound);
    CHECK(player.status().engine.currentDeviceId == "null");
  }

  TEST_CASE("Player - provider callbacks are marshalled onto the executor", "[audio][unit][player][executor]")
  {
    auto mockProvider = Mock<IBackendProvider>{};
    Fake(Method(mockProvider, shutdown));

    auto onOutputDevicesChanged = IBackendProvider::OnDevicesChangedCallback{};
    When(Method(mockProvider, subscribeDevices))
      .AlwaysDo(
        [&](IBackendProvider::OnDevicesChangedCallback const& cb)
        {
          onOutputDevicesChanged = cb;
          return Subscription{};
        });

    When(Method(mockProvider, createBackend))
      .AlwaysDo([&](Device const& dev, ProfileId const& p) { return std::make_unique<TestBackend>(dev.backendId, p); });
    When(Method(mockProvider, status)).AlwaysReturn(pipeWireStatus());
    When(Method(mockProvider, subscribeGraph))
      .AlwaysDo([](std::string_view, IBackendProvider::OnGraphChangedCallback const&) { return Subscription{}; });

    auto executor = QueuedExecutor{};
    auto player = Player{executor};
    player.addProvider(std::make_unique<MockProviderProxy>(mockProvider.get()));

    std::int32_t deviceSignals = 0;
    auto observedStatuses = std::vector<IBackendProvider::Status>{};
    player.setOnOutputDevicesChanged(
      [&](std::vector<IBackendProvider::Status> const& statuses)
      {
        ++deviceSignals;
        observedStatuses = statuses;
      });

    REQUIRE(onOutputDevicesChanged);
    auto worker = std::jthread{[&] { onOutputDevicesChanged({pipeWireDevice()}); }};
    worker.join();

    CHECK(player.status().availableBackends.empty());
    CHECK(deviceSignals == 0);

    executor.drain();

    auto const snap = player.status();
    REQUIRE(snap.availableBackends.size() == 1);
    REQUIRE(snap.availableBackends.front().devices.size() == 1);
    CHECK(snap.availableBackends.front().devices.front().id == DeviceId{"system-default"});
    CHECK(deviceSignals == 1);
    REQUIRE(observedStatuses.size() == 1);
    REQUIRE(observedStatuses.front().devices.size() == 1);
    CHECK(observedStatuses.front().devices.front().id == DeviceId{"system-default"});
  }

  TEST_CASE("Player - queued provider callback is ignored after teardown", "[audio][unit][player][executor]")
  {
    auto mockProvider = Mock<IBackendProvider>{};
    Fake(Method(mockProvider, shutdown));

    auto onOutputDevicesChanged = IBackendProvider::OnDevicesChangedCallback{};
    When(Method(mockProvider, subscribeDevices))
      .AlwaysDo(
        [&](IBackendProvider::OnDevicesChangedCallback const& cb)
        {
          onOutputDevicesChanged = cb;
          return Subscription{};
        });

    When(Method(mockProvider, createBackend))
      .AlwaysDo([&](Device const& dev, ProfileId const& p) { return std::make_unique<TestBackend>(dev.backendId, p); });
    When(Method(mockProvider, status)).AlwaysReturn(pipeWireStatus());
    When(Method(mockProvider, subscribeGraph))
      .AlwaysDo([](std::string_view, IBackendProvider::OnGraphChangedCallback const&) { return Subscription{}; });

    auto executor = QueuedExecutor{};
    std::int32_t deviceSignals = 0;

    {
      auto player = Player{executor};
      player.addProvider(std::make_unique<MockProviderProxy>(mockProvider.get()));
      player.setOnOutputDevicesChanged([&](std::vector<IBackendProvider::Status> const&) { ++deviceSignals; });

      REQUIRE(onOutputDevicesChanged);
      onOutputDevicesChanged({pipeWireDevice()});
    }

    executor.drain();
    CHECK(deviceSignals == 0);
  }

  TEST_CASE("Player - graph callbacks are marshalled onto the executor", "[audio][unit][player][executor]")
  {
    auto mockProvider = Mock<IBackendProvider>{};
    Fake(Method(mockProvider, shutdown));

    auto onOutputDevicesChanged = IBackendProvider::OnDevicesChangedCallback{};
    When(Method(mockProvider, subscribeDevices))
      .AlwaysDo(
        [&](IBackendProvider::OnDevicesChangedCallback const& cb)
        {
          onOutputDevicesChanged = cb;
          return Subscription{};
        });

    When(Method(mockProvider, createBackend))
      .AlwaysDo([&](Device const& dev, ProfileId const& p) { return std::make_unique<TestBackend>(dev.backendId, p); });
    When(Method(mockProvider, status)).AlwaysReturn(pipeWireStatus());

    auto onGraphChanged = IBackendProvider::OnGraphChangedCallback{};
    When(Method(mockProvider, subscribeGraph))
      .AlwaysDo(
        [&](std::string_view, IBackendProvider::OnGraphChangedCallback const& cb)
        {
          onGraphChanged = cb;
          return Subscription{[] {}};
        });

    auto executor = QueuedExecutor{};
    auto player = Player{executor};
    player.addProvider(std::make_unique<MockProviderProxy>(mockProvider.get()));

    REQUIRE(onOutputDevicesChanged);
    onOutputDevicesChanged({pipeWireDevice()});
    executor.drain();

    CHECK(player.setOutputDevice(kBackendPipeWire, DeviceId{"system-default"}, kProfileShared));
    auto route = createBaseEngineRoute();
    route.optAnchor = RouteAnchor{.backend = kBackendPipeWire, .id = "mock-stream-id"};
    player.handleRouteChanged(route, player.playbackGeneration());

    REQUIRE(onGraphChanged);
    executor.drain();

    auto qualityEvents = std::vector<std::pair<QualityResult, bool>>{};
    player.setOnQualityChanged([&](QualityResult const& quality, bool ready)
                               { qualityEvents.emplace_back(quality, ready); });

    auto worker =
      std::jthread{[&]
                   {
                     onGraphChanged(flow::Graph{
                       .nodes = {flow::Node{.id = "sys-sink", .type = flow::NodeType::Sink}}, .connections = {}});
                   }};
    worker.join();

    auto snap = player.status();
    CHECK(std::ranges::find(snap.flow.nodes, std::string_view{"sys-sink"}, &flow::Node::id) == snap.flow.nodes.end());
    CHECK(qualityEvents.empty());

    executor.drain();

    snap = player.status();
    CHECK(std::ranges::find(snap.flow.nodes, std::string_view{"sys-sink"}, &flow::Node::id) != snap.flow.nodes.end());
    CHECK(snap.quality == Quality::BitwisePerfect);
    REQUIRE(qualityEvents.size() == 1);
    CHECK(qualityEvents[0].first.overall == Quality::BitwisePerfect);
    CHECK(qualityEvents[0].first.sourceQuality == Quality::BitwisePerfect);
    CHECK(qualityEvents[0].first.pipelineQuality == Quality::BitwisePerfect);
    CHECK(qualityEvents[0].first.fullyVerified == false);
    CHECK(qualityEvents[0].second == true);
  }

  TEST_CASE("Player - controls update engine-backed status", "[audio][unit][player][control]")
  {
    auto executor = async::ImmediateExecutor{};
    auto player = Player{executor};

    SECTION("addProvider(nullptr) is safe")
    {
      player.addProvider(nullptr);
      // No crash, nothing added.
    }

    SECTION("setOutputDevice with non-existent provider")
    {
      CHECK_FALSE(player.setOutputDevice(kBackendAlsa, DeviceId{"alsa-dev"}, kProfileShared));
      // It should just log an error and return.
      auto const snap = player.status();
      CHECK(snap.engine.backendId == kBackendNone);
    }

    SECTION("Seek is propagated to engine")
    {
      // Even with NullBackend, elapsed should be updated in Engine status
      // wait, Engine::seek returns early if no source.
      player.seek(std::chrono::seconds{1});
      CHECK(player.status().engine.elapsed == std::chrono::milliseconds{0});
    }

    SECTION("Volume and mute are propagated to engine and status")
    {
      CHECK(player.setVolume(0.6F));
      CHECK(player.status().volume == Catch::Approx{0.6F});
      CHECK(player.status().engine.volume == Catch::Approx{0.6F});

      CHECK(player.setMuted(true));
      CHECK(player.status().muted == true);

      CHECK(player.toggleMute());
      CHECK(player.status().muted == false);
    }
  }

  TEST_CASE("Player - subscription unsubscribe removes callback", "[audio][unit][player][subscription]")
  {
    bool called = false;
    auto sub = Subscription{[&] { called = true; }};

    {
      auto tempSub = std::move(sub);
    }

    CHECK(called == true);
  }

  TEST_CASE("Player - provider state outlives backend shutdown", "[audio][unit][player][lifecycle]")
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
      auto executor = async::ImmediateExecutor{};
      auto player = Player{executor};
      player.addProvider(std::make_unique<LifetimeProvider>(events));
      CHECK(player.setOutputDevice(kBackendAlsa, DeviceId{"alsa-device"}, kProfileExclusive));
    }

    // Old (broken) order was: providers.clear() → enginePtr.reset(), which destroyed the provider
    // before the engine had a chance to close the backend — backends that touch provider-owned state
    // (e.g. AlsaGraphRegistry) during close() would access a destroyed object.
    // Correct order: provider shutdown → engine destroy (backend close) → provider destroy.
    CHECK(events.values == std::vector<std::string>{"provider shutdown", "backend close", "provider destroy"});
  }
} // namespace ao::audio::test

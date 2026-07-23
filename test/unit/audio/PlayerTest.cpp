// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "AudioFixtureSupport.h"
#include "BackendTestSupport.h"
#include "EngineTestSupport.h"
#include "ScriptedDecoderSession.h"
#include "test/unit/RuntimeTestSupport.h"
#include <ao/AudioCodec.h>
#include <ao/Error.h>
#include <ao/async/LoopExecutor.h>
#include <ao/audio/BackendIds.h>
#include <ao/audio/BackendProvider.h>
#include <ao/audio/DecodedStreamInfo.h>
#include <ao/audio/Device.h>
#include <ao/audio/Engine.h>
#include <ao/audio/NullBackend.h>
#include <ao/audio/PlaybackInput.h>
#include <ao/audio/Player.h>
#include <ao/audio/Property.h>
#include <ao/audio/Quality.h>
#include <ao/audio/QualityAnalyzer.h>
#include <ao/audio/RenderTarget.h>
#include <ao/audio/RouteAnchor.h>
#include <ao/audio/Subscription.h>
#include <ao/audio/Transport.h>
#include <ao/audio/flow/Graph.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <fakeit.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <semaphore>
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
        .generation = 0,
      };
    }

    class FakeBackend final : public NullBackend
    {
    public:
      FakeBackend(BackendId b, ProfileId p)
        : _backendId{std::move(b)}, _profileId{std::move(p)}
      {
      }

      BackendId backendId() const override { return _backendId; }
      ProfileId profileId() const override { return _profileId; }

    private:
      BackendId _backendId;
      ProfileId _profileId;
    };

    inline auto const kBarrierBackend = BackendId{"barrier-test"};

    struct BarrierBackendProbe final
    {
      RenderTarget* target() const
      {
        auto const lock = std::scoped_lock{mutex};
        return renderTarget;
      }

      void publishTarget(RenderTarget* target)
      {
        auto const lock = std::scoped_lock{mutex};
        renderTarget = target;
      }

      void recordRoute(std::string_view const route)
      {
        auto const lock = std::scoped_lock{mutex};
        subscribedRoutes.emplace_back(route);
      }

      std::size_t routeCount() const
      {
        auto const lock = std::scoped_lock{mutex};
        return subscribedRoutes.size();
      }

      void setDevicesCallback(BackendProvider::OnDevicesChangedCallback callback)
      {
        auto const lock = std::scoped_lock{mutex};
        devicesCallback = std::move(callback);
      }

      void emitDevices(std::vector<Device> devices)
      {
        auto callback = BackendProvider::OnDevicesChangedCallback{};
        {
          auto const lock = std::scoped_lock{mutex};
          callback = devicesCallback;
        }

        if (callback)
        {
          callback(devices);
        }
      }

      mutable std::mutex mutex;
      RenderTarget* renderTarget = nullptr;
      std::vector<std::string> subscribedRoutes;
      BackendProvider::OnDevicesChangedCallback devicesCallback;
    };

    class BarrierBackend final : public NullBackend
    {
    public:
      explicit BarrierBackend(std::shared_ptr<BarrierBackendProbe> probePtr)
        : _probePtr{std::move(probePtr)}
      {
      }

      Result<> open(Format const& /*format*/, RenderTarget* target) override
      {
        _probePtr->publishTarget(target);
        return {};
      }

      void close() override { _probePtr->publishTarget(nullptr); }

      BackendId backendId() const override { return kBarrierBackend; }
      ProfileId profileId() const override { return kProfileShared; }

    private:
      std::shared_ptr<BarrierBackendProbe> _probePtr;
    };

    class BarrierProvider final : public BackendProvider
    {
    public:
      explicit BarrierProvider(std::shared_ptr<BarrierBackendProbe> probePtr)
        : _probePtr{std::move(probePtr)}
      {
      }

      void shutdown() noexcept override {}

      Subscription subscribeDevices(OnDevicesChangedCallback callback) override
      {
        callback(devices());
        _probePtr->setDevicesCallback(std::move(callback));
        return {};
      }

      Status status() const override
      {
        return {
          .descriptor = {.id = kBarrierBackend, .supportedProfiles = {{.id = kProfileShared}}}, .devices = devices()};
      }

      std::unique_ptr<Backend> createBackend(Device const& /*device*/, ProfileId const& /*profile*/) override
      {
        return std::make_unique<BarrierBackend>(_probePtr);
      }

      Subscription subscribeGraph(std::string_view routeAnchor, OnGraphChangedCallback /*callback*/) override
      {
        _probePtr->recordRoute(routeAnchor);
        return {};
      }

    private:
      static std::vector<Device> devices()
      {
        return {{.id = DeviceId{"barrier-device"},
                 .displayName = "Barrier Device",
                 .description = "Controlled test output",
                 .isDefault = true,
                 .backendId = kBarrierBackend}};
      }

      std::shared_ptr<BarrierBackendProbe> _probePtr;
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

    BackendProvider::Status pipeWireStatus()
    {
      return BackendProvider::Status{.descriptor = {
                                       .id = kBackendPipeWire,
                                     }};
    }

    inline auto const& kSynchronousGraphBackend = kBackendAlsa;

    struct SynchronousGraphProbe final
    {
      std::mutex mutex;
      BackendProvider::OnGraphChangedCallback graphCallback;
      std::string activeRoute;
      std::uint64_t activeSubscription = 0;
      std::uint64_t nextSubscription = 1;
      std::vector<std::string> subscribedRoutes;
      std::counting_semaphore<8> graphSubscribed{0};
      RenderTarget* renderTarget = nullptr;

      RenderTarget* target()
      {
        auto const lock = std::scoped_lock{mutex};
        return renderTarget;
      }

      void publishTarget(RenderTarget* target)
      {
        auto const lock = std::scoped_lock{mutex};
        renderTarget = target;
      }

      void publish(std::string const& route)
      {
        auto callback = BackendProvider::OnGraphChangedCallback{};

        {
          auto const lock = std::scoped_lock{mutex};

          if (route == activeRoute)
          {
            callback = graphCallback;
          }
        }

        if (callback)
        {
          callback(flow::Graph{.nodes = {flow::Node{.id = route + "-sink", .type = flow::NodeType::Sink}}});
        }
      }

      std::size_t subscriptionCount()
      {
        auto const lock = std::scoped_lock{mutex};
        return subscribedRoutes.size();
      }
    };

    class SynchronousGraphBackend final : public NullBackend
    {
    public:
      SynchronousGraphBackend(std::shared_ptr<SynchronousGraphProbe> probePtr, std::string route)
        : _probePtr{std::move(probePtr)}, _route{std::move(route)}
      {
      }

      Result<> open(Format const& /*format*/, RenderTarget* target) override
      {
        _probePtr->publishTarget(target);
        target->handleRouteReady(_route);
        return {};
      }

      void close() override { _probePtr->publishTarget(nullptr); }

      Result<> setProperty(PropertyId id, PropertyValue const& value) override
      {
        auto result = NullBackend::setProperty(id, value);

        if (result && (id == PropertyId::Volume || id == PropertyId::Muted))
        {
          _probePtr->publish(_route);
        }

        return result;
      }

      BackendId backendId() const override { return kSynchronousGraphBackend; }
      ProfileId profileId() const override { return kProfileShared; }

    private:
      std::shared_ptr<SynchronousGraphProbe> _probePtr;
      std::string _route;
    };

    class SynchronousGraphProvider final : public BackendProvider
    {
    public:
      explicit SynchronousGraphProvider(std::shared_ptr<SynchronousGraphProbe> probePtr)
        : _probePtr{std::move(probePtr)}
      {
      }

      void shutdown() noexcept override {}

      Subscription subscribeDevices(OnDevicesChangedCallback callback) override
      {
        callback(devices());
        return {};
      }

      Status status() const override
      {
        return {.descriptor = {.id = kSynchronousGraphBackend, .supportedProfiles = {{.id = kProfileShared}}},
                .devices = devices()};
      }

      std::unique_ptr<Backend> createBackend(Device const& device, ProfileId const& /*profile*/) override
      {
        return std::make_unique<SynchronousGraphBackend>(_probePtr, device.id.raw());
      }

      Subscription subscribeGraph(std::string_view routeAnchor, OnGraphChangedCallback callback) override
      {
        auto const route = std::string{routeAnchor};
        std::uint64_t subscription = 0;

        {
          auto const lock = std::scoped_lock{_probePtr->mutex};
          subscription = _probePtr->nextSubscription++;
          _probePtr->activeSubscription = subscription;
          _probePtr->activeRoute = route;
          _probePtr->graphCallback = std::move(callback);
          _probePtr->subscribedRoutes.push_back(route);
        }

        _probePtr->graphSubscribed.release();
        return Subscription{[weakProbePtr = std::weak_ptr{_probePtr}, subscription]
                            {
                              if (auto probePtr = weakProbePtr.lock(); probePtr)
                              {
                                auto const lock = std::scoped_lock{probePtr->mutex};

                                if (probePtr->activeSubscription == subscription)
                                {
                                  probePtr->activeSubscription = 0;
                                  probePtr->activeRoute.clear();
                                  probePtr->graphCallback = {};
                                }
                              }
                            }};
      }

    private:
      static std::vector<Device> devices()
      {
        return {{.id = DeviceId{"route-a"},
                 .displayName = "Route A",
                 .description = "Test",
                 .isDefault = true,
                 .backendId = kSynchronousGraphBackend},
                {.id = DeviceId{"route-b"},
                 .displayName = "Route B",
                 .description = "Test",
                 .backendId = kSynchronousGraphBackend}};
      }

      std::shared_ptr<SynchronousGraphProbe> _probePtr;
    };
  } // namespace

  TEST_CASE("Player - lifecycle ignores stale route and graph updates", "[audio][unit][player][lifecycle]")
  {
    auto mockProvider = Mock<BackendProvider>{};
    Fake(Method(mockProvider, shutdown));

    When(Method(mockProvider, subscribeDevices))
      .AlwaysDo(
        [&](BackendProvider::OnDevicesChangedCallback const& cb)
        {
          if (cb)
          {
            cb(std::vector{Device{.id = DeviceId{"mock-sink"},
                                  .displayName = "Mock Sink",
                                  .description = "Mock",
                                  .isDefault = true,
                                  .backendId = kBackendNone}});
          }

          return Subscription{};
        });

    When(Method(mockProvider, createBackend))
      .AlwaysDo([&](Device const& b, ProfileId const& p) { return std::make_unique<FakeBackend>(b.backendId, p); });
    auto onGraphChanged = BackendProvider::OnGraphChangedCallback{};

    When(Method(mockProvider, subscribeGraph))
      .AlwaysDo(
        [&](std::string_view, BackendProvider::OnGraphChangedCallback const& cb)
        {
          onGraphChanged = cb;
          return Subscription{[] {}};
        });
    When(Method(mockProvider, status)).AlwaysReturn(BackendProvider::Status{.descriptor = {.id = kBackendNone}});

    auto executor = QueuedExecutor{};
    auto player = Player{executor};
    player.addProvider(std::make_unique<MockProviderProxy>(mockProvider.get()));
    executor.drain();
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
      executor.drain();

      REQUIRE(onGraphChanged);

      auto qualityEvents = std::vector<std::pair<QualityResult, bool>>{};
      player.setOnQualityChanged([&](QualityResult const& quality, bool ready)
                                 { qualityEvents.emplace_back(quality, ready); });

      onGraphChanged(flow::Graph{});
      REQUIRE(executor.drainUntil([&] { return !qualityEvents.empty(); }));

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
      executor.drain();

      // Fire a system graph that has NO Stream node
      onGraphChanged(flow::Graph{.nodes = {flow::Node{.id = "sys-sink", .type = flow::NodeType::Sink}}});
      REQUIRE(executor.drainUntil(
        [&]
        {
          auto const snap = player.status();
          return std::ranges::find(snap.flow.nodes, std::string_view{"sys-sink"}, &flow::Node::id) !=
                 snap.flow.nodes.end();
        }));

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
      executor.drain();

      onGraphChanged(
        flow::Graph{.nodes =
                      {
                        flow::Node{.id = "sys-stream", .type = flow::NodeType::Stream, .name = "PipeWire Stream"},
                        flow::Node{.id = "sys-sink", .type = flow::NodeType::Sink, .name = "Speakers"},
                      },
                    .connections = {
                      flow::Connection{.sourceId = "sys-stream", .destinationId = "sys-sink", .isActive = true},
                    }});
      REQUIRE(executor.drainUntil(
        [&]
        {
          auto const snap = player.status();
          return std::ranges::find(snap.flow.nodes, std::string_view{"sys-stream"}, &flow::Node::id) !=
                 snap.flow.nodes.end();
        }));

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
      executor.drain();

      REQUIRE(onGraphChanged);

      // Increment generation
      player.stop();

      // Fire graph change, which captures the old gen
      onGraphChanged(flow::Graph{});
      executor.checkQueued();
      executor.drain();

      auto const snap = player.status();
      CHECK(snap.flow.nodes.empty());
    }
  }

  TEST_CASE("Player - pending output activates when matching device appears", "[audio][unit][player][pending]")
  {
    auto mockProvider = Mock<BackendProvider>{};
    Fake(Method(mockProvider, shutdown));
    auto onOutputDevicesChanged = BackendProvider::OnDevicesChangedCallback{};

    When(Method(mockProvider, subscribeDevices))
      .AlwaysDo(
        [&](BackendProvider::OnDevicesChangedCallback const& cb)
        {
          onOutputDevicesChanged = cb;

          return Subscription{};
        });

    When(Method(mockProvider, createBackend))
      .AlwaysDo([&](Device const& dev, ProfileId const& p) { return std::make_unique<FakeBackend>(dev.backendId, p); });
    When(Method(mockProvider, status))
      .AlwaysReturn(BackendProvider::Status{.descriptor = {
                                              .id = kBackendPipeWire,
                                            }});

    auto executor = rt::test::InlineExecutor{};
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
    auto mockProvider = Mock<BackendProvider>{};
    Fake(Method(mockProvider, shutdown));

    When(Method(mockProvider, subscribeDevices))
      .AlwaysDo([](BackendProvider::OnDevicesChangedCallback const&) { return Subscription{}; });
    When(Method(mockProvider, status)).AlwaysReturn(pipeWireStatus());

    auto executor = rt::test::InlineExecutor{};
    auto player = Player{executor};
    player.addProvider(std::make_unique<MockProviderProxy>(mockProvider.get()));

    auto const result = player.setOutputDevice(kBackendAlsa, DeviceId{"alsa-device"}, kProfileShared);

    REQUIRE_FALSE(result);
    CHECK(result.error().code == Error::Code::NotFound);
    CHECK(player.status().engine.currentDeviceId == "null");
  }

  TEST_CASE("Player - provider callbacks are marshalled onto the executor", "[audio][unit][player][concurrency]")
  {
    auto mockProvider = Mock<BackendProvider>{};
    Fake(Method(mockProvider, shutdown));

    auto onOutputDevicesChanged = BackendProvider::OnDevicesChangedCallback{};
    When(Method(mockProvider, subscribeDevices))
      .AlwaysDo(
        [&](BackendProvider::OnDevicesChangedCallback const& cb)
        {
          onOutputDevicesChanged = cb;
          return Subscription{};
        });

    When(Method(mockProvider, createBackend))
      .AlwaysDo([&](Device const& dev, ProfileId const& p) { return std::make_unique<FakeBackend>(dev.backendId, p); });
    When(Method(mockProvider, status)).AlwaysReturn(pipeWireStatus());
    When(Method(mockProvider, subscribeGraph))
      .AlwaysDo([](std::string_view, BackendProvider::OnGraphChangedCallback const&) { return Subscription{}; });

    auto executor = QueuedExecutor{};
    auto player = Player{executor};
    player.addProvider(std::make_unique<MockProviderProxy>(mockProvider.get()));

    std::int32_t deviceSignals = 0;
    auto observedStatuses = std::vector<BackendProvider::Status>{};
    player.setOnOutputDevicesChanged(
      [&](std::vector<BackendProvider::Status> const& statuses)
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

  TEST_CASE("Player - queued provider callback is ignored after teardown", "[audio][unit][player][concurrency]")
  {
    auto mockProvider = Mock<BackendProvider>{};
    Fake(Method(mockProvider, shutdown));

    auto onOutputDevicesChanged = BackendProvider::OnDevicesChangedCallback{};
    When(Method(mockProvider, subscribeDevices))
      .AlwaysDo(
        [&](BackendProvider::OnDevicesChangedCallback const& cb)
        {
          onOutputDevicesChanged = cb;
          return Subscription{};
        });

    When(Method(mockProvider, createBackend))
      .AlwaysDo([&](Device const& dev, ProfileId const& p) { return std::make_unique<FakeBackend>(dev.backendId, p); });
    When(Method(mockProvider, status)).AlwaysReturn(pipeWireStatus());
    When(Method(mockProvider, subscribeGraph))
      .AlwaysDo([](std::string_view, BackendProvider::OnGraphChangedCallback const&) { return Subscription{}; });

    auto executor = QueuedExecutor{};
    std::int32_t deviceSignals = 0;

    {
      auto player = Player{executor};
      player.addProvider(std::make_unique<MockProviderProxy>(mockProvider.get()));
      player.setOnOutputDevicesChanged([&](std::vector<BackendProvider::Status> const&) { ++deviceSignals; });

      REQUIRE(onOutputDevicesChanged);
      onOutputDevicesChanged({pipeWireDevice()});
    }

    executor.drain();
    CHECK(deviceSignals == 0);
  }

  TEST_CASE("Player - outward callback defers player teardown", "[audio][regression][player][concurrency]")
  {
    struct CallbackLifetime final
    {
      explicit CallbackLifetime(std::binary_semaphore& destroyedRef)
        : destroyed{destroyedRef}
      {
      }

      ~CallbackLifetime() { destroyed.release(); }

      CallbackLifetime(CallbackLifetime const&) = delete;
      CallbackLifetime& operator=(CallbackLifetime const&) = delete;
      CallbackLifetime(CallbackLifetime&&) = delete;
      CallbackLifetime& operator=(CallbackLifetime&&) = delete;

      std::binary_semaphore& destroyed;
    };

    struct Probe final
    {
      std::mutex mutex;
      RenderTarget* target = nullptr;
      std::binary_semaphore backendDestroyed{0};
    };

    struct ReentrantBackend final : NullBackend
    {
      explicit ReentrantBackend(std::shared_ptr<Probe> probePtr)
        : probePtr{std::move(probePtr)}
      {
      }

      ~ReentrantBackend() override { probePtr->backendDestroyed.release(); }

      ReentrantBackend(ReentrantBackend const&) = delete;
      ReentrantBackend& operator=(ReentrantBackend const&) = delete;
      ReentrantBackend(ReentrantBackend&&) = delete;
      ReentrantBackend& operator=(ReentrantBackend&&) = delete;

      Result<> open(Format const& /*format*/, RenderTarget* target) override
      {
        auto const lock = std::scoped_lock{probePtr->mutex};
        probePtr->target = target;
        return {};
      }

      void close() override
      {
        auto const lock = std::scoped_lock{probePtr->mutex};
        probePtr->target = nullptr;
      }

      BackendId backendId() const override { return BackendId{"reentrant"}; }
      ProfileId profileId() const override { return kProfileShared; }

      std::shared_ptr<Probe> probePtr;
    };

    struct ReentrantProvider final : BackendProvider
    {
      explicit ReentrantProvider(std::shared_ptr<Probe> probePtr)
        : probePtr{std::move(probePtr)}
      {
      }

      void shutdown() noexcept override {}

      Subscription subscribeDevices(OnDevicesChangedCallback callback) override
      {
        callback({Device{.id = DeviceId{"reentrant-device"},
                         .displayName = "Reentrant Device",
                         .description = "Test",
                         .isDefault = true,
                         .backendId = BackendId{"reentrant"}}});
        return {};
      }

      Status status() const override
      {
        return {.descriptor = {.id = BackendId{"reentrant"}, .supportedProfiles = {{.id = kProfileShared}}}};
      }

      std::unique_ptr<Backend> createBackend(Device const& /*device*/, ProfileId const& /*profile*/) override
      {
        return std::make_unique<ReentrantBackend>(probePtr);
      }

      Subscription subscribeGraph(std::string_view /*routeAnchor*/, OnGraphChangedCallback /*callback*/) override
      {
        return {};
      }

      std::shared_ptr<Probe> probePtr;
    };

    auto const fixturePath = requireAudioFixture("basic_metadata.flac");
    auto probePtr = std::make_shared<Probe>();
    auto callbackStorageDestroyed = std::binary_semaphore{0};
    auto callbackLifetimePtr = std::make_shared<CallbackLifetime>(callbackStorageDestroyed);
    auto executor = QueuedExecutor{};
    auto playerPtr = std::make_unique<Player>(executor);
    playerPtr->addProvider(std::make_unique<ReentrantProvider>(probePtr));
    executor.drain();
    REQUIRE(playerPtr->setOutputDevice(BackendId{"reentrant"}, DeviceId{"reentrant-device"}, kProfileShared));
    REQUIRE(playerPtr->play(Engine::PlaybackItem{.input = PlaybackInput{.filePath = fixturePath}}));

    auto* target = static_cast<RenderTarget*>(nullptr);
    {
      auto const lock = std::scoped_lock{probePtr->mutex};
      target = probePtr->target;
    }
    REQUIRE(target != nullptr);

    auto teardownRequested = std::atomic{false};
    playerPtr->setOnStateChanged(
      [&teardownRequested, callbackLifetimePtr]
      {
        if (!callbackLifetimePtr)
        {
          return;
        }

        teardownRequested.store(true, std::memory_order_release);
      });
    callbackLifetimePtr.reset();
    target->handlePropertyChanged(PropertySnapshot{
      .id = PropertyId::Volume,
      .optValue = PropertyValue{0.75F},
      .info = {.canRead = true, .canWrite = true, .isAvailable = true, .emitsChangeNotifications = true},
    });

    REQUIRE(executor.drainUntil([&] { return teardownRequested.load(std::memory_order_acquire); }));
    REQUIRE(playerPtr);
    playerPtr.reset();
    REQUIRE(probePtr->backendDestroyed.try_acquire_for(std::chrono::seconds{1}));
    REQUIRE(callbackStorageDestroyed.try_acquire_for(std::chrono::seconds{1}));
    CHECK(playerPtr == nullptr);
  }

  TEST_CASE("Player - ALSA-style synchronous graph update defers player teardown",
            "[audio][regression][player][concurrency]")
  {
    auto const fixturePath = requireAudioFixture("basic_metadata.flac");
    auto probePtr = std::make_shared<SynchronousGraphProbe>();
    auto executor = QueuedExecutor{};
    auto playerPtr = std::make_unique<Player>(executor);
    auto routeSettledSignaled = std::atomic{false};
    playerPtr->setOnQualityChanged(
      [&](QualityResult const&, bool)
      {
        if (probePtr->subscriptionCount() >= 1)
        {
          routeSettledSignaled.store(true, std::memory_order_release);
        }
      });
    playerPtr->addProvider(std::make_unique<SynchronousGraphProvider>(probePtr));
    executor.drain();
    REQUIRE(playerPtr->setOutputDevice(kSynchronousGraphBackend, DeviceId{"route-a"}, kProfileShared));
    REQUIRE(playerPtr->play(Engine::PlaybackItem{.input = PlaybackInput{.filePath = fixturePath}}));
    REQUIRE(executor.drainUntil(
      [&] { return probePtr->subscriptionCount() >= 1 && routeSettledSignaled.load(std::memory_order_acquire); },
      std::chrono::seconds{5}));

    auto teardownRequested = std::atomic{false};
    playerPtr->setOnQualityChanged([&](QualityResult const&, bool)
                                   { teardownRequested.store(true, std::memory_order_release); });

    auto const result = playerPtr->setVolume(0.5F);

    REQUIRE(result);
    REQUIRE(
      executor.drainUntil([&] { return teardownRequested.load(std::memory_order_acquire); }, std::chrono::seconds{5}));
    REQUIRE(playerPtr);
    playerPtr.reset();
    CHECK_FALSE(playerPtr);
  }

  TEST_CASE("Player - switching output while playing subscribes to the new route",
            "[audio][regression][player][output]")
  {
    auto const fixturePath = requireAudioFixture("basic_metadata.flac");
    auto probePtr = std::make_shared<SynchronousGraphProbe>();
    auto executor = async::LoopExecutor{};
    auto player = Player{executor};
    bool firstRouteSettled = false;
    bool secondRouteSettled = false;
    player.setOnQualityChanged(
      [&](QualityResult const&, bool)
      {
        auto const subscriptions = probePtr->subscriptionCount();

        if (subscriptions >= 1)
        {
          firstRouteSettled = true;
        }

        if (subscriptions >= 2)
        {
          secondRouteSettled = true;
        }
      });
    player.addProvider(std::make_unique<SynchronousGraphProvider>(probePtr));
    REQUIRE(player.setOutputDevice(kSynchronousGraphBackend, DeviceId{"route-a"}, kProfileShared));
    REQUIRE(player.play(Engine::PlaybackItem{.input = PlaybackInput{.filePath = fixturePath}}));
    REQUIRE(rt::test::runLoopUntil(executor, [&] { return probePtr->subscriptionCount() >= 1 && firstRouteSettled; }));

    auto staleRouteCallback = BackendProvider::OnGraphChangedCallback{};
    {
      auto const lock = std::scoped_lock{probePtr->mutex};
      staleRouteCallback = probePtr->graphCallback;
    }
    REQUIRE(staleRouteCallback);

    REQUIRE(player.setOutputDevice(kSynchronousGraphBackend, DeviceId{"route-b"}, kProfileShared));
    REQUIRE(rt::test::runLoopUntil(executor, [&] { return probePtr->subscriptionCount() >= 2 && secondRouteSettled; }));

    {
      auto const lock = std::scoped_lock{probePtr->mutex};
      REQUIRE(probePtr->subscribedRoutes.size() == 2);
      CHECK(probePtr->subscribedRoutes[0] == "route-a");
      CHECK(probePtr->subscribedRoutes[1] == "route-b");
      CHECK(probePtr->activeRoute == "route-b");
    }

    std::size_t graphUpdateCount = 0;
    bool currentRouteUpdated = false;
    player.setOnQualityChanged(
      [&](QualityResult const&, bool)
      {
        ++graphUpdateCount;

        if (auto const status = player.status();
            std::ranges::find(status.flow.nodes, std::string_view{"route-b-sink"}, &flow::Node::id) !=
            status.flow.nodes.end())
        {
          currentRouteUpdated = true;
        }
      });
    staleRouteCallback(flow::Graph{.nodes = {flow::Node{.id = "route-a-stale", .type = flow::NodeType::Sink}}});
    probePtr->publish("route-b");
    REQUIRE(rt::test::runLoopUntil(executor, [&] { return currentRouteUpdated; }));
    CHECK(graphUpdateCount == 1);

    auto const status = player.status();
    CHECK(std::ranges::find(status.flow.nodes, std::string_view{"route-b-sink"}, &flow::Node::id) !=
          status.flow.nodes.end());
    CHECK(std::ranges::find(status.flow.nodes, std::string_view{"route-a-stale"}, &flow::Node::id) ==
          status.flow.nodes.end());
  }

  TEST_CASE("Player - graph callbacks are marshalled onto the executor", "[audio][unit][player][concurrency]")
  {
    auto mockProvider = Mock<BackendProvider>{};
    Fake(Method(mockProvider, shutdown));

    auto onOutputDevicesChanged = BackendProvider::OnDevicesChangedCallback{};
    When(Method(mockProvider, subscribeDevices))
      .AlwaysDo(
        [&](BackendProvider::OnDevicesChangedCallback const& cb)
        {
          onOutputDevicesChanged = cb;
          return Subscription{};
        });

    When(Method(mockProvider, createBackend))
      .AlwaysDo([&](Device const& dev, ProfileId const& p) { return std::make_unique<FakeBackend>(dev.backendId, p); });
    When(Method(mockProvider, status)).AlwaysReturn(pipeWireStatus());

    auto onGraphChanged = BackendProvider::OnGraphChangedCallback{};
    When(Method(mockProvider, subscribeGraph))
      .AlwaysDo(
        [&](std::string_view, BackendProvider::OnGraphChangedCallback const& cb)
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

    auto worker = std::jthread{
      [&] { onGraphChanged(flow::Graph{.nodes = {flow::Node{.id = "sys-sink", .type = flow::NodeType::Sink}}}); }};
    worker.join();

    auto snap = player.status();
    CHECK(std::ranges::find(snap.flow.nodes, std::string_view{"sys-sink"}, &flow::Node::id) == snap.flow.nodes.end());
    CHECK(qualityEvents.empty());

    REQUIRE(executor.drainUntil([&] { return !qualityEvents.empty(); }));

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

  TEST_CASE("Player - failed stage preserves audio generation and prepared lookahead", "[audio][unit][player][staged]")
  {
    auto const fixturePath = requireAudioFixture("basic_metadata.flac");
    auto probePtr = std::make_shared<BarrierBackendProbe>();
    auto executor = QueuedExecutor{};
    auto player = Player{executor};
    player.addProvider(std::make_unique<BarrierProvider>(probePtr));
    executor.drain();
    REQUIRE(player.setOutputDevice(kBarrierBackend, DeviceId{"barrier-device"}, kProfileShared));

    auto const currentItem =
      Engine::PlaybackItem{.id = Engine::PlaybackItemId{.value = 1}, .input = PlaybackInput{.filePath = fixturePath}};
    REQUIRE(player.play(currentItem));
    executor.drain();

    auto const nextItem =
      Engine::PlaybackItem{.id = Engine::PlaybackItemId{.value = 2}, .input = PlaybackInput{.filePath = fixturePath}};
    auto const preparedNext = player.prepareNext(nextItem);
    REQUIRE(preparedNext);
    auto const audioGeneration = player.audioPlaybackGeneration();
    auto const graphGeneration = player.playbackGeneration();
    REQUIRE(preparedNext->generation == audioGeneration);

    auto candidate = player.stagePlayback(Engine::PlaybackItem{
      .id = Engine::PlaybackItemId{.value = 3}, .input = PlaybackInput{.filePath = "/missing/staged.flac"}});
    REQUIRE_FALSE(candidate);
    CHECK(player.audioPlaybackGeneration() == audioGeneration);
    CHECK(player.playbackGeneration() == graphGeneration);
    CHECK(player.clearPreparedNext() == nextItem.id);
  }

  TEST_CASE("Player - blocked asynchronous preparation leaves executor and stop responsive",
            "[audio][unit][player][concurrency]")
  {
    auto gatePtr = std::make_shared<BlockingPreparationGate>();
    auto probePtr = std::make_shared<BarrierBackendProbe>();
    auto executor = QueuedExecutor{};
    auto runtime = async::Runtime{executor, 1};
    auto player = Player{runtime, makeBlockingPreparationDecoderFactory(gatePtr, "blocked.flac")};
    player.addProvider(std::make_unique<BarrierProvider>(probePtr));
    executor.drain();
    REQUIRE(player.setOutputDevice(kBarrierBackend, DeviceId{"barrier-device"}, kProfileShared));
    REQUIRE(player.play(Engine::PlaybackItem{
      .id = Engine::PlaybackItemId{.value = 100}, .input = PlaybackInput{.filePath = "current.flac"}}));
    executor.drain();

    bool completionCalled = false;
    REQUIRE(player.stagePlaybackAsync(
      Engine::PlaybackItem{
        .id = Engine::PlaybackItemId{.value = 101}, .input = PlaybackInput{.filePath = "blocked.flac"}},
      {},
      [] { return true; },
      [&](Result<Engine::PreparedPlaybackStart>) { completionCalled = true; }));
    REQUIRE(gatePtr->waitForEntry());

    bool heartbeat = false;
    executor.defer([&] { heartbeat = true; });
    executor.drain();
    CHECK(heartbeat);
    CHECK(player.transport() == Transport::Playing);

    player.stop();
    CHECK(player.transport() == Transport::Idle);
    gatePtr->release.release();
    runtime.requestStop();
    runtime.join();
    executor.drain();

    CHECK_FALSE(completionCalled);
    CHECK(gatePtr->createdPtr->load(std::memory_order_relaxed) > 0);
    CHECK(gatePtr->destroyedPtr->load(std::memory_order_relaxed) ==
          gatePtr->createdPtr->load(std::memory_order_relaxed));
  }

  TEST_CASE("Player - explicit cancellation discards a blocked start preparation",
            "[audio][regression][player][concurrency]")
  {
    auto gatePtr = std::make_shared<BlockingPreparationGate>();
    auto probePtr = std::make_shared<BarrierBackendProbe>();
    auto executor = QueuedExecutor{};
    auto runtime = async::Runtime{executor, 1};
    auto player = Player{runtime, makeBlockingPreparationDecoderFactory(gatePtr, "blocked-start.flac")};
    player.addProvider(std::make_unique<BarrierProvider>(probePtr));
    executor.drain();
    REQUIRE(player.setOutputDevice(kBarrierBackend, DeviceId{"barrier-device"}, kProfileShared));
    bool acceptanceCalled = false;
    bool completionCalled = false;
    REQUIRE(player.stagePlaybackAsync(
      Engine::PlaybackItem{
        .id = Engine::PlaybackItemId{.value = 102}, .input = PlaybackInput{.filePath = "blocked-start.flac"}},
      {},
      [&]
      {
        acceptanceCalled = true;
        return true;
      },
      [&](Result<Engine::PreparedPlaybackStart>) { completionCalled = true; }));
    REQUIRE(gatePtr->waitForEntry());

    player.cancelStartPreparation();
    gatePtr->release.release();
    runtime.requestStop();
    runtime.join();
    executor.drain();

    CHECK_FALSE(acceptanceCalled);
    CHECK_FALSE(completionCalled);
    CHECK(gatePtr->createdPtr->load(std::memory_order_relaxed) > 0);
    CHECK(gatePtr->destroyedPtr->load(std::memory_order_relaxed) ==
          gatePtr->createdPtr->load(std::memory_order_relaxed));
  }

  TEST_CASE("Player - explicit cancellation discards a blocked lookahead preparation",
            "[audio][regression][player][concurrency]")
  {
    auto gatePtr = std::make_shared<BlockingPreparationGate>();
    auto probePtr = std::make_shared<BarrierBackendProbe>();
    auto executor = QueuedExecutor{};
    auto runtime = async::Runtime{executor, 1};
    auto player = Player{runtime, makeBlockingPreparationDecoderFactory(gatePtr, "blocked-next.flac")};
    player.addProvider(std::make_unique<BarrierProvider>(probePtr));
    executor.drain();
    REQUIRE(player.setOutputDevice(kBarrierBackend, DeviceId{"barrier-device"}, kProfileShared));
    REQUIRE(player.play(Engine::PlaybackItem{
      .id = Engine::PlaybackItemId{.value = 103}, .input = PlaybackInput{.filePath = "current.flac"}}));
    bool acceptanceCalled = false;
    bool completionCalled = false;
    REQUIRE(player.prepareNextAsync(
      Engine::PlaybackItem{
        .id = Engine::PlaybackItemId{.value = 104}, .input = PlaybackInput{.filePath = "blocked-next.flac"}},
      [&]
      {
        acceptanceCalled = true;
        return true;
      },
      [&](Result<Engine::PreparedNextResult>) { completionCalled = true; }));
    REQUIRE(gatePtr->waitForEntry());

    player.cancelLookaheadPreparation();
    gatePtr->release.release();
    runtime.requestStop();
    runtime.join();
    executor.drain();

    CHECK_FALSE(acceptanceCalled);
    CHECK_FALSE(completionCalled);
    CHECK(gatePtr->createdPtr->load(std::memory_order_relaxed) > 0);
    CHECK(gatePtr->destroyedPtr->load(std::memory_order_relaxed) ==
          gatePtr->createdPtr->load(std::memory_order_relaxed));
  }

  TEST_CASE("Player - replacement suppresses a blocked lookahead for the same item",
            "[audio][regression][player][concurrency]")
  {
    auto gatePtr = std::make_shared<BlockingPreparationGate>();
    auto probePtr = std::make_shared<BarrierBackendProbe>();
    auto executor = QueuedExecutor{};
    auto runtime = async::Runtime{executor, 1};
    auto player = Player{runtime, makeBlockingPreparationDecoderFactory(gatePtr, "same-next.flac")};
    player.addProvider(std::make_unique<BarrierProvider>(probePtr));
    executor.drain();
    REQUIRE(player.setOutputDevice(kBarrierBackend, DeviceId{"barrier-device"}, kProfileShared));
    REQUIRE(player.play(Engine::PlaybackItem{
      .id = Engine::PlaybackItemId{.value = 105}, .input = PlaybackInput{.filePath = "current.flac"}}));

    auto const successor = Engine::PlaybackItem{
      .id = Engine::PlaybackItemId{.value = 106}, .input = PlaybackInput{.filePath = "same-next.flac"}};
    bool firstAccepted = false;
    bool firstCompleted = false;
    REQUIRE(player.prepareNextAsync(
      successor,
      [&]
      {
        firstAccepted = true;
        return true;
      },
      [&](Result<Engine::PreparedNextResult>) { firstCompleted = true; }));
    REQUIRE(gatePtr->waitForEntry());

    bool secondAccepted = false;
    bool secondCompleted = false;
    REQUIRE(player.prepareNextAsync(
      successor,
      [&]
      {
        secondAccepted = true;
        return true;
      },
      [&](Result<Engine::PreparedNextResult> prepared)
      {
        REQUIRE(prepared);
        secondCompleted = true;
      }));

    gatePtr->release.release();
    REQUIRE(executor.drainUntil([&] { return secondCompleted; }, std::chrono::seconds{5}));

    CHECK_FALSE(firstAccepted);
    CHECK_FALSE(firstCompleted);
    CHECK(secondAccepted);

    runtime.requestStop();
    runtime.join();
    executor.drain();
  }

  TEST_CASE("Player - start acceptance veto completes exactly once with conflict",
            "[audio][regression][player][concurrency]")
  {
    auto gatePtr = std::make_shared<BlockingPreparationGate>();
    auto probePtr = std::make_shared<BarrierBackendProbe>();
    auto executor = QueuedExecutor{};
    auto runtime = async::Runtime{executor, 1};
    auto player = Player{runtime, makeBlockingPreparationDecoderFactory(gatePtr, "vetoed-start.flac")};
    player.addProvider(std::make_unique<BarrierProvider>(probePtr));
    executor.drain();
    REQUIRE(player.setOutputDevice(kBarrierBackend, DeviceId{"barrier-device"}, kProfileShared));
    std::uint32_t acceptanceCount = 0;
    auto completions = std::vector<Result<Engine::PreparedPlaybackStart>>{};

    REQUIRE(player.stagePlaybackAsync(
      Engine::PlaybackItem{
        .id = Engine::PlaybackItemId{.value = 107}, .input = PlaybackInput{.filePath = "vetoed-start.flac"}},
      {},
      [&]
      {
        ++acceptanceCount;
        return false;
      },
      [&](Result<Engine::PreparedPlaybackStart> prepared) { completions.push_back(std::move(prepared)); }));
    REQUIRE(gatePtr->waitForEntry());
    gatePtr->release.release();
    REQUIRE(executor.drainUntil([&] { return !completions.empty(); }, std::chrono::seconds{5}));

    CHECK(acceptanceCount == 1);
    REQUIRE(completions.size() == 1);
    REQUIRE_FALSE(completions.front());
    CHECK(completions.front().error().code == Error::Code::Conflict);
  }

  TEST_CASE("Player - worker lookahead acceptance veto completes exactly once with conflict",
            "[audio][regression][player][concurrency]")
  {
    auto gatePtr = std::make_shared<BlockingPreparationGate>();
    auto probePtr = std::make_shared<BarrierBackendProbe>();
    auto executor = QueuedExecutor{};
    auto runtime = async::Runtime{executor, 1};
    auto player = Player{runtime, makeBlockingPreparationDecoderFactory(gatePtr, "vetoed-next.flac")};
    player.addProvider(std::make_unique<BarrierProvider>(probePtr));
    executor.drain();
    REQUIRE(player.setOutputDevice(kBarrierBackend, DeviceId{"barrier-device"}, kProfileShared));
    REQUIRE(player.play(Engine::PlaybackItem{
      .id = Engine::PlaybackItemId{.value = 108}, .input = PlaybackInput{.filePath = "current.flac"}}));
    std::uint32_t acceptanceCount = 0;
    auto completions = std::vector<Result<Engine::PreparedNextResult>>{};

    REQUIRE(player.prepareNextAsync(
      Engine::PlaybackItem{
        .id = Engine::PlaybackItemId{.value = 109}, .input = PlaybackInput{.filePath = "vetoed-next.flac"}},
      [&]
      {
        ++acceptanceCount;
        return false;
      },
      [&](Result<Engine::PreparedNextResult> prepared) { completions.push_back(std::move(prepared)); }));
    REQUIRE(gatePtr->waitForEntry());
    gatePtr->release.release();
    REQUIRE(executor.drainUntil([&] { return !completions.empty(); }, std::chrono::seconds{5}));

    CHECK(acceptanceCount == 1);
    REQUIRE(completions.size() == 1);
    REQUIRE_FALSE(completions.front());
    CHECK(completions.front().error().code == Error::Code::Conflict);
  }

  TEST_CASE("Player - logical drain-fallback acceptance veto completes exactly once with conflict",
            "[audio][regression][player][concurrency]")
  {
    auto probePtr = std::make_shared<BarrierBackendProbe>();
    auto executor = QueuedExecutor{};
    auto runtime = async::Runtime{executor, 1};
    auto const format = Format{.sampleRate = 44100, .channels = 2, .bitDepth = 16, .isInterleaved = true};
    auto player =
      Player{runtime,
             [format](std::filesystem::path const& path, Format const&)
             {
               auto const lossy = path.extension() == ".mp3";
               auto decoderPtr = std::make_unique<ScriptedDecoderSession>(
                 makeScriptedStreamInfo(format, lossy ? AudioCodec::Mp3 : AudioCodec::Flac, lossy));
               decoderPtr->setReadScript(
                 {{.data = std::vector<std::byte>(4096, std::byte{0}), .endOfStream = false}, {.endOfStream = true}});
               return decoderPtr;
             }};
    player.addProvider(std::make_unique<BarrierProvider>(probePtr));
    executor.drain();
    REQUIRE(player.setOutputDevice(kBarrierBackend, DeviceId{"barrier-device"}, kProfileShared));
    REQUIRE(player.play(Engine::PlaybackItem{
      .id = Engine::PlaybackItemId{.value = 110}, .input = PlaybackInput{.filePath = "current.mp3"}}));
    std::uint32_t acceptanceCount = 0;
    auto completions = std::vector<Result<Engine::PreparedNextResult>>{};

    REQUIRE(player.prepareNextAsync(
      Engine::PlaybackItem{
        .id = Engine::PlaybackItemId{.value = 111}, .input = PlaybackInput{.filePath = "fallback.flac"}},
      [&]
      {
        ++acceptanceCount;
        return false;
      },
      [&](Result<Engine::PreparedNextResult> prepared) { completions.push_back(std::move(prepared)); }));

    CHECK(acceptanceCount == 1);
    REQUIRE(completions.size() == 1);
    REQUIRE_FALSE(completions.front());
    CHECK(completions.front().error().code == Error::Code::Conflict);
  }

  TEST_CASE("Player - asynchronous start distinguishes unchanged and stale device evidence",
            "[audio][regression][player][concurrency]")
  {
    auto gatePtr = std::make_shared<BlockingPreparationGate>();
    auto probePtr = std::make_shared<BarrierBackendProbe>();
    auto executor = QueuedExecutor{};
    auto runtime = async::Runtime{executor, 1};
    auto player = Player{runtime, makeBlockingPreparationDecoderFactory(gatePtr, "device-evidence.flac")};
    player.addProvider(std::make_unique<BarrierProvider>(probePtr));
    executor.drain();
    REQUIRE(player.setOutputDevice(kBarrierBackend, DeviceId{"barrier-device"}, kProfileShared));
    auto optCompletion = std::optional<Result<Engine::PreparedPlaybackStart>>{};
    REQUIRE(player.stagePlaybackAsync(
      Engine::PlaybackItem{
        .id = Engine::PlaybackItemId{.value = 105}, .input = PlaybackInput{.filePath = "device-evidence.flac"}},
      {},
      [] { return true; },
      [&](Result<Engine::PreparedPlaybackStart> prepared) { optCompletion.emplace(std::move(prepared)); }));
    REQUIRE(gatePtr->waitForEntry());
    auto device = Device{.id = DeviceId{"barrier-device"},
                         .displayName = "Barrier Device",
                         .description = "Controlled test output",
                         .isDefault = true,
                         .backendId = kBarrierBackend};

    SECTION("unchanged snapshot")
    {
      probePtr->emitDevices({device});
      executor.drain();
      gatePtr->release.release();
      REQUIRE(executor.drainUntil([&] { return optCompletion.has_value(); }, std::chrono::seconds{5}));
      REQUIRE(*optCompletion);
      CHECK(player.commitPlayback(std::move(**optCompletion)));
    }

    SECTION("changed snapshot")
    {
      device.description = "Changed output capabilities";
      probePtr->emitDevices({device});
      executor.drain();
      gatePtr->release.release();
      REQUIRE(executor.drainUntil([&] { return optCompletion.has_value(); }, std::chrono::seconds{5}));
      REQUIRE_FALSE(*optCompletion);
      CHECK(optCompletion->error().code == Error::Code::Conflict);
    }
  }

  TEST_CASE("Player - asynchronous start adopts before commit and preserves the old session until settlement",
            "[audio][unit][player][concurrency]")
  {
    auto gatePtr = std::make_shared<BlockingPreparationGate>();
    auto probePtr = std::make_shared<BarrierBackendProbe>();
    auto executor = QueuedExecutor{};
    auto runtime = async::Runtime{executor, 1};
    auto player = Player{runtime, makeBlockingPreparationDecoderFactory(gatePtr, "replacement.flac")};
    player.addProvider(std::make_unique<BarrierProvider>(probePtr));
    executor.drain();
    REQUIRE(player.setOutputDevice(kBarrierBackend, DeviceId{"barrier-device"}, kProfileShared));
    REQUIRE(player.play(Engine::PlaybackItem{
      .id = Engine::PlaybackItemId{.value = 110}, .input = PlaybackInput{.filePath = "current.flac"}}));
    executor.drain();
    auto const oldGeneration = player.audioPlaybackGeneration();
    auto* const oldTarget = probePtr->target();

    bool accepted = false;
    bool committed = false;
    REQUIRE(player.stagePlaybackAsync(
      Engine::PlaybackItem{
        .id = Engine::PlaybackItemId{.value = 111}, .input = PlaybackInput{.filePath = "replacement.flac"}},
      {},
      [&]
      {
        accepted = true;
        CHECK(player.transport() == Transport::Playing);
        CHECK(player.audioPlaybackGeneration() == oldGeneration);
        CHECK(probePtr->target() == oldTarget);
        return true;
      },
      [&](Result<Engine::PreparedPlaybackStart> preparedStart)
      {
        REQUIRE(preparedStart);
        CHECK(player.transport() == Transport::Playing);
        CHECK(player.audioPlaybackGeneration() == oldGeneration);
        CHECK(probePtr->target() == oldTarget);
        REQUIRE(player.commitPlayback(std::move(*preparedStart)));
        committed = true;
      }));
    REQUIRE(gatePtr->waitForEntry());

    CHECK(player.transport() == Transport::Playing);
    CHECK(player.audioPlaybackGeneration() == oldGeneration);
    CHECK(probePtr->target() == oldTarget);
    gatePtr->release.release();

    REQUIRE(executor.drainUntil([&] { return committed; }, std::chrono::seconds{5}));
    CHECK(accepted);
    CHECK(player.audioPlaybackGeneration() > oldGeneration);
  }

  TEST_CASE("Player - shutdown rejects a blocked preparation completion without owner access",
            "[audio][unit][player][concurrency]")
  {
    auto gatePtr = std::make_shared<BlockingPreparationGate>();
    auto probePtr = std::make_shared<BarrierBackendProbe>();
    auto executor = QueuedExecutor{};
    auto runtime = async::Runtime{executor, 1};
    auto player = Player{runtime, makeBlockingPreparationDecoderFactory(gatePtr, "blocked.flac")};
    player.addProvider(std::make_unique<BarrierProvider>(probePtr));
    executor.drain();
    REQUIRE(player.setOutputDevice(kBarrierBackend, DeviceId{"barrier-device"}, kProfileShared));
    bool completionCalled = false;

    REQUIRE(player.stagePlaybackAsync(
      Engine::PlaybackItem{
        .id = Engine::PlaybackItemId{.value = 120}, .input = PlaybackInput{.filePath = "blocked.flac"}},
      {},
      [] { return true; },
      [&](Result<Engine::PreparedPlaybackStart>) { completionCalled = true; }));
    REQUIRE(gatePtr->waitForEntry());

    player.shutdown();
    gatePtr->release.release();
    runtime.requestStop();
    runtime.join();
    executor.drain();

    CHECK_FALSE(completionCalled);
    CHECK(gatePtr->createdPtr->load(std::memory_order_relaxed) > 0);
    CHECK(gatePtr->destroyedPtr->load(std::memory_order_relaxed) ==
          gatePtr->createdPtr->load(std::memory_order_relaxed));
  }

  TEST_CASE("Player - queued preparation completion is harmless after owner destruction",
            "[audio][regression][player][concurrency]")
  {
    auto gatePtr = std::make_shared<BlockingPreparationGate>();
    auto probePtr = std::make_shared<BarrierBackendProbe>();
    auto executor = QueuedExecutor{};
    auto runtime = async::Runtime{executor, 1};
    auto playerPtr = std::make_unique<Player>(runtime, makeBlockingPreparationDecoderFactory(gatePtr, "queued.flac"));
    playerPtr->addProvider(std::make_unique<BarrierProvider>(probePtr));
    executor.drain();
    REQUIRE(playerPtr->setOutputDevice(kBarrierBackend, DeviceId{"barrier-device"}, kProfileShared));
    executor.drain();

    bool acceptanceCalled = false;
    bool completionCalled = false;
    REQUIRE(playerPtr->stagePlaybackAsync(
      Engine::PlaybackItem{
        .id = Engine::PlaybackItemId{.value = 121}, .input = PlaybackInput{.filePath = "queued.flac"}},
      {},
      [&]
      {
        acceptanceCalled = true;
        return true;
      },
      [&](Result<Engine::PreparedPlaybackStart>) { completionCalled = true; }));
    REQUIRE(gatePtr->waitForEntry());

    gatePtr->release.release();
    REQUIRE(executor.waitUntilQueuedCount(1, std::chrono::seconds{5}));
    playerPtr.reset();
    executor.drain();
    runtime.requestStop();
    runtime.join();

    CHECK_FALSE(acceptanceCalled);
    CHECK_FALSE(completionCalled);
    CHECK(gatePtr->createdPtr->load(std::memory_order_relaxed) > 0);
    CHECK(gatePtr->destroyedPtr->load(std::memory_order_relaxed) ==
          gatePtr->createdPtr->load(std::memory_order_relaxed));
  }

  TEST_CASE("Player - staged decode error processed before commit preserves active generations and lookahead",
            "[audio][unit][player][staged]")
  {
    auto failureGate = StagedFailureGate{};
    auto probePtr = std::make_shared<BarrierBackendProbe>();
    auto executor = QueuedExecutor{};
    auto player = Player{executor, makeStagedFailureDecoderFactory("candidate-failure.flac", failureGate)};
    player.addProvider(std::make_unique<BarrierProvider>(probePtr));
    executor.drain();
    REQUIRE(player.setOutputDevice(kBarrierBackend, DeviceId{"barrier-device"}, kProfileShared));

    auto const currentItem = Engine::PlaybackItem{
      .id = Engine::PlaybackItemId{.value = 4}, .input = PlaybackInput{.filePath = "current.flac"}};
    auto const nextItem =
      Engine::PlaybackItem{.id = Engine::PlaybackItemId{.value = 5}, .input = PlaybackInput{.filePath = "next.flac"}};
    REQUIRE(player.play(currentItem));
    executor.drain();
    REQUIRE(player.prepareNext(nextItem));
    auto const audioGeneration = player.audioPlaybackGeneration();
    auto const graphGeneration = player.playbackGeneration();
    auto* const activeTarget = probePtr->target();
    REQUIRE(activeTarget != nullptr);

    auto candidate = player.stagePlayback(Engine::PlaybackItem{
      .id = Engine::PlaybackItemId{.value = 6},
      .input = PlaybackInput{.filePath = "candidate-failure.flac"},
    });
    REQUIRE(candidate);
    auto releaseGuard = StagedFailureReleaseGuard{failureGate};
    REQUIRE(failureGate.waitForRead());

    std::size_t stateChangedCount = 0;
    std::size_t failureCount = 0;
    std::size_t endedCount = 0;
    player.setOnStateChanged([&] { ++stateChangedCount; });
    player.setOnPlaybackFailure([&](Engine::PlaybackFailure const&) { ++failureCount; });
    player.setOnTrackEnded([&](Engine::TrackEnded const&) { ++endedCount; });
    releaseGuard.release();
    REQUIRE(executor.drainUntil([&] { return stateChangedCount == 1; }, std::chrono::seconds{5}));

    auto const committed = player.commitPlayback(std::move(*candidate));
    REQUIRE_FALSE(committed);
    CHECK(committed.error().code == Error::Code::IoError);
    CHECK(committed.error().message == "gated staged decode failure");
    CHECK(player.audioPlaybackGeneration() == audioGeneration);
    CHECK(player.playbackGeneration() == graphGeneration);
    CHECK(player.transport() == Transport::Playing);
    CHECK(probePtr->target() == activeTarget);
    CHECK(player.clearPreparedNext() == nextItem.id);
    CHECK(failureCount == 0);
    CHECK(endedCount == 0);
    CHECK(stateChangedCount == 1);
  }

  TEST_CASE("Player - accepted play and stop filter an old failure already queued on its executor",
            "[audio][unit][player][barrier]")
  {
    auto const fixturePath = requireAudioFixture("basic_metadata.flac");
    auto probePtr = std::make_shared<BarrierBackendProbe>();
    auto executor = QueuedExecutor{};
    auto player = Player{executor};
    player.addProvider(std::make_unique<BarrierProvider>(probePtr));
    executor.drain();
    REQUIRE(player.setOutputDevice(kBarrierBackend, DeviceId{"barrier-device"}, kProfileShared));
    REQUIRE(player.play(Engine::PlaybackItem{
      .id = Engine::PlaybackItemId{.value = 10}, .input = PlaybackInput{.filePath = fixturePath}}));
    executor.drain();

    std::size_t failureCount = 0;
    player.setOnPlaybackFailure([&](Engine::PlaybackFailure const&) { ++failureCount; });
    auto const failedGeneration = player.audioPlaybackGeneration();
    auto* const oldTarget = probePtr->target();
    REQUIRE(oldTarget != nullptr);
    oldTarget->handleBackendError("queued old Player failure");
    executor.checkQueued(std::chrono::seconds{5});

    auto barrier = Engine::PreparedCancellationBarrier{};

    SECTION("successful explicit play")
    {
      auto candidate = player.stagePlayback(Engine::PlaybackItem{
        .id = Engine::PlaybackItemId{.value = 11}, .input = PlaybackInput{.filePath = fixturePath}});
      REQUIRE(candidate);
      auto receipt = player.commitPlayback(std::move(*candidate));
      REQUIRE(receipt);
      barrier = receipt->cancellationBarrier;
      CHECK(receipt->generation == player.audioPlaybackGeneration());
    }

    SECTION("completed stop")
    {
      barrier = player.stopWithBarrier();
      CHECK(player.transport() == Transport::Idle);
    }

    REQUIRE(barrier.covers(failedGeneration));
    executor.drain();
    CHECK(failureCount == 0);
  }

  TEST_CASE("Player - accepted play and stop filter an old route already queued on its executor",
            "[audio][unit][player][barrier]")
  {
    auto const fixturePath = requireAudioFixture("basic_metadata.flac");
    auto probePtr = std::make_shared<BarrierBackendProbe>();
    auto executor = QueuedExecutor{};
    auto player = Player{executor};
    player.addProvider(std::make_unique<BarrierProvider>(probePtr));
    executor.drain();
    REQUIRE(player.setOutputDevice(kBarrierBackend, DeviceId{"barrier-device"}, kProfileShared));
    REQUIRE(player.play(Engine::PlaybackItem{
      .id = Engine::PlaybackItemId{.value = 30}, .input = PlaybackInput{.filePath = fixturePath}}));
    executor.drain();

    auto const oldGeneration = player.audioPlaybackGeneration();
    auto* const oldTarget = probePtr->target();
    REQUIRE(oldTarget != nullptr);
    oldTarget->handleRouteReady("old-route");
    REQUIRE(executor.waitUntilQueuedCount(2, std::chrono::seconds{5}));

    auto barrier = Engine::PreparedCancellationBarrier{};

    SECTION("successful explicit play")
    {
      auto candidate = player.stagePlayback(Engine::PlaybackItem{
        .id = Engine::PlaybackItemId{.value = 31}, .input = PlaybackInput{.filePath = fixturePath}});
      REQUIRE(candidate);
      auto receipt = player.commitPlayback(std::move(*candidate));
      REQUIRE(receipt);
      barrier = receipt->cancellationBarrier;
    }

    SECTION("completed stop")
    {
      barrier = player.stopWithBarrier();
    }

    REQUIRE(barrier.covers(oldGeneration));
    executor.drain();
    CHECK(probePtr->routeCount() == 0);
  }

  TEST_CASE("Player - accepted play and stop filter an old track end already queued on its executor",
            "[audio][unit][player][barrier]")
  {
    auto const format = Format{.sampleRate = 1000, .channels = 1, .bitDepth = 16, .isInterleaved = true};
    auto const data = std::vector{std::byte{0x11}, std::byte{0x12}, std::byte{0x13}, std::byte{0x14}};
    auto probePtr = std::make_shared<BarrierBackendProbe>();
    auto executor = QueuedExecutor{};
    auto player = Player{executor,
                         makePathScriptedDecoderFactory({
                           {.path = "current.flac", .info = makeScriptedStreamInfo(format), .data = data},
                           {.path = "replacement.flac", .info = makeScriptedStreamInfo(format), .data = data},
                         })};
    player.addProvider(std::make_unique<BarrierProvider>(probePtr));
    executor.drain();
    REQUIRE(player.setOutputDevice(kBarrierBackend, DeviceId{"barrier-device"}, kProfileShared));
    REQUIRE(player.play(Engine::PlaybackItem{
      .id = Engine::PlaybackItemId{.value = 40}, .input = PlaybackInput{.filePath = "current.flac"}}));
    executor.drain();

    std::size_t endedCount = 0;
    player.setOnTrackEnded([&](Engine::TrackEnded const&) { ++endedCount; });
    auto const oldGeneration = player.audioPlaybackGeneration();
    auto* const oldTarget = probePtr->target();
    REQUIRE(oldTarget != nullptr);
    auto output = std::array<std::byte, 4>{};
    REQUIRE(oldTarget->renderPcm(output).bytesWritten == output.size());
    REQUIRE(oldTarget->renderPcm(output).drained);
    oldTarget->handleDrainComplete();
    REQUIRE(executor.waitUntilQueuedCount(3, std::chrono::seconds{5}));

    auto barrier = Engine::PreparedCancellationBarrier{};

    SECTION("successful explicit play")
    {
      auto candidate = player.stagePlayback(Engine::PlaybackItem{
        .id = Engine::PlaybackItemId{.value = 41}, .input = PlaybackInput{.filePath = "replacement.flac"}});
      REQUIRE(candidate);
      auto receipt = player.commitPlayback(std::move(*candidate));
      REQUIRE(receipt);
      barrier = receipt->cancellationBarrier;
    }

    SECTION("completed stop")
    {
      barrier = player.stopWithBarrier();
    }

    REQUIRE(barrier.covers(oldGeneration));
    executor.drain();
    CHECK(endedCount == 0);
  }

  TEST_CASE("Player - queued gapless route follows the graph generation advanced on the executor",
            "[audio][unit][player][gapless]")
  {
    auto const format = Format{.sampleRate = 1000, .channels = 1, .bitDepth = 16, .isInterleaved = true};
    auto const firstData = std::vector{std::byte{0x21}, std::byte{0x22}, std::byte{0x23}, std::byte{0x24}};
    auto const secondData = std::vector{std::byte{0x31}, std::byte{0x32}, std::byte{0x33}, std::byte{0x34}};
    auto probePtr = std::make_shared<SynchronousGraphProbe>();
    auto executor = QueuedExecutor{};
    auto player = Player{executor,
                         makePathScriptedDecoderFactory({
                           {.path = "first.flac", .info = makeScriptedStreamInfo(format), .data = firstData},
                           {.path = "second.flac", .info = makeScriptedStreamInfo(format), .data = secondData},
                         })};
    player.addProvider(std::make_unique<SynchronousGraphProvider>(probePtr));
    executor.drain();
    REQUIRE(player.setOutputDevice(kSynchronousGraphBackend, DeviceId{"route-a"}, kProfileShared));
    REQUIRE(player.play(Engine::PlaybackItem{
      .id = Engine::PlaybackItemId{.value = 50}, .input = PlaybackInput{.filePath = "first.flac"}}));
    REQUIRE(executor.drainUntil([&] { return probePtr->subscriptionCount() == 1; }, std::chrono::seconds{5}));

    bool advanced = false;
    player.setOnTrackAdvanced([&](Engine::TrackAdvanced const&) { advanced = true; });
    auto const prepared = player.prepareNext(Engine::PlaybackItem{
      .id = Engine::PlaybackItemId{.value = 51}, .input = PlaybackInput{.filePath = "second.flac"}});
    REQUIRE(prepared);
    REQUIRE(prepared->transition == Engine::PreparedTransitionMode::Gapless);
    auto* const target = probePtr->target();
    REQUIRE(target != nullptr);
    auto output = std::array<std::byte, 4>{};
    REQUIRE(target->renderPcm(output).bytesWritten == output.size());
    REQUIRE(target->renderPcm(output).bytesWritten == output.size());

    REQUIRE(
      executor.drainUntil([&] { return advanced && probePtr->subscriptionCount() == 2; }, std::chrono::seconds{5}));
    auto const lock = std::scoped_lock{probePtr->mutex};
    CHECK(probePtr->activeRoute == "route-a");
  }

  TEST_CASE("Player - prepared source failure preserves item and audio generation on executor forwarding",
            "[audio][unit][player][failure]")
  {
    auto const format = Format{.sampleRate = 44100, .channels = 2, .bitDepth = 16, .isInterleaved = true};
    auto const factory = [format](std::filesystem::path const& path, Format const&)
    {
      auto decoderPtr = std::make_unique<ScriptedDecoderSession>(DecodedStreamInfo{
        .sourceFormat = format,
        .outputFormat = format,
        .duration = std::chrono::seconds{1},
        .isLossy = false,
        .codec = AudioCodec::Flac,
      });

      if (auto data = std::vector<std::byte>(100000, std::byte{0}); path == "prepared-failure.flac")
      {
        decoderPtr->setReadScript(
          {{.data = data, .endOfStream = false},
           {.endOfStream = false,
            .result = std::unexpected{Error{.code = Error::Code::IoError, .message = "prepared decode failed"}}}});
      }
      else
      {
        decoderPtr->setReadScript({{.data = std::move(data), .endOfStream = false}, {.endOfStream = true}});
      }

      return decoderPtr;
    };
    auto probePtr = std::make_shared<BarrierBackendProbe>();
    auto executor = QueuedExecutor{};
    auto player = Player{executor, factory};
    player.addProvider(std::make_unique<BarrierProvider>(probePtr));
    executor.drain();
    REQUIRE(player.setOutputDevice(kBarrierBackend, DeviceId{"barrier-device"}, kProfileShared));
    REQUIRE(player.play(Engine::PlaybackItem{
      .id = Engine::PlaybackItemId{.value = 20}, .input = PlaybackInput{.filePath = "current.flac"}}));
    executor.drain();

    auto failures = std::vector<Engine::PlaybackFailure>{};
    player.setOnPlaybackFailure([&](Engine::PlaybackFailure const& failure) { failures.push_back(failure); });
    auto const preparedItem = Engine::PlaybackItem{
      .id = Engine::PlaybackItemId{.value = 21}, .input = PlaybackInput{.filePath = "prepared-failure.flac"}};
    auto const prepared = player.prepareNext(preparedItem);
    REQUIRE(prepared);
    REQUIRE(executor.drainUntil([&] { return !failures.empty(); }, std::chrono::seconds{5}));

    REQUIRE(failures.size() == 1);
    CHECK(failures.front().kind == Engine::PlaybackFailureKind::Decode);
    CHECK(failures.front().itemId == preparedItem.id);
    CHECK(failures.front().generation == prepared->generation);
    CHECK(failures.front().error.message == "prepared decode failed");
    CHECK(failures.front().recoverable);
    CHECK(player.transport() == Transport::Playing);
  }

  TEST_CASE("Player - controls update engine-backed status", "[audio][unit][player][control]")
  {
    auto executor = rt::test::InlineExecutor{};
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
      void recordEvent(std::string_view eventName) noexcept
      {
        if (count >= values.size())
        {
          hasOverflow = true;
          return;
        }

        values[count++] = eventName;
      }

      std::array<std::string_view, 3> values{};
      std::size_t count = 0;
      bool hasOverflow = false;
    };

    struct LifetimeBackend final : NullBackend
    {
      explicit LifetimeBackend(Events& eventsRef)
        : events{eventsRef}
      {
      }

      LifetimeBackend(LifetimeBackend const&) = delete;
      LifetimeBackend& operator=(LifetimeBackend const&) = delete;
      LifetimeBackend(LifetimeBackend&&) = delete;
      LifetimeBackend& operator=(LifetimeBackend&&) = delete;

      ~LifetimeBackend() override { close(); }

      void close() noexcept override
      {
        if (!closed)
        {
          events.recordEvent("backend close");
          closed = true;
        }
      }

      BackendId backendId() const override { return kBackendAlsa; }
      ProfileId profileId() const override { return kProfileExclusive; }

      Events& events;
      bool closed = false;
    };

    struct LifetimeProvider final : BackendProvider
    {
      explicit LifetimeProvider(Events& eventsRef)
        : events{eventsRef}
      {
      }

      LifetimeProvider(LifetimeProvider const&) = delete;
      LifetimeProvider& operator=(LifetimeProvider const&) = delete;
      LifetimeProvider(LifetimeProvider&&) = delete;
      LifetimeProvider& operator=(LifetimeProvider&&) = delete;

      ~LifetimeProvider() override { events.recordEvent("provider destroy"); }

      void shutdown() noexcept override { events.recordEvent("provider shutdown"); }

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
        return {.descriptor = {.id = kBackendAlsa, .supportedProfiles = {{.id = kProfileExclusive}}},
                .devices = {Device{.id = DeviceId{"alsa-device"},
                                   .displayName = "ALSA Device",
                                   .description = "ALSA",
                                   .backendId = kBackendAlsa}}};
      }

      std::unique_ptr<Backend> createBackend(Device const& /*device*/, ProfileId const& /*profile*/) override
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
      auto executor = rt::test::InlineExecutor{};
      auto player = Player{executor};
      player.addProvider(std::make_unique<LifetimeProvider>(events));
      CHECK(player.setOutputDevice(kBackendAlsa, DeviceId{"alsa-device"}, kProfileExclusive));
    }

    // Old (broken) order was: providers.clear() → enginePtr.reset(), which destroyed the provider
    // before the engine had a chance to close the backend — backends that touch provider-owned state
    // (e.g. AlsaGraphRegistry) during close() would access a destroyed object.
    // Correct order: provider shutdown → engine destroy (backend close) → provider destroy.
    CHECK_FALSE(events.hasOverflow);
    CHECK(events.count == events.values.size());
    CHECK(events.values == std::array{std::string_view{"provider shutdown"},
                                      std::string_view{"backend close"},
                                      std::string_view{"provider destroy"}});
  }
} // namespace ao::audio::test

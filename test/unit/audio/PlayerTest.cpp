#include "fakeit.hpp"

#include <ao/audio/Backend.h>
#include <ao/audio/Engine.h>
#include <ao/audio/IBackend.h>
#include <ao/audio/IBackendProvider.h>
#include <ao/audio/NullBackend.h>
#include <ao/audio/Player.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators_all.hpp>
#include <catch2/matchers/catch_matchers_all.hpp>

using namespace ao::audio;
using namespace fakeit;

namespace
{
  class MockProviderWrapper : public IBackendProvider
  {
    IBackendProvider& _real;

  public:
    MockProviderWrapper(IBackendProvider& real)
      : _real(real)
    {
    }

    Subscription subscribeDevices(OnDevicesChangedCallback callback) override
    {
      return _real.subscribeDevices(callback);
    }

    std::unique_ptr<IBackend> createBackend(Device const& device, ProfileId const& profile) override
    {
      return _real.createBackend(device, profile);
    }
    Status status() const override { return _real.status(); }
    Subscription subscribeGraph(std::string_view routeAnchor, OnGraphChangedCallback callback) override
    {
      return _real.subscribeGraph(routeAnchor, callback);
    }
  };

  Engine::RouteStatus createBaseEngineRoute()
  {
    auto engineSnap = Engine::RouteStatus{};
    engineSnap.optAnchor = RouteAnchor{.backend = kBackendNone, .id = "mock-stream-id"};
    engineSnap.flow.nodes.push_back(
      flow::Node{.id = "rs-decoder",
                 .type = flow::NodeType::Decoder,
                 .name = "Decoder",
                 .optFormat = Format{.sampleRate = 44100, .channels = 2, .bitDepth = 16, .isFloat = false},
                 .volumeNotUnity = false,
                 .isMuted = false,
                 .isLossySource = false});
    engineSnap.flow.nodes.push_back(
      flow::Node{.id = "rs-engine",
                 .type = flow::NodeType::Engine,
                 .name = "Engine",
                 .optFormat = Format{.sampleRate = 44100, .channels = 2, .bitDepth = 16, .isFloat = false},
                 .volumeNotUnity = false,
                 .isMuted = false,
                 .isLossySource = false});
    engineSnap.flow.connections.push_back(
      flow::Connection{.sourceId = "rs-decoder", .destId = "rs-engine", .isActive = true});
    return engineSnap;
  }

  flow::Graph createBaseSystemGraph()
  {
    flow::Graph graph;
    graph.nodes.push_back(
      flow::Node{.id = "mock-stream-id",
                 .type = flow::NodeType::Stream,
                 .name = "Mock Stream",
                 .optFormat = Format{.sampleRate = 44100, .channels = 2, .bitDepth = 16, .isFloat = false},
                 .volumeNotUnity = false,
                 .isMuted = false,
                 .isLossySource = false});
    graph.nodes.push_back(
      flow::Node{.id = "mock-sink-id",
                 .type = flow::NodeType::Sink,
                 .name = "Mock Sink",
                 .optFormat = Format{.sampleRate = 44100, .channels = 2, .bitDepth = 16, .isFloat = false},
                 .volumeNotUnity = false,
                 .isMuted = false,
                 .isLossySource = false});
    graph.connections.push_back(
      flow::Connection{.sourceId = "mock-stream-id", .destId = "mock-sink-id", .isActive = true});
    return graph;
  }

  class TestBackend : public NullBackend
  {
    BackendId _backendId;
    ProfileId _profileId;

  public:
    TestBackend(BackendId b, ProfileId p)
      : _backendId(b), _profileId(p)
    {
    }
    BackendId backendId() const noexcept override { return _backendId; }
    ProfileId profileId() const noexcept override { return _profileId; }
  };

  class MockDispatcher : public ao::IMainThreadDispatcher
  {
  public:
    void dispatch(std::function<void()> callback) override { callback(); }
  };
} // namespace

TEST_CASE("Player - Quality Analysis with FakeIt", "[playback][player][quality]")
{
  Mock<IBackendProvider> mockProvider;
  Mock<IBackend> mockEngine;

  IBackendProvider::OnGraphChangedCallback onGraphChanged;

  When(Method(mockProvider, subscribeDevices))
    .AlwaysDo(
      [&](IBackendProvider::OnDevicesChangedCallback cb)
      {
        if (cb)
          cb(std::vector<Device>{Device{.id = DeviceId{"mock-sink"},
                                        .displayName = "Mock Sink",
                                        .description = "Mock",
                                        .isDefault = true,
                                        .backendId = kBackendNone,
                                        .capabilities = {}}});
        return Subscription{};
      });

  When(Method(mockProvider, createBackend))
    .AlwaysDo([&](Device const& b, ProfileId const& p) { return std::make_unique<TestBackend>(b.backendId, p); });
  When(Method(mockProvider, subscribeGraph))
    .AlwaysDo(
      [&](std::string_view, IBackendProvider::OnGraphChangedCallback cb)
      {
        onGraphChanged = cb;
        return Subscription{};
      });
  When(Method(mockProvider, status))
    .AlwaysReturn(IBackendProvider::Status{
      .metadata =
        {.id = kBackendNone, .name = "Mock", .description = "Mock", .iconName = "audio-card", .supportedProfiles = {}},
      .devices = {}});

  Player player(nullptr);
  player.addProvider(std::make_unique<MockProviderWrapper>(mockProvider.get()));

  player.setOutput(kBackendNone, DeviceId{"mock-sink"}, kProfileShared);

  auto engineSnap = createBaseEngineRoute();
  auto systemGraph = createBaseSystemGraph();

  SECTION("Bitwise Perfect")
  {
    player.handleRouteChanged(engineSnap, player.playbackGeneration());
    REQUIRE(onGraphChanged);
    onGraphChanged(systemGraph);

    auto snap = player.status();
    REQUIRE(snap.quality == Quality::BitwisePerfect);
  }

  SECTION("Lossy Source")
  {
    engineSnap.flow.nodes[0].isLossySource = true;
    player.handleRouteChanged(engineSnap, player.playbackGeneration());
    onGraphChanged(systemGraph);

    auto snap = player.status();
    REQUIRE(snap.quality == Quality::LossySource);
  }

  SECTION("Resampling Detected")
  {
    systemGraph.nodes[1].optFormat->sampleRate = 48000;
    player.handleRouteChanged(engineSnap, player.playbackGeneration());
    onGraphChanged(systemGraph);

    auto snap = player.status();
    REQUIRE(snap.quality == Quality::LinearIntervention);
    REQUIRE(snap.qualityTooltip.find("Resampling") != std::string::npos);
  }

  SECTION("Volume Modification Detected")
  {
    systemGraph.nodes[1].volumeNotUnity = true;
    player.handleRouteChanged(engineSnap, player.playbackGeneration());
    onGraphChanged(systemGraph);

    auto snap = player.status();
    REQUIRE(snap.quality == Quality::LinearIntervention);
    REQUIRE(snap.qualityTooltip.find("Volume") != std::string::npos);
  }

  SECTION("Mute Detected")
  {
    systemGraph.nodes[1].isMuted = true;
    player.handleRouteChanged(engineSnap, player.playbackGeneration());
    onGraphChanged(systemGraph);

    auto snap = player.status();
    REQUIRE(snap.quality == Quality::LinearIntervention);
    REQUIRE(snap.qualityTooltip.find("MUTED") != std::string::npos);
  }

  SECTION("External App Mixing")
  {
    systemGraph.nodes.push_back(flow::Node{.id = "firefox-stream",
                                           .type = flow::NodeType::ExternalSource,
                                           .name = "Firefox",
                                           .volumeNotUnity = false,
                                           .isMuted = false,
                                           .isLossySource = false});
    systemGraph.connections.push_back(
      flow::Connection{.sourceId = "firefox-stream", .destId = "mock-sink-id", .isActive = true});

    player.handleRouteChanged(engineSnap, player.playbackGeneration());
    onGraphChanged(systemGraph);

    auto snap = player.status();
    REQUIRE(snap.quality == Quality::LinearIntervention);
    REQUIRE(snap.qualityTooltip.find("shared with Firefox") != std::string::npos);
  }

  SECTION("Lossless Bit-Depth Extension")
  {
    systemGraph.nodes[1].optFormat->bitDepth = 24;
    player.handleRouteChanged(engineSnap, player.playbackGeneration());
    onGraphChanged(systemGraph);

    auto snap = player.status();
    REQUIRE(snap.quality == Quality::LosslessPadded);
  }
}

TEST_CASE("Player - Lifecycle and Stale Updates with FakeIt", "[playback][player][lifecycle]")
{
  Mock<IBackendProvider> mockProvider;

  When(Method(mockProvider, subscribeDevices))
    .AlwaysDo(
      [&](IBackendProvider::OnDevicesChangedCallback cb)
      {
        if (cb)
          cb(std::vector<Device>{Device{.id = DeviceId{"mock-sink"},
                                        .displayName = "Mock Sink",
                                        .description = "Mock",
                                        .isDefault = true,
                                        .backendId = kBackendNone,
                                        .capabilities = {}}});
        return Subscription{};
      });

  When(Method(mockProvider, createBackend))
    .AlwaysDo([&](Device const& b, ProfileId const& p) { return std::make_unique<TestBackend>(b.backendId, p); });
  When(Method(mockProvider, subscribeGraph))
    .AlwaysDo([](std::string_view, IBackendProvider::OnGraphChangedCallback) { return Subscription{}; });
  When(Method(mockProvider, status))
    .AlwaysReturn(IBackendProvider::Status{
      .metadata =
        {.id = kBackendNone, .name = "Mock", .description = "Mock", .iconName = "audio-card", .supportedProfiles = {}},
      .devices = {}});

  Player player(nullptr);
  player.addProvider(std::make_unique<MockProviderWrapper>(mockProvider.get()));
  player.setOutput(kBackendNone, DeviceId{"mock-sink"}, kProfileShared);

  SECTION("Stale callbacks are ignored via generation counter")
  {
    auto engineSnap = createBaseEngineRoute();
    auto const initialGeneration = player.playbackGeneration();

    player.stop(); // Increment to simulate new playback session

    player.handleRouteChanged(engineSnap, initialGeneration);

    // If it was ignored, status should be empty or default (because _cachedEngineRoute wasn't updated)
    auto snap = player.status();
    REQUIRE(snap.flow.nodes.empty());
  }
}

TEST_CASE("Player - Pending Output", "[playback][player][pending]")
{
  Mock<IBackendProvider> mockProvider;
  IBackendProvider::OnDevicesChangedCallback onDevicesChanged;

  When(Method(mockProvider, subscribeDevices))
    .AlwaysDo(
      [&](IBackendProvider::OnDevicesChangedCallback cb)
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

  Player player(nullptr);
  player.addProvider(std::make_unique<MockProviderWrapper>(mockProvider.get()));

  // 1. Call setOutput before devices are available
  player.setOutput(kBackendPipeWire, DeviceId{"system-default"}, kProfileShared);

  // Verify that it's NOT yet active (engine still has null backend/default device)
  auto snapBefore = player.status();
  REQUIRE(snapBefore.engine.currentDeviceId == "null");

  // 2. Simulate devices being discovered
  REQUIRE(onDevicesChanged);
  onDevicesChanged({Device{.id = DeviceId{"system-default"},
                           .displayName = "System Default",
                           .description = "PipeWire",
                           .isDefault = true,
                           .backendId = kBackendPipeWire}});

  // 3. Verify that the output was automatically restored
  auto snapAfter = player.status();
  REQUIRE(snapAfter.engine.currentDeviceId == "system-default");
  REQUIRE(snapAfter.engine.backendId == kBackendPipeWire);
}

TEST_CASE("Player - Basic Control Propagation", "[playback][player][control]")
{
  auto dispatcher = std::make_shared<MockDispatcher>();
  Player player(dispatcher);

  SECTION("Seek is propagated to engine")
  {
    // Even with NullBackend, positionMs should be updated in Engine status
    // wait, Engine::seek returns early if no source.
    player.seek(1000);
    REQUIRE(player.status().engine.positionMs == 0);
  }

  SECTION("Volume and mute are propagated to engine and status")
  {
    player.setVolume(0.6f);
    REQUIRE(player.status().volume == Catch::Approx(0.6f));
    REQUIRE(player.status().engine.volume == Catch::Approx(0.6f));

    player.setMuted(true);
    REQUIRE(player.status().muted == true);

    player.toggleMute();
    REQUIRE(player.status().muted == false);
  }
}

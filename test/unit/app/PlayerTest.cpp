#include "fakeit.hpp"
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators_all.hpp>
#include <catch2/matchers/catch_matchers_all.hpp>

#include <ao/audio/Backend.h>
#include <ao/audio/Engine.h>
#include <ao/audio/IBackend.h>
#include <ao/audio/IBackendProvider.h>
#include <ao/audio/NullBackend.h>
#include <ao/audio/Player.h>

using namespace ao::audio;
using namespace ao::audio;
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

    std::unique_ptr<IBackend> createBackend(Device const& device) override { return _real.createBackend(device); }
    Subscription subscribeGraph(std::string_view routeAnchor, OnGraphChangedCallback callback) override
    {
      return _real.subscribeGraph(routeAnchor, callback);
    }
  };

  EngineRouteSnapshot createBaseEngineRoute()
  {
    EngineRouteSnapshot engineSnap;
    engineSnap.optAnchor = BackendRouteAnchor{.backend = BackendKind::None, .id = "mock-stream-id"};
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
    BackendKind _kind;

  public:
    TestBackend(BackendKind k)
      : _kind(k)
    {
    }
    BackendKind kind() const noexcept override { return _kind; }
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
          cb(std::vector<Device>{{.id = "mock-sink",
                                  .displayName = "Mock Sink",
                                  .description = "Mock",
                                  .isDefault = true,
                                  .backendKind = BackendKind::None,
                                  .capabilities = {}}});
        return Subscription{};
      });

  When(Method(mockProvider, createBackend))
    .AlwaysDo([&](Device const&) { return std::make_unique<ao::audio::NullBackend>(); });
  When(Method(mockProvider, subscribeGraph))
    .AlwaysDo(
      [&](std::string_view, IBackendProvider::OnGraphChangedCallback cb)
      {
        onGraphChanged = cb;
        return Subscription{};
      });

  Player player(nullptr);
  player.addProvider(std::make_unique<MockProviderWrapper>(mockProvider.get()));

  player.setOutput(BackendKind::None, "mock-sink");

  auto engineSnap = createBaseEngineRoute();
  auto systemGraph = createBaseSystemGraph();

  SECTION("Bitwise Perfect")
  {
    player.handleRouteChanged(engineSnap, player._playbackGeneration);
    REQUIRE(onGraphChanged);
    onGraphChanged(systemGraph);

    auto snap = player.snapshot();
    REQUIRE(snap.quality == Quality::BitwisePerfect);
  }

  SECTION("Lossy Source")
  {
    engineSnap.flow.nodes[0].isLossySource = true;
    player.handleRouteChanged(engineSnap, player._playbackGeneration);
    onGraphChanged(systemGraph);

    auto snap = player.snapshot();
    REQUIRE(snap.quality == Quality::LossySource);
  }

  SECTION("Resampling Detected")
  {
    systemGraph.nodes[1].optFormat->sampleRate = 48000;
    player.handleRouteChanged(engineSnap, player._playbackGeneration);
    onGraphChanged(systemGraph);

    auto snap = player.snapshot();
    REQUIRE(snap.quality == Quality::LinearIntervention);
    REQUIRE(snap.qualityTooltip.find("Resampling") != std::string::npos);
  }

  SECTION("Volume Modification Detected")
  {
    systemGraph.nodes[1].volumeNotUnity = true;
    player.handleRouteChanged(engineSnap, player._playbackGeneration);
    onGraphChanged(systemGraph);

    auto snap = player.snapshot();
    REQUIRE(snap.quality == Quality::LinearIntervention);
    REQUIRE(snap.qualityTooltip.find("Volume") != std::string::npos);
  }

  SECTION("Mute Detected")
  {
    systemGraph.nodes[1].isMuted = true;
    player.handleRouteChanged(engineSnap, player._playbackGeneration);
    onGraphChanged(systemGraph);

    auto snap = player.snapshot();
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

    player.handleRouteChanged(engineSnap, player._playbackGeneration);
    onGraphChanged(systemGraph);

    auto snap = player.snapshot();
    REQUIRE(snap.quality == Quality::LinearIntervention);
    REQUIRE(snap.qualityTooltip.find("shared with Firefox") != std::string::npos);
  }

  SECTION("Lossless Bit-Depth Extension")
  {
    systemGraph.nodes[1].optFormat->bitDepth = 24;
    player.handleRouteChanged(engineSnap, player._playbackGeneration);
    onGraphChanged(systemGraph);

    auto snap = player.snapshot();
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
          cb(std::vector<Device>{{.id = "mock-sink",
                                  .displayName = "Mock Sink",
                                  .description = "Mock",
                                  .isDefault = true,
                                  .backendKind = BackendKind::None,
                                  .capabilities = {}}});
        return Subscription{};
      });

  When(Method(mockProvider, createBackend))
    .AlwaysDo([&](Device const&) { return std::make_unique<ao::audio::NullBackend>(); });
  When(Method(mockProvider, subscribeGraph))
    .AlwaysDo([](std::string_view, IBackendProvider::OnGraphChangedCallback) { return Subscription{}; });

  Player player(nullptr);
  player.addProvider(std::make_unique<MockProviderWrapper>(mockProvider.get()));
  player.setOutput(BackendKind::None, "mock-sink");

  SECTION("Stale callbacks are ignored via generation counter")
  {
    auto engineSnap = createBaseEngineRoute();
    auto initialGeneration = player._playbackGeneration;

    player._playbackGeneration++; // Increment to simulate new playback session

    player.handleRouteChanged(engineSnap, initialGeneration);

    // If it was ignored, snapshot should be empty or default (because _cachedEngineRoute wasn't updated)
    auto snap = player.snapshot();
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
    .AlwaysDo([&](Device const& dev) { return std::make_unique<TestBackend>(dev.backendKind); });

  Player player(nullptr);
  player.addProvider(std::make_unique<MockProviderWrapper>(mockProvider.get()));

  // 1. Call setOutput before devices are available
  player.setOutput(BackendKind::PipeWire, "system-default");

  // Verify that it's NOT yet active (engine still has null backend/default device)
  auto snapBefore = player.snapshot();
  REQUIRE(snapBefore.currentDeviceId == "null");

  // 2. Simulate devices being discovered
  REQUIRE(onDevicesChanged);
  onDevicesChanged({{.id = "system-default",
                     .displayName = "System Default",
                     .description = "PipeWire",
                     .isDefault = true,
                     .backendKind = BackendKind::PipeWire}});

  // 3. Verify that the output was automatically restored
  auto snapAfter = player.snapshot();
  REQUIRE(snapAfter.currentDeviceId == "system-default");
  REQUIRE(snapAfter.backend == BackendKind::PipeWire);
}

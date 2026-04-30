#include "fakeit.hpp"
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators_all.hpp>
#include <catch2/matchers/catch_matchers_all.hpp>

#include <rs/audio/BackendTypes.h>
#include <rs/audio/IAudioBackend.h>
#include <rs/audio/IBackendManager.h>
#include <rs/audio/NullBackend.h>
#include <rs/audio/PlaybackController.h>
#include <rs/audio/PlaybackEngine.h>

using namespace rs::audio;
using namespace rs::audio;
using namespace rs::audio;
using namespace fakeit;

namespace
{
  class MockSubscription : public IGraphSubscription
  {
  public:
    virtual ~MockSubscription() override = default;
  };

  class MockManagerWrapper : public IBackendManager
  {
    IBackendManager& _real;

  public:
    MockManagerWrapper(IBackendManager& real)
      : _real(real)
    {
    }
    void setDevicesChangedCallback(OnDevicesChangedCallback callback) override
    {
      _real.setDevicesChangedCallback(callback);
    }
    std::vector<AudioDevice> enumerateDevices() override { return _real.enumerateDevices(); }
    std::unique_ptr<IAudioBackend> createBackend(AudioDevice const& device) override
    {
      return _real.createBackend(device);
    }
    std::unique_ptr<IGraphSubscription> subscribeGraph(std::string_view routeAnchor,
                                                       OnGraphChangedCallback callback) override
    {
      return _real.subscribeGraph(routeAnchor, callback);
    }
  };

  EngineRouteSnapshot createBaseEngineRoute()
  {
    EngineRouteSnapshot engineSnap;
    engineSnap.anchor = BackendRouteAnchor{.backend = BackendKind::None, .id = "mock-stream-id"};
    engineSnap.graph.nodes.push_back(
      AudioNode{.id = "rs-decoder",
                .type = AudioNodeType::Decoder,
                .name = "Decoder",
                .format = AudioFormat{.sampleRate = 44100, .channels = 2, .bitDepth = 16, .isFloat = false},
                .volumeNotUnity = false,
                .isMuted = false,
                .isLossySource = false});
    engineSnap.graph.nodes.push_back(
      AudioNode{.id = "rs-engine",
                .type = AudioNodeType::Engine,
                .name = "Engine",
                .format = AudioFormat{.sampleRate = 44100, .channels = 2, .bitDepth = 16, .isFloat = false},
                .volumeNotUnity = false,
                .isMuted = false,
                .isLossySource = false});
    engineSnap.graph.links.push_back(AudioLink{.sourceId = "rs-decoder", .destId = "rs-engine", .isActive = true});
    return engineSnap;
  }

  AudioGraph createBaseSystemGraph()
  {
    AudioGraph graph;
    graph.nodes.push_back(
      AudioNode{.id = "mock-stream-id",
                .type = AudioNodeType::Stream,
                .name = "Mock Stream",
                .format = AudioFormat{.sampleRate = 44100, .channels = 2, .bitDepth = 16, .isFloat = false},
                .volumeNotUnity = false,
                .isMuted = false,
                .isLossySource = false});
    graph.nodes.push_back(
      AudioNode{.id = "mock-sink-id",
                .type = AudioNodeType::Sink,
                .name = "Mock Sink",
                .format = AudioFormat{.sampleRate = 44100, .channels = 2, .bitDepth = 16, .isFloat = false},
                .volumeNotUnity = false,
                .isMuted = false,
                .isLossySource = false});
    graph.links.push_back(AudioLink{.sourceId = "mock-stream-id", .destId = "mock-sink-id", .isActive = true});
    return graph;
  }
} // namespace

TEST_CASE("PlaybackController - Quality Analysis with FakeIt", "[playback][controller][quality]")
{
  Mock<IBackendManager> mockManager;
  Mock<IAudioBackend> mockBackend;

  IBackendManager::OnGraphChangedCallback capturedCallback;

  Fake(Method(mockManager, setDevicesChangedCallback));
  When(Method(mockManager, enumerateDevices))
    .AlwaysReturn(std::vector<AudioDevice>{{.id = "mock-sink",
                                            .displayName = "Mock Sink",
                                            .description = "Mock",
                                            .isDefault = true,
                                            .backendKind = BackendKind::None,
                                            .capabilities = {}}});
  When(Method(mockManager, createBackend))
    .AlwaysDo([&](AudioDevice const&) { return std::make_unique<rs::audio::NullBackend>(); });
  When(Method(mockManager, subscribeGraph))
    .AlwaysDo(
      [&](std::string_view, IBackendManager::OnGraphChangedCallback cb)
      {
        capturedCallback = cb;
        return std::make_unique<MockSubscription>();
      });

  PlaybackController controller(nullptr);
  controller.addManager(std::make_unique<MockManagerWrapper>(mockManager.get()));

  controller.setOutput(BackendKind::None, "mock-sink");

  auto engineSnap = createBaseEngineRoute();
  auto systemGraph = createBaseSystemGraph();

  SECTION("Bitwise Perfect")
  {
    controller.handleRouteChanged(engineSnap, controller._playbackGeneration);
    REQUIRE(capturedCallback);
    capturedCallback(systemGraph);

    auto snap = controller.snapshot();
    REQUIRE(snap.quality == AudioQuality::BitwisePerfect);
  }

  SECTION("Lossy Source")
  {
    engineSnap.graph.nodes[0].isLossySource = true;
    controller.handleRouteChanged(engineSnap, controller._playbackGeneration);
    capturedCallback(systemGraph);

    auto snap = controller.snapshot();
    REQUIRE(snap.quality == AudioQuality::LossySource);
  }

  SECTION("Resampling Detected")
  {
    systemGraph.nodes[1].format->sampleRate = 48000;
    controller.handleRouteChanged(engineSnap, controller._playbackGeneration);
    capturedCallback(systemGraph);

    auto snap = controller.snapshot();
    REQUIRE(snap.quality == AudioQuality::LinearIntervention);
    REQUIRE(snap.qualityTooltip.find("Resampling") != std::string::npos);
  }

  SECTION("Volume Modification Detected")
  {
    systemGraph.nodes[1].volumeNotUnity = true;
    controller.handleRouteChanged(engineSnap, controller._playbackGeneration);
    capturedCallback(systemGraph);

    auto snap = controller.snapshot();
    REQUIRE(snap.quality == AudioQuality::LinearIntervention);
    REQUIRE(snap.qualityTooltip.find("Volume") != std::string::npos);
  }

  SECTION("Mute Detected")
  {
    systemGraph.nodes[1].isMuted = true;
    controller.handleRouteChanged(engineSnap, controller._playbackGeneration);
    capturedCallback(systemGraph);

    auto snap = controller.snapshot();
    REQUIRE(snap.quality == AudioQuality::LinearIntervention);
    REQUIRE(snap.qualityTooltip.find("MUTED") != std::string::npos);
  }

  SECTION("External App Mixing")
  {
    systemGraph.nodes.push_back(AudioNode{.id = "firefox-stream",
                                          .type = AudioNodeType::ExternalSource,
                                          .name = "Firefox",
                                          .volumeNotUnity = false,
                                          .isMuted = false,
                                          .isLossySource = false});
    systemGraph.links.push_back(AudioLink{.sourceId = "firefox-stream", .destId = "mock-sink-id", .isActive = true});

    controller.handleRouteChanged(engineSnap, controller._playbackGeneration);
    capturedCallback(systemGraph);

    auto snap = controller.snapshot();
    REQUIRE(snap.quality == AudioQuality::LinearIntervention);
    REQUIRE(snap.qualityTooltip.find("shared with Firefox") != std::string::npos);
  }

  SECTION("Lossless Bit-Depth Extension")
  {
    systemGraph.nodes[1].format->bitDepth = 24;
    controller.handleRouteChanged(engineSnap, controller._playbackGeneration);
    capturedCallback(systemGraph);

    auto snap = controller.snapshot();
    REQUIRE(snap.quality == AudioQuality::LosslessPadded);
  }
}

TEST_CASE("PlaybackController - Lifecycle and Stale Updates with FakeIt", "[playback][controller][lifecycle]")
{
  Mock<IBackendManager> mockManager;
  Fake(Method(mockManager, setDevicesChangedCallback));
  When(Method(mockManager, enumerateDevices))
    .AlwaysReturn(std::vector<AudioDevice>{{.id = "mock-sink",
                                            .displayName = "Mock Sink",
                                            .description = "Mock",
                                            .isDefault = true,
                                            .backendKind = BackendKind::None,
                                            .capabilities = {}}});
  When(Method(mockManager, createBackend))
    .AlwaysDo([&](AudioDevice const&) { return std::make_unique<rs::audio::NullBackend>(); });
  When(Method(mockManager, subscribeGraph))
    .AlwaysDo([](std::string_view, IBackendManager::OnGraphChangedCallback)
              { return std::make_unique<MockSubscription>(); });

  PlaybackController controller(nullptr);
  controller.addManager(std::make_unique<MockManagerWrapper>(mockManager.get()));
  controller.setOutput(BackendKind::None, "mock-sink");

  SECTION("Stale callbacks are ignored via generation counter")
  {
    auto engineSnap = createBaseEngineRoute();
    auto initialGeneration = controller._playbackGeneration;

    controller._playbackGeneration++; // Increment to simulate new playback session

    controller.handleRouteChanged(engineSnap, initialGeneration);

    // If it was ignored, snapshot should be empty or default (because _cachedEngineRoute wasn't updated)
    auto snap = controller.snapshot();
    REQUIRE(snap.graph.nodes.empty());
  }
}

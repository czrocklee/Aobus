#include <catch2/catch.hpp>

#include "core/playback/PlaybackController.h"

#include "core/playback/PlaybackEngine.h"
#include "core/backend/BackendTypes.h"
#include "core/backend/NullBackend.h"

using namespace app::core::playback;
using namespace app::core::backend;
using namespace app::core;

TEST_CASE("PlaybackController - Graph Merging and Quality Analysis", "[playback][controller]")
{
  PlaybackController controller(nullptr);
  controller.addManager(std::make_unique<NullBackend::NullManager>());

  // Default device is None (NullManager)
  controller.setOutput(BackendKind::None, "null");

  // Directly call the private method to simulate engine route change
  EngineRouteSnapshot engineSnap;
  engineSnap.anchor = BackendRouteAnchor{.backend = BackendKind::None, .id = "null-sink-id"};
  engineSnap.graph.nodes.push_back(AudioNode{
    .id = "rs-engine", 
    .type = AudioNodeType::Intermediary, 
    .name = "RockStudio Engine",
    .format = AudioFormat{.sampleRate = 44100, .channels = 2, .bitDepth = 16, .isFloat = false},
    .volumeNotUnity = false,
    .isMuted = false,
    .isLossySource = false
  });
  engineSnap.graph.links.push_back(AudioLink{.sourceId = "rs-decoder", .destId = "rs-engine", .isActive = true});

  // Call the private handler
  controller.handleRouteChanged(engineSnap, controller._playbackGeneration);

  // The NullManager statically returns a graph containing "null-sink" linked to the stream node.
  // PlaybackController merges it.
  auto snap = controller.snapshot();
  
  REQUIRE(snap.graph.nodes.size() > 0);
  
  bool hasEngine = false;
  bool hasSink = false;
  for (auto const& node : snap.graph.nodes) {
    if (node.id == "rs-engine") hasEngine = true;
    if (node.id == "null-sink") hasSink = true;
  }
  REQUIRE(hasEngine);
  REQUIRE(hasSink);

  // Verify Quality Analysis was run
  REQUIRE(snap.quality == AudioQuality::BitwisePerfect);
  REQUIRE(snap.qualityTooltip.find("Audio Routing Analysis") != std::string::npos);
}

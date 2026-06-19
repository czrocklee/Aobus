// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/audio/Backend.h>
#include <ao/audio/Format.h>
#include <ao/audio/QualityAnalyzer.h>
#include <ao/audio/flow/Graph.h>
#include <ao/uimodel/playback/AudioQualityFormat.h>

#include <catch2/catch_test_macros.hpp>

namespace ao::uimodel::playback
{
  TEST_CASE("AudioQualityFormat - audioNodeTypeLabel", "[uimodel][playback]")
  {
    CHECK(audioNodeTypeLabel(audio::flow::NodeType::Source) == "[Source]");
    CHECK(audioNodeTypeLabel(audio::flow::NodeType::Decoder) == "[Decoder]");
    CHECK(audioNodeTypeLabel(audio::flow::NodeType::Engine) == "[Engine]");
    CHECK(audioNodeTypeLabel(audio::flow::NodeType::Stream) == "[Stream]");
    CHECK(audioNodeTypeLabel(audio::flow::NodeType::Intermediary) == "[Filter]");
    CHECK(audioNodeTypeLabel(audio::flow::NodeType::Sink) == "[Device]");
    CHECK(audioNodeTypeLabel(audio::flow::NodeType::ExternalSource) == "[Other Source]");
  }

  TEST_CASE("AudioQualityFormat - audioFormatLabel", "[uimodel][playback]")
  {
    auto format = audio::Format{.sampleRate = 44100, .channels = 2, .bitDepth = 16};
    CHECK(audioFormatLabel(format) == "44.1 kHz · 16-bit · Stereo");

    format.channels = 1;
    CHECK(audioFormatLabel(format) == "44.1 kHz · 16-bit · Mono");

    format.channels = 6;
    format.sampleRate = 48000;
    format.bitDepth = 24;
    CHECK(audioFormatLabel(format) == "48.0 kHz · 24-bit · 6 ch");

    // A low-resolution source padded into a wider container: by default the
    // container width is shown (downstream nodes), but with preferValidBits the
    // true precision is reported (source node).
    format.channels = 2;
    format.sampleRate = 44100;
    format.bitDepth = 32;
    format.validBits = 16;
    CHECK(audioFormatLabel(format) == "44.1 kHz · 32-bit · Stereo");
    CHECK(audioFormatLabel(format, true) == "44.1 kHz · 16-bit · Stereo");

    format.validBits = 24;
    CHECK(audioFormatLabel(format, true) == "44.1 kHz · 24-bit · Stereo");

    // preferValidBits with validBits == 0 falls back to the container width.
    format.validBits = 0;
    CHECK(audioFormatLabel(format, true) == "44.1 kHz · 32-bit · Stereo");
  }

  TEST_CASE("AudioQualityFormat - qualityForFinding", "[uimodel][playback]")
  {
    CHECK(qualityForFinding(audio::QualityFinding{.kind = audio::QualityFindingKind::BitPerfect}) ==
          audio::Quality::BitwisePerfect);
    CHECK(qualityForFinding(audio::QualityFinding{.kind = audio::QualityFindingKind::LossySource}) ==
          audio::Quality::LossySource);
    CHECK(qualityForFinding(audio::QualityFinding{.kind = audio::QualityFindingKind::Resampling}) ==
          audio::Quality::LinearIntervention);
    CHECK(qualityForFinding(audio::QualityFinding{.kind = audio::QualityFindingKind::SoftwareVolumeModification}) ==
          audio::Quality::LinearIntervention);
    CHECK(qualityForFinding(audio::QualityFinding{.kind = audio::QualityFindingKind::HardwareVolumeModification}) ==
          audio::Quality::BitwisePerfect);
    CHECK(qualityForFinding(audio::QualityFinding{.kind = audio::QualityFindingKind::UnclassifiedVolumeModification}) ==
          audio::Quality::LinearIntervention);
    CHECK(qualityForFinding(audio::QualityFinding{.kind = audio::QualityFindingKind::LosslessPadding}) ==
          audio::Quality::LosslessPadded);
    CHECK(qualityForFinding(audio::QualityFinding{.kind = audio::QualityFindingKind::LosslessFloat}) ==
          audio::Quality::LosslessFloat);
  }

  TEST_CASE("AudioQualityFormat - playbackPath", "[uimodel][playback]")
  {
    auto graph = audio::flow::Graph{};

    // Empty graph
    CHECK(playbackPath(graph).empty());

    graph.nodes.push_back({.id = "ao-source", .name = "Source"});
    graph.nodes.push_back({.id = "ao-decoder", .name = "Decoder"});
    graph.nodes.push_back({.id = "ao-engine", .name = "Engine"});
    graph.nodes.push_back({.id = "ao-sink", .name = "Sink"});
    graph.nodes.push_back({.id = "ao-other", .name = "Other"}); // Unconnected

    graph.connections.push_back({.sourceId = "ao-source", .destId = "ao-decoder", .isActive = true});
    graph.connections.push_back({.sourceId = "ao-decoder", .destId = "ao-engine", .isActive = true});
    graph.connections.push_back({.sourceId = "ao-engine", .destId = "ao-sink", .isActive = true});

    auto const path = playbackPath(graph);
    REQUIRE(path.size() == 4);
    CHECK(path[0]->id == "ao-source");
    CHECK(path[1]->id == "ao-decoder");
    CHECK(path[2]->id == "ao-engine");
    CHECK(path[3]->id == "ao-sink");
  }
} // namespace ao::uimodel::playback

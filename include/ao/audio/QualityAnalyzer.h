// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/audio/Format.h>
#include <ao/audio/Quality.h>
#include <ao/audio/flow/Graph.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ao::audio
{
  /**
   * @brief Classification of a quality-relevant observation at a single node.
   */
  enum class QualityFindingKind : std::uint8_t
  {
    Unknown,
    BitPerfect,
    LossySource,
    SoftwareVolumeModification,
    SoftwareAmplification,
    HardwareVolumeModification,
    UnclassifiedVolumeModification,
    Muted,
    Resampling,
    ChannelMapping,
    LosslessPadding,
    LosslessFloat,
    LosslessRoundTrip,
    Truncation,
    MixedSources,
  };

  /**
   * @brief A single quality-relevant observation at a node in the audio path.
   *
   * Format transition findings (Resampling, ChannelMapping, LosslessPadding,
   * LosslessFloat, Truncation) carry the source and destination formats.
   * MixedSources findings carry the names of other applications sharing the node.
   * Software volume findings carry gain when the backend reported a magnitude.
   */
  struct QualityFinding final
  {
    QualityFindingKind kind = QualityFindingKind::Unknown;
    Quality quality = Quality::Unknown;
    float gain = 0.0F;
    std::optional<Format> optFromFormat{};
    std::optional<Format> optToFormat{};
    std::vector<std::string> sharedApps{};

    bool operator==(QualityFinding const&) const = default;
  };

  /**
   * @brief Quality assessment for a single node in the playback path.
   *
   * Every node in the analyzed path receives exactly one assessment.
   * Nodes with no quality issues receive a single BitPerfect finding.
   */
  struct NodeQualityAssessment final
  {
    std::string nodeId{};
    std::string nodeName{};
    flow::NodeType nodeType = flow::NodeType::Intermediary;
    std::optional<Format> optFormat{};
    Quality worstQuality = Quality::BitwisePerfect;
    std::vector<QualityFinding> findings{};

    bool operator==(NodeQualityAssessment const&) const = default;
  };

  /**
   * @brief The result of an audio path quality analysis.
   */
  struct QualityResult final
  {
    Quality sourceQuality = Quality::Unknown;
    Quality pipelineQuality = Quality::Unknown;
    Quality overall = Quality::Unknown;
    bool fullyVerified = true;
    std::vector<NodeQualityAssessment> assessments{};

    bool operator==(QualityResult const&) const = default;
  };

  /**
   * @brief Returns the worse of two quality levels.
   *
   * Encapsulates the severity ordering so callers are not coupled
   * to the numeric values of the Quality enum.
   */
  Quality worseQuality(Quality lhs, Quality rhs) noexcept;

  /**
   * @brief Analyzes a flow graph to determine the overall audio quality.
   *
   * This is a pure function that evaluates the signal path from the decoder
   * to the final output device, producing a per-node assessment of quality
   * findings.
   *
   * @param graph The complete system audio graph to analyze.
   * @return A QualityResult containing per-node assessments and the global verdict.
   */
  QualityResult analyzeAudioQuality(flow::Graph const& graph);
} // namespace ao::audio

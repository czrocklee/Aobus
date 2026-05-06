// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/audio/Backend.h>
#include <ao/audio/flow/Graph.h>

#include <string>

namespace ao::audio
{
  /**
   * @brief The result of an audio path quality analysis.
   */
  struct QualityResult final
  {
    Quality quality = Quality::Unknown;
    std::string tooltip;

    bool operator==(QualityResult const&) const = default;
  };

  /**
   * @brief Analyzes a flow graph to determine the overall audio quality.
   *
   * This is a pure function that evaluates the signal path from the decoder
   * to the final output device, identifying any lossy steps or linear
   * interventions.
   *
   * @param graph The complete system audio graph to analyze.
   * @return A QualityResult containing the verdict and a descriptive tooltip.
   */
  QualityResult analyzeAudioQuality(flow::Graph const& graph);
} // namespace ao::audio

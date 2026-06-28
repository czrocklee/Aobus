// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/audio/Backend.h>
#include <ao/audio/QualityAnalyzer.h>
#include <ao/audio/flow/Graph.h>

#include <string>
#include <vector>

namespace ao::audio
{
  struct Format;
}

namespace ao::uimodel
{
  std::string audioNodeTypeLabel(audio::flow::NodeType type);

  /**
   * @brief Formats a sample format as "kHz · bit · channels" for display.
   *
   * @param preferValidBits When true and the format carries a non-zero valid-bit
   * count, report the meaningful precision (validBits) instead of the storage
   * container width (bitDepth). Used for the source node so a low-resolution
   * track padded into a wider container (e.g. 16-bit into a 32-bit word) is shown
   * at its true resolution, while downstream nodes still report the transport
   * container width.
   */
  std::string audioFormatLabel(audio::Format const& format, bool preferValidBits = false);
  std::string audioFindingLabel(audio::QualityFinding const& finding);
  std::string audioQualityConclusion(audio::Quality quality);

  /**
   * @brief Maps an individual quality finding to a UI quality category.
   * This is used solely for determining the color of the finding's bullet point in the UI.
   */
  audio::Quality qualityForFinding(audio::QualityFinding const& finding) noexcept;

  /**
   * @brief Returns the active nodes in the playback path, in order from decoder to sink.
   *
   * @note The returned pointers point directly into the provided graph. The caller
   * must ensure that the graph outlives the returned vector.
   */
  std::vector<audio::flow::Node const*> playbackPath(audio::flow::Graph const& graph);
} // namespace ao::uimodel

// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/audio/Backend.h>
#include <ao/audio/Format.h>
#include <ao/audio/QualityAnalyzer.h>
#include <ao/audio/flow/Graph.h>

#include <string>
#include <vector>

namespace ao::uimodel::playback
{
  std::string audioNodeTypeLabel(audio::flow::NodeType type);
  std::string audioFormatLabel(audio::Format const& format);
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
} // namespace ao::uimodel::playback

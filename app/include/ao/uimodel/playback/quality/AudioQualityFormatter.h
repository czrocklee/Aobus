// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/audio/NodeFormat.h>
#include <ao/audio/Quality.h>
#include <ao/audio/QualityAnalyzer.h>
#include <ao/audio/flow/Graph.h>
#include <ao/rt/PlaybackState.h>

#include <cstdint>
#include <string>

namespace ao::uimodel
{
  enum class AudioQualityCategory : std::uint8_t
  {
    Unknown,
    Medal,
    Positive,
    Diagnostic,
    Warning,
    Informational,
    Clipped,
  };

  struct AudioQualityPresentation final
  {
    std::string headline{};
    AudioQualityCategory category = AudioQualityCategory::Unknown;
  };

  std::string audioNodeTypeLabel(audio::flow::NodeType type);

  /**
   * @brief Formats a sample format as "kHz · bit · channels" for display.
   *
   * Signal nodes report logical precision; PCM nodes report their concrete
   * container width.
   */
  std::string audioFormatLabel(audio::NodeFormat const& format);
  std::string audioFindingLabel(audio::QualityFinding const& finding);
  AudioQualityCategory audioFindingCategory(audio::QualityFinding const& finding) noexcept;
  std::string audioQualityConclusion(audio::Quality quality);
  AudioQualityCategory audioQualityCategory(audio::Quality quality) noexcept;
  AudioQualityPresentation audioQualityPresentation(rt::QualityState const& state);
} // namespace ao::uimodel

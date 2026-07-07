// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/audio/Backend.h>
#include <ao/audio/QualityAnalyzer.h>
#include <ao/audio/flow/Graph.h>
#include <ao/rt/PlaybackState.h>

#include <cstdint>
#include <string>

namespace ao::audio
{
  struct Format;
}

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
   * @param preferValidBits When true and the format carries a non-zero valid-bit
   * count, report the meaningful precision (validBits) instead of the storage
   * container width (bitDepth). Used for the source node so a low-resolution
   * track padded into a wider container (e.g. 16-bit into a 32-bit word) is shown
   * at its true resolution, while downstream nodes still report the transport
   * container width.
   */
  std::string audioFormatLabel(audio::Format const& format, bool preferValidBits = false);
  std::string audioFindingLabel(audio::QualityFinding const& finding);
  AudioQualityCategory audioFindingCategory(audio::QualityFinding const& finding) noexcept;
  std::string audioQualityConclusion(audio::Quality quality);
  AudioQualityCategory audioQualityCategory(audio::Quality quality) noexcept;
  AudioQualityPresentation audioQualityPresentation(rt::QualityState const& state);
} // namespace ao::uimodel

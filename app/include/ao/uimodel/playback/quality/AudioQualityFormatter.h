// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/audio/NodeFormat.h>
#include <ao/audio/Quality.h>
#include <ao/audio/QualityAnalyzer.h>
#include <ao/audio/flow/Graph.h>
#include <ao/i18n/MessageCatalog.h>
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

  class AudioQualityFormatter final
  {
  public:
    explicit AudioQualityFormatter(i18n::MessageCatalog catalog);

    std::string nodeTypeLabel(audio::flow::NodeType type) const;

    /**
     * @brief Formats a sample format as "kHz · bit · channels" for display.
     *
     * Signal nodes report logical precision; PCM nodes report their concrete
     * container width.
     */
    std::string formatLabel(audio::NodeFormat const& format) const;
    std::string findingLabel(audio::QualityFinding const& finding) const;
    std::string qualityConclusion(audio::Quality quality) const;
    AudioQualityPresentation presentation(rt::QualityState const& state) const;

  private:
    i18n::MessageCatalog _catalog;
  };

  AudioQualityCategory audioFindingCategory(audio::QualityFinding const& finding) noexcept;
  AudioQualityCategory audioQualityCategory(audio::Quality quality) noexcept;
} // namespace ao::uimodel

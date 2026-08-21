// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "TrackDetailLines.h"

#include "PlaybackStatusFormatter.h"
#include "TrackListEntry.h"
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackRow.h>
#include <ao/uimodel/field/TrackFieldFormatter.h>
#include <ao/uimodel/presentation/PresentationTextCatalog.h>

#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::tui
{
  namespace
  {
    std::string blankFallback(std::string_view value)
    {
      return value.empty() ? std::string{"-"} : std::string{value};
    }

    std::string numberFallback(std::uint32_t const value)
    {
      return value == 0 ? std::string{"-"} : std::format("{}", value);
    }

    std::string textOrPlaceholder(std::string value)
    {
      return value.empty() ? std::string{"-"} : std::move(value);
    }
  } // namespace

  std::vector<TrackDetailLine> trackDetailLines(uimodel::PresentationTextCatalog const& textCatalog,
                                                rt::TrackRow const& row)
  {
    auto lines = std::vector<TrackDetailLine>{};
    constexpr std::size_t kTrackDetailLineCount = 16;
    lines.reserve(kTrackDetailLineCount);
    lines.push_back({.label = std::string{textCatalog.trackFieldLabel(rt::TrackField::Title)},
                     .value = trackDisplayTitle(textCatalog, row)});
    lines.push_back(
      {.label = std::string{textCatalog.trackFieldLabel(rt::TrackField::Artist)}, .value = blankFallback(row.artist)});
    lines.push_back(
      {.label = std::string{textCatalog.trackFieldLabel(rt::TrackField::Album)}, .value = blankFallback(row.album)});
    lines.push_back({.label = std::string{textCatalog.trackFieldLabel(rt::TrackField::AlbumArtist)},
                     .value = blankFallback(row.albumArtist)});
    lines.push_back({.label = std::string{textCatalog.trackFieldLabel(rt::TrackField::Composer)},
                     .value = blankFallback(row.composer)});
    lines.push_back({.label = std::string{textCatalog.trackFieldLabel(rt::TrackField::Conductor)},
                     .value = blankFallback(row.conductor)});
    lines.push_back({.label = std::string{textCatalog.trackFieldLabel(rt::TrackField::Ensemble)},
                     .value = blankFallback(row.ensemble)});
    lines.push_back({.label = std::string{textCatalog.trackFieldLabel(rt::TrackField::Soloist)},
                     .value = blankFallback(row.soloist)});
    lines.push_back(
      {.label = std::string{textCatalog.trackFieldLabel(rt::TrackField::Genre)}, .value = blankFallback(row.genre)});
    lines.push_back(
      {.label = std::string{textCatalog.trackFieldLabel(rt::TrackField::Year)}, .value = numberFallback(row.year)});
    lines.push_back(
      {.label = std::string{textCatalog.trackFieldLabel(rt::TrackField::DisplayTrackNumber)},
       .value = textOrPlaceholder(uimodel::formatDisplayTrackNumber(row.discNumber, row.discTotal, row.trackNumber))});
    lines.push_back({.label = std::string{textCatalog.trackFieldLabel(rt::TrackField::Duration)},
                     .value = row.duration.count() > 0 ? formatDuration(row.duration) : std::string{"-"}});
    lines.push_back({.label = std::string{textCatalog.trackFieldLabel(rt::TrackField::Codec)},
                     .value = textOrPlaceholder(uimodel::formatCodec(row.codec))});
    lines.push_back({.label = std::string{textCatalog.trackFieldLabel(rt::TrackField::SampleRate)},
                     .value = textOrPlaceholder(uimodel::formatSampleRate(row.sampleRate))});
    lines.push_back({.label = std::string{textCatalog.trackFieldLabel(rt::TrackField::BitDepth)},
                     .value = textOrPlaceholder(uimodel::formatBitDepth(row.bitDepth))});
    lines.push_back(
      {.label = std::string{textCatalog.trackFieldLabel(rt::TrackField::Tags)}, .value = blankFallback(row.tags)});
    return lines;
  }
} // namespace ao::tui

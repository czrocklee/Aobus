// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "TrackDetailLines.h"

#include "PlaybackStatusFormatter.h"
#include "TrackListEntry.h"
#include <ao/rt/TrackRow.h>
#include <ao/uimodel/field/TrackFieldFormatter.h>

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

  std::vector<TrackDetailLine> trackDetailLines(rt::TrackRow const& row)
  {
    auto lines = std::vector<TrackDetailLine>{};
    constexpr std::size_t kTrackDetailLineCount = 16;
    lines.reserve(kTrackDetailLineCount);
    lines.push_back({.label = "Title", .value = trackDisplayTitle(row)});
    lines.push_back({.label = "Artist", .value = blankFallback(row.artist)});
    lines.push_back({.label = "Album", .value = blankFallback(row.album)});
    lines.push_back({.label = "Album Artist", .value = blankFallback(row.albumArtist)});
    lines.push_back({.label = "Composer", .value = blankFallback(row.composer)});
    lines.push_back({.label = "Conductor", .value = blankFallback(row.conductor)});
    lines.push_back({.label = "Ensemble", .value = blankFallback(row.ensemble)});
    lines.push_back({.label = "Soloist", .value = blankFallback(row.soloist)});
    lines.push_back({.label = "Genre", .value = blankFallback(row.genre)});
    lines.push_back({.label = "Year", .value = numberFallback(row.year)});
    lines.push_back(
      {.label = "Track",
       .value = textOrPlaceholder(uimodel::formatDisplayTrackNumber(row.discNumber, row.discTotal, row.trackNumber))});
    lines.push_back(
      {.label = "Duration", .value = row.duration.count() > 0 ? formatDuration(row.duration) : std::string{"-"}});
    lines.push_back({.label = "Codec", .value = textOrPlaceholder(uimodel::formatCodec(row.codec))});
    lines.push_back({.label = "Sample Rate", .value = textOrPlaceholder(uimodel::formatSampleRate(row.sampleRate))});
    lines.push_back({.label = "Bit Depth", .value = textOrPlaceholder(uimodel::formatBitDepth(row.bitDepth))});
    lines.push_back({.label = "Tags", .value = blankFallback(row.tags)});
    return lines;
  }
} // namespace ao::tui
